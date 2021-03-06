/*
 * This file is part of trust|me
 * Copyright(c) 2013 - 2017 Fraunhofer AISEC
 * Fraunhofer-Gesellschaft zur Förderung der angewandten Forschung e.V.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2 (GPL 2), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GPL 2 license for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Fraunhofer AISEC <trustme@aisec.fraunhofer.de>
 */

#include "guestos_mgr.h"
#include "guestos.h"
#include "guestos_config.h"

#include "cmld.h"
#include "download.h"
#include "smartcard.h"

#include "common/macro.h"
#include "common/list.h"
#include "common/mem.h"
#include "common/file.h"
#include "common/fd.h"
#include "common/dir.h"
#include "common/event.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>

#define GUESTOS_MGR_VERIFY_HASH_ALGO SHA512
#define GUESTOS_MGR_FILE_MOVE_BLOCKSIZE 4096

// Log title and messages regarding trustme system updates.
#define GUESTOS_MGR_UPDATE_TITLE "Trustme Update"
#define GUESTOS_MGR_UPDATE_DOWNLOAD "Downloading..."
#define GUESTOS_MGR_UPDATE_DOWNLOAD_NO_WIFI "Waiting for WiFi connection..."
#define GUESTOS_MGR_UPDATE_SUCCESS "Reboot to install"
#define GUESTOS_MGR_UPDATE_FAILED "Download failed"

#define SCD_TOKEN_DIR "/data/cml/tokens"
#define LOCALCA_ROOT_CERT SCD_TOKEN_DIR "/localca_rootca.cert"
#define TRUSTED_CA_STORE SCD_TOKEN_DIR "/ca"

static list_t *guestos_list = NULL;

static const char *guestos_basepath = NULL;
static bool guestos_mgr_allow_locally_signed = false;

/******************************************************************************/

static void
guestos_mgr_purge_obsolete(void)
{
	INFO("Looking for obsolete GuestOSes to purge...");
	for (list_t *l = guestos_list; l;) {
		list_t *next = l->next;
		guestos_t *os = l->data;
		guestos_t *latest = guestos_mgr_get_latest_by_name(guestos_get_name(os), true);
		if ((latest && guestos_get_version(os) < guestos_get_version(latest))) {
			guestos_list = list_unlink(guestos_list, l);
			guestos_purge(os);
			guestos_free(os);
		}
		l = next;
	}
}

static int
guestos_mgr_load_operatingsystems_cb(const char *path, const char *name, UNUSED void *data)
{
	int res = 0; // counter
	guestos_verify_result_t guestos_verified = GUESTOS_UNSIGNED;

	char *dir = mem_printf("%s/%s", path, name);
	if (!file_is_dir(dir))
		goto cleanup;

	char *cfg_file = guestos_get_cfg_file_new(dir);
	char *sig_file = guestos_get_sig_file_new(dir);
	char *cert_file = guestos_get_cert_file_new(dir);

	smartcard_crypto_verify_result_t verify_result = smartcard_crypto_verify_file_block(
		cfg_file, sig_file, cert_file, GUESTOS_MGR_VERIFY_HASH_ALGO);

	switch (verify_result) {
	case VERIFY_GOOD:
		guestos_verified = GUESTOS_SIGNED;
		INFO("Signature of GuestOS OK (GOOD)");
		break;
	case VERIFY_LOCALLY_SIGNED:
		guestos_verified = GUESTOS_LOCALLY_SIGNED;
		if (guestos_mgr_allow_locally_signed) {
			INFO("Signature of GuestOS OK (locally signed)");
			break;
		}
		// fallthrough
	default:
		guestos_verified = GUESTOS_UNSIGNED;
		ERROR("Signature verification failed (%d) while loading GuestOS config %s, skipping.",
		      verify_result, cfg_file);
		res = 1;
		goto cleanup_files;
	}

	if (guestos_mgr_add_from_file(cfg_file, guestos_verified) < 0) {
		WARN("Could not add guest operating system from file %s.", cfg_file);
	} else {
		res = 1;
	}

cleanup_files:
	mem_free(cfg_file);
	mem_free(sig_file);
	mem_free(cert_file);
cleanup:
	mem_free(dir);
	return res;
}

static int
guestos_mgr_load_operatingsystems(void)
{
	if (dir_foreach(guestos_basepath, &guestos_mgr_load_operatingsystems_cb, NULL) < 0) {
		WARN("Could not open %s to load operating system", guestos_basepath);
		return -1;
	}

	if (!guestos_list) {
		// Seems we dont have any operating system on storage
		WARN("No guest OS found on storage.");
		return -1;
	}

	guestos_mgr_purge_obsolete();

	return 0;
}

static bool
guestos_mgr_is_guestos_used_by_containers(const char *os_name)
{
	ASSERT(os_name);
	int n = cmld_containers_get_count();
	for (int i = 0; i < n; i++) {
		container_t *c = cmld_container_get_by_index(i);
		const char *container_os_name = guestos_get_name(container_get_os(c));
		if (!strcmp(container_os_name, os_name)) {
			return true;
		}
	}
	return false;
}

/******************************************************************************/

int
guestos_mgr_init(const char *path, bool allow_locally_signed)
{
	ASSERT(path);
	ASSERT(!guestos_basepath);

	if (mkdir(path, 0700) < 0 && errno != EEXIST)
		return -1;

	guestos_basepath = mem_strdup(path);
	guestos_mgr_allow_locally_signed = allow_locally_signed;

	return guestos_mgr_load_operatingsystems();
}

int
guestos_mgr_add_from_file(const char *file, guestos_verify_result_t verify_result)
{
	ASSERT(file);

	guestos_t *os = guestos_new_from_file(file, guestos_basepath);
	if (!os) {
		return -1;
	}

	guestos_set_verify_result(os, verify_result);
	guestos_list = list_append(guestos_list, os);

	return 0;
}

void
guestos_mgr_delete(guestos_t *os)
{
	ASSERT(os);
	const char *os_name = guestos_get_name(os);
	if (guestos_mgr_is_guestos_used_by_containers(os_name)) {
		WARN("Containers which use guestos %s still exist! Not deleting anything.",
		     os_name);
		return;
	}

	INFO("Deleting GuestOS: %s", os_name);

	guestos_purge(os);
	guestos_list = list_remove(guestos_list, os);
	guestos_free(os);
}

/******************************************************************************/

static void
download_complete_cb(bool complete, unsigned int count, guestos_t *os, UNUSED void *data)
{
	IF_NULL_RETURN_ERROR(os);

	if (!complete || count > 0) {
		INFO("%s %s", GUESTOS_MGR_UPDATE_TITLE,
		     complete ? GUESTOS_MGR_UPDATE_SUCCESS : GUESTOS_MGR_UPDATE_FAILED);
	}
}

/**
 * Downloads, if necessary, the images for the latest (by version) available GuestOS with the given name.
 * @param name name of the GuestOS
 */
static void
guestos_mgr_download_latest(const char *name)
{
	IF_NULL_RETURN(name);

	guestos_t *os = guestos_mgr_get_latest_by_name(name, false);
	IF_NULL_RETURN_WARN(os);
	if (!guestos_images_are_complete(os, false))
		guestos_images_download(os, download_complete_cb, NULL);
}

void
guestos_mgr_update_images(void)
{
	// TODO: iterate containers and then download latest os?
	size_t n = guestos_mgr_get_guestos_count();
	for (size_t i = 0; i < n; i++) {
		guestos_t *os = guestos_mgr_get_guestos_by_index(i);
		const char *os_name = guestos_get_name(os);
		if (guestos_mgr_is_guestos_used_by_containers(os_name)) {
			guestos_mgr_download_latest(os_name);
		}
	}
}

static char *
write_to_tmpfile_new(unsigned char *buf, size_t buflen)
{
	char *file = mem_strdup("/tmp/tmpXXXXXXXX");
	int fd = mkstemp(file);
	if (fd != -1) {
		int len = fd_write(fd, (char *)buf, buflen);
		close(fd);
		if (len >= 0 && (size_t)len == buflen)
			return file;
		ERROR("Failed to write entire data (%zu bytes) to temp file %s", buflen, file);
	} else {
		ERROR("Failed to create temp file.");
	}
	mem_free(file);
	return NULL;
}

static void
push_config_verify_cb(smartcard_crypto_verify_result_t verify_result, const char *cfg_file,
		      const char *sig_file, const char *cert_file,
		      UNUSED smartcard_crypto_hashalgo_t hash_algo, UNUSED void *data)
{
	INFO("Push GuestOS config (Phase 2)");

	switch (verify_result) {
	case VERIFY_GOOD:
		INFO("Signature of GuestOS OK (GOOD)");
		break;
	case VERIFY_LOCALLY_SIGNED:
		if (guestos_mgr_allow_locally_signed) {
			INFO("Signature of GuestOS OK (locally signed)");
			break;
		}
		// fallthrough
	default:
		ERROR("Signature verification failed (%d) for pushed GuestOS config %s, skipping.",
		      verify_result, cfg_file);
		goto cleanup_tmpfiles;
	}

	guestos_t *os = guestos_new_from_file(cfg_file, guestos_basepath);
	if (!os) {
		ERROR("Could not instantiate GuestOS from temp file %s", cfg_file);
		goto cleanup_tmpfiles;
	}

	const char *os_name = guestos_get_name(os);
	uint64_t os_ver = guestos_get_version(os);
	const guestos_t *old_os = guestos_mgr_get_latest_by_name(os_name, false);
	if (old_os) {
		// existing os of same name => verify and update
		uint64_t old_ver = guestos_get_version(old_os);
		if (os_ver <= old_ver) {
			WARN("Skipping update of GuestOS %s version %" PRIu64
			     " to older/same version %" PRIu64 ".",
			     os_name, old_ver, os_ver);
			goto cleanup_os;
		}
		DEBUG("Updating GuestOS config for %s from v%" PRIu64 " to v%" PRIu64 ".", os_name,
		      old_ver, os_ver);
	} else {
		// Fresh install
		DEBUG("Installing GuestOS config for %s v%" PRIu64 ".", os_name, os_ver);
	}

	// 1. create new guestos folder
	const char *dir = guestos_get_dir(os);
	if (mkdir(dir, 00755) < 0 && errno != EEXIST) {
		ERROR_ERRNO("Could not mkdir GuestOS directory %s", dir);
		goto cleanup_purge;
	}

	// 2. save pushed config, signature and cert
	if (file_move(cfg_file, guestos_get_cfg_file(os), GUESTOS_MGR_FILE_MOVE_BLOCKSIZE) < 0) {
		ERROR_ERRNO("Failed to move GuestOS config %s to %s", cfg_file,
			    guestos_get_cfg_file(os));
		goto cleanup_purge;
	}
	if (file_move(sig_file, guestos_get_sig_file(os), GUESTOS_MGR_FILE_MOVE_BLOCKSIZE) < 0) {
		ERROR_ERRNO("Failed to move GuestOS config signature %s to %s", sig_file,
			    guestos_get_sig_file(os));
		goto cleanup_purge;
	}
	if (file_move(cert_file, guestos_get_cert_file(os), GUESTOS_MGR_FILE_MOVE_BLOCKSIZE) < 0) {
		ERROR_ERRNO("Failed to move GuestOS config certificate %s to %s", cert_file,
			    guestos_get_cert_file(os));
		goto cleanup_purge;
	}

	// 3. register new os instance
	guestos_list = list_append(guestos_list, os);

	INFO("%s: %s", GUESTOS_MGR_UPDATE_TITLE,
	     cmld_is_wifi_active() ? GUESTOS_MGR_UPDATE_DOWNLOAD :
				     GUESTOS_MGR_UPDATE_DOWNLOAD_NO_WIFI);

	// 4. trigger image download if os is used by a container or a fresh install
	if (guestos_mgr_is_guestos_used_by_containers(os_name) || !old_os) {
		INFO("%s: %s", GUESTOS_MGR_UPDATE_TITLE, GUESTOS_MGR_UPDATE_DOWNLOAD);
		guestos_mgr_download_latest(os_name);
	}
	return;

cleanup_purge:
	guestos_purge(os);
cleanup_os:
	guestos_free(os);
cleanup_tmpfiles:
	unlink(cfg_file);
	unlink(sig_file);
	unlink(cert_file);
}

int
guestos_mgr_push_config(unsigned char *cfg, size_t cfglen, unsigned char *sig, size_t siglen,
			unsigned char *cert, size_t certlen)
{
	INFO("Push GuestOS config (Phase 1)");

	char *tmp_cfg_file = write_to_tmpfile_new(cfg, cfglen);
	char *tmp_sig_file = write_to_tmpfile_new(sig, siglen);
	char *tmp_cert_file = write_to_tmpfile_new(cert, certlen);
	int res = -1;
	if (tmp_cfg_file && tmp_sig_file && tmp_cert_file) {
		res = smartcard_crypto_verify_file(tmp_cfg_file, tmp_sig_file, tmp_cert_file,
						   GUESTOS_MGR_VERIFY_HASH_ALGO,
						   push_config_verify_cb, NULL);
	}
	if (res < 0) {
		unlink(tmp_cfg_file);
		unlink(tmp_sig_file);
		unlink(tmp_cert_file);
	}
	mem_free(tmp_cfg_file);
	mem_free(tmp_sig_file);
	mem_free(tmp_cert_file);
	return res;
}

int
guestos_mgr_register_localca(unsigned char *cacert, size_t cacertlen)
{
	int ret = -1;
	IF_TRUE_RETVAL(file_exists(LOCALCA_ROOT_CERT), ret);

	char *tmp_cacert_file = write_to_tmpfile_new(cacert, cacertlen);
	IF_NULL_RETVAL(tmp_cacert_file, ret);

	if ((ret = file_move(tmp_cacert_file, LOCALCA_ROOT_CERT, GUESTOS_MGR_FILE_MOVE_BLOCKSIZE)) <
	    0) {
		ERROR_ERRNO("Failed to move localca root certificate %s to %s", tmp_cacert_file,
			    LOCALCA_ROOT_CERT);
	} else {
		INFO("Successfully installed localca root certificate %s to %s", tmp_cacert_file,
		     LOCALCA_ROOT_CERT);
	}
	mem_free(tmp_cacert_file);
	return ret;
}

int
guestos_mgr_register_newca(unsigned char *cacert, size_t cacertlen)
{
	const char *begin_cert_str = "-----BEGIN CERTIFICATE-----";
	const char *end_cert_str = "-----END CERTIFICATE-----";

	int ret = -1;
	if (!file_is_dir(TRUSTED_CA_STORE))
		IF_TRUE_RETVAL(dir_mkdir_p(TRUSTED_CA_STORE, 0600), ret);

	// Sanity check file is a certificate
	size_t end_offset = cacertlen - strlen(end_cert_str) - 1;
	if (strncmp((char *)cacert, begin_cert_str, strlen(begin_cert_str)) != 0 ||
	    strncmp((char *)cacert + end_offset, end_cert_str, strlen(end_cert_str)) != 0) {
		ERROR("Sanity check failed. provided data is not an encoded certificate");
		return ret;
	}

	char *tmp_cacert_file = write_to_tmpfile_new(cacert, cacertlen);
	IF_NULL_RETVAL(tmp_cacert_file, ret);

	char *cacert_hash = smartcard_crypto_hash_file_block_new(tmp_cacert_file, SHA1);
	char *cacert_file = mem_printf("%s/%s", TRUSTED_CA_STORE, cacert_hash);
	if (file_exists(cacert_file)) {
		INFO("Certificate with hash %s already installed!", cacert_hash);
		if (unlink(tmp_cacert_file))
			WARN_ERRNO("Failed to delete tmpfile %s failed!", tmp_cacert_file);
		ret = 0;
		goto out;
	}

	if ((ret = file_move(tmp_cacert_file, cacert_file, GUESTOS_MGR_FILE_MOVE_BLOCKSIZE)) < 0) {
		ERROR_ERRNO("Failed to move new ca certificate %s to %s", tmp_cacert_file,
			    cacert_file);
	} else {
		INFO("Successfully installed new ca certificate %s to %s", tmp_cacert_file,
		     cacert_file);
	}
out:
	mem_free(cacert_file);
	mem_free(cacert_hash);
	mem_free(tmp_cacert_file);
	return ret;
}

/******************************************************************************/

guestos_t *
guestos_mgr_get_latest_by_name(const char *name, bool complete)
{
	IF_NULL_RETVAL(name, NULL);

	uint64_t latest_version = 0;
	guestos_t *latest_os = NULL;
	for (list_t *l = guestos_list; l; l = l->next) {
		if (!strcmp(name, guestos_get_name(l->data))) {
			guestos_t *os = l->data;
			uint64_t version = guestos_get_version(os);
			// TODO cache image complete result in guestos instance and get rid of check here?
			if (complete && !guestos_images_are_complete(os, false)) {
				DEBUG("GuestOS %s v%" PRIu64
				      " is incomplete (missing images) or broken, skipping.",
				      guestos_get_name(os), version);
				continue;
			}
			if (version > latest_version) {
				latest_version = version;
				latest_os = os;
			}
		}
	}
	return latest_os;
}

size_t
guestos_mgr_get_guestos_count(void)
{
	return list_length(guestos_list);
}

guestos_t *
guestos_mgr_get_guestos_by_index(size_t index)
{
	return list_nth_data(guestos_list, index);
}

#if 0
const guestos_mount_t *
guestos_get_mount(const guestos_t *os, size_t nth)
{
	ASSERT(os);
	if (nth >= os->mntc)
		return NULL;
	else
		return &os->mntv[nth];
}

size_t
guestos_get_mount_count(const guestos_t *os)
{
	ASSERT(os);
	return os->mntc;
}

const char *
guestos_mount_get_img(const guestos_mount_t *mnt)
{
	ASSERT(mnt);
	return mnt->img;
}

const char *
guestos_mount_get_dir(const guestos_mount_t *mnt)
{
	ASSERT(mnt);
	return mnt->dir;
}

const char *
guestos_mount_get_fs(const guestos_mount_t *mnt)
{
	ASSERT(mnt);
	return mnt->fs;
}

enum guestos_mount_type
guestos_mount_get_type(const guestos_mount_t *mnt)
{
	ASSERT(mnt);
	return mnt->type;
}
#endif
