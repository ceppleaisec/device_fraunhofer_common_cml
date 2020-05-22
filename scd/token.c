/*
 * This file is part of trust|me
 * Copyright(c) 2013 - 2020 Fraunhofer AISEC
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

#include "token.h"

#include "common/macro.h"
#include "common/mem.h"
#include "file.h"

struct scd_token_data {
    union{
        softtoken_t *softtoken;
        usbtoken_t *usbtoken;
    } int_token;

    scd_tokentype_t type;
    uuid_t *token_uuid;
};



/*****************************************************************************/
/******************* internal helper functions *******************************/
/*****************************************************************************/

int
int_lock_st(scd_token_t *token) {
    return softtoken_lock(token->token_data->int_token.softtoken);
}

int
int_unlock_st(scd_token_t *token, char *passwd,
				UNUSED unsigned char *pairing_secret,
				UNUSED size_t pairing_sec_len) {
    return softtoken_unlock(token->token_data->int_token.softtoken, passwd);
}

bool
int_is_locked_st(scd_token_t *token) {
    return softtoken_is_locked(token->token_data->int_token.softtoken);
}

bool
int_is_locked_till_reboot_st(scd_token_t *token) {
    return softtoken_is_locked_till_reboot(token->token_data->int_token.softtoken);
}

int
int_wrap_st(scd_token_t *token,
			UNUSED char *label,
			unsigned char *plain_key, size_t plain_key_len,
			unsigned char **wrapped_key, int *wrapped_key_len)
{

    return softtoken_wrap_key(token->token_data->int_token.softtoken, plain_key, plain_key_len,
                              wrapped_key, wrapped_key_len);
}

int
int_unwrap_st(scd_token_t *token,
				UNUSED char *label,
                unsigned char *wrapped_key, size_t wrapped_key_len,
		        unsigned char **plain_key, int *plain_key_len)
{
    return softtoken_unwrap_key(token->token_data->int_token.softtoken, wrapped_key,
                                wrapped_key_len, plain_key, plain_key_len);
}

int
int_change_pw_st(scd_token_t *token, const char *oldpass, const char *newpass)
{
	return softtoken_change_passphrase(token->token_data->int_token.softtoken, oldpass,
										newpass);
}

/*  -----------------------------------------------------------------------  */
int
int_lock_usb(scd_token_t *token) {
    return usbtoken_lock(token->token_data->int_token.usbtoken);
}

int
int_unlock_usb(scd_token_t *token, char *passwd,
				unsigned char *pairing_secret, size_t pairing_sec_len) {
    TRACE("SCD: int_usb_unlock");
    return usbtoken_unlock(token->token_data->int_token.usbtoken, passwd,
							pairing_secret, pairing_sec_len);
}

bool
int_is_locked_usb(scd_token_t *token) {
    return usbtoken_is_locked(token->token_data->int_token.usbtoken);
}

bool
int_is_locked_till_reboot_usb(scd_token_t *token) {
    return usbtoken_is_locked_till_reboot(token->token_data->int_token.usbtoken);
}

int
int_wrap_usb(scd_token_t *token, char *label,
			unsigned char *plain_key, size_t plain_key_len,
			unsigned char **wrapped_key, int *wrapped_key_len)
{
    return usbtoken_wrap_key(token->token_data->int_token.usbtoken,
							(unsigned char *) label, strlen(label),
                            plain_key, plain_key_len,
                            wrapped_key, wrapped_key_len);
}

int
int_unwrap_usb(scd_token_t *token, char *label,
                unsigned char *wrapped_key, size_t wrapped_key_len,
		        unsigned char **plain_key, int *plain_key_len)
{
    return usbtoken_unwrap_key(token->token_data->int_token.usbtoken,
								(unsigned char *) label, strlen(label),
                                wrapped_key, wrapped_key_len,
                                plain_key, plain_key_len);
}

int
int_change_pw_usb(scd_token_t *token, const char *oldpass, const char *newpass)
{
	return usbtoken_change_passphrase(token->token_data->int_token.usbtoken, oldpass,
										newpass);
}

/*  -----------------------------------------------------------------------  */

/**
 * creates a new generic token
 * calls the respective create function for the selected type of token and
 * sets the function pointer appropriately
 */
scd_token_t *
scd_token_new(scd_tokentype_t type, const char *name, const char *st_path) {

    scd_token_t *new_token;

	TRACE("SCD: scd_token_new");

	new_token = mem_new0(scd_token_t, 1);
    if (!new_token) {
        ERROR("Could not allocate new scd_token_t");
        return NULL;
    }

    new_token->token_data = mem_new(scd_token_data_t, 1);
    if (!new_token->token_data) {
        ERROR("Could not allocate memory for token_data_t");
        /* TODO: cleanup */
        return NULL;
    }

    new_token->token_data->token_uuid = uuid_new(name);
    if (!new_token->token_data->token_uuid) {
        ERROR("Could not allocate memory for token_uuid");
        /* TODO: cleanup */
        return NULL;
    }

    switch (type) {
        case (NONE): {
            WARN("Create scd_token with internal type 'NONE' selected");
            new_token->token_data->type       = NONE;
            break;
        }
        case (DEVICE): {
            DEBUG("Create scd_token with internal type 'DEVICE'");

            ASSERT(name);
            ASSERT(st_path);

            /* TODO:
             * if this method is called, the softtoken that the scd was referring
             * has not been intialized before. However, its associate p12 structure
             * might have been. We must check that.
             */
            char *token_file =
			    mem_printf("%s/%s%s", st_path, name, STOKEN_DEFAULT_EXT);
            if (!file_exists(token_file)) {
                if (softtoken_create_p12(token_file, STOKEN_DEFAULT_PASS, name) != 0) {
                    ERROR("could not create new softtoken file");
                    /* TODO: cleanup */
                }
            }
			new_token->token_data->int_token.softtoken =
                softtoken_new_from_p12(token_file);
            if (!new_token->token_data->int_token.softtoken) {
                ERROR("Creation of softtoken failed");
                mem_free(new_token);
                return NULL;
            }
            mem_free(token_file);

            new_token->token_data->type       = DEVICE;
            new_token->lock       = int_lock_st;
            new_token->unlock     = int_unlock_st;
            new_token->is_locked  = int_is_locked_st;
            new_token->is_locked_till_reboot = int_is_locked_till_reboot_st;
            new_token->wrap_key   = int_wrap_st;
            new_token->unwrap_key  = int_unwrap_st;
			new_token->change_passphrase = int_change_pw_st;
           break;
        }
        case (USB): {
            DEBUG("Create scd_token with internal type 'USB'");
            new_token->token_data->int_token.usbtoken = usbtoken_init();
            ASSERT(new_token->token_data->int_token.usbtoken);
            if (NULL == new_token->token_data->int_token.usbtoken) {
                ERROR("Creation of usbtoken failed");
                mem_free(new_token);
                return NULL;
            }
            new_token->token_data->type       = USB;
            new_token->lock       = int_lock_usb;
            new_token->unlock     = int_unlock_usb;
            new_token->is_locked  = int_is_locked_usb;
            new_token->is_locked_till_reboot = int_is_locked_till_reboot_usb;
            new_token->wrap_key   = int_wrap_usb;
            new_token->unwrap_key   = int_unwrap_usb;
			new_token->change_passphrase = int_change_pw_usb;
           break;
        }
        default: {
            ERROR("Unrecognized token type");
            mem_free(new_token);
            return NULL;
        }
    }


    return new_token;
}

scd_tokentype_t
scd_token_get_type(scd_token_t *token) {
    return token->token_data->type;
}

uuid_t *
scd_token_get_uuid(scd_token_t *token) {
    return token->token_data->token_uuid;
}

static void
token_data_free(scd_token_data_t *token_data) {

    switch (token_data->type) {
        case (NONE): break;
        case (DEVICE):
            softtoken_free(token_data->int_token.softtoken);
            break;
        case (USB):
            usbtoken_free(token_data->int_token.usbtoken);
            break;
        default:
            ERROR("Failed to determine token type. Cannot clean up");
            return;
    }

    if (token_data->token_uuid)
        uuid_free(token_data->token_uuid);
    mem_free(token_data);
}

void
scd_token_free(scd_token_t *token) {

    IF_NULL_RETURN(token);

    if (token->token_data)
        token_data_free(token->token_data);

    mem_free(token);
}
