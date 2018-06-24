/*
 * Wi-Fi Protected Setup - Registrar
 * Copyright (c) 2008-2009, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include "utils/includes.h"

#include "utils/common.h"
#include "utils/base64.h"
#include "utils/eloop.h"
#include "utils/uuid.h"
#include "utils/list.h"
#include "crypto/crypto.h"
#include "crypto/sha256.h"
#include "common/ieee802_11_defs.h"
#include "wps_i.h"
#include "wps_dev_attr.h"
#include "wps_upnp.h"
#include "wps_upnp_i.h"

#include "pixie.h"

#define WPS_WORKAROUNDS

struct wps_uuid_pin {
	struct dl_list list;
	u8 uuid[WPS_UUID_LEN];
	int wildcard_uuid;
	u8 *pin;
	size_t pin_len;
#define PIN_LOCKED BIT(0)
#define PIN_EXPIRES BIT(1)
	int flags;
	struct os_time expiration;
};


static void wps_free_pin(struct wps_uuid_pin *pin)
{
	os_free(pin->pin);
	os_free(pin);
}


static void wps_remove_pin(struct wps_uuid_pin *pin)
{
	dl_list_del(&pin->list);
	wps_free_pin(pin);
}


static void wps_free_pins(struct dl_list *pins)
{
	struct wps_uuid_pin *pin, *prev;
	dl_list_for_each_safe(pin, prev, pins, struct wps_uuid_pin, list)
		wps_remove_pin(pin);
}


struct wps_pbc_session {
	struct wps_pbc_session *next;
	u8 addr[ETH_ALEN];
	u8 uuid_e[WPS_UUID_LEN];
	struct os_time timestamp;
};


static void wps_free_pbc_sessions(struct wps_pbc_session *pbc)
{
	struct wps_pbc_session *prev;

	while (pbc) {
		prev = pbc;
		pbc = pbc->next;
		os_free(prev);
	}
}


struct wps_registrar_device {
	struct wps_registrar_device *next;
	struct wps_device_data dev;
	u8 uuid[WPS_UUID_LEN];
};


struct wps_registrar {
	struct wps_context *wps;

	int pbc;
	int selected_registrar;

	int (*new_psk_cb)(void *ctx, const u8 *mac_addr, const u8 *psk,
			  size_t psk_len);
	int (*set_ie_cb)(void *ctx, struct wpabuf *beacon_ie,
			 struct wpabuf *probe_resp_ie);
	void (*pin_needed_cb)(void *ctx, const u8 *uuid_e,
			      const struct wps_device_data *dev);
	void (*reg_success_cb)(void *ctx, const u8 *mac_addr,
			       const u8 *uuid_e);
	void (*set_sel_reg_cb)(void *ctx, int sel_reg, u16 dev_passwd_id,
			       u16 sel_reg_config_methods);
	void (*enrollee_seen_cb)(void *ctx, const u8 *addr, const u8 *uuid_e,
				 const u8 *pri_dev_type, u16 config_methods,
				 u16 dev_password_id, u8 request_type,
				 const char *dev_name);
	void *cb_ctx;

	struct dl_list pins;
	struct wps_pbc_session *pbc_sessions;

	int skip_cred_build;
	struct wpabuf *extra_cred;
	int disable_auto_conf;
	int sel_reg_union;
	int sel_reg_dev_password_id_override;
	int sel_reg_config_methods_override;
	int static_wep_only;

	struct wps_registrar_device *devices;

	int force_pbc_overlap;
};


static int wps_set_ie(struct wps_registrar *reg);
static void wps_registrar_pbc_timeout(void *eloop_ctx, void *timeout_ctx);
static void wps_registrar_set_selected_timeout(void *eloop_ctx,
					       void *timeout_ctx);


static void wps_free_devices(struct wps_registrar_device *dev)
{
	struct wps_registrar_device *prev;

	while (dev) {
		prev = dev;
		dev = dev->next;
		wps_device_data_free(&prev->dev);
		os_free(prev);
	}
}


static struct wps_registrar_device * wps_device_get(struct wps_registrar *reg,
						    const u8 *addr)
{
	struct wps_registrar_device *dev;

	for (dev = reg->devices; dev; dev = dev->next) {
		if (os_memcmp(dev->dev.mac_addr, addr, ETH_ALEN) == 0)
			return dev;
	}
	return NULL;
}


static void wps_device_clone_data(struct wps_device_data *dst,
				  struct wps_device_data *src)
{
	os_memcpy(dst->mac_addr, src->mac_addr, ETH_ALEN);
	os_memcpy(dst->pri_dev_type, src->pri_dev_type, WPS_DEV_TYPE_LEN);

#define WPS_STRDUP(n) \
	os_free(dst->n); \
	dst->n = src->n ? os_strdup(src->n) : NULL

	WPS_STRDUP(device_name);
	WPS_STRDUP(manufacturer);
	WPS_STRDUP(model_name);
	WPS_STRDUP(model_number);
	WPS_STRDUP(serial_number);
#undef WPS_STRDUP
}


int wps_device_store(struct wps_registrar *reg,
		     struct wps_device_data *dev, const u8 *uuid)
{
	struct wps_registrar_device *d;

	d = wps_device_get(reg, dev->mac_addr);
	if (d == NULL) {
		d = os_zalloc(sizeof(*d));
		if (d == NULL)
			return -1;
		d->next = reg->devices;
		reg->devices = d;
	}

	wps_device_clone_data(&d->dev, dev);
	os_memcpy(d->uuid, uuid, WPS_UUID_LEN);

	return 0;
}


static void wps_registrar_add_pbc_session(struct wps_registrar *reg,
					  const u8 *addr, const u8 *uuid_e)
{
	struct wps_pbc_session *pbc, *prev = NULL;
	struct os_time now;

	os_get_time(&now);

	pbc = reg->pbc_sessions;
	while (pbc) {
		if (os_memcmp(pbc->addr, addr, ETH_ALEN) == 0 &&
		    os_memcmp(pbc->uuid_e, uuid_e, WPS_UUID_LEN) == 0) {
			if (prev)
				prev->next = pbc->next;
			else
				reg->pbc_sessions = pbc->next;
			break;
		}
		prev = pbc;
		pbc = pbc->next;
	}

	if (!pbc) {
		pbc = os_zalloc(sizeof(*pbc));
		if (pbc == NULL)
			return;
		os_memcpy(pbc->addr, addr, ETH_ALEN);
		if (uuid_e)
			os_memcpy(pbc->uuid_e, uuid_e, WPS_UUID_LEN);
	}

	pbc->next = reg->pbc_sessions;
	reg->pbc_sessions = pbc;
	pbc->timestamp = now;

	/* remove entries that have timed out */
	prev = pbc;
	pbc = pbc->next;

	while (pbc) {
		if (now.sec > pbc->timestamp.sec + WPS_PBC_WALK_TIME) {
			prev->next = NULL;
			wps_free_pbc_sessions(pbc);
			break;
		}
		prev = pbc;
		pbc = pbc->next;
	}
}


static void wps_registrar_remove_pbc_session(struct wps_registrar *reg,
					     const u8 *addr, const u8 *uuid_e)
{
	struct wps_pbc_session *pbc, *prev = NULL;

	pbc = reg->pbc_sessions;
	while (pbc) {
		if (os_memcmp(pbc->addr, addr, ETH_ALEN) == 0 &&
		    os_memcmp(pbc->uuid_e, uuid_e, WPS_UUID_LEN) == 0) {
			if (prev)
				prev->next = pbc->next;
			else
				reg->pbc_sessions = pbc->next;
			os_free(pbc);
			break;
		}
		prev = pbc;
		pbc = pbc->next;
	}
}


static int wps_registrar_pbc_overlap(struct wps_registrar *reg,
				     const u8 *addr, const u8 *uuid_e)
{
	int count = 0;
	struct wps_pbc_session *pbc;
	struct os_time now;

	os_get_time(&now);

	for (pbc = reg->pbc_sessions; pbc; pbc = pbc->next) {
		if (now.sec > pbc->timestamp.sec + WPS_PBC_WALK_TIME)
			break;
		if (addr == NULL || os_memcmp(addr, pbc->addr, ETH_ALEN) ||
		    uuid_e == NULL ||
		    os_memcmp(uuid_e, pbc->uuid_e, WPS_UUID_LEN))
			count++;
	}

	if (addr || uuid_e)
		count++;

	return count > 1 ? 1 : 0;
}


static int wps_build_wps_state(struct wps_context *wps, struct wpabuf *msg)
{
	wpa_printf(MSG_DEBUG, "WPS:  * Wi-Fi Protected Setup State (%d)",
		   wps->wps_state);
	wpabuf_put_be16(msg, ATTR_WPS_STATE);
	wpabuf_put_be16(msg, 1);
	wpabuf_put_u8(msg, wps->wps_state);
	return 0;
}


#ifdef CONFIG_WPS_UPNP
static void wps_registrar_free_pending_m2(struct wps_context *wps)
{
	struct upnp_pending_message *p, *p2, *prev = NULL;
	p = wps->upnp_msgs;
	while (p) {
		if (p->type == WPS_M2 || p->type == WPS_M2D) {
			if (prev == NULL)
				wps->upnp_msgs = p->next;
			else
				prev->next = p->next;
			wpa_printf(MSG_DEBUG, "WPS UPnP: Drop pending M2/M2D");
			p2 = p;
			p = p->next;
			wpabuf_free(p2->msg);
			os_free(p2);
			continue;
		}
		prev = p;
		p = p->next;
	}
}
#endif /* CONFIG_WPS_UPNP */


static int wps_build_ap_setup_locked(struct wps_context *wps,
				     struct wpabuf *msg)
{
	if (wps->ap_setup_locked) {
		wpa_printf(MSG_DEBUG, "WPS:  * AP Setup Locked");
		wpabuf_put_be16(msg, ATTR_AP_SETUP_LOCKED);
		wpabuf_put_be16(msg, 1);
		wpabuf_put_u8(msg, 1);
	}
	return 0;
}


static int wps_build_selected_registrar(struct wps_registrar *reg,
					struct wpabuf *msg)
{
	if (!reg->sel_reg_union)
		return 0;
	wpa_printf(MSG_DEBUG, "WPS:  * Selected Registrar");
	wpabuf_put_be16(msg, ATTR_SELECTED_REGISTRAR);
	wpabuf_put_be16(msg, 1);
	wpabuf_put_u8(msg, 1);
	return 0;
}


static int wps_build_sel_reg_dev_password_id(struct wps_registrar *reg,
					     struct wpabuf *msg)
{
	u16 id = reg->pbc ? DEV_PW_PUSHBUTTON : DEV_PW_DEFAULT;
	if (!reg->sel_reg_union)
		return 0;
	if (reg->sel_reg_dev_password_id_override >= 0)
		id = reg->sel_reg_dev_password_id_override;
	wpa_printf(MSG_DEBUG, "WPS:  * Device Password ID (%d)", id);
	wpabuf_put_be16(msg, ATTR_DEV_PASSWORD_ID);
	wpabuf_put_be16(msg, 2);
	wpabuf_put_be16(msg, id);
	return 0;
}


static int wps_build_sel_reg_config_methods(struct wps_registrar *reg,
					    struct wpabuf *msg)
{
	u16 methods;
	if (!reg->sel_reg_union)
		return 0;
	methods = reg->wps->config_methods & ~WPS_CONFIG_PUSHBUTTON;
	if (reg->pbc)
		methods |= WPS_CONFIG_PUSHBUTTON;
	if (reg->sel_reg_config_methods_override >= 0)
		methods = reg->sel_reg_config_methods_override;
	wpa_printf(MSG_DEBUG, "WPS:  * Selected Registrar Config Methods (%x)",
		   methods);
	wpabuf_put_be16(msg, ATTR_SELECTED_REGISTRAR_CONFIG_METHODS);
	wpabuf_put_be16(msg, 2);
	wpabuf_put_be16(msg, methods);
	return 0;
}


static int wps_build_probe_config_methods(struct wps_registrar *reg,
					  struct wpabuf *msg)
{
	u16 methods;
	/*
	 * These are the methods that the AP supports as an Enrollee for adding
	 * external Registrars.
	 */
	methods = reg->wps->config_methods & ~WPS_CONFIG_PUSHBUTTON;
	wpa_printf(MSG_DEBUG, "WPS:  * Config Methods (%x)", methods);
	wpabuf_put_be16(msg, ATTR_CONFIG_METHODS);
	wpabuf_put_be16(msg, 2);
	wpabuf_put_be16(msg, methods);
	return 0;
}


static int wps_build_config_methods_r(struct wps_registrar *reg,
				      struct wpabuf *msg)
{
	u16 methods;
	methods = reg->wps->config_methods & ~WPS_CONFIG_PUSHBUTTON;
	if (reg->pbc)
		methods |= WPS_CONFIG_PUSHBUTTON;
	return wps_build_config_methods(msg, methods);
}


/**
 * wps_registrar_init - Initialize WPS Registrar data
 * @wps: Pointer to longterm WPS context
 * @cfg: Registrar configuration
 * Returns: Pointer to allocated Registrar data or %NULL on failure
 *
 * This function is used to initialize WPS Registrar functionality. It can be
 * used for a single Registrar run (e.g., when run in a supplicant) or multiple
 * runs (e.g., when run as an internal Registrar in an AP). Caller is
 * responsible for freeing the returned data with wps_registrar_deinit() when
 * Registrar functionality is not needed anymore.
 */
struct wps_registrar *
wps_registrar_init(struct wps_context *wps,
		   const struct wps_registrar_config *cfg)
{
	struct wps_registrar *reg = os_zalloc(sizeof(*reg));
	if (reg == NULL)
		return NULL;

	dl_list_init(&reg->pins);
	reg->wps = wps;
	reg->new_psk_cb = cfg->new_psk_cb;
	reg->set_ie_cb = cfg->set_ie_cb;
	reg->pin_needed_cb = cfg->pin_needed_cb;
	reg->reg_success_cb = cfg->reg_success_cb;
	reg->set_sel_reg_cb = cfg->set_sel_reg_cb;
	reg->enrollee_seen_cb = cfg->enrollee_seen_cb;
	reg->cb_ctx = cfg->cb_ctx;
	reg->skip_cred_build = cfg->skip_cred_build;
	if (cfg->extra_cred) {
		reg->extra_cred = wpabuf_alloc_copy(cfg->extra_cred,
						    cfg->extra_cred_len);
		if (reg->extra_cred == NULL) {
			os_free(reg);
			return NULL;
		}
	}
	reg->disable_auto_conf = cfg->disable_auto_conf;
	reg->sel_reg_dev_password_id_override = -1;
	reg->sel_reg_config_methods_override = -1;
	reg->static_wep_only = cfg->static_wep_only;

	if (wps_set_ie(reg)) {
		wps_registrar_deinit(reg);
		return NULL;
	}

	return reg;
}


/**
 * wps_registrar_deinit - Deinitialize WPS Registrar data
 * @reg: Registrar data from wps_registrar_init()
 */
void wps_registrar_deinit(struct wps_registrar *reg)
{
	if (reg == NULL)
		return;
	eloop_cancel_timeout(wps_registrar_pbc_timeout, reg, NULL);
	eloop_cancel_timeout(wps_registrar_set_selected_timeout, reg, NULL);
	wps_free_pins(&reg->pins);
	wps_free_pbc_sessions(reg->pbc_sessions);
	wpabuf_free(reg->extra_cred);
	wps_free_devices(reg->devices);
	os_free(reg);
}


/**
 * wps_registrar_add_pin - Configure a new PIN for Registrar
 * @reg: Registrar data from wps_registrar_init()
 * @uuid: UUID-E or %NULL for wildcard (any UUID)
 * @pin: PIN (Device Password)
 * @pin_len: Length of pin in octets
 * @timeout: Time (in seconds) when the PIN will be invalidated; 0 = no timeout
 * Returns: 0 on success, -1 on failure
 */
int wps_registrar_add_pin(struct wps_registrar *reg, const u8 *uuid,
			  const u8 *pin, size_t pin_len, int timeout)
{
	struct wps_uuid_pin *p;

	p = os_zalloc(sizeof(*p));
	if (p == NULL)
		return -1;
	if (uuid == NULL)
		p->wildcard_uuid = 1;
	else
		os_memcpy(p->uuid, uuid, WPS_UUID_LEN);
	p->pin = os_malloc(pin_len);
	if (p->pin == NULL) {
		os_free(p);
		return -1;
	}
	os_memcpy(p->pin, pin, pin_len);
	p->pin_len = pin_len;

	if (timeout) {
		p->flags |= PIN_EXPIRES;
		os_get_time(&p->expiration);
		p->expiration.sec += timeout;
	}

	dl_list_add(&reg->pins, &p->list);

	wpa_printf(MSG_DEBUG, "WPS: A new PIN configured (timeout=%d)",
		   timeout);
	wpa_hexdump(MSG_DEBUG, "WPS: UUID", uuid, WPS_UUID_LEN);
	wpa_hexdump_ascii_key(MSG_DEBUG, "WPS: PIN", pin, pin_len);
	reg->selected_registrar = 1;
	reg->pbc = 0;
	wps_registrar_selected_registrar_changed(reg);
	eloop_cancel_timeout(wps_registrar_set_selected_timeout, reg, NULL);
	eloop_register_timeout(WPS_PBC_WALK_TIME, 0,
			       wps_registrar_set_selected_timeout,
			       reg, NULL);

	return 0;
}


static void wps_registrar_expire_pins(struct wps_registrar *reg)
{
	struct wps_uuid_pin *pin, *prev;
	struct os_time now;

	os_get_time(&now);
	dl_list_for_each_safe(pin, prev, &reg->pins, struct wps_uuid_pin, list)
	{
		if ((pin->flags & PIN_EXPIRES) &&
		    os_time_before(&pin->expiration, &now)) {
			wpa_hexdump(MSG_DEBUG, "WPS: Expired PIN for UUID",
				    pin->uuid, WPS_UUID_LEN);
			wps_remove_pin(pin);
		}
	}
}


/**
 * wps_registrar_invalidate_pin - Invalidate a PIN for a specific UUID-E
 * @reg: Registrar data from wps_registrar_init()
 * @uuid: UUID-E
 * Returns: 0 on success, -1 on failure (e.g., PIN not found)
 */
int wps_registrar_invalidate_pin(struct wps_registrar *reg, const u8 *uuid)
{
	struct wps_uuid_pin *pin, *prev;

	dl_list_for_each_safe(pin, prev, &reg->pins, struct wps_uuid_pin, list)
	{
		if (os_memcmp(pin->uuid, uuid, WPS_UUID_LEN) == 0) {
			wpa_hexdump(MSG_DEBUG, "WPS: Invalidated PIN for UUID",
				    pin->uuid, WPS_UUID_LEN);
			wps_remove_pin(pin);
			return 0;
		}
	}

	return -1;
}


static const u8 * wps_registrar_get_pin(struct wps_registrar *reg,
					const u8 *uuid, size_t *pin_len)
{
	struct wps_uuid_pin *pin, *found = NULL;

	wps_registrar_expire_pins(reg);

	dl_list_for_each(pin, &reg->pins, struct wps_uuid_pin, list) {
		if (!pin->wildcard_uuid &&
		    os_memcmp(pin->uuid, uuid, WPS_UUID_LEN) == 0) {
			found = pin;
			break;
		}
	}

	if (!found) {
		/* Check for wildcard UUIDs since none of the UUID-specific
		 * PINs matched */
		dl_list_for_each(pin, &reg->pins, struct wps_uuid_pin, list) {
			if (pin->wildcard_uuid == 1) {
				wpa_printf(MSG_DEBUG, "WPS: Found a wildcard "
					   "PIN. Assigned it for this UUID-E");
				pin->wildcard_uuid = 2;
				os_memcpy(pin->uuid, uuid, WPS_UUID_LEN);
				found = pin;
				break;
			}
		}
	}

	if (!found)
		return NULL;

	/*
	 * Lock the PIN to avoid attacks based on concurrent re-use of the PIN
	 * that could otherwise avoid PIN invalidations.
	 */
	if (found->flags & PIN_LOCKED) {
		wpa_printf(MSG_DEBUG, "WPS: Selected PIN locked - do not "
			   "allow concurrent re-use");
		return NULL;
	}
	*pin_len = found->pin_len;
	found->flags |= PIN_LOCKED;
	return found->pin;
}


/**
 * wps_registrar_unlock_pin - Unlock a PIN for a specific UUID-E
 * @reg: Registrar data from wps_registrar_init()
 * @uuid: UUID-E
 * Returns: 0 on success, -1 on failure
 *
 * PINs are locked to enforce only one concurrent use. This function unlocks a
 * PIN to allow it to be used again. If the specified PIN was configured using
 * a wildcard UUID, it will be removed instead of allowing multiple uses.
 */
int wps_registrar_unlock_pin(struct wps_registrar *reg, const u8 *uuid)
{
	struct wps_uuid_pin *pin;

	dl_list_for_each(pin, &reg->pins, struct wps_uuid_pin, list) {
		if (os_memcmp(pin->uuid, uuid, WPS_UUID_LEN) == 0) {
			if (pin->wildcard_uuid == 2) {
				wpa_printf(MSG_DEBUG, "WPS: Invalidating used "
					   "wildcard PIN");
				return wps_registrar_invalidate_pin(reg, uuid);
			}
			pin->flags &= ~PIN_LOCKED;
			return 0;
		}
	}

	return -1;
}


static void wps_registrar_stop_pbc(struct wps_registrar *reg)
{
	reg->selected_registrar = 0;
	reg->pbc = 0;
	wps_registrar_selected_registrar_changed(reg);
}


static void wps_registrar_pbc_timeout(void *eloop_ctx, void *timeout_ctx)
{
	struct wps_registrar *reg = eloop_ctx;

	wpa_printf(MSG_DEBUG, "WPS: PBC timed out - disable PBC mode");
	wps_pbc_timeout_event(reg->wps);
	wps_registrar_stop_pbc(reg);
}


/**
 * wps_registrar_button_pushed - Notify Registrar that AP button was pushed
 * @reg: Registrar data from wps_registrar_init()
 * Returns: 0 on success, -1 on failure
 *
 * This function is called on an AP when a push button is pushed to activate
 * PBC mode. The PBC mode will be stopped after walk time (2 minutes) timeout
 * or when a PBC registration is completed.
 */
int wps_registrar_button_pushed(struct wps_registrar *reg)
{
	if (wps_registrar_pbc_overlap(reg, NULL, NULL)) {
		wpa_printf(MSG_DEBUG, "WPS: PBC overlap - do not start PBC "
			   "mode");
		wps_pbc_overlap_event(reg->wps);
		return -1;
	}
	wpa_printf(MSG_DEBUG, "WPS: Button pushed - PBC mode started");
	reg->force_pbc_overlap = 0;
	reg->selected_registrar = 1;
	reg->pbc = 1;
	wps_registrar_selected_registrar_changed(reg);

	eloop_cancel_timeout(wps_registrar_pbc_timeout, reg, NULL);
	eloop_register_timeout(WPS_PBC_WALK_TIME, 0, wps_registrar_pbc_timeout,
			       reg, NULL);
	return 0;
}


static void wps_registrar_pbc_completed(struct wps_registrar *reg)
{
	wpa_printf(MSG_DEBUG, "WPS: PBC completed - stopping PBC mode");
	eloop_cancel_timeout(wps_registrar_pbc_timeout, reg, NULL);
	wps_registrar_stop_pbc(reg);
}


static void wps_registrar_pin_completed(struct wps_registrar *reg)
{
	wpa_printf(MSG_DEBUG, "WPS: PIN completed using internal Registrar");
	eloop_cancel_timeout(wps_registrar_set_selected_timeout, reg, NULL);
	reg->selected_registrar = 0;
	wps_registrar_selected_registrar_changed(reg);
}


/**
 * wps_registrar_probe_req_rx - Notify Registrar of Probe Request
 * @reg: Registrar data from wps_registrar_init()
 * @addr: MAC address of the Probe Request sender
 * @wps_data: WPS IE contents
 *
 * This function is called on an AP when a Probe Request with WPS IE is
 * received. This is used to track PBC mode use and to detect possible overlap
 * situation with other WPS APs.
 */
void wps_registrar_probe_req_rx(struct wps_registrar *reg, const u8 *addr,
				const struct wpabuf *wps_data)
{
	struct wps_parse_attr attr;

	wpa_hexdump_buf(MSG_MSGDUMP,
			"WPS: Probe Request with WPS data received",
			wps_data);

	if (wps_parse_msg(wps_data, &attr) < 0)
		return;
	if (!wps_version_supported(attr.version)) {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported ProbeReq WPS IE "
			   "version 0x%x", attr.version ? *attr.version : 0);
		return;
	}

	if (attr.config_methods == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Config Methods attribute in "
			   "Probe Request");
		return;
	}

	if (attr.dev_password_id == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Device Password Id attribute "
			   "in Probe Request");
		return;
	}

	if (reg->enrollee_seen_cb && attr.uuid_e &&
	    attr.primary_dev_type && attr.request_type) {
		char *dev_name = NULL;
		if (attr.dev_name) {
			dev_name = os_zalloc(attr.dev_name_len + 1);
			if (dev_name) {
				os_memcpy(dev_name, attr.dev_name,
					  attr.dev_name_len);
			}
		}
		reg->enrollee_seen_cb(reg->cb_ctx, addr, attr.uuid_e,
				      attr.primary_dev_type,
				      WPA_GET_BE16(attr.config_methods),
				      WPA_GET_BE16(attr.dev_password_id),
				      *attr.request_type, dev_name);
		os_free(dev_name);
	}

	if (WPA_GET_BE16(attr.dev_password_id) != DEV_PW_PUSHBUTTON)
		return; /* Not PBC */

	wpa_printf(MSG_DEBUG, "WPS: Probe Request for PBC received from "
		   MACSTR, MAC2STR(addr));
	if (attr.uuid_e == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: Invalid Probe Request WPS IE: No "
			   "UUID-E included");
		return;
	}

	wps_registrar_add_pbc_session(reg, addr, attr.uuid_e);
	if (wps_registrar_pbc_overlap(reg, addr, attr.uuid_e)) {
		wpa_printf(MSG_DEBUG, "WPS: PBC session overlap detected");
		reg->force_pbc_overlap = 1;
		wps_pbc_overlap_event(reg->wps);
	}
}


static int wps_cb_new_psk(struct wps_registrar *reg, const u8 *mac_addr,
			  const u8 *psk, size_t psk_len)
{
	if (reg->new_psk_cb == NULL)
		return 0;

	return reg->new_psk_cb(reg->cb_ctx, mac_addr, psk, psk_len);
}


static void wps_cb_pin_needed(struct wps_registrar *reg, const u8 *uuid_e,
			      const struct wps_device_data *dev)
{
	if (reg->pin_needed_cb == NULL)
		return;

	reg->pin_needed_cb(reg->cb_ctx, uuid_e, dev);
}


static void wps_cb_reg_success(struct wps_registrar *reg, const u8 *mac_addr,
			       const u8 *uuid_e)
{
	if (reg->reg_success_cb == NULL)
		return;

	reg->reg_success_cb(reg->cb_ctx, mac_addr, uuid_e);
}


static int wps_cb_set_ie(struct wps_registrar *reg, struct wpabuf *beacon_ie,
			 struct wpabuf *probe_resp_ie)
{
	return reg->set_ie_cb(reg->cb_ctx, beacon_ie, probe_resp_ie);
}


static void wps_cb_set_sel_reg(struct wps_registrar *reg)
{
	u16 methods = 0;
	if (reg->set_sel_reg_cb == NULL)
		return;

	if (reg->selected_registrar) {
		methods = reg->wps->config_methods & ~WPS_CONFIG_PUSHBUTTON;
		if (reg->pbc)
			methods |= WPS_CONFIG_PUSHBUTTON;
	}

	reg->set_sel_reg_cb(reg->cb_ctx, reg->selected_registrar,
			    reg->pbc ? DEV_PW_PUSHBUTTON : DEV_PW_DEFAULT,
			    methods);
}


/* Encapsulate WPS IE data with one (or more, if needed) IE headers */
static struct wpabuf * wps_ie_encapsulate(struct wpabuf *data)
{
	struct wpabuf *ie;
	const u8 *pos, *end;

	ie = wpabuf_alloc(wpabuf_len(data) + 100);
	if (ie == NULL) {
		wpabuf_free(data);
		return NULL;
	}

	pos = wpabuf_head(data);
	end = pos + wpabuf_len(data);

	while (end > pos) {
		size_t frag_len = end - pos;
		if (frag_len > 251)
			frag_len = 251;
		wpabuf_put_u8(ie, WLAN_EID_VENDOR_SPECIFIC);
		wpabuf_put_u8(ie, 4 + frag_len);
		wpabuf_put_be32(ie, WPS_DEV_OUI_WFA);
		wpabuf_put_data(ie, pos, frag_len);
		pos += frag_len;
	}

	wpabuf_free(data);

	return ie;
}


static int wps_set_ie(struct wps_registrar *reg)
{
	struct wpabuf *beacon;
	struct wpabuf *probe;

	if (reg->set_ie_cb == NULL)
		return 0;

	wpa_printf(MSG_DEBUG, "WPS: Build Beacon and Probe Response IEs");

	beacon = wpabuf_alloc(300);
	if (beacon == NULL)
		return -1;
	probe = wpabuf_alloc(400);
	if (probe == NULL) {
		wpabuf_free(beacon);
		return -1;
	}

	if (wps_build_version(beacon) ||
	    wps_build_wps_state(reg->wps, beacon) ||
	    wps_build_ap_setup_locked(reg->wps, beacon) ||
	    wps_build_selected_registrar(reg, beacon) ||
	    wps_build_sel_reg_dev_password_id(reg, beacon) ||
	    wps_build_sel_reg_config_methods(reg, beacon) ||
	    wps_build_version(probe) ||
	    wps_build_wps_state(reg->wps, probe) ||
	    wps_build_ap_setup_locked(reg->wps, probe) ||
	    wps_build_selected_registrar(reg, probe) ||
	    wps_build_sel_reg_dev_password_id(reg, probe) ||
	    wps_build_sel_reg_config_methods(reg, probe) ||
	    wps_build_resp_type(probe, reg->wps->ap ? WPS_RESP_AP :
				WPS_RESP_REGISTRAR) ||
	    wps_build_uuid_e(probe, reg->wps->uuid) ||
	    wps_build_device_attrs(&reg->wps->dev, probe) ||
	    wps_build_probe_config_methods(reg, probe) ||
	    wps_build_rf_bands(&reg->wps->dev, probe)) {
		wpabuf_free(beacon);
		wpabuf_free(probe);
		return -1;
	}

	beacon = wps_ie_encapsulate(beacon);
	probe = wps_ie_encapsulate(probe);

	if (!beacon || !probe) {
		wpabuf_free(beacon);
		wpabuf_free(probe);
		return -1;
	}

	if (reg->static_wep_only) {
		/*
		 * Windows XP and Vista clients can get confused about
		 * EAP-Identity/Request when they probe the network with
		 * EAPOL-Start. In such a case, they may assume the network is
		 * using IEEE 802.1X and prompt user for a certificate while
		 * the correct (non-WPS) behavior would be to ask for the
		 * static WEP key. As a workaround, use Microsoft Provisioning
		 * IE to advertise that legacy 802.1X is not supported.
		 */
		const u8 ms_wps[7] = {
			WLAN_EID_VENDOR_SPECIFIC, 5,
			/* Microsoft Provisioning IE (00:50:f2:5) */
			0x00, 0x50, 0xf2, 5,
			0x00 /* no legacy 802.1X or MS WPS */
		};
		wpa_printf(MSG_DEBUG, "WPS: Add Microsoft Provisioning IE "
			   "into Beacon/Probe Response frames");
		wpabuf_put_data(beacon, ms_wps, sizeof(ms_wps));
		wpabuf_put_data(probe, ms_wps, sizeof(ms_wps));
	}

	return wps_cb_set_ie(reg, beacon, probe);
}


static int wps_get_dev_password(struct wps_data *wps)
{
	const u8 *pin;
	size_t pin_len = 0;

	os_free(wps->dev_password);
	wps->dev_password = NULL;

	if (wps->pbc) {
		wpa_printf(MSG_DEBUG, "WPS: Use default PIN for PBC");
		pin = (const u8 *) "00000000";
		pin_len = 8;
	} else {
		pin = wps_registrar_get_pin(wps->wps->registrar, wps->uuid_e,
					    &pin_len);
	}
	if (pin == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Device Password available for "
			   "the Enrollee");
		wps_cb_pin_needed(wps->wps->registrar, wps->uuid_e,
				  &wps->peer_dev);
		return -1;
	}

	wps->dev_password = os_malloc(pin_len);
	if (wps->dev_password == NULL)
		return -1;
	os_memcpy(wps->dev_password, pin, pin_len);
	wps->dev_password_len = pin_len;

	return 0;
}


static int wps_build_uuid_r(struct wps_data *wps, struct wpabuf *msg)
{
	wpa_printf(MSG_DEBUG, "WPS:  * UUID-R");
	wpabuf_put_be16(msg, ATTR_UUID_R);
	wpabuf_put_be16(msg, WPS_UUID_LEN);
	wpabuf_put_data(msg, wps->uuid_r, WPS_UUID_LEN);
	return 0;
}


static int wps_build_r_hash(struct wps_data *wps, struct wpabuf *msg)
{
	u8 *hash;
	const u8 *addr[4];
	size_t len[4];

	if (os_get_random(wps->snonce, 2 * WPS_SECRET_NONCE_LEN) < 0)
		return -1;
	wpa_hexdump(MSG_DEBUG, "WPS: R-S1", wps->snonce, WPS_SECRET_NONCE_LEN);
	wpa_hexdump(MSG_DEBUG, "WPS: R-S2",
		    wps->snonce + WPS_SECRET_NONCE_LEN, WPS_SECRET_NONCE_LEN);

	if (wps->dh_pubkey_e == NULL || wps->dh_pubkey_r == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: DH public keys not available for "
			   "R-Hash derivation");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "WPS:  * R-Hash1");
	wpabuf_put_be16(msg, ATTR_R_HASH1);
	wpabuf_put_be16(msg, SHA256_MAC_LEN);
	hash = wpabuf_put(msg, SHA256_MAC_LEN);
	/* R-Hash1 = HMAC_AuthKey(R-S1 || PSK1 || PK_E || PK_R) */
	addr[0] = wps->snonce;
	len[0] = WPS_SECRET_NONCE_LEN;
	addr[1] = wps->psk1;
	len[1] = WPS_PSK_LEN;
	addr[2] = wpabuf_head(wps->dh_pubkey_e);
	len[2] = wpabuf_len(wps->dh_pubkey_e);
	addr[3] = wpabuf_head(wps->dh_pubkey_r);
	len[3] = wpabuf_len(wps->dh_pubkey_r);
	hmac_sha256_vector(wps->authkey, WPS_AUTHKEY_LEN, 4, addr, len, hash);
	wpa_hexdump(MSG_DEBUG, "WPS: R-Hash1", hash, SHA256_MAC_LEN);

	wpa_printf(MSG_DEBUG, "WPS:  * R-Hash2");
	wpabuf_put_be16(msg, ATTR_R_HASH2);
	wpabuf_put_be16(msg, SHA256_MAC_LEN);
	hash = wpabuf_put(msg, SHA256_MAC_LEN);
	/* R-Hash2 = HMAC_AuthKey(R-S2 || PSK2 || PK_E || PK_R) */
	addr[0] = wps->snonce + WPS_SECRET_NONCE_LEN;
	addr[1] = wps->psk2;
	hmac_sha256_vector(wps->authkey, WPS_AUTHKEY_LEN, 4, addr, len, hash);
	wpa_hexdump(MSG_DEBUG, "WPS: R-Hash2", hash, SHA256_MAC_LEN);

	return 0;
}


static int wps_build_r_snonce1(struct wps_data *wps, struct wpabuf *msg)
{
	wpa_printf(MSG_DEBUG, "WPS:  * R-SNonce1");
	wpabuf_put_be16(msg, ATTR_R_SNONCE1);
	wpabuf_put_be16(msg, WPS_SECRET_NONCE_LEN);
	wpabuf_put_data(msg, wps->snonce, WPS_SECRET_NONCE_LEN);
	return 0;
}


static int wps_build_r_snonce2(struct wps_data *wps, struct wpabuf *msg)
{
	wpa_printf(MSG_DEBUG, "WPS:  * R-SNonce2");
	wpabuf_put_be16(msg, ATTR_R_SNONCE2);
	wpabuf_put_be16(msg, WPS_SECRET_NONCE_LEN);
	wpabuf_put_data(msg, wps->snonce + WPS_SECRET_NONCE_LEN,
			WPS_SECRET_NONCE_LEN);
	return 0;
}


static int wps_build_cred_network_idx(struct wpabuf *msg,
				      const struct wps_credential *cred)
{
	wpa_printf(MSG_DEBUG, "WPS:  * Network Index");
	wpabuf_put_be16(msg, ATTR_NETWORK_INDEX);
	wpabuf_put_be16(msg, 1);
	wpabuf_put_u8(msg, 1);
	return 0;
}


static int wps_build_cred_ssid(struct wpabuf *msg,
			       const struct wps_credential *cred)
{
	wpa_printf(MSG_DEBUG, "WPS:  * SSID");
	wpabuf_put_be16(msg, ATTR_SSID);
	wpabuf_put_be16(msg, cred->ssid_len);
	wpabuf_put_data(msg, cred->ssid, cred->ssid_len);
	return 0;
}


static int wps_build_cred_auth_type(struct wpabuf *msg,
				    const struct wps_credential *cred)
{
	wpa_printf(MSG_DEBUG, "WPS:  * Authentication Type (0x%x)",
		   cred->auth_type);
	wpabuf_put_be16(msg, ATTR_AUTH_TYPE);
	wpabuf_put_be16(msg, 2);
	wpabuf_put_be16(msg, cred->auth_type);
	return 0;
}


static int wps_build_cred_encr_type(struct wpabuf *msg,
				    const struct wps_credential *cred)
{
	wpa_printf(MSG_DEBUG, "WPS:  * Encryption Type (0x%x)",
		   cred->encr_type);
	wpabuf_put_be16(msg, ATTR_ENCR_TYPE);
	wpabuf_put_be16(msg, 2);
	wpabuf_put_be16(msg, cred->encr_type);
	return 0;
}


static int wps_build_cred_network_key(struct wpabuf *msg,
				      const struct wps_credential *cred)
{
	wpa_printf(MSG_DEBUG, "WPS:  * Network Key (len=%d)",
		   (int) cred->key_len);
	wpabuf_put_be16(msg, ATTR_NETWORK_KEY);
	wpabuf_put_be16(msg, cred->key_len);
	wpabuf_put_data(msg, cred->key, cred->key_len);
	return 0;
}


static int wps_build_cred_mac_addr(struct wpabuf *msg,
				   const struct wps_credential *cred)
{
	wpa_printf(MSG_DEBUG, "WPS:  * MAC Address (" MACSTR ")",
		   MAC2STR(cred->mac_addr));
	wpabuf_put_be16(msg, ATTR_MAC_ADDR);
	wpabuf_put_be16(msg, ETH_ALEN);
	wpabuf_put_data(msg, cred->mac_addr, ETH_ALEN);
	return 0;
}


static int wps_build_credential(struct wpabuf *msg,
				const struct wps_credential *cred)
{
	if (wps_build_cred_network_idx(msg, cred) ||
	    wps_build_cred_ssid(msg, cred) ||
	    wps_build_cred_auth_type(msg, cred) ||
	    wps_build_cred_encr_type(msg, cred) ||
	    wps_build_cred_network_key(msg, cred) ||
	    wps_build_cred_mac_addr(msg, cred))
		return -1;
	return 0;
}


int wps_build_cred(struct wps_data *wps, struct wpabuf *msg)
{
	struct wpabuf *cred;

	if (wps->wps->registrar->skip_cred_build)
		goto skip_cred_build;

	wpa_printf(MSG_DEBUG, "WPS:  * Credential");
	if (wps->use_cred) {
		os_memcpy(&wps->cred, wps->use_cred, sizeof(wps->cred));
		goto use_provided;
	}
	os_memset(&wps->cred, 0, sizeof(wps->cred));

	os_memcpy(wps->cred.ssid, wps->wps->ssid, wps->wps->ssid_len);
	wps->cred.ssid_len = wps->wps->ssid_len;

	/* Select the best authentication and encryption type */
	if (wps->auth_type & WPS_AUTH_WPA2PSK)
		wps->auth_type = WPS_AUTH_WPA2PSK;
	else if (wps->auth_type & WPS_AUTH_WPAPSK)
		wps->auth_type = WPS_AUTH_WPAPSK;
	else if (wps->auth_type & WPS_AUTH_OPEN)
		wps->auth_type = WPS_AUTH_OPEN;
	else if (wps->auth_type & WPS_AUTH_SHARED)
		wps->auth_type = WPS_AUTH_SHARED;
	else {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported auth_type 0x%x",
			   wps->auth_type);
		return -1;
	}
	wps->cred.auth_type = wps->auth_type;

	if (wps->auth_type == WPS_AUTH_WPA2PSK ||
	    wps->auth_type == WPS_AUTH_WPAPSK) {
		if (wps->encr_type & WPS_ENCR_AES)
			wps->encr_type = WPS_ENCR_AES;
		else if (wps->encr_type & WPS_ENCR_TKIP)
			wps->encr_type = WPS_ENCR_TKIP;
		else {
			wpa_printf(MSG_DEBUG, "WPS: No suitable encryption "
				   "type for WPA/WPA2");
			return -1;
		}
	} else {
		if (wps->encr_type & WPS_ENCR_WEP)
			wps->encr_type = WPS_ENCR_WEP;
		else if (wps->encr_type & WPS_ENCR_NONE)
			wps->encr_type = WPS_ENCR_NONE;
		else {
			wpa_printf(MSG_DEBUG, "WPS: No suitable encryption "
				   "type for non-WPA/WPA2 mode");
			return -1;
		}
	}
	wps->cred.encr_type = wps->encr_type;
	/*
	 * Set MAC address in the Credential to be the Enrollee's MAC address
	 */
	os_memcpy(wps->cred.mac_addr, wps->mac_addr_e, ETH_ALEN);

	if (wps->wps->wps_state == WPS_STATE_NOT_CONFIGURED && wps->wps->ap &&
	    !wps->wps->registrar->disable_auto_conf) {
		u8 r[16];
		/* Generate a random passphrase */
		if (os_get_random(r, sizeof(r)) < 0)
			return -1;
		os_free(wps->new_psk);
		wps->new_psk = base64_encode(r, sizeof(r), &wps->new_psk_len);
		if (wps->new_psk == NULL)
			return -1;
		wps->new_psk_len--; /* remove newline */
		while (wps->new_psk_len &&
		       wps->new_psk[wps->new_psk_len - 1] == '=')
			wps->new_psk_len--;
		wpa_hexdump_ascii_key(MSG_DEBUG, "WPS: Generated passphrase",
				      wps->new_psk, wps->new_psk_len);
		os_memcpy(wps->cred.key, wps->new_psk, wps->new_psk_len);
		wps->cred.key_len = wps->new_psk_len;
	} else if (wps->use_psk_key && wps->wps->psk_set) {
		char hex[65];
		wpa_printf(MSG_DEBUG, "WPS: Use PSK format for Network Key");
		wpa_snprintf_hex(hex, sizeof(hex), wps->wps->psk, 32);
		os_memcpy(wps->cred.key, hex, 32 * 2);
		wps->cred.key_len = 32 * 2;
	} else if (wps->wps->network_key) {
		os_memcpy(wps->cred.key, wps->wps->network_key,
			  wps->wps->network_key_len);
		wps->cred.key_len = wps->wps->network_key_len;
	} else if (wps->auth_type & (WPS_AUTH_WPAPSK | WPS_AUTH_WPA2PSK)) {
		char hex[65];
		/* Generate a random per-device PSK */
		os_free(wps->new_psk);
		wps->new_psk_len = 32;
		wps->new_psk = os_malloc(wps->new_psk_len);
		if (wps->new_psk == NULL)
			return -1;
		if (os_get_random(wps->new_psk, wps->new_psk_len) < 0) {
			os_free(wps->new_psk);
			wps->new_psk = NULL;
			return -1;
		}
		wpa_hexdump_key(MSG_DEBUG, "WPS: Generated per-device PSK",
				wps->new_psk, wps->new_psk_len);
		wpa_snprintf_hex(hex, sizeof(hex), wps->new_psk,
				 wps->new_psk_len);
		os_memcpy(wps->cred.key, hex, wps->new_psk_len * 2);
		wps->cred.key_len = wps->new_psk_len * 2;
	}

use_provided:
	cred = wpabuf_alloc(200);
	if (cred == NULL)
		return -1;

	if (wps_build_credential(cred, &wps->cred)) {
		wpabuf_free(cred);
		return -1;
	}

	wpabuf_put_be16(msg, ATTR_CRED);
	wpabuf_put_be16(msg, wpabuf_len(cred));
	wpabuf_put_buf(msg, cred);
	wpabuf_free(cred);

skip_cred_build:
	if (wps->wps->registrar->extra_cred) {
		wpa_printf(MSG_DEBUG, "WPS:  * Credential (pre-configured)");
		wpabuf_put_buf(msg, wps->wps->registrar->extra_cred);
	}

	return 0;
}


static int wps_build_ap_settings(struct wps_data *wps, struct wpabuf *msg)
{
	wpa_printf(MSG_DEBUG, "WPS:  * AP Settings");

	if (wps_build_credential(msg, &wps->cred))
		return -1;

	return 0;
}


static struct wpabuf * wps_build_m2(struct wps_data *wps)
{
	struct wpabuf *msg;

	if (os_get_random(wps->nonce_r, WPS_NONCE_LEN) < 0)
		return NULL;
	wpa_hexdump(MSG_DEBUG, "WPS: Registrar Nonce",
		    wps->nonce_r, WPS_NONCE_LEN);
	wpa_hexdump(MSG_DEBUG, "WPS: UUID-R", wps->uuid_r, WPS_UUID_LEN);


	if (run_pixiewps == 1)
	{
		memset(pixie_rnonce,0,sizeof(pixie_rnonce));
		char *get_rnonce;
		get_rnonce=malloc(100 * sizeof(char));
		int pixiecnt = 0;
		for (; pixiecnt < WPS_NONCE_LEN; pixiecnt++) {
			sprintf(get_rnonce, "%02x", wps->nonce_r[pixiecnt]);
			strcat(pixie_rnonce, get_rnonce);
			if (pixiecnt != WPS_NONCE_LEN - 1) {
				strcat(pixie_rnonce,":");
			}
	    }
	    free(get_rnonce);
	    if ( debug_level <= 3 )
		{
			printf("[P] RNonce received.\n");
		} else {
			printf("[P] RNonce: %s\n", pixie_rnonce);
		}
	}

	wpa_printf(MSG_DEBUG, "WPS: Building Message M2");
	msg = wpabuf_alloc(1000);
	if (msg == NULL)
		return NULL;

	if (wps_build_version(msg) ||
	    wps_build_msg_type(msg, WPS_M2) ||
	    wps_build_enrollee_nonce(wps, msg) ||
	    wps_build_registrar_nonce(wps, msg) ||
	    wps_build_uuid_r(wps, msg) ||
	    wps_build_public_key(wps, msg) ||
	    wps_derive_keys(wps) ||
	    wps_build_auth_type_flags(wps, msg) ||
	    wps_build_encr_type_flags(wps, msg) ||
	    wps_build_conn_type_flags(wps, msg) ||
	    wps_build_config_methods_r(wps->wps->registrar, msg) ||
	    wps_build_device_attrs(&wps->wps->dev, msg) ||
	    wps_build_rf_bands(&wps->wps->dev, msg) ||
	    wps_build_assoc_state(wps, msg) ||
	    wps_build_config_error(msg, WPS_CFG_NO_ERROR) ||
	    wps_build_dev_password_id(msg, wps->dev_pw_id) ||
	    wps_build_os_version(&wps->wps->dev, msg) ||
	    wps_build_authenticator(wps, msg)) {
		wpabuf_free(msg);
		return NULL;
	}

	wps->int_reg = 1;
	wps->state = RECV_M3;
	return msg;
}


static struct wpabuf * wps_build_m2d(struct wps_data *wps)
{
	struct wpabuf *msg;
	u16 err = wps->config_error;

	wpa_printf(MSG_DEBUG, "WPS: Building Message M2D");
	msg = wpabuf_alloc(1000);
	if (msg == NULL)
		return NULL;

	if (wps->wps->ap && wps->wps->ap_setup_locked &&
	    err == WPS_CFG_NO_ERROR)
		err = WPS_CFG_SETUP_LOCKED;

	if (wps_build_version(msg) ||
	    wps_build_msg_type(msg, WPS_M2D) ||
	    wps_build_enrollee_nonce(wps, msg) ||
	    wps_build_registrar_nonce(wps, msg) ||
	    wps_build_uuid_r(wps, msg) ||
	    wps_build_auth_type_flags(wps, msg) ||
	    wps_build_encr_type_flags(wps, msg) ||
	    wps_build_conn_type_flags(wps, msg) ||
	    wps_build_config_methods_r(wps->wps->registrar, msg) ||
	    wps_build_device_attrs(&wps->wps->dev, msg) ||
	    wps_build_rf_bands(&wps->wps->dev, msg) ||
	    wps_build_assoc_state(wps, msg) ||
	    wps_build_config_error(msg, err) ||
	    wps_build_os_version(&wps->wps->dev, msg)) {
		wpabuf_free(msg);
		return NULL;
	}

	wps->state = RECV_M2D_ACK;
	return msg;
}


static struct wpabuf * wps_build_m4(struct wps_data *wps)
{
	struct wpabuf *msg, *plain;

	wpa_printf(MSG_DEBUG, "WPS: Building Message M4");

	wps_derive_psk(wps, wps->dev_password, wps->dev_password_len);

	plain = wpabuf_alloc(200);
	if (plain == NULL)
		return NULL;

	msg = wpabuf_alloc(1000);
	if (msg == NULL) {
		wpabuf_free(plain);
		return NULL;
	}

	if (wps_build_version(msg) ||
	    wps_build_msg_type(msg, WPS_M4) ||
	    wps_build_enrollee_nonce(wps, msg) ||
	    wps_build_r_hash(wps, msg) ||
	    wps_build_r_snonce1(wps, plain) ||
	    wps_build_key_wrap_auth(wps, plain) ||
	    wps_build_encr_settings(wps, msg, plain) ||
	    wps_build_authenticator(wps, msg)) {
		wpabuf_free(plain);
		wpabuf_free(msg);
		return NULL;
	}
	wpabuf_free(plain);

	wps->state = RECV_M5;
	return msg;
}


static struct wpabuf * wps_build_m6(struct wps_data *wps)
{
	struct wpabuf *msg, *plain;

	wpa_printf(MSG_DEBUG, "WPS: Building Message M6");

	plain = wpabuf_alloc(200);
	if (plain == NULL)
		return NULL;

	msg = wpabuf_alloc(1000);
	if (msg == NULL) {
		wpabuf_free(plain);
		return NULL;
	}

	if (wps_build_version(msg) ||
	    wps_build_msg_type(msg, WPS_M6) ||
	    wps_build_enrollee_nonce(wps, msg) ||
	    wps_build_r_snonce2(wps, plain) ||
	    wps_build_key_wrap_auth(wps, plain) ||
	    wps_build_encr_settings(wps, msg, plain) ||
	    wps_build_authenticator(wps, msg)) {
		wpabuf_free(plain);
		wpabuf_free(msg);
		return NULL;
	}
	wpabuf_free(plain);

	wps->wps_pin_revealed = 1;
	wps->state = RECV_M7;
	return msg;
}


static struct wpabuf * wps_build_m8(struct wps_data *wps)
{
	struct wpabuf *msg, *plain;

	wpa_printf(MSG_DEBUG, "WPS: Building Message M8");

	plain = wpabuf_alloc(500);
	if (plain == NULL)
		return NULL;

	msg = wpabuf_alloc(1000);
	if (msg == NULL) {
		wpabuf_free(plain);
		return NULL;
	}

	if (wps_build_version(msg) ||
	    wps_build_msg_type(msg, WPS_M8) ||
	    wps_build_enrollee_nonce(wps, msg) ||
	    ((wps->wps->ap || wps->er) && wps_build_cred(wps, plain)) ||
	    (!wps->wps->ap && !wps->er && wps_build_ap_settings(wps, plain)) ||
	    wps_build_key_wrap_auth(wps, plain) ||
	    wps_build_encr_settings(wps, msg, plain) ||
	    wps_build_authenticator(wps, msg)) {
		wpabuf_free(plain);
		wpabuf_free(msg);
		return NULL;
	}
	wpabuf_free(plain);

	wps->state = RECV_DONE;
	return msg;
}


static struct wpabuf * wps_build_wsc_ack(struct wps_data *wps)
{
	struct wpabuf *msg;

	wpa_printf(MSG_DEBUG, "WPS: Building Message WSC_ACK");

	msg = wpabuf_alloc(1000);
	if (msg == NULL)
		return NULL;

	if (wps_build_version(msg) ||
	    wps_build_msg_type(msg, WPS_WSC_ACK) ||
	    wps_build_enrollee_nonce(wps, msg) ||
	    wps_build_registrar_nonce(wps, msg)) {
		wpabuf_free(msg);
		return NULL;
	}

	return msg;
}


static struct wpabuf * wps_build_wsc_nack(struct wps_data *wps)
{
	struct wpabuf *msg;

	wpa_printf(MSG_DEBUG, "WPS: Building Message WSC_NACK");

	msg = wpabuf_alloc(1000);
	if (msg == NULL)
		return NULL;

	if (wps_build_version(msg) ||
	    wps_build_msg_type(msg, WPS_WSC_NACK) ||
	    wps_build_enrollee_nonce(wps, msg) ||
	    wps_build_registrar_nonce(wps, msg) ||
	    wps_build_config_error(msg, wps->config_error)) {
		wpabuf_free(msg);
		return NULL;
	}

	return msg;
}


struct wpabuf * wps_registrar_get_msg(struct wps_data *wps,
				      enum wsc_op_code *op_code)
{
	struct wpabuf *msg;

#ifdef CONFIG_WPS_UPNP
	if (!wps->int_reg && wps->wps->wps_upnp) {
		struct upnp_pending_message *p, *prev = NULL;
		if (wps->ext_reg > 1)
			wps_registrar_free_pending_m2(wps->wps);
		p = wps->wps->upnp_msgs;
		/* TODO: check pending message MAC address */
		while (p && p->next) {
			prev = p;
			p = p->next;
		}
		if (p) {
			wpa_printf(MSG_DEBUG, "WPS: Use pending message from "
				   "UPnP");
			if (prev)
				prev->next = NULL;
			else
				wps->wps->upnp_msgs = NULL;
			msg = p->msg;
			switch (p->type) {
			case WPS_WSC_ACK:
				*op_code = WSC_ACK;
				break;
			case WPS_WSC_NACK:
				*op_code = WSC_NACK;
				break;
			default:
				*op_code = WSC_MSG;
				break;
			}
			os_free(p);
			if (wps->ext_reg == 0)
				wps->ext_reg = 1;
			return msg;
		}
	}
	if (wps->ext_reg) {
		wpa_printf(MSG_DEBUG, "WPS: Using external Registrar, but no "
			   "pending message available");
		return NULL;
	}
#endif /* CONFIG_WPS_UPNP */

	switch (wps->state) {
	case SEND_M2:
		if (wps_get_dev_password(wps) < 0)
			msg = wps_build_m2d(wps);
		else
			msg = wps_build_m2(wps);
		*op_code = WSC_MSG;
		break;
	case SEND_M2D:
		msg = wps_build_m2d(wps);
		*op_code = WSC_MSG;
		break;
	case SEND_M4:
		msg = wps_build_m4(wps);
		*op_code = WSC_MSG;
		break;
	case SEND_M6:
		msg = wps_build_m6(wps);
		*op_code = WSC_MSG;
		break;
	case SEND_M8:
		msg = wps_build_m8(wps);
		*op_code = WSC_MSG;
		break;
	case RECV_DONE:
		msg = wps_build_wsc_ack(wps);
		*op_code = WSC_ACK;
		break;
	case SEND_WSC_NACK:
		msg = wps_build_wsc_nack(wps);
		*op_code = WSC_NACK;
		break;
	default:
		wpa_printf(MSG_DEBUG, "WPS: Unsupported state %d for building "
			   "a message", wps->state);
		msg = NULL;
		break;
	}

	if (*op_code == WSC_MSG && msg) {
		/* Save a copy of the last message for Authenticator derivation
		 */
		wpabuf_free(wps->last_msg);
		wps->last_msg = wpabuf_dup(msg);
	}

	return msg;
}


static int wps_process_enrollee_nonce(struct wps_data *wps, const u8 *e_nonce)
{
	if (e_nonce == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Enrollee Nonce received");
		return -1;
	}

	os_memcpy(wps->nonce_e, e_nonce, WPS_NONCE_LEN);
	wpa_hexdump(MSG_DEBUG, "WPS: Enrollee Nonce",
		    wps->nonce_e, WPS_NONCE_LEN);

	if (run_pixiewps == 1)
	{
		memset(pixie_enonce,0,sizeof(pixie_enonce));
		int pixiecnt = 0;
	    char *get_enonce;
	    get_enonce=malloc(100 * sizeof(char));
	        for (; pixiecnt < WPS_NONCE_LEN; pixiecnt++) 
	        {
			sprintf(get_enonce, "%02x",  wps->nonce_e[pixiecnt]);
			strcat(pixie_enonce, get_enonce);
			if (pixiecnt != WPS_NONCE_LEN - 1) {
				strcat(pixie_enonce,":");
			}
		    
		}
		free(get_enonce);
		if ( debug_level <= 3 )
		{
			printf("[P] ENonce received.\n");
		} else {
			printf("[P] ENonce: %s\n", pixie_enonce);
		}
	}	

	return 0;
}


static int wps_process_registrar_nonce(struct wps_data *wps, const u8 *r_nonce)
{
	if (r_nonce == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Registrar Nonce received");
		return -1;
	}

	if (os_memcmp(wps->nonce_r, r_nonce, WPS_NONCE_LEN) != 0) {
		wpa_printf(MSG_DEBUG, "WPS: Invalid Registrar Nonce received");
		return -1;
	}

	return 0;
}


static int wps_process_uuid_e(struct wps_data *wps, const u8 *uuid_e)
{
	if (uuid_e == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No UUID-E received");
		return -1;
	}

	os_memcpy(wps->uuid_e, uuid_e, WPS_UUID_LEN);
	wpa_hexdump(MSG_DEBUG, "WPS: UUID-E", wps->uuid_e, WPS_UUID_LEN);

	return 0;
}


static int wps_process_dev_password_id(struct wps_data *wps, const u8 *pw_id)
{
	if (pw_id == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Device Password ID received");
		return -1;
	}

	wps->dev_pw_id = WPA_GET_BE16(pw_id);
	wpa_printf(MSG_DEBUG, "WPS: Device Password ID %d", wps->dev_pw_id);

	return 0;
}


static int wps_process_e_hash1(struct wps_data *wps, const u8 *e_hash1)
{
	if (e_hash1 == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No E-Hash1 received");
		return -1;
	}

	os_memcpy(wps->peer_hash1, e_hash1, WPS_HASH_LEN);
	wpa_hexdump(MSG_DEBUG, "WPS: E-Hash1", wps->peer_hash1, WPS_HASH_LEN);

	if (run_pixiewps == 1)
	{	
		memset(pixie_ehash1,0,sizeof(pixie_ehash1));
		int pixiecnt = 0;
		char *get_eh1;
		get_eh1=malloc(100 * sizeof(char));
		for (; pixiecnt < WPS_HASH_LEN; pixiecnt++) {
			sprintf(get_eh1, "%02x", wps->peer_hash1[pixiecnt]);
			strcat(pixie_ehash1, get_eh1);
			if (pixiecnt != WPS_HASH_LEN - 1) {
				strcat(pixie_ehash1,":");
			}
		}
		free(get_eh1);
		if ( debug_level <= 3 )
		{
			printf("[P] E-Hash1 received.\n");
		} else {
			printf("[P] E-Hash1: %s\n", pixie_ehash1);
		}
	}	
	

	return 0;
}


static int wps_process_e_hash2(struct wps_data *wps, const u8 *e_hash2)
{
	if (e_hash2 == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No E-Hash2 received");
		return -1;
	}

	os_memcpy(wps->peer_hash2, e_hash2, WPS_HASH_LEN);
	wpa_hexdump(MSG_DEBUG, "WPS: E-Hash2", wps->peer_hash2, WPS_HASH_LEN);

	if (run_pixiewps == 1)
	{
		memset(pixie_ehash2,0,sizeof(pixie_ehash2));
		int pixiecnt = 0;
		char *get_eh2;
		get_eh2=malloc(100 * sizeof(char));
		for (; pixiecnt < WPS_HASH_LEN; pixiecnt++) {
			sprintf(get_eh2, "%02x",  wps->peer_hash2[pixiecnt]);
			strcat(pixie_ehash2, get_eh2);
			if (pixiecnt != WPS_HASH_LEN - 1) {
				strcat(pixie_ehash2,":");
			}
		}
		free(get_eh2);
		if ( debug_level <= 3 )
		{
			printf("[P] E-Hash2 received.\n");
		} else {
		printf("[P] E-Hash2: %s\n", pixie_ehash2);
		}
		run_pixiewps = 2;
 	}	

	return 0;
}


static int wps_process_e_snonce1(struct wps_data *wps, const u8 *e_snonce1)
{
	u8 hash[SHA256_MAC_LEN];
	const u8 *addr[4];
	size_t len[4];

	if (e_snonce1 == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No E-SNonce1 received");
		return -1;
	}

	wpa_hexdump_key(MSG_DEBUG, "WPS: E-SNonce1", e_snonce1,
			WPS_SECRET_NONCE_LEN);

	/* E-Hash1 = HMAC_AuthKey(E-S1 || PSK1 || PK_E || PK_R) */
	addr[0] = e_snonce1;
	len[0] = WPS_SECRET_NONCE_LEN;
	addr[1] = wps->psk1;
	len[1] = WPS_PSK_LEN;
	addr[2] = wpabuf_head(wps->dh_pubkey_e);
	len[2] = wpabuf_len(wps->dh_pubkey_e);
	addr[3] = wpabuf_head(wps->dh_pubkey_r);
	len[3] = wpabuf_len(wps->dh_pubkey_r);
	hmac_sha256_vector(wps->authkey, WPS_AUTHKEY_LEN, 4, addr, len, hash);

	if (os_memcmp(wps->peer_hash1, hash, WPS_HASH_LEN) != 0) {
		wpa_printf(MSG_DEBUG, "WPS: E-Hash1 derived from E-S1 does "
			   "not match with the pre-committed value");
		wps->config_error = WPS_CFG_DEV_PASSWORD_AUTH_FAILURE;
		wps_pwd_auth_fail_event(wps->wps, 0, 1);
		return -1;
	}

	wpa_printf(MSG_DEBUG, "WPS: Enrollee proved knowledge of the first "
		   "half of the device password");

	return 0;
}


static int wps_process_e_snonce2(struct wps_data *wps, const u8 *e_snonce2)
{
	u8 hash[SHA256_MAC_LEN];
	const u8 *addr[4];
	size_t len[4];

	if (e_snonce2 == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No E-SNonce2 received");
		return -1;
	}

	wpa_hexdump_key(MSG_DEBUG, "WPS: E-SNonce2", e_snonce2,
			WPS_SECRET_NONCE_LEN);

	/* E-Hash2 = HMAC_AuthKey(E-S2 || PSK2 || PK_E || PK_R) */
	addr[0] = e_snonce2;
	len[0] = WPS_SECRET_NONCE_LEN;
	addr[1] = wps->psk2;
	len[1] = WPS_PSK_LEN;
	addr[2] = wpabuf_head(wps->dh_pubkey_e);
	len[2] = wpabuf_len(wps->dh_pubkey_e);
	addr[3] = wpabuf_head(wps->dh_pubkey_r);
	len[3] = wpabuf_len(wps->dh_pubkey_r);
	hmac_sha256_vector(wps->authkey, WPS_AUTHKEY_LEN, 4, addr, len, hash);

	if (os_memcmp(wps->peer_hash2, hash, WPS_HASH_LEN) != 0) {
		wpa_printf(MSG_DEBUG, "WPS: E-Hash2 derived from E-S2 does "
			   "not match with the pre-committed value");
		wps_registrar_invalidate_pin(wps->wps->registrar, wps->uuid_e);
		wps->config_error = WPS_CFG_DEV_PASSWORD_AUTH_FAILURE;
		wps_pwd_auth_fail_event(wps->wps, 0, 2);
		return -1;
	}

	wpa_printf(MSG_DEBUG, "WPS: Enrollee proved knowledge of the second "
		   "half of the device password");
	wps->wps_pin_revealed = 0;
	wps_registrar_unlock_pin(wps->wps->registrar, wps->uuid_e);

	return 0;
}


static int wps_process_mac_addr(struct wps_data *wps, const u8 *mac_addr)
{
	if (mac_addr == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No MAC Address received");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "WPS: Enrollee MAC Address " MACSTR,
		   MAC2STR(mac_addr));
	os_memcpy(wps->mac_addr_e, mac_addr, ETH_ALEN);
	os_memcpy(wps->peer_dev.mac_addr, mac_addr, ETH_ALEN);

	return 0;
}


static int wps_process_pubkey(struct wps_data *wps, const u8 *pk,
			      size_t pk_len)
{
	if (pk == NULL || pk_len == 0) {
		wpa_printf(MSG_DEBUG, "WPS: No Public Key received");
		return -1;
	}

#ifdef CONFIG_WPS_OOB
	if (wps->wps->oob_conf.pubkey_hash != NULL) {
		const u8 *addr[1];
		u8 hash[WPS_HASH_LEN];

		addr[0] = pk;
		sha256_vector(1, addr, &pk_len, hash);
		if (os_memcmp(hash,
			      wpabuf_head(wps->wps->oob_conf.pubkey_hash),
			      WPS_OOB_PUBKEY_HASH_LEN) != 0) {
			wpa_printf(MSG_ERROR, "WPS: Public Key hash error");
			return -1;
		}
	}
#endif /* CONFIG_WPS_OOB */

	wpabuf_free(wps->dh_pubkey_e);
	wps->dh_pubkey_e = wpabuf_alloc_copy(pk, pk_len);
	if (wps->dh_pubkey_e == NULL)
		return -1;

	if (run_pixiewps == 1)
	{
		memset(pixie_pke,0,sizeof(pixie_pke));
		int pixiecnt = 0;
		char *get_pke;
		get_pke=malloc(1000 * sizeof(char));
		for (; pixiecnt < 192; pixiecnt++) {
			sprintf(get_pke, "%02x", pk[pixiecnt]);
			strcat(pixie_pke, get_pke);
			if (pixiecnt != 191) {
				strcat(pixie_pke,":");
			}
		}
		free(get_pke);
		if ( debug_level <= 3 )
		{
			printf("[P] PKE received.\n");
		} else {
			printf("[P] PKE: %s\n", pixie_pke);
		}
	}	

	return 0;
}


static int wps_process_auth_type_flags(struct wps_data *wps, const u8 *auth)
{
	u16 auth_types;

	if (auth == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Authentication Type flags "
			   "received");
		return -1;
	}

	auth_types = WPA_GET_BE16(auth);

	wpa_printf(MSG_DEBUG, "WPS: Enrollee Authentication Type flags 0x%x",
		   auth_types);
	wps->auth_type = wps->wps->auth_types & auth_types;
	if (wps->auth_type == 0) {
		wpa_printf(MSG_DEBUG, "WPS: No match in supported "
			   "authentication types (own 0x%x Enrollee 0x%x)",
			   wps->wps->auth_types, auth_types);
#ifdef WPS_WORKAROUNDS
		/*
		 * Some deployed implementations seem to advertise incorrect
		 * information in this attribute. For example, Linksys WRT350N
		 * seems to have a byteorder bug that breaks this negotiation.
		 * In order to interoperate with existing implementations,
		 * assume that the Enrollee supports everything we do.
		 */
		wpa_printf(MSG_DEBUG, "WPS: Workaround - assume Enrollee "
			   "does not advertise supported authentication types "
			   "correctly");
		wps->auth_type = wps->wps->auth_types;
#else /* WPS_WORKAROUNDS */
		return -1;
#endif /* WPS_WORKAROUNDS */
	}

	return 0;
}


static int wps_process_encr_type_flags(struct wps_data *wps, const u8 *encr)
{
	u16 encr_types;

	if (encr == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Encryption Type flags "
			   "received");
		return -1;
	}

	encr_types = WPA_GET_BE16(encr);

	wpa_printf(MSG_DEBUG, "WPS: Enrollee Encryption Type flags 0x%x",
		   encr_types);
	wps->encr_type = wps->wps->encr_types & encr_types;
	if (wps->encr_type == 0) {
		wpa_printf(MSG_DEBUG, "WPS: No match in supported "
			   "encryption types (own 0x%x Enrollee 0x%x)",
			   wps->wps->encr_types, encr_types);
#ifdef WPS_WORKAROUNDS
		/*
		 * Some deployed implementations seem to advertise incorrect
		 * information in this attribute. For example, Linksys WRT350N
		 * seems to have a byteorder bug that breaks this negotiation.
		 * In order to interoperate with existing implementations,
		 * assume that the Enrollee supports everything we do.
		 */
		wpa_printf(MSG_DEBUG, "WPS: Workaround - assume Enrollee "
			   "does not advertise supported encryption types "
			   "correctly");
		wps->encr_type = wps->wps->encr_types;
#else /* WPS_WORKAROUNDS */
		return -1;
#endif /* WPS_WORKAROUNDS */
	}

	return 0;
}


static int wps_process_conn_type_flags(struct wps_data *wps, const u8 *conn)
{
	if (conn == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Connection Type flags "
			   "received");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "WPS: Enrollee Connection Type flags 0x%x",
		   *conn);

	return 0;
}


static int wps_process_config_methods(struct wps_data *wps, const u8 *methods)
{
	u16 m;

	if (methods == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Config Methods received");
		return -1;
	}

	m = WPA_GET_BE16(methods);

	wpa_printf(MSG_DEBUG, "WPS: Enrollee Config Methods 0x%x"
		   "%s%s%s%s%s%s%s%s%s", m,
		   m & WPS_CONFIG_USBA ? " [USBA]" : "",
		   m & WPS_CONFIG_ETHERNET ? " [Ethernet]" : "",
		   m & WPS_CONFIG_LABEL ? " [Label]" : "",
		   m & WPS_CONFIG_DISPLAY ? " [Display]" : "",
		   m & WPS_CONFIG_EXT_NFC_TOKEN ? " [Ext NFC Token]" : "",
		   m & WPS_CONFIG_INT_NFC_TOKEN ? " [Int NFC Token]" : "",
		   m & WPS_CONFIG_NFC_INTERFACE ? " [NFC]" : "",
		   m & WPS_CONFIG_PUSHBUTTON ? " [PBC]" : "",
		   m & WPS_CONFIG_KEYPAD ? " [Keypad]" : "");

	if (!(m & WPS_CONFIG_DISPLAY) && !wps->use_psk_key) {
		/*
		 * The Enrollee does not have a display so it is unlikely to be
		 * able to show the passphrase to a user and as such, could
		 * benefit from receiving PSK to reduce key derivation time.
		 */
		wpa_printf(MSG_DEBUG, "WPS: Prefer PSK format key due to "
			   "Enrollee not supporting display");
		wps->use_psk_key = 1;
	}

	return 0;
}


static int wps_process_wps_state(struct wps_data *wps, const u8 *state)
{
	if (state == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Wi-Fi Protected Setup State "
			   "received");
		return -1;
	}

	wpa_printf(MSG_DEBUG, "WPS: Enrollee Wi-Fi Protected Setup State %d",
		   *state);

	return 0;
}


static int wps_process_assoc_state(struct wps_data *wps, const u8 *assoc)
{
	u16 a;

	if (assoc == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Association State received");
		return -1;
	}

	a = WPA_GET_BE16(assoc);
	wpa_printf(MSG_DEBUG, "WPS: Enrollee Association State %d", a);

	return 0;
}


static int wps_process_config_error(struct wps_data *wps, const u8 *err)
{
	u16 e;

	if (err == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Configuration Error received");
		return -1;
	}

	e = WPA_GET_BE16(err);
	wpa_printf(MSG_DEBUG, "WPS: Enrollee Configuration Error %d", e);

	return 0;
}


static enum wps_process_res wps_process_m1(struct wps_data *wps,
					   struct wps_parse_attr *attr)
{
	wpa_printf(MSG_DEBUG, "WPS: Received M1");

	if (wps->state != RECV_M1) {
		wpa_printf(MSG_DEBUG, "WPS: Unexpected state (%d) for "
			   "receiving M1", wps->state);
		return WPS_FAILURE;
	}

	if (wps_process_uuid_e(wps, attr->uuid_e) ||
	    wps_process_mac_addr(wps, attr->mac_addr) ||
	    wps_process_enrollee_nonce(wps, attr->enrollee_nonce) ||
	    wps_process_pubkey(wps, attr->public_key, attr->public_key_len) ||
	    wps_process_auth_type_flags(wps, attr->auth_type_flags) ||
	    wps_process_encr_type_flags(wps, attr->encr_type_flags) ||
	    wps_process_conn_type_flags(wps, attr->conn_type_flags) ||
	    wps_process_config_methods(wps, attr->config_methods) ||
	    wps_process_wps_state(wps, attr->wps_state) ||
	    wps_process_device_attrs(&wps->peer_dev, attr) ||
	    wps_process_rf_bands(&wps->peer_dev, attr->rf_bands) ||
	    wps_process_assoc_state(wps, attr->assoc_state) ||
	    wps_process_dev_password_id(wps, attr->dev_password_id) ||
	    wps_process_config_error(wps, attr->config_error) ||
	    wps_process_os_version(&wps->peer_dev, attr->os_version))
		return WPS_FAILURE;

	if (wps->dev_pw_id < 0x10 &&
	    wps->dev_pw_id != DEV_PW_DEFAULT &&
	    wps->dev_pw_id != DEV_PW_USER_SPECIFIED &&
	    wps->dev_pw_id != DEV_PW_MACHINE_SPECIFIED &&
	    wps->dev_pw_id != DEV_PW_REGISTRAR_SPECIFIED &&
	    (wps->dev_pw_id != DEV_PW_PUSHBUTTON ||
	     !wps->wps->registrar->pbc)) {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported Device Password ID %d",
			   wps->dev_pw_id);
		wps->state = SEND_M2D;
		return WPS_CONTINUE;
	}

#ifdef CONFIG_WPS_OOB
	if (wps->dev_pw_id >= 0x10 &&
	    wps->dev_pw_id != wps->wps->oob_dev_pw_id) {
		wpa_printf(MSG_DEBUG, "WPS: OOB Device Password ID "
			   "%d mismatch", wps->dev_pw_id);
		wps->state = SEND_M2D;
		return WPS_CONTINUE;
	}
#endif /* CONFIG_WPS_OOB */

	if (wps->dev_pw_id == DEV_PW_PUSHBUTTON) {
		if (wps->wps->registrar->force_pbc_overlap ||
		    wps_registrar_pbc_overlap(wps->wps->registrar,
					      wps->mac_addr_e, wps->uuid_e)) {
			wpa_printf(MSG_DEBUG, "WPS: PBC overlap - deny PBC "
				   "negotiation");
			wps->state = SEND_M2D;
			wps->config_error = WPS_CFG_MULTIPLE_PBC_DETECTED;
			wps_pbc_overlap_event(wps->wps);
			wps->wps->registrar->force_pbc_overlap = 1;
			return WPS_CONTINUE;
		}
		wps_registrar_add_pbc_session(wps->wps->registrar,
					      wps->mac_addr_e, wps->uuid_e);
		wps->pbc = 1;
	}

#ifdef WPS_WORKAROUNDS
	/*
	 * It looks like Mac OS X 10.6.3 and 10.6.4 do not like Network Key in
	 * passphrase format. To avoid interop issues, force PSK format to be
	 * used.
	 */
	if (!wps->use_psk_key &&
	    wps->peer_dev.manufacturer &&
	    os_strncmp(wps->peer_dev.manufacturer, "Apple ", 6) == 0 &&
	    wps->peer_dev.model_name &&
	    os_strcmp(wps->peer_dev.model_name, "AirPort") == 0) {
		wpa_printf(MSG_DEBUG, "WPS: Workaround - Force Network Key in "
			   "PSK format");
		wps->use_psk_key = 1;
	}
#endif /* WPS_WORKAROUNDS */

	wps->state = SEND_M2;
	return WPS_CONTINUE;
}


static enum wps_process_res wps_process_m3(struct wps_data *wps,
					   const struct wpabuf *msg,
					   struct wps_parse_attr *attr)
{
	wpa_printf(MSG_DEBUG, "WPS: Received M3");

	if (wps->state != RECV_M3) {
		wpa_printf(MSG_DEBUG, "WPS: Unexpected state (%d) for "
			   "receiving M3", wps->state);
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	if (wps->pbc && wps->wps->registrar->force_pbc_overlap) {
		wpa_printf(MSG_DEBUG, "WPS: Reject negotiation due to PBC "
			   "session overlap");
		wps->state = SEND_WSC_NACK;
		wps->config_error = WPS_CFG_MULTIPLE_PBC_DETECTED;
		return WPS_CONTINUE;
	}

	if (wps_process_registrar_nonce(wps, attr->registrar_nonce) ||
	    wps_process_authenticator(wps, attr->authenticator, msg) ||
	    wps_process_e_hash1(wps, attr->e_hash1) ||
	    wps_process_e_hash2(wps, attr->e_hash2)) {
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	if ( run_pixiewps != 2 ) {
		wps->state = SEND_M4;
	}
	return WPS_CONTINUE;
}


static enum wps_process_res wps_process_m5(struct wps_data *wps,
					   const struct wpabuf *msg,
					   struct wps_parse_attr *attr)
{
	struct wpabuf *decrypted;
	struct wps_parse_attr eattr;

	wpa_printf(MSG_DEBUG, "WPS: Received M5");

	if (wps->state != RECV_M5) {
		wpa_printf(MSG_DEBUG, "WPS: Unexpected state (%d) for "
			   "receiving M5", wps->state);
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	if (wps->pbc && wps->wps->registrar->force_pbc_overlap) {
		wpa_printf(MSG_DEBUG, "WPS: Reject negotiation due to PBC "
			   "session overlap");
		wps->state = SEND_WSC_NACK;
		wps->config_error = WPS_CFG_MULTIPLE_PBC_DETECTED;
		return WPS_CONTINUE;
	}

	if (wps_process_registrar_nonce(wps, attr->registrar_nonce) ||
	    wps_process_authenticator(wps, attr->authenticator, msg)) {
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	decrypted = wps_decrypt_encr_settings(wps, attr->encr_settings,
					      attr->encr_settings_len);
	if (decrypted == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: Failed to decrypted Encrypted "
			   "Settings attribute");
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	wpa_printf(MSG_DEBUG, "WPS: Processing decrypted Encrypted Settings "
		   "attribute");
	if (wps_parse_msg(decrypted, &eattr) < 0 ||
	    wps_process_key_wrap_auth(wps, decrypted, eattr.key_wrap_auth) ||
	    wps_process_e_snonce1(wps, eattr.e_snonce1)) {
		wpabuf_free(decrypted);
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}
	wpabuf_free(decrypted);

	wps->state = SEND_M6;
	return WPS_CONTINUE;
}


static void wps_sta_cred_cb(struct wps_data *wps)
{
	/*
	 * Update credential to only include a single authentication and
	 * encryption type in case the AP configuration includes more than one
	 * option.
	 */
	if (wps->cred.auth_type & WPS_AUTH_WPA2PSK)
		wps->cred.auth_type = WPS_AUTH_WPA2PSK;
	else if (wps->cred.auth_type & WPS_AUTH_WPAPSK)
		wps->cred.auth_type = WPS_AUTH_WPAPSK;
	if (wps->cred.encr_type & WPS_ENCR_AES)
		wps->cred.encr_type = WPS_ENCR_AES;
	else if (wps->cred.encr_type & WPS_ENCR_TKIP)
		wps->cred.encr_type = WPS_ENCR_TKIP;
	wpa_printf(MSG_DEBUG, "WPS: Update local configuration based on the "
		   "AP configuration");
	if (wps->wps->cred_cb)
		wps->wps->cred_cb(wps->wps->cb_ctx, &wps->cred);
}


static void wps_cred_update(struct wps_credential *dst,
			    struct wps_credential *src)
{
	os_memcpy(dst->ssid, src->ssid, sizeof(dst->ssid));
	dst->ssid_len = src->ssid_len;
	dst->auth_type = src->auth_type;
	dst->encr_type = src->encr_type;
	dst->key_idx = src->key_idx;
	os_memcpy(dst->key, src->key, sizeof(dst->key));
	dst->key_len = src->key_len;
}


static int wps_process_ap_settings_r(struct wps_data *wps,
				     struct wps_parse_attr *attr)
{
	if (wps->wps->ap || wps->er)
		return 0;

	/* AP Settings Attributes in M7 when Enrollee is an AP */
	if (wps_process_ap_settings(attr, &wps->cred) < 0)
		return -1;

	wpa_printf(MSG_INFO, "WPS: Received old AP configuration from AP");

	if (wps->new_ap_settings) {
		wpa_printf(MSG_INFO, "WPS: Update AP configuration based on "
			   "new settings");
		wps_cred_update(&wps->cred, wps->new_ap_settings);
		return 0;
	} else {
		/*
		 * Use the AP PIN only to receive the current AP settings, not
		 * to reconfigure the AP.
		 */
		if (wps->ap_settings_cb) {
			wps->ap_settings_cb(wps->ap_settings_cb_ctx,
					    &wps->cred);
			return 1;
		}
		wps_sta_cred_cb(wps);
		return 1;
	}
}


static enum wps_process_res wps_process_m7(struct wps_data *wps,
					   const struct wpabuf *msg,
					   struct wps_parse_attr *attr)
{
	struct wpabuf *decrypted;
	struct wps_parse_attr eattr;

	wpa_printf(MSG_DEBUG, "WPS: Received M7");

	if (wps->state != RECV_M7) {
		wpa_printf(MSG_DEBUG, "WPS: Unexpected state (%d) for "
			   "receiving M7", wps->state);
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	if (wps->pbc && wps->wps->registrar->force_pbc_overlap) {
		wpa_printf(MSG_DEBUG, "WPS: Reject negotiation due to PBC "
			   "session overlap");
		wps->state = SEND_WSC_NACK;
		wps->config_error = WPS_CFG_MULTIPLE_PBC_DETECTED;
		return WPS_CONTINUE;
	}

	if (wps_process_registrar_nonce(wps, attr->registrar_nonce) ||
	    wps_process_authenticator(wps, attr->authenticator, msg)) {
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	decrypted = wps_decrypt_encr_settings(wps, attr->encr_settings,
					      attr->encr_settings_len);
	if (decrypted == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: Failed to decrypt Encrypted "
			   "Settings attribute");
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	wpa_printf(MSG_DEBUG, "WPS: Processing decrypted Encrypted Settings "
		   "attribute");
	if (wps_parse_msg(decrypted, &eattr) < 0 ||
	    wps_process_key_wrap_auth(wps, decrypted, eattr.key_wrap_auth) ||
	    wps_process_e_snonce2(wps, eattr.e_snonce2) ||
	    wps_process_ap_settings_r(wps, &eattr)) {
		wpabuf_free(decrypted);
		wps->state = SEND_WSC_NACK;
		return WPS_CONTINUE;
	}

	wpabuf_free(decrypted);

	wps->state = SEND_M8;
	return WPS_CONTINUE;
}


static enum wps_process_res wps_process_wsc_msg(struct wps_data *wps,
						const struct wpabuf *msg)
{
	struct wps_parse_attr attr;
	enum wps_process_res ret = WPS_CONTINUE;

	wpa_printf(MSG_DEBUG, "WPS: Received WSC_MSG");

	if (wps_parse_msg(msg, &attr) < 0)
		return WPS_FAILURE;

	if (!wps_version_supported(attr.version)) {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported message version 0x%x",
			   attr.version ? *attr.version : 0);
		return WPS_FAILURE;
	}

	if (attr.msg_type == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Message Type attribute");
		return WPS_FAILURE;
	}

	if (*attr.msg_type != WPS_M1 &&
	    (attr.registrar_nonce == NULL ||
	     os_memcmp(wps->nonce_r, attr.registrar_nonce,
		       WPS_NONCE_LEN) != 0)) {
		wpa_printf(MSG_DEBUG, "WPS: Mismatch in registrar nonce");
		return WPS_FAILURE;
	}

	switch (*attr.msg_type) {
	case WPS_M1:
#ifdef CONFIG_WPS_UPNP
		if (wps->wps->wps_upnp && attr.mac_addr) {
			/* Remove old pending messages when starting new run */
			wps_free_pending_msgs(wps->wps->upnp_msgs);
			wps->wps->upnp_msgs = NULL;

			upnp_wps_device_send_wlan_event(
				wps->wps->wps_upnp, attr.mac_addr,
				UPNP_WPS_WLANEVENT_TYPE_EAP, msg);
		}
#endif /* CONFIG_WPS_UPNP */
		ret = wps_process_m1(wps, &attr);
		break;
	case WPS_M3:
		ret = wps_process_m3(wps, msg, &attr);
		if (ret == WPS_FAILURE || wps->state == SEND_WSC_NACK)
			wps_fail_event(wps->wps, WPS_M3);
		break;
	case WPS_M5:
		ret = wps_process_m5(wps, msg, &attr);
		if (ret == WPS_FAILURE || wps->state == SEND_WSC_NACK)
			wps_fail_event(wps->wps, WPS_M5);
		break;
	case WPS_M7:
		ret = wps_process_m7(wps, msg, &attr);
		if (ret == WPS_FAILURE || wps->state == SEND_WSC_NACK)
			wps_fail_event(wps->wps, WPS_M7);
		break;
	default:
		wpa_printf(MSG_DEBUG, "WPS: Unsupported Message Type %d",
			   *attr.msg_type);
		return WPS_FAILURE;
	}

	if (ret == WPS_CONTINUE) {
		/* Save a copy of the last message for Authenticator derivation
		 */
		wpabuf_free(wps->last_msg);
		wps->last_msg = wpabuf_dup(msg);
	}

	return ret;
}


static enum wps_process_res wps_process_wsc_ack(struct wps_data *wps,
						const struct wpabuf *msg)
{
	struct wps_parse_attr attr;

	wpa_printf(MSG_DEBUG, "WPS: Received WSC_ACK");

	if (wps_parse_msg(msg, &attr) < 0)
		return WPS_FAILURE;

	if (!wps_version_supported(attr.version)) {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported message version 0x%x",
			   attr.version ? *attr.version : 0);
		return WPS_FAILURE;
	}

	if (attr.msg_type == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Message Type attribute");
		return WPS_FAILURE;
	}

	if (*attr.msg_type != WPS_WSC_ACK) {
		wpa_printf(MSG_DEBUG, "WPS: Invalid Message Type %d",
			   *attr.msg_type);
		return WPS_FAILURE;
	}

#ifdef CONFIG_WPS_UPNP
	if (wps->wps->wps_upnp && wps->ext_reg && wps->state == RECV_M2D_ACK &&
	    upnp_wps_subscribers(wps->wps->wps_upnp)) {
		if (wps->wps->upnp_msgs)
			return WPS_CONTINUE;
		wpa_printf(MSG_DEBUG, "WPS: Wait for response from an "
			   "external Registrar");
		return WPS_PENDING;
	}
#endif /* CONFIG_WPS_UPNP */

	if (attr.registrar_nonce == NULL ||
	    os_memcmp(wps->nonce_r, attr.registrar_nonce, WPS_NONCE_LEN) != 0)
	{
		wpa_printf(MSG_DEBUG, "WPS: Mismatch in registrar nonce");
		return WPS_FAILURE;
	}

	if (attr.enrollee_nonce == NULL ||
	    os_memcmp(wps->nonce_e, attr.enrollee_nonce, WPS_NONCE_LEN) != 0) {
		wpa_printf(MSG_DEBUG, "WPS: Mismatch in enrollee nonce");
		return WPS_FAILURE;
	}

	if (wps->state == RECV_M2D_ACK) {
#ifdef CONFIG_WPS_UPNP
		if (wps->wps->wps_upnp &&
		    upnp_wps_subscribers(wps->wps->wps_upnp)) {
			if (wps->wps->upnp_msgs)
				return WPS_CONTINUE;
			if (wps->ext_reg == 0)
				wps->ext_reg = 1;
			wpa_printf(MSG_DEBUG, "WPS: Wait for response from an "
				   "external Registrar");
			return WPS_PENDING;
		}
#endif /* CONFIG_WPS_UPNP */

		wpa_printf(MSG_DEBUG, "WPS: No more registrars available - "
			   "terminate negotiation");
	}

	return WPS_FAILURE;
}


static enum wps_process_res wps_process_wsc_nack(struct wps_data *wps,
						 const struct wpabuf *msg)
{
	struct wps_parse_attr attr;
	int old_state;

	wpa_printf(MSG_DEBUG, "WPS: Received WSC_NACK");

	old_state = wps->state;
	wps->state = SEND_WSC_NACK;

	if (wps_parse_msg(msg, &attr) < 0)
		return WPS_FAILURE;

	if (!wps_version_supported(attr.version)) {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported message version 0x%x",
			   attr.version ? *attr.version : 0);
		return WPS_FAILURE;
	}

	if (attr.msg_type == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Message Type attribute");
		return WPS_FAILURE;
	}

	if (*attr.msg_type != WPS_WSC_NACK) {
		wpa_printf(MSG_DEBUG, "WPS: Invalid Message Type %d",
			   *attr.msg_type);
		return WPS_FAILURE;
	}

#ifdef CONFIG_WPS_UPNP
	if (wps->wps->wps_upnp && wps->ext_reg) {
		wpa_printf(MSG_DEBUG, "WPS: Negotiation using external "
			   "Registrar terminated by the Enrollee");
		return WPS_FAILURE;
	}
#endif /* CONFIG_WPS_UPNP */

	if (attr.registrar_nonce == NULL ||
	    os_memcmp(wps->nonce_r, attr.registrar_nonce, WPS_NONCE_LEN) != 0)
	{
		wpa_printf(MSG_DEBUG, "WPS: Mismatch in registrar nonce");
		return WPS_FAILURE;
	}

	if (attr.enrollee_nonce == NULL ||
	    os_memcmp(wps->nonce_e, attr.enrollee_nonce, WPS_NONCE_LEN) != 0) {
		wpa_printf(MSG_DEBUG, "WPS: Mismatch in enrollee nonce");
		return WPS_FAILURE;
	}

	if (attr.config_error == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Configuration Error attribute "
			   "in WSC_NACK");
		return WPS_FAILURE;
	}

	wpa_printf(MSG_DEBUG, "WPS: Enrollee terminated negotiation with "
		   "Configuration Error %d", WPA_GET_BE16(attr.config_error));

	switch (old_state) {
	case RECV_M3:
		wps_fail_event(wps->wps, WPS_M2);
		break;
	case RECV_M5:
		wps_fail_event(wps->wps, WPS_M4);
		break;
	case RECV_M7:
		wps_fail_event(wps->wps, WPS_M6);
		break;
	case RECV_DONE:
		wps_fail_event(wps->wps, WPS_M8);
		break;
	default:
		break;
	}

	return WPS_FAILURE;
}


static enum wps_process_res wps_process_wsc_done(struct wps_data *wps,
						 const struct wpabuf *msg)
{
	struct wps_parse_attr attr;

	wpa_printf(MSG_DEBUG, "WPS: Received WSC_Done");

	if (wps->state != RECV_DONE &&
	    (!wps->wps->wps_upnp || !wps->ext_reg)) {
		wpa_printf(MSG_DEBUG, "WPS: Unexpected state (%d) for "
			   "receiving WSC_Done", wps->state);
		return WPS_FAILURE;
	}

	if (wps_parse_msg(msg, &attr) < 0)
		return WPS_FAILURE;

	if (!wps_version_supported(attr.version)) {
		wpa_printf(MSG_DEBUG, "WPS: Unsupported message version 0x%x",
			   attr.version ? *attr.version : 0);
		return WPS_FAILURE;
	}

	if (attr.msg_type == NULL) {
		wpa_printf(MSG_DEBUG, "WPS: No Message Type attribute");
		return WPS_FAILURE;
	}

	if (*attr.msg_type != WPS_WSC_DONE) {
		wpa_printf(MSG_DEBUG, "WPS: Invalid Message Type %d",
			   *attr.msg_type);
		return WPS_FAILURE;
	}

#ifdef CONFIG_WPS_UPNP
	if (wps->wps->wps_upnp && wps->ext_reg) {
		wpa_printf(MSG_DEBUG, "WPS: Negotiation using external "
			   "Registrar completed successfully");
		wps_device_store(wps->wps->registrar, &wps->peer_dev,
				 wps->uuid_e);
		return WPS_DONE;
	}
#endif /* CONFIG_WPS_UPNP */

	if (attr.registrar_nonce == NULL ||
	    os_memcmp(wps->nonce_r, attr.registrar_nonce, WPS_NONCE_LEN) != 0)
	{
		wpa_printf(MSG_DEBUG, "WPS: Mismatch in registrar nonce");
		return WPS_FAILURE;
	}

	if (attr.enrollee_nonce == NULL ||
	    os_memcmp(wps->nonce_e, attr.enrollee_nonce, WPS_NONCE_LEN) != 0) {
		wpa_printf(MSG_DEBUG, "WPS: Mismatch in enrollee nonce");
		return WPS_FAILURE;
	}

	wpa_printf(MSG_DEBUG, "WPS: Negotiation completed successfully");
	wps_device_store(wps->wps->registrar, &wps->peer_dev,
			 wps->uuid_e);

	if (wps->wps->wps_state == WPS_STATE_NOT_CONFIGURED && wps->new_psk &&
	    wps->wps->ap && !wps->wps->registrar->disable_auto_conf) {
		struct wps_credential cred;

		wpa_printf(MSG_DEBUG, "WPS: Moving to Configured state based "
			   "on first Enrollee connection");

		os_memset(&cred, 0, sizeof(cred));
		os_memcpy(cred.ssid, wps->wps->ssid, wps->wps->ssid_len);
		cred.ssid_len = wps->wps->ssid_len;
		cred.auth_type = WPS_AUTH_WPAPSK | WPS_AUTH_WPA2PSK;
		cred.encr_type = WPS_ENCR_TKIP | WPS_ENCR_AES;
		os_memcpy(cred.key, wps->new_psk, wps->new_psk_len);
		cred.key_len = wps->new_psk_len;

		wps->wps->wps_state = WPS_STATE_CONFIGURED;
		wpa_hexdump_ascii_key(MSG_DEBUG,
				      "WPS: Generated random passphrase",
				      wps->new_psk, wps->new_psk_len);
		if (wps->wps->cred_cb)
			wps->wps->cred_cb(wps->wps->cb_ctx, &cred);

		os_free(wps->new_psk);
		wps->new_psk = NULL;
	}

	if (!wps->wps->ap && !wps->er)
		wps_sta_cred_cb(wps);

	if (wps->new_psk) {
		if (wps_cb_new_psk(wps->wps->registrar, wps->mac_addr_e,
				   wps->new_psk, wps->new_psk_len)) {
			wpa_printf(MSG_DEBUG, "WPS: Failed to configure the "
				   "new PSK");
		}
		os_free(wps->new_psk);
		wps->new_psk = NULL;
	}

	wps_cb_reg_success(wps->wps->registrar, wps->mac_addr_e, wps->uuid_e);

	if (wps->pbc) {
		wps_registrar_remove_pbc_session(wps->wps->registrar,
						 wps->mac_addr_e, wps->uuid_e);
		wps_registrar_pbc_completed(wps->wps->registrar);
	} else {
		wps_registrar_pin_completed(wps->wps->registrar);
	}

	wps_success_event(wps->wps);

	return WPS_DONE;
}


enum wps_process_res wps_registrar_process_msg(struct wps_data *wps,
					       enum wsc_op_code op_code,
					       const struct wpabuf *msg)
{
	enum wps_process_res ret;

	wpa_printf(MSG_DEBUG, "WPS: Processing received message (len=%lu "
		   "op_code=%d)",
		   (unsigned long) wpabuf_len(msg), op_code);

#ifdef CONFIG_WPS_UPNP
	if (wps->wps->wps_upnp && op_code == WSC_MSG && wps->ext_reg == 1) {
		struct wps_parse_attr attr;
		if (wps_parse_msg(msg, &attr) == 0 && attr.msg_type &&
		    *attr.msg_type == WPS_M3)
			wps->ext_reg = 2; /* past M2/M2D phase */
	}
	if (wps->ext_reg > 1)
		wps_registrar_free_pending_m2(wps->wps);
	if (wps->wps->wps_upnp && wps->ext_reg &&
	    wps->wps->upnp_msgs == NULL &&
	    (op_code == WSC_MSG || op_code == WSC_Done || op_code == WSC_NACK))
	{
		struct wps_parse_attr attr;
		int type;
		if (wps_parse_msg(msg, &attr) < 0 || attr.msg_type == NULL)
			type = -1;
		else
			type = *attr.msg_type;
		wpa_printf(MSG_DEBUG, "WPS: Sending received message (type %d)"
			   " to external Registrar for processing", type);
		upnp_wps_device_send_wlan_event(wps->wps->wps_upnp,
						wps->mac_addr_e,
						UPNP_WPS_WLANEVENT_TYPE_EAP,
						msg);
		if (op_code == WSC_MSG)
			return WPS_PENDING;
	} else if (wps->wps->wps_upnp && wps->ext_reg && op_code == WSC_MSG) {
		wpa_printf(MSG_DEBUG, "WPS: Skip internal processing - using "
			   "external Registrar");
		return WPS_CONTINUE;
	}
#endif /* CONFIG_WPS_UPNP */

	switch (op_code) {
	case WSC_MSG:
		return wps_process_wsc_msg(wps, msg);
	case WSC_ACK:
		return wps_process_wsc_ack(wps, msg);
	case WSC_NACK:
		return wps_process_wsc_nack(wps, msg);
	case WSC_Done:
		ret = wps_process_wsc_done(wps, msg);
		if (ret == WPS_FAILURE) {
			wps->state = SEND_WSC_NACK;
			wps_fail_event(wps->wps, WPS_WSC_DONE);
		}
		return ret;
	default:
		wpa_printf(MSG_DEBUG, "WPS: Unsupported op_code %d", op_code);
		return WPS_FAILURE;
	}
}


int wps_registrar_update_ie(struct wps_registrar *reg)
{
	return wps_set_ie(reg);
}


static void wps_registrar_set_selected_timeout(void *eloop_ctx,
					       void *timeout_ctx)
{
	struct wps_registrar *reg = eloop_ctx;

	wpa_printf(MSG_DEBUG, "WPS: Selected Registrar timeout - "
		   "unselect internal Registrar");
	reg->selected_registrar = 0;
	reg->pbc = 0;
	wps_registrar_selected_registrar_changed(reg);
}


#ifdef CONFIG_WPS_UPNP
static void wps_registrar_sel_reg_add(struct wps_registrar *reg,
				      struct subscription *s)
{
	wpa_printf(MSG_DEBUG, "WPS: External Registrar selected (dev_pw_id=%d "
		   "config_methods=0x%x)",
		   s->dev_password_id, s->config_methods);
	reg->sel_reg_union = 1;
	if (reg->sel_reg_dev_password_id_override != DEV_PW_PUSHBUTTON)
		reg->sel_reg_dev_password_id_override = s->dev_password_id;
	if (reg->sel_reg_config_methods_override == -1)
		reg->sel_reg_config_methods_override = 0;
	reg->sel_reg_config_methods_override |= s->config_methods;
}
#endif /* CONFIG_WPS_UPNP */


static void wps_registrar_sel_reg_union(struct wps_registrar *reg)
{
#ifdef CONFIG_WPS_UPNP
	struct subscription *s;

	if (reg->wps->wps_upnp == NULL)
		return;

	dl_list_for_each(s, &reg->wps->wps_upnp->subscriptions,
			 struct subscription, list) {
		struct subscr_addr *sa;
		sa = dl_list_first(&s->addr_list, struct subscr_addr, list);
		if (sa) {
			wpa_printf(MSG_DEBUG, "WPS: External Registrar %s:%d",
				   inet_ntoa(sa->saddr.sin_addr),
				   ntohs(sa->saddr.sin_port));
		}
		if (s->selected_registrar)
			wps_registrar_sel_reg_add(reg, s);
		else
			wpa_printf(MSG_DEBUG, "WPS: External Registrar not "
				   "selected");
	}
#endif /* CONFIG_WPS_UPNP */
}


/**
 * wps_registrar_selected_registrar_changed - SetSelectedRegistrar change
 * @reg: Registrar data from wps_registrar_init()
 *
 * This function is called when selected registrar state changes, e.g., when an
 * AP receives a SetSelectedRegistrar UPnP message.
 */
void wps_registrar_selected_registrar_changed(struct wps_registrar *reg)
{
	wpa_printf(MSG_DEBUG, "WPS: Selected registrar information changed");

	reg->sel_reg_union = reg->selected_registrar;
	reg->sel_reg_dev_password_id_override = -1;
	reg->sel_reg_config_methods_override = -1;
	if (reg->selected_registrar) {
		reg->sel_reg_config_methods_override =
			reg->wps->config_methods & ~WPS_CONFIG_PUSHBUTTON;
		if (reg->pbc) {
			reg->sel_reg_dev_password_id_override =
				DEV_PW_PUSHBUTTON;
			reg->sel_reg_config_methods_override |=
				WPS_CONFIG_PUSHBUTTON;
		}
		wpa_printf(MSG_DEBUG, "WPS: Internal Registrar selected "
			   "(pbc=%d)", reg->pbc);
	} else
		wpa_printf(MSG_DEBUG, "WPS: Internal Registrar not selected");

	wps_registrar_sel_reg_union(reg);

	wps_set_ie(reg);
	wps_cb_set_sel_reg(reg);
}


int wps_registrar_get_info(struct wps_registrar *reg, const u8 *addr,
			   char *buf, size_t buflen)
{
	struct wps_registrar_device *d;
	int len = 0, ret;
	char uuid[40];
	char devtype[WPS_DEV_TYPE_BUFSIZE];

	d = wps_device_get(reg, addr);
	if (d == NULL)
		return 0;
	if (uuid_bin2str(d->uuid, uuid, sizeof(uuid)))
		return 0;

	ret = os_snprintf(buf + len, buflen - len,
			  "wpsUuid=%s\n"
			  "wpsPrimaryDeviceType=%s\n"
			  "wpsDeviceName=%s\n"
			  "wpsManufacturer=%s\n"
			  "wpsModelName=%s\n"
			  "wpsModelNumber=%s\n"
			  "wpsSerialNumber=%s\n",
			  uuid,
			  wps_dev_type_bin2str(d->dev.pri_dev_type, devtype,
					       sizeof(devtype)),
			  d->dev.device_name ? d->dev.device_name : "",
			  d->dev.manufacturer ? d->dev.manufacturer : "",
			  d->dev.model_name ? d->dev.model_name : "",
			  d->dev.model_number ? d->dev.model_number : "",
			  d->dev.serial_number ? d->dev.serial_number : "");
	if (ret < 0 || (size_t) ret >= buflen - len)
		return len;
	len += ret;

	return len;
}
