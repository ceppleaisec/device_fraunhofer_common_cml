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

#include "scd.h"
#include "tpm2d_shared.h"

#include "control.h"
#ifdef ANDROID
#include <cutils/properties.h>
#include "device/fraunhofer/common/cml/scd/device.pb-c.h"
#else
#include "device.pb-c.h"
#endif

#define LOGF_LOG_MIN_PRIO LOGF_PRIO_TRACE
#include "common/macro.h"
#include "common/mem.h"
#include "common/event.h"
#include "common/logf.h"
#include "common/sock.h"
#include "common/dir.h"
#include "common/file.h"
#include "common/uuid.h"
#include "common/protobuf.h"
#include "common/reboot.h"
#include "common/list.h"
#include "ssl_util.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// clang-format off
#define SCD_CONTROL_SOCKET SOCK_PATH(scd-control)
// clang-format on

// Do not edit! This path is also configured in cmld.c
#define DEVICE_CONF "/data/cml/device.conf"

#ifdef ANDROID
#define PROP_SERIALNO "ro.boot.serialno"
#define PROP_HARDWARE "ro.hardware"
#endif

#define TOKEN_DEFAULT_PASS "trustme"
#define TOKEN_DEFAULT_NAME "testuser"
#define TOKEN_DEFAULT_EXT ".p12"

#ifdef DEBUG_BUILD
#define SYSTEM_DEFAULT_REBOOT HALT
#else
#define SYSTEM_DEFAULT_REBOOT REBOOT
#endif

static list_t *scd_token_list = NULL;

static scd_control_t *scd_control_cmld = NULL;
static logf_handler_t *scd_logfile_handler = NULL;


/**
 * returns 1 if a given file is a p12 token, otherwise 0
 */
static int
is_softtoken(const char *path, const char *file, UNUSED void *data)
{
	char *location = mem_printf("%s/%s", path, file);
	char *ext;

	// regular file
	if (!file_is_regular(location))
		return 0;
	// with fixed extesion
	if (!(ext = file_get_extension(file)))
		return 0;
	if (!strncmp(ext, TOKEN_DEFAULT_EXT, strlen(TOKEN_DEFAULT_EXT))) {
		DEBUG("Found token file: %s", location);
		mem_free(location);
		return 1;
	}

	mem_free(location);
	return 0;
}

/**
 * returns >0 if one or more token files exist
 */
static int
token_file_exists()
{
	int ret = dir_foreach(SCD_TOKEN_DIR, &is_softtoken, NULL);

	if (ret < 0)
		FATAL("Could not open token directory");
	else {
		DEBUG("%d Token files exist", ret);
		return ret;
	}
}

bool
scd_in_provisioning_mode(void)
{
	return file_is_regular(PROVISIONING_MODE_FILE);
}

static void
provisioning_mode()
{
	INFO("Check for existence of device certificate and user token");

	bool need_initialization = (!file_exists(DEVICE_CERT_FILE) || !token_file_exists());
	bool use_tpm = false;
	char *dev_key_file = DEVICE_KEY_FILE;

	// device needs a device certificate
	if (!file_exists(DEVICE_CONF)) {
		ERROR("Going back to bootloader mode, device config does not exist (Proper"
		      "userdata image needs to be flashed first)");
		reboot_reboot(SYSTEM_DEFAULT_REBOOT);
	}

	// if available, use tpm to create and store device key
	if (file_exists("/dev/tpm0")) {
		use_tpm = true;
		// assumption: tpm2d is launched prior to scd, and creates a keypair on first boot
		if (!file_exists(TPM2D_ATT_TSS_FILE)) {
			ERROR("TPM keypair not found, missing %s", TPM2D_ATT_TSS_FILE);
			reboot_reboot(SYSTEM_DEFAULT_REBOOT);
		}
		dev_key_file = TPM2D_ATT_TSS_FILE;
	}

	if (need_initialization) {
		sleep(5);
	}

	// if no certificate exists, create a csr
	if (!file_exists(DEVICE_CERT_FILE)) {
		INFO("Device certificate not available. Switch to device provisioning mode");
		file_printf(PROVISIONING_MODE_FILE, "provisioning mode");

		if (ssl_init(use_tpm) == -1) {
			FATAL("Failed to initialize OpenSSL stack for device cert");
		}

		if (!file_exists(DEVICE_CSR_FILE) || (!use_tpm && !file_exists(DEVICE_KEY_FILE))) {
			DEBUG("Create CSR (recreate if corresponding private key misses)");

			if (file_exists(SCD_TOKEN_DIR) && file_is_dir(SCD_TOKEN_DIR)) {
				DEBUG("CSR folder already exists");
			} else if (mkdir(SCD_TOKEN_DIR, 00755) != 0) {
				FATAL("Failed to create CSR directory");
			}

#ifdef ANDROID
			char *hw_serial = mem_alloc0(PROPERTY_VALUE_MAX);
			char *hw_name = mem_alloc0(PROPERTY_VALUE_MAX);
			bool property_read_failure = false;
			if (!(property_get(PROP_SERIALNO, hw_serial, NULL) > 0)) {
				ERROR("Failed to read hardware serialno property");
				property_read_failure = true;
			}
			if (!(property_get(PROP_HARDWARE, hw_name, NULL) > 0)) {
				ERROR("Failed to read hardware name property");
				property_read_failure = true;
			}
			char *common_name;
			if (!property_read_failure)
				common_name = mem_printf("%s %s", hw_name, hw_serial);
			else
				common_name = mem_printf("%s %s", "x86", "0000");
			DEBUG("Using common name %s", common_name);
#else
			char *common_name = mem_strdup("common_name");
			char *hw_serial = mem_strdup("hw_serial");
			char *hw_name = mem_strdup("hw_unknown");
#endif

			// create device uuid and write to device.conf
			uuid_t *dev_uuid = uuid_new(NULL);
			const char *uid;
			if (!dev_uuid || (uid = uuid_string(dev_uuid)) == NULL) {
				FATAL("Could not create device uuid");
			}

			DeviceConfig *dev_cfg = (DeviceConfig *)protobuf_message_new_from_textfile(
				DEVICE_CONF, &device_config__descriptor);

			if (!dev_cfg) {
				FATAL("Failed load device config from file \"%s\"!", DEVICE_CONF);
			}

			// set device uuid
			char *proto_uid = mem_strdup(uid);
			// free this element, as it is overwritten
			mem_free(dev_cfg->uuid);
			dev_cfg->uuid = proto_uid;

			// write the uuid to device config file
			if (protobuf_message_write_to_file(DEVICE_CONF,
							   (ProtobufCMessage *)dev_cfg) < 0) {
				FATAL("Could not write device config to \"%s\"!", DEVICE_CONF);
			}

			if (ssl_create_csr(DEVICE_CSR_FILE, dev_key_file, NULL, common_name, uid,
					   use_tpm) != 0) {
				FATAL("Unable to create CSR");
			}

			// this call also frees proto_uid
			protobuf_free_message((ProtobufCMessage *)dev_cfg);
			mem_free(hw_serial);
			mem_free(hw_name);
			mem_free(common_name);
			uuid_free(dev_uuid);
			DEBUG("CSR with privkey created and stored");
		} else {
			DEBUG("CSR with privkey already exists");
		}

		// self-sign device csr to bring the device up
		// corresponding cert is overwritten during provisioning
		DEBUG("Create self-signed certificate from CSR");

		if (ssl_self_sign_csr(DEVICE_CSR_FILE, DEVICE_CERT_FILE, dev_key_file, use_tpm) !=
		    0) {
			FATAL("Unable to self sign existing device.csr");
		}

		ssl_free();
	} else {
		INFO("Device certificate found");
		if (file_exists(DEVICE_CSR_FILE)) {
			// this is the case when a non-provisioned trustme phone
			// created its own device.cert and user.p12
			WARN("Device CSR still exists. Device was not correctly provisioned!");
		}
	}

	// self-create a user token to bring the device up
	// is removed during provisioning
	if (!token_file_exists()) {
		DEBUG("Create initial soft token");
		// TPM not used for soft token
		if (ssl_init(false) == -1) {
			FATAL("Failed to initialize OpenSSL stack for softtoken");
		}

		char *token_file =
			mem_printf("%s/%s%s", SCD_TOKEN_DIR, TOKEN_DEFAULT_NAME, TOKEN_DEFAULT_EXT);
		if (ssl_create_pkcs12_token(token_file, NULL, TOKEN_DEFAULT_PASS,
					    TOKEN_DEFAULT_NAME) != 0) {
			FATAL("Unable to create initial user token");
		}
		mem_free(token_file);
		ssl_free();
	}

	// we now have anything for a clean startup so just die and let us be restarted by init
	if (need_initialization) {
		exit(0);
	}

	// remark: no certificate validation checks are carried out
	if ((!use_tpm && !file_exists(DEVICE_KEY_FILE)) || !file_exists(SSIG_ROOT_CERT) ||
	    !token_file_exists()) {
		FATAL("Missing certificate chains, user token, or private key for device certificate");
	}
}

static void
scd_logfile_rename_cb(UNUSED event_timer_t *timer, UNUSED void *data)
{
	INFO("Logfile must be closed and a new file opened");
	logf_unregister(scd_logfile_handler);
	scd_logfile_handler = logf_register(&logf_file_write, logf_file_new("/data/logs/cml-scd"));
	logf_handler_set_prio(scd_logfile_handler, LOGF_PRIO_WARN);
}

int
main(int argc, char **argv)
{
	ASSERT(argc >= 1);

	if (file_exists("/dev/log/main"))
		logf_register(&logf_android_write, logf_android_new(argv[0]));
	else
		logf_register(&logf_klog_write, logf_klog_new(argv[0]));
	logf_register(&logf_file_write, stdout);

	scd_logfile_handler = logf_register(&logf_file_write, logf_file_new("/data/logs/cml-scd"));
	logf_handler_set_prio(scd_logfile_handler, LOGF_PRIO_WARN);

	event_timer_t *logfile_timer = event_timer_new(
		HOURS_TO_MILLISECONDS(24), EVENT_TIMER_REPEAT_FOREVER, scd_logfile_rename_cb, NULL);
	event_add_timer(logfile_timer);

	provisioning_mode();

	INFO("Starting scd ...");

	// for now, the scd is using the tpm engine only for provisioning
	if (ssl_init(false) == -1) {
		FATAL("Failed to initialize OpenSSL stack for scd runtime");
	}

	DEBUG("Try to create directory for socket if not existing");
	if (dir_mkdir_p(CMLD_SOCKET_DIR, 0755) < 0) {
		FATAL("Could not create directory for scd_control socket");
	}

	scd_control_cmld = scd_control_new(SCD_CONTROL_SOCKET);
	if (!scd_control_cmld) {
		FATAL("Could not init scd_control socket");
	}

	INFO("created control socket.");

#ifdef ANDROID
	/* trigger start of cmld */
	if (property_set("trustme.provisioning.mode", "no") != 0) {
		FATAL("Unable to set property. Cannot trigger CMLD");
	}
#endif

	event_loop();
	ssl_free();

	return 0;
}

const char *
scd_get_softtoken_dir(void)
{
	return SCD_TOKEN_DIR;
}

softtoken_t *
scd_load_softtoken(const char *path, const char *name)
{
	ASSERT(path);
	ASSERT(name);

	softtoken_t *ntoken;

	TRACE("scd_load_softtoken path: %s", path);
	TRACE("scd_load_softtoken name: %s", name);

	if (strlen(name) >= strlen(TOKEN_DEFAULT_EXT) &&
	    !strcmp(name + strlen(name) - strlen(TOKEN_DEFAULT_EXT), TOKEN_DEFAULT_EXT)) {
		char *token_file = mem_printf("%s/%s", path, name);
		TRACE("Softtoken filename: %s", token_file);

		ntoken = softtoken_new_from_p12(token_file);
		mem_free(token_file);
		return (ntoken);
	}

	ERROR("SCD: scd_load_softtoken failed");
	return NULL;
}

scd_tokentype_t
scd_proto_to_tokentype(const DaemonToToken *msg) {
	switch (msg->token_type) {
		case TOKEN_TYPE__NONE:
			return NONE;
		case TOKEN_TYPE__DEVICE:
			return DEVICE;
		case TOKEN_TYPE__USB:
			return USB;
		default: {
			ERROR("Invalid token type value");
			return -1;
		}
	}
	return -1; // never reached
}

static scd_token_t *
token_list_search(scd_tokentype_t type, const char *name) {

	ASSERT(name);

	for (list_t *l = scd_token_list; l; l = l->next) {
		scd_token_t *t = l->data;

		if (type == scd_token_get_type(t)) {
			if (strncmp(name, uuid_string(scd_token_get_uuid(t)), strlen(name)) == 0) {
				return t;
			}
		}
	}
	return NULL;
}

/**
 * Get a generic scd token.
 */
scd_token_t *
scd_get_token (const DaemonToToken *msg)
{
	ASSERT(msg);

	TRACE("SCD: scd_get_token. proto_tokentype: %d", msg->token_type);

	scd_token_t *token;
	scd_tokentype_t type;

	type = scd_proto_to_tokentype(msg);
	token = token_list_search(type, msg->token_uuid);

	if (!token) {
		token = scd_token_new(type, msg->token_uuid, SCD_TOKEN_DIR);
		if(!token) {
			ERROR("Could not create new scd_token");
			return NULL;
		}
		scd_token_list = list_append(scd_token_list, token);
	}

	return token;
}

/**
 * free a scd token and remove it from the global list of initialized tokens.
 */
void
scd_free_token(scd_token_t *token) {
	IF_NULL_RETURN(token);

	scd_token_list = list_remove(scd_token_list, token);

	scd_token_free(token);

	token = NULL;
}
