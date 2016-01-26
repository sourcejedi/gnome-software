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
	g_autoptr (GSocketAddress) address = NULL;

	socket = g_socket_new (G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, error);
	if (!socket)
		return NULL;
	address = g_unix_socket_address_new (SNAPD_SOCKET_PATH);
	if (!g_socket_connect (socket, address, NULL, error)) {
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
send_snapd_request (GSocket *socket, const gchar *request, guint *status_code, gchar **response_type, gchar **response, gsize *response_length, GError **error)
{
	gsize max_data_length = 65535, data_length = 0, header_length;
	gchar data[max_data_length + 1], *body = NULL;
	g_autoptr (SoupMessageHeaders) headers = NULL;
	g_autofree gchar *reason_phrase = NULL;
	gsize chunk_length, n_required;
	gchar *chunk_start = NULL;

	// NOTE: Would love to use libsoup but it doesn't support unix sockets
	// https://bugzilla.gnome.org/show_bug.cgi?id=727563

	/* Send HTTP request */
	gssize n_written;
	n_written = g_socket_send (socket, request, strlen (request), NULL, error);
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
	if (!soup_headers_parse_response (data, header_length, headers, NULL, status_code, &reason_phrase)) {
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
get_apps (GsPlugin *plugin, GList **list, AppFilterFunc filter_func, gpointer user_data, GError **error)
{
	g_autoptr(GSocket) socket = NULL;
	guint status_code;
	g_autofree gchar *response_type = NULL, *response = NULL;
	JsonParser *parser;
	JsonObject *root, *result, *packages;
	GList *package_list, *link;
	g_autoptr(GError) sub_error = NULL;

	socket = open_snapd_socket (&sub_error);
	if (!socket) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to open snapd socket: %s", sub_error->message);
		return FALSE;
	}

	/* Get all the apps */
	if (!send_snapd_request (socket, "GET /1.0/packages HTTP/1.1\r\n\r\n", &status_code, &response_type, &response, NULL, error))
		return FALSE;

	if (status_code != 200) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %d", status_code);
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
	packages = json_object_get_object_member (result, "packages");
	package_list = json_object_get_members (packages);
	for (link = package_list; link; link = link->next) {
		const gchar *id = link->data;
		JsonObject *package;
		const gchar *status, *icon_url;
		g_autoptr(GsApp) app = NULL;

		package = json_object_get_object_member (packages, id);
		if (!filter_func (id, package, user_data))
			continue;

		app = gs_app_new (id);
		gs_app_set_management_plugin (app, "snappy");
		gs_app_set_kind (app, GS_APP_KIND_NORMAL);
		status = json_object_get_string_member (package, "status");
		if (g_strcmp0 (status, "installed") == 0 || g_strcmp0 (status, "active") == 0) {
			const gchar *update_available;
			update_available = json_object_has_member (package, "update_available") ? json_object_get_string_member (package, "update_available") : NULL;
			if (update_available)
				gs_app_set_state (app, AS_APP_STATE_UPDATABLE);
			else
				gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		}
		else if (g_strcmp0 (status, "not installed") == 0)
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		gs_app_set_size (app, json_object_get_int_member (package, "installed_size"));
		gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, json_object_get_string_member (package, "name"));
		gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST, json_object_get_string_member (package, "description"));
		gs_app_set_version (app, json_object_get_string_member (package, "version"));
		icon_url = json_object_get_string_member (package, "icon");
		if (g_str_has_prefix (icon_url, "/")) {
#if 0
			g_autofree gchar *request = NULL, *icon_response = NULL;
			gsize icon_response_length;

			request = g_strdup_printf ("GET %s HTTP/1.1\r\n\r\n", icon_url);
			if (send_snapd_request (plugin, request, &status_code, NULL, &icon_response, &icon_response_length, NULL)) {
				g_autoptr(GdkPixbufLoader) loader = NULL;

				loader = gdk_pixbuf_loader_new ();
				gdk_pixbuf_loader_write (loader, (guchar *) icon_response, icon_response_length, NULL);
				gs_app_set_pixbuf (app, gdk_pixbuf_loader_get_pixbuf (loader));
			}
			else
				g_printerr ("Failed to get icon\n");
#endif
		}
		else {
#if 0
			g_autoptr(AsIcon) as_icon = NULL;

			as_icon = as_icon_new ();
			as_icon_set_kind (as_icon, AS_ICON_KIND_REMOTE);
			as_icon_set_url (as_icon, icon_url);
			gs_app_set_icon (app, as_icon);
#endif
		}
		gs_app_set_pixbuf (app, gdk_pixbuf_new_from_file ("/usr/share/icons/gnome/48x48/mimetypes/package-x-generic.png", NULL));
		gs_plugin_add_app (list, app);
		g_printerr ("SNAPPY: +%s\n", gs_app_to_string (app));
	}
	g_object_unref (parser);

	return TRUE;
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
}

static gboolean
is_installed (const gchar *id, JsonObject *object, gpointer data)
{
	const gchar *status = json_object_get_string_member (object, "status");
	return status && (strcmp (status, "installed") == 0 || strcmp (status, "active") == 0);
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_add_installed\n");
	return get_apps (plugin, list, is_installed, NULL, error);
}

static gboolean
matches_search (const gchar *id, JsonObject *object, gpointer data)
{
	gchar **values = data;
	return TRUE;
}

gboolean
gs_plugin_add_search (GsPlugin *plugin,
		      gchar **values,
		      GList **list,
		      GCancellable *cancellable,
		      GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_add_search\n");
	return get_apps (plugin, list, matches_search, values, error);
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_app_install\n");

	/* We can only install apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snappy") != 0)
		return TRUE;

	return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_app_remove\n");

	/* We can only remove apps we know of */
	if (g_strcmp0 (gs_app_get_management_plugin (app), "snappy") != 0)
		return TRUE;

	return TRUE;
}

#if 0
gboolean
gs_plugin_add_sources (GsPlugin *plugin,
		       GList **list,
		       GCancellable *cancellable,
		       GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_add_sources\n");
	return FALSE;
}

gboolean
gs_plugin_add_search_files (GsPlugin *plugin,
                            gchar **search,
                            GList **list,
                            GCancellable *cancellable,
                            GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_add_search_files\n");
	return FALSE;
}

gboolean
gs_plugin_add_search_what_provides (GsPlugin *plugin,
                                    gchar **search,
                                    GList **list,
                                    GCancellable *cancellable,
                                    GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_add_search_what_provides\n");
	return FALSE;
}

gboolean
gs_plugin_add_categories (GsPlugin *plugin,
			  GList **list,
			  GCancellable *cancellable,
			  GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_add_categories\n");
	return FALSE;
}

gboolean
gs_plugin_add_category_apps (GsPlugin *plugin,
			     GsCategory *category,
			     GList **list,
			     GCancellable *cancellable,
			     GError **error)
{
	g_printerr ("SNAPPY: gs_plugin_add_category_apps\n");
	return FALSE;
}
#endif
