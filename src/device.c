/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2010  Nokia Corporation
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/uuid.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <bluetooth/mgmt.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "log.h"

#include "att.h"
#include "hcid.h"
#include "adapter.h"
#include "gattrib.h"
#include "attio.h"
#include "device.h"
#include "profile.h"
#include "dbus-common.h"
#include "error.h"
#include "glib-helper.h"
#include "sdp-client.h"
#include "gatt.h"
#include "agent.h"
#include "sdp-xml.h"
#include "storage.h"
#include "btio.h"
#include "attrib-server.h"
#include "attrib/client.h"

#define DISCONNECT_TIMER	2
#define DISCOVERY_TIMER		2

struct btd_disconnect_data {
	guint id;
	disconnect_watch watch;
	void *user_data;
	GDestroyNotify destroy;
};

struct bonding_req {
	DBusMessage *msg;
	guint listener_id;
	struct btd_device *device;
};

struct authentication_req {
	auth_type_t type;
	void *cb;
	struct agent *agent;
	struct btd_device *device;
	uint32_t passkey;
	char *pincode;
	gboolean secure;
};

struct browse_req {
	DBusMessage *msg;
	struct btd_device *device;
	GSList *match_uuids;
	GSList *profiles_added;
	GSList *profiles_removed;
	sdp_list_t *records;
	int search_uuid;
	int reconnect_attempt;
	guint listener_id;
};

struct included_search {
	struct browse_req *req;
	GSList *services;
	GSList *current;
};

struct attio_data {
	guint id;
	attio_connect_cb cfunc;
	attio_disconnect_cb dcfunc;
	gpointer user_data;
};

struct pnpid_notifier {
	pnpid_ready_func func;
	void *user_data;
};

typedef void (*attio_error_cb) (const GError *gerr, gpointer user_data);
typedef void (*attio_success_cb) (gpointer user_data);

struct att_callbacks {
	attio_error_cb error;		/* Callback for error */
	attio_success_cb success;	/* Callback for success */
	gpointer user_data;
};

struct btd_device {
	bdaddr_t	bdaddr;
	uint8_t		bdaddr_type;
	gchar		*path;
	char		name[MAX_NAME_LENGTH + 1];
	char		*alias;
	uint16_t	vendor_src;
	uint16_t	vendor;
	uint16_t	product;
	uint16_t	version;
	struct btd_adapter	*adapter;
	GSList		*uuids;
	GSList		*services;		/* Primary services path */
	GSList		*primaries;		/* List of primary services */
	GSList		*profiles;		/* Probed profiles */
	GSList		*pending;		/* Pending profiles */
	GSList		*watches;		/* List of disconnect_data */
	gboolean	temporary;
	struct agent	*agent;
	guint		disconn_timer;
	guint		discov_timer;
	struct browse_req *browse;		/* service discover request */
	struct bonding_req *bonding;
	struct authentication_req *authr;	/* authentication request */
	GSList		*disconnects;		/* disconnects message */
	DBusMessage	*connect;		/* connect message */
	GAttrib		*attrib;
	GSList		*attios;
	GSList		*attios_offline;
	guint		attachid;		/* Attrib server attach */
	guint		auto_id;		/* Auto connect source id */

	gboolean	connected;
	gboolean	profiles_connected;	/* Profile level connected */

	sdp_list_t	*tmp_records;

	gboolean	trusted;
	gboolean	paired;
	gboolean	blocked;
	gboolean	bonded;
	gboolean	auto_connect;

	gint		ref;
	GSList		*batteries;

	GIOChannel      *att_io;
	guint		cleanup_id;
	uint16_t	att_mtu;

	GSList		*pnpid_notifiers;
};

static uint16_t uuid_list[] = {
	L2CAP_UUID,
	PNP_INFO_SVCLASS_ID,
	PUBLIC_BROWSE_GROUP,
	0
};

struct btd_battery {
	uint16_t		level;
	char			*path;
	struct btd_device	*device;
	refresh_battery_cb	refresh;
};

static void battery_free(gpointer user_data)
{
	struct btd_battery *b = user_data;

	g_dbus_unregister_interface(btd_get_dbus_connection(), b->path, BATTERY_INTERFACE);
	btd_device_unref(b->device);
	g_free(b->path);
	g_free(b);

}

static void browse_request_free(struct browse_req *req)
{
	if (req->listener_id)
		g_dbus_remove_watch(btd_get_dbus_connection(),
							req->listener_id);
	if (req->msg)
		dbus_message_unref(req->msg);
	if (req->device)
		btd_device_unref(req->device);
	g_slist_free_full(req->profiles_added, g_free);
	g_slist_free(req->profiles_removed);
	if (req->records)
		sdp_list_free(req->records, (sdp_free_func_t) sdp_record_free);

	g_free(req);
}

static void attio_cleanup(struct btd_device *device)
{
	if (device->attachid) {
		attrib_channel_detach(device->attrib, device->attachid);
		device->attachid = 0;
	}

	if (device->cleanup_id) {
		g_source_remove(device->cleanup_id);
		device->cleanup_id = 0;
	}

	if (device->att_io) {
		g_io_channel_shutdown(device->att_io, FALSE, NULL);
		g_io_channel_unref(device->att_io);
		device->att_io = NULL;
	}

	if (device->attrib) {
		g_attrib_unref(device->attrib);
		device->attrib = NULL;
	}
}

static void browse_request_cancel(struct browse_req *req)
{
	struct btd_device *device = req->device;
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t src;

	if (device_is_creating(device, NULL))
		device_set_temporary(device, TRUE);

	adapter_get_address(adapter, &src);

	bt_cancel_discovery(&src, &device->bdaddr);

	attio_cleanup(device);

	device->browse = NULL;
	browse_request_free(req);
}

static void device_free(gpointer user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device->adapter;
	struct agent *agent = adapter_get_agent(adapter);

	if (device->agent)
		agent_free(device->agent);

	if (agent && (agent_is_busy(agent, device) ||
				agent_is_busy(agent, device->authr)))
		agent_cancel(agent);

	g_slist_free_full(device->services, g_free);
	g_slist_free_full(device->uuids, g_free);
	g_slist_free_full(device->primaries, g_free);
	g_slist_free_full(device->attios, g_free);
	g_slist_free_full(device->attios_offline, g_free);
	g_slist_free_full(device->batteries, battery_free);
	g_slist_free_full(device->pnpid_notifiers, g_free);

	attio_cleanup(device);

	if (device->tmp_records)
		sdp_list_free(device->tmp_records,
					(sdp_free_func_t) sdp_record_free);

	if (device->disconn_timer)
		g_source_remove(device->disconn_timer);

	if (device->discov_timer)
		g_source_remove(device->discov_timer);

	if (device->auto_id)
		g_source_remove(device->auto_id);

	DBG("%p", device);

	if (device->authr)
		g_free(device->authr->pincode);
	g_free(device->authr);
	g_free(device->path);
	g_free(device->alias);
	g_free(device);
}

gboolean device_is_bredr(struct btd_device *device)
{
	return (device->bdaddr_type == BDADDR_BREDR);
}

gboolean device_is_le(struct btd_device *device)
{
	return (device->bdaddr_type != BDADDR_BREDR);
}

gboolean device_is_paired(struct btd_device *device)
{
	return device->paired;
}

gboolean device_is_bonded(struct btd_device *device)
{
	return device->bonded;
}

gboolean device_is_trusted(struct btd_device *device)
{
	return device->trusted;
}

static DBusMessage *get_properties(DBusConnection *conn,
				DBusMessage *msg, void *user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device->adapter;
	DBusMessage *reply;
	DBusMessageIter iter;
	DBusMessageIter dict;
	bdaddr_t src;
	char name[MAX_NAME_LENGTH + 1], dstaddr[18];
	char **str;
	const char *ptr, *icon = NULL;
	dbus_bool_t boolean;
	uint32_t class;
	uint16_t app;
	int i;
	GSList *l;

	ba2str(&device->bdaddr, dstaddr);

	reply = dbus_message_new_method_return(msg);
	if (!reply)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	/* Address */
	ptr = dstaddr;
	dict_append_entry(&dict, "Address", DBUS_TYPE_STRING, &ptr);

	/* Name */
	ptr = NULL;
	memset(name, 0, sizeof(name));
	adapter_get_address(adapter, &src);

	ptr = device->name;
	dict_append_entry(&dict, "Name", DBUS_TYPE_STRING, &ptr);

	/* Alias (fallback to name or address) */
	if (device->alias != NULL)
		ptr = device->alias;
	else if (strlen(ptr) == 0) {
		g_strdelimit(dstaddr, ":", '-');
		ptr = dstaddr;
	}

	dict_append_entry(&dict, "Alias", DBUS_TYPE_STRING, &ptr);

	/* Class */
	if (read_remote_class(&src, &device->bdaddr, &class) == 0) {
		icon = class_to_icon(class);

		dict_append_entry(&dict, "Class", DBUS_TYPE_UINT32, &class);
	} else if (read_remote_appearance(&src, &device->bdaddr,
					device->bdaddr_type, &app) == 0) {
		/* Appearance */
		icon = gap_appearance_to_icon(app);

		dict_append_entry(&dict, "Appearance", DBUS_TYPE_UINT16, &app);
	}

	dict_append_entry(&dict, "Icon", DBUS_TYPE_STRING, &icon);

	/* Vendor */
	if (device->vendor)
		dict_append_entry(&dict, "Vendor", DBUS_TYPE_UINT16,
							&device->vendor);

	/* Vendor Source*/
	if (device->vendor_src)
		dict_append_entry(&dict, "VendorSource", DBUS_TYPE_UINT16,
							&device->vendor_src);

	/* Product */
	if (device->product)
		dict_append_entry(&dict, "Product", DBUS_TYPE_UINT16,
							&device->product);

	/* Version */
	if (device->version)
		dict_append_entry(&dict, "Version", DBUS_TYPE_UINT16,
							&device->version);

	/* Paired */
	boolean = device_is_paired(device);
	dict_append_entry(&dict, "Paired", DBUS_TYPE_BOOLEAN, &boolean);

	/* Trusted */
	boolean = device_is_trusted(device);
	dict_append_entry(&dict, "Trusted", DBUS_TYPE_BOOLEAN, &boolean);

	/* Blocked */
	boolean = device->blocked;
	dict_append_entry(&dict, "Blocked", DBUS_TYPE_BOOLEAN, &boolean);

	/* Connected */
	dict_append_entry(&dict, "Connected", DBUS_TYPE_BOOLEAN,
							&device->connected);

	/* UUIDs */
	str = g_new0(char *, g_slist_length(device->uuids) + 1);
	for (i = 0, l = device->uuids; l; l = l->next, i++)
		str[i] = l->data;
	dict_append_array(&dict, "UUIDs", DBUS_TYPE_STRING, &str, i);
	g_free(str);

	/* Services */
	str = g_new0(char *, g_slist_length(device->services) + 1);
	for (i = 0, l = device->services; l; l = l->next, i++)
		str[i] = l->data;
	dict_append_array(&dict, "Services", DBUS_TYPE_OBJECT_PATH, &str, i);
	g_free(str);

	/* Adapter */
	ptr = adapter_get_path(adapter);
	dict_append_entry(&dict, "Adapter", DBUS_TYPE_OBJECT_PATH, &ptr);

	/* Batteries */
	str = g_new0(char *, g_slist_length(device->batteries) + 1);
	for (i = 0, l = device->batteries; l; l = l->next, i++) {
		struct btd_battery *b = l->data;
		str[i] = b->path;
	}
	dict_append_array(&dict, "Batteries", DBUS_TYPE_OBJECT_PATH, &str, i);
	g_free(str);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static DBusMessage *set_alias(DBusMessage *msg, const char *alias, void *data)
{
	struct btd_device *device = data;
	struct btd_adapter *adapter = device->adapter;
	char srcaddr[18], dstaddr[18];
	bdaddr_t src;
	int err;

	/* No change */
	if ((device->alias == NULL && g_str_equal(alias, "")) ||
			g_strcmp0(device->alias, alias) == 0)
		return dbus_message_new_method_return(msg);

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	/* Remove alias if empty string */
	err = write_device_alias(srcaddr, dstaddr, device->bdaddr_type,
					g_str_equal(alias, "") ? NULL : alias);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	g_free(device->alias);
	device->alias = g_str_equal(alias, "") ? NULL : g_strdup(alias);

	emit_property_changed(dbus_message_get_path(msg),
				DEVICE_INTERFACE, "Alias",
				DBUS_TYPE_STRING, &alias);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *set_trust(DBusMessage *msg, gboolean value, void *data)
{
	struct btd_device *device = data;
	struct btd_adapter *adapter = device->adapter;
	char srcaddr[18], dstaddr[18];
	bdaddr_t src;
	int err;

	if (device->trusted == value)
		return dbus_message_new_method_return(msg);

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	err = write_trust(srcaddr, dstaddr, device->bdaddr_type, value);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	device->trusted = value;

	emit_property_changed(dbus_message_get_path(msg),
				DEVICE_INTERFACE, "Trusted",
				DBUS_TYPE_BOOLEAN, &value);

	return dbus_message_new_method_return(msg);
}

static void profile_remove(struct btd_profile *profile,
						struct btd_device *device)
{
	profile->device_remove(profile, device);

	device->profiles = g_slist_remove(device->profiles, profile);
}

static gboolean do_disconnect(gpointer user_data)
{
	struct btd_device *device = user_data;

	device->disconn_timer = 0;

	btd_adapter_disconnect_device(device->adapter, &device->bdaddr,
							device->bdaddr_type);

	return FALSE;
}

int device_block(struct btd_device *device, gboolean update_only)
{
	int err = 0;
	bdaddr_t src;

	if (device->blocked)
		return 0;

	if (device->connected)
		do_disconnect(device);

	g_slist_foreach(device->profiles, (GFunc) profile_remove, device);

	if (!update_only)
		err = btd_adapter_block_address(device->adapter,
					&device->bdaddr, device->bdaddr_type);

	if (err < 0)
		return err;

	device->blocked = TRUE;

	adapter_get_address(device->adapter, &src);

	err = write_blocked(&src, &device->bdaddr, device->bdaddr_type, TRUE);
	if (err < 0)
		error("write_blocked(): %s (%d)", strerror(-err), -err);

	device_set_temporary(device, FALSE);

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Blocked",
				DBUS_TYPE_BOOLEAN, &device->blocked);

	return 0;
}

int device_unblock(struct btd_device *device, gboolean silent,
							gboolean update_only)
{
	int err = 0;
	bdaddr_t src;

	if (!device->blocked)
		return 0;

	if (!update_only)
		err = btd_adapter_unblock_address(device->adapter,
					&device->bdaddr, device->bdaddr_type);

	if (err < 0)
		return err;

	device->blocked = FALSE;

	adapter_get_address(device->adapter, &src);

	err = write_blocked(&src, &device->bdaddr, device->bdaddr_type, FALSE);
	if (err < 0)
		error("write_blocked(): %s (%d)", strerror(-err), -err);

	if (!silent) {
		emit_property_changed(device->path,
					DEVICE_INTERFACE, "Blocked",
					DBUS_TYPE_BOOLEAN, &device->blocked);
		device_probe_profiles(device, device->uuids);
	}

	return 0;
}

static DBusMessage *set_blocked(DBusMessage *msg, gboolean value, void *data)
{
	struct btd_device *device = data;
	int err;

	if (value)
		err = device_block(device, FALSE);
	else
		err = device_unblock(device, FALSE, FALSE);

	switch (-err) {
	case 0:
		return dbus_message_new_method_return(msg);
	case EINVAL:
		return btd_error_failed(msg, "Kernel lacks blacklist support");
	default:
		return btd_error_failed(msg, strerror(-err));
	}
}

static DBusMessage *set_property(DBusConnection *conn,
				DBusMessage *msg, void *data)
{
	DBusMessageIter iter;
	DBusMessageIter sub;
	const char *property;

	if (!dbus_message_iter_init(msg, &iter))
		return btd_error_invalid_args(msg);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_STRING)
		return btd_error_invalid_args(msg);

	dbus_message_iter_get_basic(&iter, &property);
	dbus_message_iter_next(&iter);

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT)
		return btd_error_invalid_args(msg);
	dbus_message_iter_recurse(&iter, &sub);

	if (g_str_equal("Trusted", property)) {
		dbus_bool_t value;
		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return btd_error_invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &value);

		return set_trust(msg, value, data);
	} else if (g_str_equal("Alias", property)) {
		const char *alias;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_STRING)
			return btd_error_invalid_args(msg);
		dbus_message_iter_get_basic(&sub, &alias);

		return set_alias(msg, alias, data);
	} else if (g_str_equal("Blocked", property)) {
		dbus_bool_t value;

		if (dbus_message_iter_get_arg_type(&sub) != DBUS_TYPE_BOOLEAN)
			return btd_error_invalid_args(msg);

		dbus_message_iter_get_basic(&sub, &value);

		return set_blocked(msg, value, data);
	}

	return btd_error_invalid_args(msg);
}

static void discover_services_req_exit(DBusConnection *conn, void *user_data)
{
	struct browse_req *req = user_data;

	DBG("DiscoverServices requestor exited");

	browse_request_cancel(req);
}

static DBusMessage *discover_services(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct btd_device *device = user_data;
	const char *pattern;
	int err;

	if (device->browse)
		return btd_error_in_progress(msg);

	if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &pattern,
						DBUS_TYPE_INVALID) == FALSE)
		return btd_error_invalid_args(msg);

	if (strlen(pattern) == 0) {
		err = device_browse_sdp(device, msg, NULL, FALSE);
		if (err < 0)
			goto fail;
	} else {
		uuid_t uuid;

		if (bt_string2uuid(&uuid, pattern) < 0)
			return btd_error_invalid_args(msg);

		sdp_uuid128_to_uuid(&uuid);

		err = device_browse_sdp(device, msg, &uuid, FALSE);
		if (err < 0)
			goto fail;
	}

	return NULL;

fail:
	return btd_error_failed(msg, strerror(-err));
}

static const char *browse_request_get_requestor(struct browse_req *req)
{
	if (!req->msg)
		return NULL;

	return dbus_message_get_sender(req->msg);
}

static void iter_append_record(DBusMessageIter *dict, uint32_t handle,
							const char *record)
{
	DBusMessageIter entry;

	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY,
							NULL, &entry);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_UINT32, &handle);

	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &record);

	dbus_message_iter_close_container(dict, &entry);
}

static void discover_services_reply(struct browse_req *req, int err,
							sdp_list_t *recs)
{
	DBusConnection *conn = btd_get_dbus_connection();
	DBusMessage *reply;
	DBusMessageIter iter, dict;
	sdp_list_t *seq;

	if (err) {
		const char *err_if;

		if (err == -EHOSTDOWN)
			err_if = ERROR_INTERFACE ".ConnectionAttemptFailed";
		else
			err_if = ERROR_INTERFACE ".Failed";

		reply = dbus_message_new_error(req->msg, err_if,
							strerror(-err));
		g_dbus_send_message(conn, reply);
		return;
	}

	reply = dbus_message_new_method_return(req->msg);
	if (!reply)
		return;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_UINT32_AS_STRING DBUS_TYPE_STRING_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	for (seq = recs; seq; seq = seq->next) {
		sdp_record_t *rec = (sdp_record_t *) seq->data;
		GString *result;

		if (!rec)
			break;

		result = g_string_new(NULL);

		convert_sdp_record_to_xml(rec, result,
				(void *) g_string_append);

		if (result->len)
			iter_append_record(&dict, rec->handle, result->str);

		g_string_free(result, TRUE);
	}

	dbus_message_iter_close_container(&iter, &dict);

	g_dbus_send_message(conn, reply);
}

static DBusMessage *cancel_discover(DBusConnection *conn,
					DBusMessage *msg, void *user_data)
{
	struct btd_device *device = user_data;
	const char *sender = dbus_message_get_sender(msg);
	const char *requestor;

	if (!device->browse)
		return btd_error_does_not_exist(msg);

	if (!dbus_message_is_method_call(device->browse->msg, DEVICE_INTERFACE,
					"DiscoverServices"))
		return btd_error_not_authorized(msg);

	requestor = browse_request_get_requestor(device->browse);

	/* only the discover requestor can cancel the inquiry process */
	if (!requestor || !g_str_equal(requestor, sender))
		return btd_error_not_authorized(msg);

	discover_services_reply(device->browse, -ECANCELED, NULL);

	browse_request_cancel(device->browse);

	return dbus_message_new_method_return(msg);
}

static void bonding_request_cancel(struct bonding_req *bonding)
{
	struct btd_device *device = bonding->device;
	struct btd_adapter *adapter = device->adapter;

	adapter_cancel_bonding(adapter, &device->bdaddr);
}

static void dev_disconn_profile(gpointer a, gpointer b)
{
	struct btd_profile *profile = a;
	struct btd_device *dev = b;

	if (!profile->disconnect)
		return;

	profile->disconnect(dev, profile, NULL);
}

void device_request_disconnect(struct btd_device *device, DBusMessage *msg)
{
	if (device->bonding)
		bonding_request_cancel(device->bonding);

	if (device->browse) {
		discover_services_reply(device->browse, -ECANCELED, NULL);
		browse_request_cancel(device->browse);
	}

	if (msg)
		device->disconnects = g_slist_append(device->disconnects,
						dbus_message_ref(msg));

	if (device->disconn_timer)
		return;

	if (device->profiles_connected)
		g_slist_foreach(device->profiles, dev_disconn_profile, device);

	g_slist_free(device->pending);
	device->pending = NULL;

	while (device->watches) {
		struct btd_disconnect_data *data = device->watches->data;

		if (data->watch)
			/* temporary is set if device is going to be removed */
			data->watch(device, device->temporary,
							data->user_data);

		/* Check if the watch has been removed by callback function */
		if (!g_slist_find(device->watches, data))
			continue;

		device->watches = g_slist_remove(device->watches, data);
		g_free(data);
	}

	device->disconn_timer = g_timeout_add_seconds(DISCONNECT_TIMER,
						do_disconnect, device);

	g_dbus_emit_signal(btd_get_dbus_connection(),
				device->path,
				DEVICE_INTERFACE, "DisconnectRequested",
				DBUS_TYPE_INVALID);
}

static DBusMessage *disconnect(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct btd_device *device = user_data;

	if (!device->connected)
		return btd_error_not_connected(msg);

	device_request_disconnect(device, msg);

	return NULL;
}

static int connect_next(struct btd_device *dev, btd_profile_cb cb)
{
	struct btd_profile *profile;
	int err = -ENOENT;

	while (dev->pending) {
		int err;

		profile = dev->pending->data;

		err = profile->connect(dev, profile, cb);
		if (err == 0)
			return 0;

		error("Failed to connect %s: %s", profile->name,
							strerror(-err));
		dev->pending = g_slist_remove(dev->pending, profile);
	}

	return err;
}

static void dev_profile_connected(struct btd_profile *profile,
					struct btd_device *dev, int err)
{
	dev->pending = g_slist_remove(dev->pending, profile);

	if (connect_next(dev, dev_profile_connected) == 0)
		return;

	dev->profiles_connected = TRUE;

	if (!dev->connect)
		return;

	g_dbus_send_reply(btd_get_dbus_connection(), dev->connect,
							DBUS_TYPE_INVALID);
	dbus_message_unref(dev->connect);
	dev->connect = NULL;
}

static DBusMessage *dev_connect(DBusConnection *conn, DBusMessage *msg,
							void *user_data)
{
	struct btd_device *dev = user_data;
	struct btd_profile *p;
	GSList *l;
	int err;

	if (dev->profiles_connected)
		return btd_error_already_connected(msg);

	if (dev->pending || dev->connect)
		return btd_error_in_progress(msg);

	for (l = dev->profiles; l != NULL; l = g_slist_next(l)) {
		p = l->data;

		if (p->auto_connect)
			dev->pending = g_slist_append(dev->pending, p);
	}

	if (!dev->pending)
		return btd_error_not_available(msg);

	err = connect_next(dev, dev_profile_connected);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	dev->connect = dbus_message_ref(msg);

	return NULL;
}

static const GDBusMethodTable device_methods[] = {
	{ GDBUS_METHOD("GetProperties",
				NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
				get_properties) },
	{ GDBUS_METHOD("SetProperty",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" }), NULL,
			set_property) },
	{ GDBUS_ASYNC_METHOD("DiscoverServices",
			GDBUS_ARGS({ "pattern", "s" }),
			GDBUS_ARGS({ "services", "a{us}" }),
			discover_services) },
	{ GDBUS_METHOD("CancelDiscovery", NULL, NULL, cancel_discover) },
	{ GDBUS_ASYNC_METHOD("Disconnect", NULL, NULL, disconnect) },
	{ GDBUS_ASYNC_METHOD("Connect", NULL, NULL, dev_connect) },
	{ }
};

static const GDBusSignalTable device_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
			GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ GDBUS_SIGNAL("DisconnectRequested", NULL) },
	{ }
};

gboolean device_is_connected(struct btd_device *device)
{
	return device->connected;
}

void device_add_connection(struct btd_device *device)
{
	if (device->connected) {
		char addr[18];
		ba2str(&device->bdaddr, addr);
		error("Device %s is already connected", addr);
		return;
	}

	device->connected = TRUE;

	emit_property_changed(device->path,
					DEVICE_INTERFACE, "Connected",
					DBUS_TYPE_BOOLEAN, &device->connected);
}

void device_remove_connection(struct btd_device *device)
{
	if (!device->connected) {
		char addr[18];
		ba2str(&device->bdaddr, addr);
		error("Device %s isn't connected", addr);
		return;
	}

	device->connected = FALSE;
	device->profiles_connected = FALSE;

	if (device->disconn_timer > 0) {
		g_source_remove(device->disconn_timer);
		device->disconn_timer = 0;
	}

	while (device->disconnects) {
		DBusMessage *msg = device->disconnects->data;

		g_dbus_send_reply(btd_get_dbus_connection(),
							msg, DBUS_TYPE_INVALID);
		device->disconnects = g_slist_remove(device->disconnects, msg);
		dbus_message_unref(msg);
	}

	if (device_is_paired(device) && !device_is_bonded(device))
		device_set_paired(device, FALSE);

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Connected",
				DBUS_TYPE_BOOLEAN, &device->connected);
}

guint device_add_disconnect_watch(struct btd_device *device,
				disconnect_watch watch, void *user_data,
				GDestroyNotify destroy)
{
	struct btd_disconnect_data *data;
	static guint id = 0;

	data = g_new0(struct btd_disconnect_data, 1);
	data->id = ++id;
	data->watch = watch;
	data->user_data = user_data;
	data->destroy = destroy;

	device->watches = g_slist_append(device->watches, data);

	return data->id;
}

void device_remove_disconnect_watch(struct btd_device *device, guint id)
{
	GSList *l;

	for (l = device->watches; l; l = l->next) {
		struct btd_disconnect_data *data = l->data;

		if (data->id == id) {
			device->watches = g_slist_remove(device->watches,
							data);
			if (data->destroy)
				data->destroy(data->user_data);
			g_free(data);
			return;
		}
	}
}

static void device_set_vendor(struct btd_device *device, uint16_t value)
{
	if (device->vendor == value)
		return;

	device->vendor = value;

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Vendor",
				DBUS_TYPE_UINT16, &value);
}

static void device_set_vendor_src(struct btd_device *device, uint16_t value)
{
	if (device->vendor_src == value)
		return;

	device->vendor_src = value;

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "VendorSource",
				DBUS_TYPE_UINT16, &value);
}

static void device_set_product(struct btd_device *device, uint16_t value)
{
	if (device->product == value)
		return;

	device->product = value;

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Product",
				DBUS_TYPE_UINT16, &value);
}

static void device_set_version(struct btd_device *device, uint16_t value)
{
	if (device->version == value)
		return;

	device->version = value;

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Version",
				DBUS_TYPE_UINT16, &value);
}

struct btd_device *device_create(struct btd_adapter *adapter,
				const gchar *address, uint8_t bdaddr_type)
{
	gchar *address_up;
	struct btd_device *device;
	const gchar *adapter_path = adapter_get_path(adapter);
	bdaddr_t src;
	char srcaddr[18], alias[MAX_NAME_LENGTH + 1];
	uint16_t vendor, product, version;

	device = g_try_malloc0(sizeof(struct btd_device));
	if (device == NULL)
		return NULL;

	address_up = g_ascii_strup(address, -1);
	device->path = g_strdup_printf("%s/dev_%s", adapter_path, address_up);
	g_strdelimit(device->path, ":", '_');
	g_free(address_up);

	DBG("Creating device %s", device->path);

	if (g_dbus_register_interface(btd_get_dbus_connection(),
					device->path, DEVICE_INTERFACE,
					device_methods, device_signals, NULL,
					device, device_free) == FALSE) {
		device_free(device);
		return NULL;
	}

	str2ba(address, &device->bdaddr);
	device->adapter = adapter;
	device->bdaddr_type = bdaddr_type;
	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);

	read_device_name(srcaddr, address, bdaddr_type, device->name);
	if (read_device_alias(srcaddr, address, bdaddr_type, alias,
							sizeof(alias)) == 0)
		device->alias = g_strdup(alias);
	device->trusted = read_trust(&src, address, device->bdaddr_type);

	if (read_blocked(&src, &device->bdaddr, device->bdaddr_type))
		device_block(device, FALSE);

	if (read_link_key(&src, &device->bdaddr, device->bdaddr_type, NULL,
								NULL) == 0) {
		device_set_paired(device, TRUE);
		device_set_bonded(device, TRUE);
	}

	if (device_is_le(device) && has_longtermkeys(&src, &device->bdaddr,
							device->bdaddr_type)) {
		device_set_paired(device, TRUE);
		device_set_bonded(device, TRUE);
	}

	if (read_device_id(srcaddr, address, bdaddr_type, NULL, &vendor,
						&product, &version) == 0) {
		device_set_vendor(device, vendor);
		device_set_product(device, product);
		device_set_version(device, version);
	}

	return btd_device_ref(device);
}

void device_set_name(struct btd_device *device, const char *name)
{
	if (strncmp(name, device->name, MAX_NAME_LENGTH) == 0)
		return;

	strncpy(device->name, name, MAX_NAME_LENGTH);

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Name",
				DBUS_TYPE_STRING, &name);

	if (device->alias != NULL)
		return;

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Alias",
				DBUS_TYPE_STRING, &name);
}

void device_get_name(struct btd_device *device, char *name, size_t len)
{
	strncpy(name, device->name, len);
}

uint16_t btd_device_get_vendor(struct btd_device *device)
{
	return device->vendor;
}

uint16_t btd_device_get_vendor_src(struct btd_device *device)
{
	return device->vendor_src;
}

uint16_t btd_device_get_product(struct btd_device *device)
{
	return device->product;
}

uint16_t btd_device_get_version(struct btd_device *device)
{
	return device->version;
}

static void device_remove_stored(struct btd_device *device)
{
	bdaddr_t src, dst;
	uint8_t dst_type;

	adapter_get_address(device->adapter, &src);
	device_get_address(device, &dst, &dst_type);

	delete_entry(&src, "profiles", &dst, dst_type);
	delete_entry(&src, "trusts", &dst, dst_type);

	if (device_is_bonded(device)) {
		delete_entry(&src, "linkkeys", &dst, dst_type);
		delete_entry(&src, "aliases", &dst, dst_type);
		delete_entry(&src, "longtermkeys", &dst, dst_type);

		device_set_bonded(device, FALSE);
		device->paired = FALSE;
		btd_adapter_remove_bonding(device->adapter, &dst, dst_type);
	}

	delete_all_records(&src, &dst, dst_type);
	delete_device_service(&src, &dst, dst_type);

	if (device->blocked)
		device_unblock(device, TRUE, FALSE);
}

void device_remove(struct btd_device *device, gboolean remove_stored)
{

	DBG("Removing device %s", device->path);

	if (device->agent)
		agent_free(device->agent);

	if (device->bonding) {
		uint8_t status;

		if (device->connected)
			status = MGMT_STATUS_DISCONNECTED;
		else
			status = MGMT_STATUS_CONNECT_FAILED;

		device_cancel_bonding(device, status);
	}

	if (device->browse) {
		discover_services_reply(device->browse, -ECANCELED, NULL);
		browse_request_cancel(device->browse);
	}

	if (device->profiles_connected)
		g_slist_foreach(device->profiles, dev_disconn_profile, device);

	g_slist_free(device->pending);
	device->pending = NULL;

	if (device->connected)
		do_disconnect(device);

	if (remove_stored)
		device_remove_stored(device);

	g_slist_foreach(device->profiles, (GFunc) profile_remove, device);
	g_slist_free(device->profiles);
	device->profiles = NULL;

	attrib_client_unregister(device->services);

	btd_device_unref(device);
}

gint device_address_cmp(struct btd_device *device, const gchar *address)
{
	char addr[18];

	ba2str(&device->bdaddr, addr);
	return strcasecmp(addr, address);
}

gint device_bdaddr_cmp(struct btd_device *device, bdaddr_t *bdaddr)
{
	return bacmp(&device->bdaddr, bdaddr);
}

static gboolean record_has_uuid(const sdp_record_t *rec,
				const char *profile_uuid)
{
	sdp_list_t *pat;

	for (pat = rec->pattern; pat != NULL; pat = pat->next) {
		char *uuid;
		int ret;

		uuid = bt_uuid2string(pat->data);
		if (!uuid)
			continue;

		ret = strcasecmp(uuid, profile_uuid);

		g_free(uuid);

		if (ret == 0)
			return TRUE;
	}

	return FALSE;
}

static GSList *device_match_profile(struct btd_device *device,
					struct btd_profile *profile,
					GSList *uuids)
{
	const char **uuid;
	GSList *match_uuids = NULL;

	for (uuid = profile->remote_uuids; *uuid; uuid++) {
		GSList *match;

		/* skip duplicated uuids */
		if (g_slist_find_custom(match_uuids, *uuid, bt_uuid_strcmp))
			continue;

		/* match profile uuid */
		match = g_slist_find_custom(uuids, *uuid, bt_uuid_strcmp);
		if (match)
			match_uuids = g_slist_append(match_uuids, match->data);
	}

	return match_uuids;
}

struct probe_data {
	struct btd_device *dev;
	GSList *uuids;
	char addr[18];
};

static void dev_probe(struct btd_profile *p, void *user_data)
{
	struct probe_data *d = user_data;
	GSList *probe_uuids;
	int err;

	if (p->device_probe == NULL)
		return;

	probe_uuids = device_match_profile(d->dev, p, d->uuids);
	if (!probe_uuids)
		return;

	err = p->device_probe(p, d->dev, probe_uuids);
	if (err < 0) {
		error("%s profile probe failed for %s", p->name, d->addr);
		g_slist_free(probe_uuids);
		return;
	}

	d->dev->profiles = g_slist_append(d->dev->profiles, p);
	g_slist_free(probe_uuids);
}

void device_probe_profile(gpointer a, gpointer b)
{
	struct btd_device *device = a;
	struct btd_profile *profile = b;
	GSList *probe_uuids;
	char addr[18];
	int err;

	if (profile->device_probe == NULL)
		return;

	probe_uuids = device_match_profile(device, profile, device->uuids);
	if (!probe_uuids)
		return;

	ba2str(&device->bdaddr, addr);

	err = profile->device_probe(profile, device, probe_uuids);
	if (err < 0)
		error("%s profile probe failed for %s", profile->name, addr);
	else
		device->profiles = g_slist_append(device->profiles, profile);

	g_slist_free(probe_uuids);
}

void device_remove_profile(gpointer a, gpointer b)
{
	struct btd_device *device = a;
	struct btd_profile *profile = b;

	if (!g_slist_find(device->profiles, profile))
		return;

	device->profiles = g_slist_remove(device->profiles, profile);

	profile->device_remove(profile, device);
}

void device_probe_profiles(struct btd_device *device, GSList *uuids)
{
	struct probe_data d = { device, uuids };
	GSList *l;

	ba2str(&device->bdaddr, d.addr);

	if (device->blocked) {
		DBG("Skipping profiles for blocked device %s", d.addr);
		goto add_uuids;
	}

	DBG("Probing profiles for device %s", d.addr);

	btd_profile_foreach(dev_probe, &d);

add_uuids:
	for (l = uuids; l != NULL; l = g_slist_next(l)) {
		GSList *match = g_slist_find_custom(device->uuids, l->data,
							bt_uuid_strcmp);
		if (match)
			continue;

		device->uuids = g_slist_insert_sorted(device->uuids,
						g_strdup(l->data),
						bt_uuid_strcmp);
	}
}

static void device_remove_profiles(struct btd_device *device, GSList *uuids)
{
	struct btd_adapter *adapter = device_get_adapter(device);
	char srcaddr[18], dstaddr[18];
	bdaddr_t src;
	sdp_list_t *records;
	GSList *l;

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	records = read_records(&src, &device->bdaddr);

	DBG("Removing profiles for %s", dstaddr);

	for (l = uuids; l != NULL; l = g_slist_next(l)) {
		sdp_record_t *rec;

		device->uuids = g_slist_remove(device->uuids, l->data);

		rec = find_record_in_list(records, l->data);
		if (!rec)
			continue;

		delete_record(srcaddr, dstaddr, device->bdaddr_type,
							rec->handle);

		records = sdp_list_remove(records, rec);
		sdp_record_free(rec);
	}

	if (records)
		sdp_list_free(records, (sdp_free_func_t) sdp_record_free);

	for (l = device->profiles; l != NULL; l = g_slist_next(l)) {
		struct btd_profile *profile = l->data;
		GSList *probe_uuids;

		probe_uuids = device_match_profile(device, profile,
								device->uuids);
		if (probe_uuids != NULL) {
			g_slist_free(probe_uuids);
			continue;
		}

		profile->device_remove(profile, device);
		device->profiles = g_slist_remove(device->profiles, profile);
	}
}

static void uuids_changed(struct btd_device *device)
{
	char **uuids;
	GSList *l;
	int i;

	uuids = g_new0(char *, g_slist_length(device->uuids) + 1);
	for (i = 0, l = device->uuids; l; l = l->next, i++)
		uuids[i] = l->data;

	emit_array_property_changed(device->path,
					DEVICE_INTERFACE, "UUIDs",
					DBUS_TYPE_STRING, &uuids, i);

	g_free(uuids);
}

static int rec_cmp(const void *a, const void *b)
{
	const sdp_record_t *r1 = a;
	const sdp_record_t *r2 = b;

	return r1->handle - r2->handle;
}

static void update_bredr_services(struct browse_req *req, sdp_list_t *recs)
{
	struct btd_device *device = req->device;
	struct btd_adapter *adapter = device_get_adapter(device);
	sdp_list_t *seq;
	char srcaddr[18], dstaddr[18];
	uint8_t dst_type;
	bdaddr_t src;

	adapter_get_address(adapter, &src);
	ba2str(&src, srcaddr);
	ba2str(&device->bdaddr, dstaddr);

	for (seq = recs; seq; seq = seq->next) {
		sdp_record_t *rec = (sdp_record_t *) seq->data;
		sdp_list_t *svcclass = NULL;
		gchar *profile_uuid;
		GSList *l;

		if (!rec)
			break;

		if (sdp_get_service_classes(rec, &svcclass) < 0)
			continue;

		/* Check for empty service classes list */
		if (svcclass == NULL) {
			DBG("Skipping record with no service classes");
			continue;
		}

		/* Extract the first element and skip the remainning */
		profile_uuid = bt_uuid2string(svcclass->data);
		if (!profile_uuid) {
			sdp_list_free(svcclass, free);
			continue;
		}

		if (bt_uuid_strcmp(profile_uuid, PNP_UUID) == 0) {
			uint16_t source, vendor, product, version;
			sdp_data_t *pdlist;

			pdlist = sdp_data_get(rec, SDP_ATTR_VENDOR_ID_SOURCE);
			source = pdlist ? pdlist->val.uint16 : 0x0000;

			pdlist = sdp_data_get(rec, SDP_ATTR_VENDOR_ID);
			vendor = pdlist ? pdlist->val.uint16 : 0x0000;

			device_set_vendor(device, vendor);

			pdlist = sdp_data_get(rec, SDP_ATTR_PRODUCT_ID);
			product = pdlist ? pdlist->val.uint16 : 0x0000;

			device_set_product(device, product);

			pdlist = sdp_data_get(rec, SDP_ATTR_VERSION);
			version = pdlist ? pdlist->val.uint16 : 0x0000;

			device_set_version(device, version);

			if (source || vendor || product || version)
				store_device_id(srcaddr, dstaddr,
						device->bdaddr_type, source,
						vendor, product, version);
		}

		/* Check for duplicates */
		if (sdp_list_find(req->records, rec, rec_cmp)) {
			g_free(profile_uuid);
			sdp_list_free(svcclass, free);
			continue;
		}

		dst_type = device_get_addr_type(device);

		store_record(srcaddr, dstaddr, dst_type, rec);

		/* Copy record */
		req->records = sdp_list_append(req->records,
							sdp_copy_record(rec));

		l = g_slist_find_custom(device->uuids, profile_uuid,
							(GCompareFunc) strcmp);
		if (!l)
			req->profiles_added =
					g_slist_append(req->profiles_added,
							profile_uuid);
		else {
			req->profiles_removed =
					g_slist_remove(req->profiles_removed,
							l->data);
			g_free(profile_uuid);
		}

		sdp_list_free(svcclass, free);
	}
}

static gint primary_cmp(gconstpointer a, gconstpointer b)
{
	return memcmp(a, b, sizeof(struct gatt_primary));
}

static void update_gatt_services(struct browse_req *req, GSList *current,
								GSList *found)
{
	GSList *l, *lmatch, *left = g_slist_copy(current);

	/* Added Profiles */
	for (l = found; l; l = g_slist_next(l)) {
		struct gatt_primary *prim = l->data;

		/* Entry found ? */
		lmatch = g_slist_find_custom(current, prim, primary_cmp);
		if (lmatch) {
			left = g_slist_remove(left, lmatch->data);
			continue;
		}

		/* New entry */
		req->profiles_added = g_slist_append(req->profiles_added,
							g_strdup(prim->uuid));

		DBG("UUID Added: %s", prim->uuid);
	}

	/* Removed Profiles */
	for (l = left; l; l = g_slist_next(l)) {
		struct gatt_primary *prim = l->data;
		req->profiles_removed = g_slist_append(req->profiles_removed,
							g_strdup(prim->uuid));

		DBG("UUID Removed: %s", prim->uuid);
	}

	g_slist_free(left);
}

static void store_profiles(struct btd_device *device)
{
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t src;
	char *str;

	adapter_get_address(adapter, &src);

	if (!device->uuids) {
		write_device_profiles(&src, &device->bdaddr,
				      device->bdaddr_type, "");
		return;
	}

	str = bt_list2string(device->uuids);
	write_device_profiles(&src, &device->bdaddr, device->bdaddr_type, str);
	g_free(str);
}

static void create_device_reply(struct btd_device *device, struct browse_req *req)
{
	DBusMessage *reply;

	reply = dbus_message_new_method_return(req->msg);
	if (!reply)
		return;

	dbus_message_append_args(reply, DBUS_TYPE_OBJECT_PATH, &device->path,
					DBUS_TYPE_INVALID);

	g_dbus_send_message(btd_get_dbus_connection(), reply);
}

GSList *device_services_from_record(struct btd_device *device, GSList *profiles)
{
	GSList *l, *prim_list = NULL;
	char *att_uuid;
	uuid_t proto_uuid;

	sdp_uuid16_create(&proto_uuid, ATT_UUID);
	att_uuid = bt_uuid2string(&proto_uuid);

	for (l = profiles; l; l = l->next) {
		const char *profile_uuid = l->data;
		const sdp_record_t *rec;
		struct gatt_primary *prim;
		uint16_t start = 0, end = 0, psm = 0;
		uuid_t prim_uuid;

		rec = btd_device_get_record(device, profile_uuid);
		if (!rec)
			continue;

		if (!record_has_uuid(rec, att_uuid))
			continue;

		if (!gatt_parse_record(rec, &prim_uuid, &psm, &start, &end))
			continue;

		prim = g_new0(struct gatt_primary, 1);
		prim->range.start = start;
		prim->range.end = end;
		sdp_uuid2strn(&prim_uuid, prim->uuid, sizeof(prim->uuid));

		prim_list = g_slist_append(prim_list, prim);
	}

	g_free(att_uuid);

	return prim_list;
}

static void search_cb(sdp_list_t *recs, int err, gpointer user_data)
{
	struct browse_req *req = user_data;
	struct btd_device *device = req->device;
	char addr[18];

	ba2str(&device->bdaddr, addr);

	if (err < 0) {
		error("%s: error updating services: %s (%d)",
				addr, strerror(-err), -err);
		goto send_reply;
	}

	update_bredr_services(req, recs);

	if (device->tmp_records)
		sdp_list_free(device->tmp_records,
					(sdp_free_func_t) sdp_record_free);

	device->tmp_records = req->records;
	req->records = NULL;

	if (!req->profiles_added && !req->profiles_removed) {
		DBG("%s: No service update", addr);
		goto send_reply;
	}

	/* Probe matching profiles for services added */
	if (req->profiles_added) {
		GSList *list;

		list = device_services_from_record(device, req->profiles_added);
		if (list)
			device_register_services(device, list, ATT_PSM);

		device_probe_profiles(device, req->profiles_added);
	}

	/* Remove profiles for services removed */
	if (req->profiles_removed)
		device_remove_profiles(device, req->profiles_removed);

	/* Propagate services changes */
	uuids_changed(req->device);

send_reply:
	if (!req->msg)
		goto cleanup;

	if (dbus_message_is_method_call(req->msg, DEVICE_INTERFACE,
					"DiscoverServices"))
		discover_services_reply(req, err, device->tmp_records);
	else if (dbus_message_is_method_call(req->msg, ADAPTER_INTERFACE,
						"CreatePairedDevice"))
		create_device_reply(device, req);
	else if (dbus_message_is_method_call(req->msg, ADAPTER_INTERFACE,
						"CreateDevice")) {
		if (err < 0) {
			DBusMessage *reply;
			reply = btd_error_failed(req->msg, strerror(-err));
			g_dbus_send_message(btd_get_dbus_connection(), reply);
			goto cleanup;
		}

		create_device_reply(device, req);
		device_set_temporary(device, FALSE);
	}

cleanup:
	if (!device->temporary)
		store_profiles(device);

	device->browse = NULL;
	browse_request_free(req);
}

static void browse_cb(sdp_list_t *recs, int err, gpointer user_data)
{
	struct browse_req *req = user_data;
	struct btd_device *device = req->device;
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t src;
	uuid_t uuid;

	/* If we have a valid response and req->search_uuid == 2, then L2CAP
	 * UUID & PNP searching was successful -- we are done */
	if (err < 0 || (req->search_uuid == 2 && req->records)) {
		if (err == -ECONNRESET && req->reconnect_attempt < 1) {
			req->search_uuid--;
			req->reconnect_attempt++;
		} else
			goto done;
	}

	update_bredr_services(req, recs);

	adapter_get_address(adapter, &src);

	/* Search for mandatory uuids */
	if (uuid_list[req->search_uuid]) {
		sdp_uuid16_create(&uuid, uuid_list[req->search_uuid++]);
		bt_search_service(&src, &device->bdaddr, &uuid,
						browse_cb, user_data, NULL);
		return;
	}

done:
	search_cb(recs, err, user_data);
}

static void init_browse(struct browse_req *req, gboolean reverse)
{
	GSList *l;

	/* If we are doing reverse-SDP don't try to detect removed profiles
	 * since some devices hide their service records while they are
	 * connected
	 */
	if (reverse)
		return;

	for (l = req->device->uuids; l; l = l->next)
		req->profiles_removed = g_slist_append(req->profiles_removed,
						l->data);
}

static char *primary_list_to_string(GSList *primary_list)
{
	GString *services;
	GSList *l;

	services = g_string_new(NULL);

	for (l = primary_list; l; l = l->next) {
		struct gatt_primary *primary = l->data;
		char service[64];

		memset(service, 0, sizeof(service));

		snprintf(service, sizeof(service), "%04X#%04X#%s ",
				primary->range.start, primary->range.end, primary->uuid);

		services = g_string_append(services, service);
	}

	return g_string_free(services, FALSE);
}

static void store_services(struct btd_device *device)
{
	struct btd_adapter *adapter = device->adapter;
	bdaddr_t dba, sba;
	char *str = primary_list_to_string(device->primaries);

	adapter_get_address(adapter, &sba);
	device_get_address(device, &dba, NULL);

	write_device_services(&sba, &dba, device->bdaddr_type, str);

	g_free(str);
}

static void attio_connected(gpointer data, gpointer user_data)
{
	struct attio_data *attio = data;
	GAttrib *attrib = user_data;

	if (attio->cfunc)
		attio->cfunc(attrib, attio->user_data);
}

static void attio_disconnected(gpointer data, gpointer user_data)
{
	struct attio_data *attio = data;

	if (attio->dcfunc)
		attio->dcfunc(attio->user_data);
}

static gboolean attrib_disconnected_cb(GIOChannel *io, GIOCondition cond,
							gpointer user_data)
{
	struct btd_device *device = user_data;
	int sock, err = 0;
	socklen_t len;

	if (device->browse)
		goto done;

	sock = g_io_channel_unix_get_fd(io);
	len = sizeof(err);
	getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len);

	g_slist_foreach(device->attios, attio_disconnected, NULL);

	if (device->auto_connect == FALSE) {
		DBG("Automatic connection disabled");
		goto done;
	}

	/*
	 * Keep scanning/re-connection active if disconnection reason
	 * is connection timeout, remote user terminated connection or local
	 * initiated disconnection.
	 */
	if (err == ETIMEDOUT || err == ECONNRESET || err == ECONNABORTED)
		adapter_connect_list_add(device_get_adapter(device), device);

done:
	attio_cleanup(device);

	return FALSE;
}

static void device_unregister_services(struct btd_device *device)
{
	attrib_client_unregister(device->services);
	g_slist_free_full(device->services, g_free);
	device->services = NULL;
}

static void register_all_services(struct browse_req *req, GSList *services)
{
	struct btd_device *device = req->device;

	device_set_temporary(device, FALSE);

	if (device->services)
		device_unregister_services(device);

	update_gatt_services(req, device->primaries, services);
	g_slist_free_full(device->primaries, g_free);
	device->primaries = NULL;

	device_register_services(device, g_slist_copy(services), -1);
	if (req->profiles_removed)
		device_remove_profiles(device, req->profiles_removed);

	device_probe_profiles(device, req->profiles_added);

	if (device->attios == NULL && device->attios_offline == NULL)
		attio_cleanup(device);

	uuids_changed(device);
	if (req->msg)
		create_device_reply(device, req);

	store_services(device);

	device->browse = NULL;
	browse_request_free(req);
}

static int service_by_range_cmp(gconstpointer a, gconstpointer b)
{
	const struct gatt_primary *prim = a;
	const struct att_range *range = b;

	return memcmp(&prim->range, range, sizeof(*range));
}

static void find_included_cb(GSList *includes, uint8_t status,
						gpointer user_data)
{
	struct included_search *search = user_data;
	struct btd_device *device = search->req->device;
	struct gatt_primary *prim;
	GSList *l;

	if (includes == NULL)
		goto done;

	for (l = includes; l; l = l->next) {
		struct gatt_included *incl = l->data;

		if (g_slist_find_custom(search->services, &incl->range,
						service_by_range_cmp))
			continue;

		prim = g_new0(struct gatt_primary, 1);
		memcpy(prim->uuid, incl->uuid, sizeof(prim->uuid));
		memcpy(&prim->range, &incl->range, sizeof(prim->range));

		search->services = g_slist_append(search->services, prim);
	}

done:
	search->current = search->current->next;
	if (search->current == NULL) {
		register_all_services(search->req, search->services);
		g_slist_free(search->services);
		g_free(search);
		return;
	}

	prim = search->current->data;
	gatt_find_included(device->attrib, prim->range.start, prim->range.end,
					find_included_cb, search);
}

static void find_included_services(struct browse_req *req, GSList *services)
{
	struct btd_device *device = req->device;
	struct included_search *search;
	struct gatt_primary *prim;

	if (services == NULL)
		return;

	search = g_new0(struct included_search, 1);
	search->req = req;
	search->services = g_slist_copy(services);
	search->current = search->services;

	prim = search->current->data;
	gatt_find_included(device->attrib, prim->range.start, prim->range.end,
					find_included_cb, search);

}

static void primary_cb(GSList *services, guint8 status, gpointer user_data)
{
	struct browse_req *req = user_data;

	if (status) {
		struct btd_device *device = req->device;

		if (req->msg) {
			DBusMessage *reply;
			reply = btd_error_failed(req->msg,
							att_ecode2str(status));
			g_dbus_send_message(btd_get_dbus_connection(), reply);
		}

		device->browse = NULL;
		browse_request_free(req);
		return;
	}

	find_included_services(req, services);
}

static void bonding_request_free(struct bonding_req *bonding)
{
	struct btd_device *device;

	if (!bonding)
		return;

	if (bonding->listener_id)
		g_dbus_remove_watch(btd_get_dbus_connection(),
							bonding->listener_id);

	if (bonding->msg)
		dbus_message_unref(bonding->msg);

	device = bonding->device;
	g_free(bonding);

	if (!device)
		return;

	device->bonding = NULL;

	if (!device->agent)
		return;

	agent_cancel(device->agent);
	agent_free(device->agent);
	device->agent = NULL;
}

static void att_connect_cb(GIOChannel *io, GError *gerr, gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;
	GAttrib *attrib;
	int err;

	g_io_channel_unref(device->att_io);
	device->att_io = NULL;

	if (gerr) {
		DBG("%s", gerr->message);

		if (attcb->error)
			attcb->error(gerr, user_data);

		g_free(attcb);
		return;
	}

	attrib = g_attrib_new(io);
	device->attachid = attrib_channel_attach(attrib);
	if (device->attachid == 0)
		error("Attribute server attach failure!");

	device->attrib = attrib;
	device->cleanup_id = g_io_add_watch(io, G_IO_HUP,
					attrib_disconnected_cb, device);

	if (attcb->success)
		attcb->success(user_data);
	else
		g_free(attcb);

	if (!device->bonding)
		return;

	/* this is a LE device during pairing */
	err = adapter_create_bonding(device->adapter,
				&device->bdaddr, device->bdaddr_type,
				agent_get_io_capability(device->agent));
	if (err < 0) {
		DBusMessage *reply = btd_error_failed(device->bonding->msg,
							strerror(-err));
		g_dbus_send_message(btd_get_dbus_connection(), reply);
		bonding_request_cancel(device->bonding);
		bonding_request_free(device->bonding);
	}
}

static void att_error_cb(const GError *gerr, gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;

	if (g_error_matches(gerr, BT_IO_ERROR, ECONNABORTED))
		return;

	if (device->auto_connect == FALSE)
		return;

	adapter_connect_list_add(device_get_adapter(device), device);
	DBG("Enabling automatic connections");
}

static void exchange_mtu_cb(guint8 status, const guint8 *pdu, guint16 plen,
							gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;
	uint16_t rmtu;

	if (status) {
		error("MTU exchange: %s", att_ecode2str(status));
		goto done;
	}

	if (!dec_mtu_resp(pdu, plen, &rmtu)) {
		error("MTU exchange: protocol error");
		goto done;
	}

	device->att_mtu = MIN(rmtu, device->att_mtu);

	if (g_attrib_set_mtu(device->attrib, device->att_mtu))
		DBG("MTU exchange succeeded: %d", device->att_mtu);
	else
		DBG("MTU exchange failed");

done:
	g_free(attcb);
}

static void exchange_mtu(gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;
	GIOChannel *io;
	uint16_t cid, imtu;
	int err;

	/* Starting ATT MTU Exchange */
	io = g_attrib_get_channel(device->attrib);
	err = bt_io_get(io, NULL, BT_IO_OPT_IMTU, &imtu,
				BT_IO_OPT_CID, &cid, BT_IO_OPT_INVALID);
	if (err == FALSE)
		return;

	gatt_exchange_mtu(device->attrib, imtu, exchange_mtu_cb, attcb);
	device->att_mtu = imtu;
	DBG("MTU Exchange: Requesting %d", imtu);

	g_slist_foreach(device->attios, attio_connected, device->attrib);
}

GIOChannel *device_att_connect(gpointer user_data)
{
	struct btd_device *device = user_data;
	struct btd_adapter *adapter = device->adapter;
	struct att_callbacks *attcb;
	GIOChannel *io;
	GError *gerr = NULL;
	char addr[18];
	bdaddr_t sba;

	adapter_get_address(adapter, &sba);
	ba2str(&device->bdaddr, addr);

	DBG("Connection attempt to: %s", addr);

	attcb = g_new0(struct att_callbacks, 1);
	attcb->error = att_error_cb;
	attcb->success = exchange_mtu;
	attcb->user_data = device;

	if (device_is_bredr(device)) {
		io = bt_io_connect(att_connect_cb,
					attcb, NULL, &gerr,
					BT_IO_OPT_SOURCE_BDADDR, &sba,
					BT_IO_OPT_DEST_BDADDR, &device->bdaddr,
					BT_IO_OPT_PSM, ATT_PSM,
					BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
					BT_IO_OPT_INVALID);
	} else if (device->bonding) {
		/* this is a LE device during pairing, using low sec level */
		io = bt_io_connect(att_connect_cb,
				attcb, NULL, &gerr,
				BT_IO_OPT_SOURCE_BDADDR, &sba,
				BT_IO_OPT_DEST_BDADDR, &device->bdaddr,
				BT_IO_OPT_DEST_TYPE, device->bdaddr_type,
				BT_IO_OPT_CID, ATT_CID,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_LOW,
				BT_IO_OPT_INVALID);
		if (io == NULL) {
			DBusMessage *reply = btd_error_failed(
					device->bonding->msg, gerr->message);
			g_dbus_send_message(btd_get_dbus_connection(), reply);
			bonding_request_cancel(device->bonding);
			bonding_request_free(device->bonding);
		}
	} else {
		io = bt_io_connect(att_connect_cb,
				attcb, NULL, &gerr,
				BT_IO_OPT_SOURCE_BDADDR, &sba,
				BT_IO_OPT_DEST_BDADDR, &device->bdaddr,
				BT_IO_OPT_DEST_TYPE, device->bdaddr_type,
				BT_IO_OPT_CID, ATT_CID,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
				BT_IO_OPT_INVALID);
	}

	if (io == NULL) {
		error("ATT bt_io_connect(%s): %s", addr, gerr->message);
		g_error_free(gerr);
		g_free(attcb);
		return NULL;
	}

	device->att_io = io;

	return g_io_channel_ref(io);
}

static void att_browse_error_cb(const GError *gerr, gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;
	struct browse_req *req = device->browse;

	if (req->msg) {
		DBusMessage *reply;

		reply = btd_error_failed(req->msg, gerr->message);
		g_dbus_send_message(btd_get_dbus_connection(), reply);
	}

	device->browse = NULL;
	browse_request_free(req);
}

static void att_browse_cb(gpointer user_data)
{
	struct att_callbacks *attcb = user_data;
	struct btd_device *device = attcb->user_data;
	uint8_t *opdu;
	uint16_t olen;
	size_t plen;

	opdu = g_attrib_get_buffer(device->attrib, &plen);
	olen = enc_confirmation(opdu, plen);

	g_attrib_send(device->attrib, 0, opdu, olen, NULL, NULL, NULL);

	gatt_discover_primary(device->attrib, NULL, primary_cb,
							device->browse);
}

int device_browse_primary(struct btd_device *device, DBusMessage *msg,
								gboolean secure)
{
	struct btd_adapter *adapter = device->adapter;
	struct att_callbacks *attcb;
	struct browse_req *req;
	BtIOSecLevel sec_level;
	bdaddr_t src;

	if (device->browse)
		return -EBUSY;

	req = g_new0(struct browse_req, 1);
	req->device = btd_device_ref(device);
	adapter_get_address(adapter, &src);

	device->browse = req;

	if (device->attrib) {
		uint8_t *opdu;
		uint16_t olen;
		size_t plen;

		opdu = g_attrib_get_buffer(device->attrib, &plen);
		olen = enc_confirmation(opdu, plen);
		g_attrib_send(device->attrib, 0, opdu, olen,
						NULL, NULL, NULL);

		gatt_discover_primary(device->attrib, NULL, primary_cb, req);
		goto done;
	}

	sec_level = secure ? BT_IO_SEC_HIGH : BT_IO_SEC_LOW;

	attcb = g_new0(struct att_callbacks, 1);
	attcb->error = att_browse_error_cb;
	attcb->success = att_browse_cb;
	attcb->user_data = device;

	device->att_io = bt_io_connect(att_connect_cb,
				attcb, NULL, NULL,
				BT_IO_OPT_SOURCE_BDADDR, &src,
				BT_IO_OPT_DEST_BDADDR, &device->bdaddr,
				BT_IO_OPT_DEST_TYPE, device->bdaddr_type,
				BT_IO_OPT_CID, ATT_CID,
				BT_IO_OPT_SEC_LEVEL, sec_level,
				BT_IO_OPT_INVALID);

	if (device->att_io == NULL) {
		device->browse = NULL;
		browse_request_free(req);
		g_free(attcb);
		return -EIO;
	}

done:

	if (msg) {
		const char *sender = dbus_message_get_sender(msg);

		req->msg = dbus_message_ref(msg);
		/* Track the request owner to cancel it
		 * automatically if the owner exits */
		req->listener_id = g_dbus_add_disconnect_watch(
						btd_get_dbus_connection(),
						sender,
						discover_services_req_exit,
						req, NULL);
	}

	return 0;
}

int device_browse_sdp(struct btd_device *device, DBusMessage *msg,
					uuid_t *search, gboolean reverse)
{
	struct btd_adapter *adapter = device->adapter;
	struct browse_req *req;
	bt_callback_t cb;
	bdaddr_t src;
	uuid_t uuid;
	int err;

	if (device->browse)
		return -EBUSY;

	adapter_get_address(adapter, &src);

	req = g_new0(struct browse_req, 1);
	req->device = btd_device_ref(device);
	if (search) {
		memcpy(&uuid, search, sizeof(uuid_t));
		cb = search_cb;
	} else {
		sdp_uuid16_create(&uuid, uuid_list[req->search_uuid++]);
		init_browse(req, reverse);
		cb = browse_cb;
	}

	err = bt_search_service(&src, &device->bdaddr, &uuid, cb, req, NULL);
	if (err < 0) {
		browse_request_free(req);
		return err;
	}

	device->browse = req;

	if (msg) {
		const char *sender = dbus_message_get_sender(msg);

		req->msg = dbus_message_ref(msg);
		/* Track the request owner to cancel it
		 * automatically if the owner exits */
		req->listener_id = g_dbus_add_disconnect_watch(
						btd_get_dbus_connection(),
						sender,
						discover_services_req_exit,
						req, NULL);
	}

	return err;
}

struct btd_adapter *device_get_adapter(struct btd_device *device)
{
	if (!device)
		return NULL;

	return device->adapter;
}

void device_get_address(struct btd_device *device, bdaddr_t *bdaddr,
							uint8_t *bdaddr_type)
{
	bacpy(bdaddr, &device->bdaddr);
	if (bdaddr_type != NULL)
		*bdaddr_type = device->bdaddr_type;
}

void device_set_addr_type(struct btd_device *device, uint8_t bdaddr_type)
{
	if (device == NULL)
		return;

	device->bdaddr_type = bdaddr_type;
}

uint8_t device_get_addr_type(struct btd_device *device)
{
	return device->bdaddr_type;
}

const gchar *device_get_path(struct btd_device *device)
{
	if (!device)
		return NULL;

	return device->path;
}

struct agent *device_get_agent(struct btd_device *device)
{
	if (!device)
		return NULL;

	if (device->agent)
		return device->agent;

	return adapter_get_agent(device->adapter);
}

gboolean device_is_busy(struct btd_device *device)
{
	return device->browse ? TRUE : FALSE;
}

gboolean device_is_temporary(struct btd_device *device)
{
	return device->temporary;
}

void device_set_temporary(struct btd_device *device, gboolean temporary)
{
	if (!device)
		return;

	DBG("temporary %d", temporary);

	if (temporary)
		adapter_connect_list_remove(device_get_adapter(device), device);

	device->temporary = temporary;
}

void device_set_bonded(struct btd_device *device, gboolean bonded)
{
	if (!device)
		return;

	DBG("bonded %d", bonded);

	device->bonded = bonded;
}

void device_set_auto_connect(struct btd_device *device, gboolean enable)
{
	struct btd_adapter *adapter = device_get_adapter(device);
	char addr[18];

	if (!device)
		return;

	ba2str(&device->bdaddr, addr);

	DBG("%s auto connect: %d", addr, enable);

	device->auto_connect = enable;

	/* Disabling auto connect */
	if (enable == FALSE) {
		adapter_connect_list_remove(adapter, device);
		return;
	}

	if (device->attrib) {
		DBG("Already connected");
		return;
	}

	if (device->attios == NULL && device->attios_offline == NULL)
		return;

	/* Enabling auto connect */
	adapter_connect_list_add(adapter, device);
}

static gboolean start_discovery(gpointer user_data)
{
	struct btd_device *device = user_data;

	if (device_is_bredr(device))
		device_browse_sdp(device, NULL, NULL, TRUE);
	else
		device_browse_primary(device, NULL, FALSE);

	device->discov_timer = 0;

	return FALSE;
}

static DBusMessage *new_authentication_return(DBusMessage *msg, uint8_t status)
{
	switch (status) {
	case MGMT_STATUS_SUCCESS:
		return dbus_message_new_method_return(msg);

	case MGMT_STATUS_CONNECT_FAILED:
		return dbus_message_new_error(msg,
				ERROR_INTERFACE ".ConnectionAttemptFailed",
				"Page Timeout");
	case MGMT_STATUS_TIMEOUT:
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationTimeout",
					"Authentication Timeout");
	case MGMT_STATUS_BUSY:
	case MGMT_STATUS_REJECTED:
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationRejected",
					"Authentication Rejected");
	case MGMT_STATUS_CANCELLED:
	case MGMT_STATUS_NO_RESOURCES:
	case MGMT_STATUS_DISCONNECTED:
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationCanceled",
					"Authentication Canceled");
	default:
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationFailed",
					"Authentication Failed");
	}
}

void device_set_paired(struct btd_device *device, gboolean value)
{
	if (device->paired == value)
		return;

	if (!value)
		btd_adapter_remove_bonding(device->adapter, &device->bdaddr,
							device->bdaddr_type);

	device->paired = value;

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Paired",
				DBUS_TYPE_BOOLEAN, &value);
}

static void device_agent_removed(struct agent *agent, void *user_data)
{
	struct btd_device *device = user_data;

	device->agent = NULL;

	if (device->authr)
		device->authr->agent = NULL;
}

static struct bonding_req *bonding_request_new(DBusMessage *msg,
						struct btd_device *device,
						const char *agent_path,
						uint8_t capability)
{
	struct bonding_req *bonding;
	const char *name = dbus_message_get_sender(msg);
	char addr[18];

	ba2str(&device->bdaddr, addr);
	DBG("Requesting bonding for %s", addr);

	if (!agent_path)
		goto proceed;

	device->agent = agent_create(device->adapter, name, agent_path,
					capability,
					device_agent_removed,
					device);

	DBG("Temporary agent registered for %s at %s:%s",
			addr, name, agent_path);

proceed:
	bonding = g_new0(struct bonding_req, 1);

	bonding->msg = dbus_message_ref(msg);

	return bonding;
}

static void create_bond_req_exit(DBusConnection *conn, void *user_data)
{
	struct btd_device *device = user_data;
	char addr[18];

	ba2str(&device->bdaddr, addr);
	DBG("%s: requestor exited before bonding was completed", addr);

	if (device->authr)
		device_cancel_authentication(device, FALSE);

	if (device->bonding) {
		device->bonding->listener_id = 0;
		device_request_disconnect(device, NULL);
	}
}

DBusMessage *device_create_bonding(struct btd_device *device,
					DBusMessage *msg,
					const char *agent_path,
					uint8_t capability)
{
	struct btd_adapter *adapter = device->adapter;
	struct bonding_req *bonding;
	int err;

	if (device->bonding)
		return btd_error_in_progress(msg);

	if (device_is_bonded(device))
		return btd_error_already_exists(msg);

	bonding = bonding_request_new(msg, device, agent_path,
					capability);

	bonding->listener_id = g_dbus_add_disconnect_watch(
						btd_get_dbus_connection(),
						dbus_message_get_sender(msg),
						create_bond_req_exit, device,
						NULL);

	device->bonding = bonding;
	bonding->device = device;

	if (device_is_le(device)) {
		adapter_connect_list_add(adapter, device);
		return NULL;
	}

	err = adapter_create_bonding(adapter, &device->bdaddr,
					device->bdaddr_type, capability);
	if (err < 0)
		return btd_error_failed(msg, strerror(-err));

	return NULL;
}

void device_simple_pairing_complete(struct btd_device *device, uint8_t status)
{
	struct authentication_req *auth = device->authr;

	if (auth && (auth->type == AUTH_TYPE_NOTIFY_PASSKEY
		     || auth->type == AUTH_TYPE_NOTIFY_PINCODE) && auth->agent)
		agent_cancel(auth->agent);
}

static void device_auth_req_free(struct btd_device *device)
{
	if (device->authr)
		g_free(device->authr->pincode);
	g_free(device->authr);
	device->authr = NULL;
}

void device_bonding_complete(struct btd_device *device, uint8_t status)
{
	struct bonding_req *bonding = device->bonding;
	struct authentication_req *auth = device->authr;

	DBG("bonding %p status 0x%02x", bonding, status);

	if (auth && (auth->type == AUTH_TYPE_NOTIFY_PASSKEY
		     || auth->type == AUTH_TYPE_NOTIFY_PINCODE) && auth->agent)
		agent_cancel(auth->agent);

	if (status) {
		device_cancel_authentication(device, TRUE);
		device_cancel_bonding(device, status);
		return;
	}

	device_auth_req_free(device);

	/* If we're already paired nothing more is needed */
	if (device->paired)
		return;

	device_set_paired(device, TRUE);

	/* If we were initiators start service discovery immediately.
	 * However if the other end was the initator wait a few seconds
	 * before SDP. This is due to potential IOP issues if the other
	 * end starts doing SDP at the same time as us */
	if (bonding) {
		DBG("Proceeding with service discovery");
		/* If we are initiators remove any discovery timer and just
		 * start discovering services directly */
		if (device->discov_timer) {
			g_source_remove(device->discov_timer);
			device->discov_timer = 0;
		}

		if (device_is_bredr(device))
			device_browse_sdp(device, bonding->msg, NULL, FALSE);
		else
			device_browse_primary(device, bonding->msg, FALSE);

		bonding_request_free(bonding);
	} else {
		if (!device->browse && !device->discov_timer &&
				main_opts.reverse_sdp) {
			/* If we are not initiators and there is no currently
			 * active discovery or discovery timer, set discovery
			 * timer */
			DBG("setting timer for reverse service discovery");
			device->discov_timer = g_timeout_add_seconds(
							DISCOVERY_TIMER,
							start_discovery,
							device);
		}
	}
}

gboolean device_is_creating(struct btd_device *device, const char *sender)
{
	DBusMessage *msg;

	if (device->bonding && device->bonding->msg)
		msg = device->bonding->msg;
	else if (device->browse && device->browse->msg)
		msg = device->browse->msg;
	else
		return FALSE;

	if (!dbus_message_is_method_call(msg, ADAPTER_INTERFACE,
						"CreatePairedDevice") &&
			!dbus_message_is_method_call(msg, ADAPTER_INTERFACE,
							"CreateDevice"))
		return FALSE;

	if (sender == NULL)
		return TRUE;

	return g_str_equal(sender, dbus_message_get_sender(msg));
}

gboolean device_is_bonding(struct btd_device *device, const char *sender)
{
	struct bonding_req *bonding = device->bonding;

	if (!device->bonding)
		return FALSE;

	if (!sender)
		return TRUE;

	return g_str_equal(sender, dbus_message_get_sender(bonding->msg));
}

void device_cancel_bonding(struct btd_device *device, uint8_t status)
{
	struct bonding_req *bonding = device->bonding;
	DBusMessage *reply;
	char addr[18];

	if (!bonding)
		return;

	ba2str(&device->bdaddr, addr);
	DBG("Canceling bonding request for %s", addr);

	if (device->authr)
		device_cancel_authentication(device, FALSE);

	reply = new_authentication_return(bonding->msg, status);
	g_dbus_send_message(btd_get_dbus_connection(), reply);

	bonding_request_cancel(bonding);
	bonding_request_free(bonding);
}

static void pincode_cb(struct agent *agent, DBusError *err,
					const char *pincode, void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;
	struct btd_adapter *adapter = device_get_adapter(device);
	struct agent *adapter_agent = adapter_get_agent(adapter);

	if (err && (g_str_equal(DBUS_ERROR_UNKNOWN_METHOD, err->name) ||
				g_str_equal(DBUS_ERROR_NO_REPLY, err->name))) {

		if (auth->agent == adapter_agent || adapter_agent == NULL)
			goto done;

		if (agent_request_pincode(adapter_agent, device, pincode_cb,
						auth->secure, auth, NULL) < 0)
			goto done;

		auth->agent = adapter_agent;
		return;
	}

done:
	/* No need to reply anything if the authentication already failed */
	if (auth->cb == NULL)
		return;

	((agent_pincode_cb) auth->cb)(agent, err, pincode, device);

	device->authr->cb = NULL;
	device->authr->agent = NULL;
}

static void confirm_cb(struct agent *agent, DBusError *err, void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;
	struct btd_adapter *adapter = device_get_adapter(device);
	struct agent *adapter_agent = adapter_get_agent(adapter);

	if (err && (g_str_equal(DBUS_ERROR_UNKNOWN_METHOD, err->name) ||
				g_str_equal(DBUS_ERROR_NO_REPLY, err->name))) {

		if (auth->agent == adapter_agent || adapter_agent == NULL)
			goto done;

		if (agent_request_confirmation(adapter_agent, device,
						auth->passkey, confirm_cb,
						auth, NULL) < 0)
			goto done;

		auth->agent = adapter_agent;
		return;
	}

done:
	/* No need to reply anything if the authentication already failed */
	if (auth->cb == NULL)
		return;

	((agent_cb) auth->cb)(agent, err, device);

	device->authr->cb = NULL;
	device->authr->agent = NULL;
}

static void passkey_cb(struct agent *agent, DBusError *err,
						uint32_t passkey, void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;
	struct btd_adapter *adapter = device_get_adapter(device);
	struct agent *adapter_agent = adapter_get_agent(adapter);

	if (err && (g_str_equal(DBUS_ERROR_UNKNOWN_METHOD, err->name) ||
				g_str_equal(DBUS_ERROR_NO_REPLY, err->name))) {

		if (auth->agent == adapter_agent || adapter_agent == NULL)
			goto done;

		if (agent_request_passkey(adapter_agent, device, passkey_cb,
							auth, NULL) < 0)
			goto done;

		auth->agent = adapter_agent;
		return;
	}

done:
	/* No need to reply anything if the authentication already failed */
	if (auth->cb == NULL)
		return;

	((agent_passkey_cb) auth->cb)(agent, err, passkey, device);

	device->authr->cb = NULL;
	device->authr->agent = NULL;
}

static void display_pincode_cb(struct agent *agent, DBusError *err, void *data)
{
	struct authentication_req *auth = data;
	struct btd_device *device = auth->device;
	struct btd_adapter *adapter = device_get_adapter(device);
	struct agent *adapter_agent = adapter_get_agent(adapter);

	if (err && (g_str_equal(DBUS_ERROR_UNKNOWN_METHOD, err->name) ||
				g_str_equal(DBUS_ERROR_NO_REPLY, err->name))) {

		/* Request a pincode if we fail to display one */
		if (auth->agent == adapter_agent || adapter_agent == NULL) {
			if (agent_request_pincode(agent, device, pincode_cb,
						auth->secure, auth, NULL) < 0)
				goto done;
			return;
		}

		if (agent_display_pincode(adapter_agent, device, auth->pincode,
					display_pincode_cb, auth, NULL) < 0)
			goto done;

		auth->agent = adapter_agent;
		return;
	}

done:
	/* No need to reply anything if the authentication already failed */
	if (auth->cb == NULL)
		return;

	((agent_pincode_cb) auth->cb)(agent, err, auth->pincode, device);

	g_free(device->authr->pincode);
	device->authr->pincode = NULL;
	device->authr->cb = NULL;
	device->authr->agent = NULL;
}

static struct authentication_req *new_auth(struct btd_device *device,
					auth_type_t type, gboolean secure,
					void *cb)
{
	struct authentication_req *auth;
	struct agent *agent;
	char addr[18];

	ba2str(&device->bdaddr, addr);
	DBG("Requesting agent authentication for %s", addr);

	if (device->authr) {
		error("Authentication already requested for %s", addr);
		return NULL;
	}

	agent = device_get_agent(device);
	if (!agent) {
		error("No agent available for request type %d", type);
		return NULL;
	}

	auth = g_new0(struct authentication_req, 1);
	auth->agent = agent;
	auth->device = device;
	auth->cb = cb;
	auth->type = type;
	auth->secure = secure;
	device->authr = auth;

	return auth;
}

int device_request_pincode(struct btd_device *device, gboolean secure,
								void *cb)
{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_PINCODE, secure, cb);
	if (!auth)
		return -EPERM;

	err = agent_request_pincode(auth->agent, device, pincode_cb, secure,
								auth, NULL);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_request_passkey(struct btd_device *device, void *cb)
{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_PASSKEY, FALSE, cb);
	if (!auth)
		return -EPERM;

	err = agent_request_passkey(auth->agent, device, passkey_cb, auth,
									NULL);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_confirm_passkey(struct btd_device *device, uint32_t passkey,
								void *cb)
{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_CONFIRM, FALSE, cb);
	if (!auth)
		return -EPERM;

	auth->passkey = passkey;

	err = agent_request_confirmation(auth->agent, device, passkey,
						confirm_cb, auth, NULL);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_notify_passkey(struct btd_device *device, uint32_t passkey,
							uint8_t entered)
{
	struct authentication_req *auth;
	int err;

	if (device->authr) {
		auth = device->authr;
		if (auth->type != AUTH_TYPE_NOTIFY_PASSKEY)
			return -EPERM;
	} else {
		auth = new_auth(device, AUTH_TYPE_NOTIFY_PASSKEY, FALSE, NULL);
		if (!auth)
			return -EPERM;
	}

	err = agent_display_passkey(auth->agent, device, passkey, entered);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

int device_notify_pincode(struct btd_device *device, gboolean secure,
						const char *pincode, void *cb)
{
	struct authentication_req *auth;
	int err;

	auth = new_auth(device, AUTH_TYPE_NOTIFY_PINCODE, secure, cb);
	if (!auth)
		return -EPERM;

	auth->pincode = g_strdup(pincode);

	err = agent_display_pincode(auth->agent, device, pincode,
					display_pincode_cb, auth, NULL);
	if (err < 0) {
		error("Failed requesting authentication");
		device_auth_req_free(device);
	}

	return err;
}

static void cancel_authentication(struct authentication_req *auth)
{
	struct btd_device *device;
	struct agent *agent;
	DBusError err;

	if (!auth || !auth->cb)
		return;

	device = auth->device;
	agent = auth->agent;

	dbus_error_init(&err);
	dbus_set_error_const(&err, "org.bluez.Error.Canceled", NULL);

	switch (auth->type) {
	case AUTH_TYPE_PINCODE:
		((agent_pincode_cb) auth->cb)(agent, &err, NULL, device);
		break;
	case AUTH_TYPE_CONFIRM:
		((agent_cb) auth->cb)(agent, &err, device);
		break;
	case AUTH_TYPE_PASSKEY:
		((agent_passkey_cb) auth->cb)(agent, &err, 0, device);
		break;
	case AUTH_TYPE_NOTIFY_PASSKEY:
		/* User Notify doesn't require any reply */
		break;
	case AUTH_TYPE_NOTIFY_PINCODE:
		((agent_pincode_cb) auth->cb)(agent, &err, NULL, device);
		break;
	}

	dbus_error_free(&err);
	auth->cb = NULL;
}

void device_cancel_authentication(struct btd_device *device, gboolean aborted)
{
	struct authentication_req *auth = device->authr;
	char addr[18];

	if (!auth)
		return;

	ba2str(&device->bdaddr, addr);
	DBG("Canceling authentication request for %s", addr);

	if (auth->agent)
		agent_cancel(auth->agent);

	if (!aborted)
		cancel_authentication(auth);

	device_auth_req_free(device);
}

gboolean device_is_authenticating(struct btd_device *device)
{
	return (device->authr != NULL);
}

void device_register_services(struct btd_device *device,
						GSList *prim_list, int psm)
{
	device->primaries = g_slist_concat(device->primaries, prim_list);
	device->services = attrib_client_register(device, psm, NULL, prim_list);
}

GSList *btd_device_get_primaries(struct btd_device *device)
{
	return device->primaries;
}

void btd_device_gatt_set_service_changed(struct btd_device *device,
						uint16_t start, uint16_t end)
{
	GSList *l;

	for (l = device->primaries; l; l = g_slist_next(l)) {
		struct gatt_primary *prim = l->data;

		if (start <= prim->range.end && end >= prim->range.start)
			prim->changed = TRUE;
	}

	device_browse_primary(device, NULL, FALSE);
}

void btd_device_add_uuid(struct btd_device *device, const char *uuid)
{
	GSList *uuid_list;
	char *new_uuid;

	if (g_slist_find_custom(device->uuids, uuid, bt_uuid_strcmp))
		return;

	new_uuid = g_strdup(uuid);
	uuid_list = g_slist_append(NULL, new_uuid);

	device_probe_profiles(device, uuid_list);

	g_free(new_uuid);
	g_slist_free(uuid_list);

	store_profiles(device);
	uuids_changed(device);
}

const sdp_record_t *btd_device_get_record(struct btd_device *device,
							const char *uuid)
{
	bdaddr_t src;

	if (device->tmp_records) {
		const sdp_record_t *record;

		record = find_record_in_list(device->tmp_records, uuid);
		if (record != NULL)
			return record;

		sdp_list_free(device->tmp_records,
					(sdp_free_func_t) sdp_record_free);
		device->tmp_records = NULL;
	}

	adapter_get_address(device->adapter, &src);

	device->tmp_records = read_records(&src, &device->bdaddr);
	if (!device->tmp_records)
		return NULL;

	return find_record_in_list(device->tmp_records, uuid);
}

struct btd_device *btd_device_ref(struct btd_device *device)
{
	device->ref++;

	DBG("%p: ref=%d", device, device->ref);

	return device;
}

void btd_device_unref(struct btd_device *device)
{
	gchar *path;

	device->ref--;

	DBG("%p: ref=%d", device, device->ref);

	if (device->ref > 0)
		return;

	path = g_strdup(device->path);

	g_dbus_unregister_interface(btd_get_dbus_connection(),
							path, DEVICE_INTERFACE);

	g_free(path);
}

void device_set_class(struct btd_device *device, uint32_t value)
{
	const char *icon = class_to_icon(value);

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Class",
				DBUS_TYPE_UINT32, &value);

	if (icon)
		emit_property_changed(device->path,
					DEVICE_INTERFACE, "Icon",
					DBUS_TYPE_STRING, &icon);
}

int device_get_appearance(struct btd_device *device, uint16_t *value)
{
	bdaddr_t src;
	uint16_t app;
	int err;

	adapter_get_address(device_get_adapter(device), &src);

	err = read_remote_appearance(&src, &device->bdaddr,
						device->bdaddr_type, &app);
	if (err < 0)
		return err;

	if (value)
		*value = app;

	return 0;
}

void device_set_appearance(struct btd_device *device, uint16_t value)
{
	const char *icon = gap_appearance_to_icon(value);
	bdaddr_t src;

	emit_property_changed(device->path,
				DEVICE_INTERFACE, "Appearance",
				DBUS_TYPE_UINT16, &value);

	if (icon)
		emit_property_changed(device->path,
					DEVICE_INTERFACE, "Icon",
					DBUS_TYPE_STRING, &icon);

	adapter_get_address(device_get_adapter(device), &src);
	write_remote_appearance(&src, &device->bdaddr, device->bdaddr_type,
									value);
}

static gboolean notify_attios(gpointer user_data)
{
	struct btd_device *device = user_data;

	if (device->attrib == NULL)
		return FALSE;

	g_slist_foreach(device->attios_offline, attio_connected, device->attrib);
	device->attios = g_slist_concat(device->attios, device->attios_offline);
	device->attios_offline = NULL;

	return FALSE;
}

guint btd_device_add_attio_callback(struct btd_device *device,
						attio_connect_cb cfunc,
						attio_disconnect_cb dcfunc,
						gpointer user_data)
{
	struct attio_data *attio;
	static guint attio_id = 0;

	DBG("%p registered ATT connection callback", device);

	attio = g_new0(struct attio_data, 1);
	attio->id = ++attio_id;
	attio->cfunc = cfunc;
	attio->dcfunc = dcfunc;
	attio->user_data = user_data;

	if (device->attrib && cfunc) {
		device->attios_offline = g_slist_append(device->attios_offline,
									attio);
		g_idle_add(notify_attios, device);
		return attio->id;
	}

	device->attios = g_slist_append(device->attios, attio);

	adapter_connect_list_add(device_get_adapter(device), device);

	return attio->id;
}

static int attio_id_cmp(gconstpointer a, gconstpointer b)
{
	const struct attio_data *attio = a;
	guint id = GPOINTER_TO_UINT(b);

	return attio->id - id;
}

gboolean btd_device_remove_attio_callback(struct btd_device *device, guint id)
{
	struct attio_data *attio;
	GSList *l;

	l = g_slist_find_custom(device->attios, GUINT_TO_POINTER(id),
								attio_id_cmp);
	if (l) {
		attio = l->data;
		device->attios = g_slist_remove(device->attios, attio);
	} else {
		l = g_slist_find_custom(device->attios_offline,
					GUINT_TO_POINTER(id), attio_id_cmp);
		if (!l)
			return FALSE;

		attio = l->data;
		device->attios_offline = g_slist_remove(device->attios_offline,
									attio);
	}

	g_free(attio);

	if (device->attios != NULL || device->attios_offline != NULL)
		return TRUE;

	if (device->auto_id) {
		g_source_remove(device->auto_id);
		device->auto_id = 0;
	}

	attio_cleanup(device);

	return TRUE;
}

void btd_device_set_pnpid(struct btd_device *device, uint8_t vendor_id_src,
			uint16_t vendor_id, uint16_t product_id,
			uint16_t product_ver)
{
	GSList *l;

	device_set_vendor(device, vendor_id);
	device_set_vendor_src(device, vendor_id_src);
	device_set_product(device, product_id);
	device_set_version(device, product_ver);

	for (l = device->pnpid_notifiers; l; l = l->next) {
		struct pnpid_notifier *notifier = l->data;

		notifier->func(notifier->user_data);
		g_free(notifier);
	}

	g_slist_free(device->pnpid_notifiers);
	device->pnpid_notifiers = NULL;
}

static void batteries_changed(struct btd_device *device)
{
	char **batteries;
	GSList *l;
	int i;

	batteries = g_new0(char *, g_slist_length(device->batteries) + 1);
	for (i = 0, l = device->batteries; l; l = l->next, i++) {
		struct btd_battery *batt = l->data;
		batteries[i] = batt->path;
	}

	emit_array_property_changed(device->path, DEVICE_INTERFACE, "Batteries",
					  DBUS_TYPE_OBJECT_PATH, &batteries, i);

	g_free(batteries);
}

static DBusMessage *refresh_batt_level(DBusConnection *conn,
						  DBusMessage *msg, void *data)
{
	struct btd_battery *b = data;

	if (!b->refresh)
		return btd_error_not_supported(msg);

	b->refresh(b);

	return dbus_message_new_method_return(msg);
}

static DBusMessage *get_batt_properties(DBusConnection *conn,
						  DBusMessage *msg, void *data)
{
	struct btd_battery *b = data;
	DBusMessageIter iter;
	DBusMessageIter dict;
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (reply == NULL)
		return NULL;

	dbus_message_iter_init_append(reply, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	dict_append_entry(&dict, "Level", DBUS_TYPE_UINT16, &b->level);

	dbus_message_iter_close_container(&iter, &dict);

	return reply;
}

static GDBusMethodTable battery_methods[] = {
	{ GDBUS_METHOD("GetProperties",
		NULL, GDBUS_ARGS({ "properties", "a{sv}" }),
		get_batt_properties) },
	{ GDBUS_METHOD("Refresh",
		NULL, NULL,
		refresh_batt_level) },
	{ }
};

static GDBusSignalTable battery_signals[] = {
	{ GDBUS_SIGNAL("PropertyChanged",
		GDBUS_ARGS({ "name", "s" }, { "value", "v" })) },
	{ }
};

struct btd_battery *btd_device_add_battery(struct btd_device *device)
{
	struct btd_battery *batt;

	batt = g_new0(struct btd_battery, 1);
	batt->path = g_strdup_printf("%s/BATT%04X", device->path,
					     g_slist_length(device->batteries));

	if (!g_dbus_register_interface(btd_get_dbus_connection(), batt->path,
			BATTERY_INTERFACE, battery_methods, battery_signals,
			NULL, batt, NULL)) {
		error("D-Bus register interface %s failed", BATTERY_INTERFACE);
		g_free(batt->path);
		g_free(batt);
		return NULL;
	}

	batt->device = btd_device_ref(device);
	device->batteries = g_slist_append(device->batteries, batt);
	batteries_changed(device);

	return batt;
}

void btd_device_remove_battery(struct btd_battery *batt)
{
	struct btd_device *dev = batt->device;

	dev->batteries = g_slist_remove(dev->batteries, batt);

	battery_free(batt);

	batteries_changed(dev);
}

gboolean btd_device_set_battery_opt(struct btd_battery *batt,
						    battery_option_t opt1, ...)
{
	va_list args;
	battery_option_t opt = opt1;
	int level;

	if (!batt)
		return FALSE;

	va_start(args, opt1);

	while (opt != BATTERY_OPT_INVALID) {
		switch (opt) {
		case BATTERY_OPT_LEVEL:
			level = va_arg(args, int);
			if (level != batt->level) {
				batt->level = level;
				emit_property_changed(batt->path,
						BATTERY_INTERFACE, "Level",
						DBUS_TYPE_UINT16, &level);
			}
			break;
		case BATTERY_OPT_REFRESH_FUNC:
			batt->refresh = va_arg(args, refresh_battery_cb);
			break;
		default:
			error("Unknown option %d", opt);
			return FALSE;
		}

		opt = va_arg(args, int);
	}

	va_end(args);

	return TRUE;
}

bool btd_device_register_pnpid_notifier(struct btd_device *device,
				pnpid_ready_func notify, void *user_data)
{
	struct pnpid_notifier *notifier;

	if (btd_device_get_vendor(device) ||
			btd_device_get_vendor_src(device) ||
			btd_device_get_product(device) ||
			btd_device_get_version(device))
		return false;

	notifier = g_new0(struct pnpid_notifier, 1);

	notifier->func = notify;
	notifier->user_data = user_data;

	device->pnpid_notifiers = g_slist_append(device->pnpid_notifiers,
								notifier);
	return true;
}
