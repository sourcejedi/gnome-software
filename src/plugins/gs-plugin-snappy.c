/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2015 Canonical Ltd
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <gs-plugin.h>
#include <gio/gunixsocketaddress.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>

// snapd API documentation is at https://github.com/ubuntu-core/snappy/blob/master/docs/rest.md

struct GsPluginPrivate {
};

#define SNAPD_SOCKET_PATH "/run/snapd.socket"

typedef gboolean (*AppFilterFunc)(const gchar *id, JsonObject *object, gpointer data);

const gchar *
gs_plugin_get_name (void)
{
	return "snappy";
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	/* create private area */
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
}

static GSocket *
open_snapd_socket (GError **error)
{
	GSocket *socket;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GError) sub_error = NULL;

	socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, &sub_error);
	if (!socket) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to open snapd socket: %s", sub_error->message);
		return NULL;
	}
	address = g_unix_socket_address_new (SNAPD_SOCKET_PATH);
	if (!g_socket_connect (socket, address, NULL, &sub_error)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to connect snapd socket: %s", sub_error->message);
		g_object_unref (socket);
		return NULL;
	}

	return socket;
}

static gboolean
read_from_snapd (GSocket *socket, gchar *buffer, gsize buffer_length, gsize *read_offset, GError **error)
{
	gssize n_read;
	n_read = g_socket_receive (socket, buffer + *read_offset, buffer_length - *read_offset, NULL, error);
	if (n_read < 0)
		return FALSE;
	*read_offset += n_read;
	buffer[*read_offset] = '\0';

	return TRUE;
}

static gboolean
send_snapd_request (GSocket *socket,
		    const gchar *method,
		    const gchar *path,
		    const gchar *content,
		    guint *status_code,
		    gchar **reason_phrase,
		    gchar **response_type,
		    gchar **response,
		    gsize *response_length,
		    GError **error)
{
	g_autoptr (GString) request = NULL;
	gssize n_written;
	gsize max_data_length = 65535, data_length = 0, header_length;
	gchar data[max_data_length + 1], *body = NULL;
	g_autoptr (SoupMessageHeaders) headers = NULL;
	gsize chunk_length, n_required;
	gchar *chunk_start = NULL;

	// NOTE: Would love to use libsoup but it doesn't support unix sockets
	// https://bugzilla.gnome.org/show_bug.cgi?id=727563

	request = g_string_new ("");
	g_string_append_printf (request, "%s %s HTTP/1.1\r\n", method, path);
	g_string_append (request, "Host:\r\n");
	if (content)
		g_string_append_printf (request, "Content-Length: %zi\r\n", strlen (content));
	g_string_append (request, "\r\n");
	if (content)
		g_string_append (request, content);

	/* Send HTTP request */
	n_written = g_socket_send (socket, request->str, request->len, NULL, error);
	if (n_written < 0)
		return FALSE;

	/* Read HTTP headers */
	while (data_length < max_data_length && !body) {
		if (!read_from_snapd (socket, data, max_data_length, &data_length, error))
			return FALSE;
		body = strstr (data, "\r\n\r\n");
	}
	if (!body) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "Unable to find header separator in snapd response");
		return FALSE;
	}

	/* Body starts after header divider */
	body += 4;
	header_length = body - data;

	/* Parse headers */
	headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_RESPONSE);
	if (!soup_headers_parse_response (data, header_length, headers, NULL, status_code, reason_phrase)) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "snapd response HTTP headers not parseable");
		return FALSE;
	}

	/* Work out how much data to follow */
	if (g_strcmp0 (soup_message_headers_get_one (headers, "Transfer-Encoding"), "chunked") == 0) {
		while (data_length < max_data_length) {
			chunk_start = strstr (body, "\r\n");
			if (chunk_start)
				break;
			if (!read_from_snapd (socket, data, max_data_length, &data_length, error))
				return FALSE;
		}
		if (!chunk_start) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_FAILED,
					     "Unable to find chunk header in snapd response");
			return FALSE;
		}
		chunk_length = strtoul (body, NULL, 16);
		chunk_start += 2;
		// FIXME: Support multiple chunks
	}
	else {
		const gchar *value;
		value = soup_message_headers_get_one (headers, "Content-Length");
		if (!value) {
			g_set_error_literal (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_FAILED,
					     "Unable to determine content length of snapd response");
			return FALSE;
		}
		chunk_length = strtoul (value, NULL, 10);
		chunk_start = body;
	}

	/* Check if enough space to read chunk */
	n_required = (chunk_start - data) + chunk_length;
	if (n_required > max_data_length) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Not enough space for snapd response, require %zi octets, have %zi", n_required, max_data_length);
		return FALSE;
	}

	/* Read chunk content */
	while (data_length < n_required)
		if (!read_from_snapd (socket, data, n_required - data_length, &data_length, error))
			return FALSE;

	if (response_type)
		*response_type = g_strdup (soup_message_headers_get_one (headers, "Content-Type"));
	if (response) {
		*response = g_malloc (chunk_length + 1);
		memcpy (*response, chunk_start, chunk_length + 1);
	}
	if (response_length)
		*response_length = chunk_length;

	return TRUE;
}

static gboolean
get_apps (GsPlugin *plugin, const gchar *sources, gchar **search_terms, GList **list, AppFilterFunc filter_func, gpointer user_data, GError **error)
{
	g_autoptr(GSocket) socket = NULL;
	guint status_code;
	GPtrArray *query_fields;
	g_autoptr (GString) path = NULL;
	g_autofree gchar *reason_phrase = NULL, *response_type = NULL, *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root, *result, *packages;
	GList *package_list, *link;
	g_autoptr(GError) sub_error = NULL;

	socket = open_snapd_socket (error);
	if (!socket)
		return FALSE;

	/* Get all the apps */
	query_fields = g_ptr_array_new_with_free_func (g_free);
	if (sources != NULL)
		g_ptr_array_add (query_fields, g_strdup_printf ("sources=%s", sources));
	if (search_terms != NULL) {
		g_autofree gchar *query = NULL;
		query = g_strjoinv ("+", search_terms);
		g_ptr_array_add (query_fields, g_strdup_printf ("q=%s", query));
	}
	g_ptr_array_add (query_fields, NULL);
	path = g_string_new ("/2.0/snaps");
	if (query_fields->len > 1) {
		g_autofree gchar *fields = NULL;
		g_string_append (path, "?");
		fields = g_strjoinv ("&", (gchar **) query_fields->pdata);
		g_string_append (path, fields);
	}
	g_ptr_array_free (query_fields, TRUE);
	if (!send_snapd_request (socket, "GET", path->str, NULL, &status_code, &reason_phrase, &response_type, &response, NULL, error))
		return FALSE;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %d: %s", status_code, reason_phrase);
		return FALSE;
	}

	if (!response_type) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "snapd returned no content type");
		return FALSE;
	}
	if (g_strcmp0 (response_type, "application/json") != 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned unexpected content type %s", response_type);
		return FALSE;
	}

	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, response, -1, &sub_error)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to parse snapd response: %s", sub_error->message);
		return FALSE;
	}
	if (!JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser))) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "snapd response does is not a valid JSON object");
		return FALSE;
	}
	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_object_member (root, "result");
	packages = json_object_get_object_member (result, "snaps");
	package_list = json_object_get_members (packages);
	for (link = package_list; link; link = link->next) {
		const gchar *id = link->data;
		JsonObject *package;
		const gchar *status, *icon_url;
		g_autoptr(GsApp) app = NULL;
		g_autoptr(GdkPixbuf) icon_pixbuf = NULL;

		package = json_object_get_object_member (packages, id);
		if (filter_func != NULL && !filter_func (id, package, user_data))
			continue;

		app = gs_app_new (id);
		gs_app_set_management_plugin (app, "snappy");
		gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
		status = json_object_get_string_member (package, "status");
		if (g_strcmp0 (status, "installed") == 0 || g_strcmp0 (status, "active") == 0) {
			const gchar *update_available;

			update_available = json_object_has_member (package, "update_available") ? json_object_get_string_member (package, "update_available") : NULL;
			if (update_available)
				gs_app_set_state (app, AS_APP_STATE_UPDATABLE);
			else
				gs_app_set_state (app, AS_APP_STATE_INSTALLED);
			gs_app_set_size (app, json_object_get_int_member (package, "installed_size"));
		}
		else if (g_strcmp0 (status, "removed") == 0) {
			// A removed app is only available if it can be downloaded (it might have been sideloaded)
			if (json_object_get_int_member (package, "download_size") > 0)
				gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		} else if (g_strcmp0 (status, "not installed") == 0) {
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
			gs_app_set_size (app, json_object_get_int_member (package, "download_size"));
		}
		gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, json_object_get_string_member (package, "name"));
		gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST, json_object_get_string_member (package, "description"));
		gs_app_set_version (app, json_object_get_string_member (package, "version"));
		gs_app_add_quirk (app, AS_APP_QUIRK_PROVENANCE);
		icon_url = json_object_get_string_member (package, "icon");
		if (g_str_has_prefix (icon_url, "/")) {
			g_autoptr(GSocket) icon_socket = NULL;
			g_autofree gchar *icon_response = NULL;
			gsize icon_response_length;

			icon_socket = open_snapd_socket (NULL);
			if (icon_socket && send_snapd_request (icon_socket, "GET", icon_url, NULL, NULL, NULL, NULL, &icon_response, &icon_response_length, NULL)) {
				g_autoptr(GdkPixbufLoader) loader = NULL;

				loader = gdk_pixbuf_loader_new ();
				gdk_pixbuf_loader_write (loader, (guchar *) icon_response, icon_response_length, NULL);
				gdk_pixbuf_loader_close (loader, NULL);
				icon_pixbuf = g_object_ref (gdk_pixbuf_loader_get_pixbuf (loader));
			}
			else
				g_printerr ("Failed to get icon\n");
		}
		else {
			g_autoptr(SoupMessage) message = NULL;
			g_autoptr(GdkPixbufLoader) loader = NULL;

			message = soup_message_new (SOUP_METHOD_GET, icon_url);
			if (message != NULL) {
				soup_session_send_message (plugin->soup_session, message);
				loader = gdk_pixbuf_loader_new ();
				gdk_pixbuf_loader_write (loader, (guint8 *) message->response_body->data,  message->response_body->length, NULL);
				gdk_pixbuf_loader_close (loader, NULL);
				icon_pixbuf = g_object_ref (gdk_pixbuf_loader_get_pixbuf (loader));
			}
		}

		if (icon_pixbuf)
			gs_app_set_pixbuf (app, icon_pixbuf);
		else {
			g_autoptr(AsIcon) icon = NULL;

			icon = as_icon_new ();
			as_icon_set_kind (icon, AS_ICON_KIND_STOCK);
			as_icon_set_name (icon, "package-x-generic");
			gs_app_set_icon (app, icon);
		}
		gs_plugin_add_app (list, app);
	}

	return TRUE;
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
}

static gboolean
is_active (const gchar *id, JsonObject *object, gpointer data)
{
	const gchar *status = json_object_get_string_member (object, "status");
	return g_strcmp0 (status, "active") == 0;
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	return get_apps (plugin, "local", NULL, list, is_active, NULL, error);
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
		      gchar **values,
		      GList **list,
		      GCancellable *cancellable,
		      GError **error)
{
	return get_apps (plugin, NULL, values, list, NULL, values, error);
}

static gboolean
send_package_action (GsPlugin *plugin, const char *id, const gchar *action, GError **error)
{
	g_autofree gchar *content = NULL, *path = NULL;
	g_autoptr(GSocket) socket = NULL;
	guint status_code;
	g_autofree gchar *reason_phrase, *response_type = NULL, *response = NULL;

	socket = open_snapd_socket (error);
	if (!socket)
		return FALSE;

	content = g_strdup_printf ("{\"action\": \"%s\"}", action);
	path = g_strdup_printf ("/2.0/snaps/%s", id);
	if (!send_snapd_request (socket, "POST", path, content, &status_code, &reason_phrase, &response_type, &response, NULL, error))
		return FALSE;

	if (status_code != SOUP_STATUS_ACCEPTED) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %d: %s", status_code, reason_phrase);
		return FALSE;
	}

	// FIXME: Need to poll for status - may stil have failed

	return TRUE;
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	gboolean result;

	/* We can only install apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snappy") != 0)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	result = send_package_action (plugin, gs_app_get_id (app), "install", error);
	gs_app_set_state (app, AS_APP_STATE_INSTALLED);

	return result;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	gboolean result;

	/* We can only remove apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snappy") != 0)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	result = send_package_action (plugin, gs_app_get_id (app), "remove", error);
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

	return result;
}
