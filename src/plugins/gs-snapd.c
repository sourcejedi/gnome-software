/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Canonical Ltd
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

#include <stdlib.h>
#include <string.h>
#include <gs-plugin.h>
#include <libsoup/soup.h>
#include <gio/gunixsocketaddress.h>
#include "gs-snapd.h"
#include "gs-ubuntuone.h"

// snapd API documentation is at https://github.com/snapcore/snapd/blob/master/docs/rest.md

#define SNAPD_SOCKET "/run/snapd.socket"

gboolean
gs_snapd_exists (void)
{
	return g_file_test (SNAPD_SOCKET, G_FILE_TEST_EXISTS);
}

static GSocket *
open_snapd_socket (GCancellable *cancellable, GError **error)
{
	GSocket *socket;
	g_autoptr(GSocketAddress) address = NULL;
	g_autoptr(GError) error_local = NULL;

	socket = g_socket_new (G_SOCKET_FAMILY_UNIX,
			       G_SOCKET_TYPE_STREAM,
			       G_SOCKET_PROTOCOL_DEFAULT,
			       &error_local);
	if (!socket) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to open snapd socket: %s",
			     error_local->message);
		return NULL;
	}
	address = g_unix_socket_address_new (SNAPD_SOCKET);
	if (!g_socket_connect (socket, address, cancellable, &error_local)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to connect snapd socket: %s",
			     error_local->message);
		g_object_unref (socket);
		return NULL;
	}

	return socket;
}

static gboolean
read_from_snapd (GSocket *socket,
		 gchar *buffer, gsize buffer_length,
		 gsize *read_offset,
		 GCancellable *cancellable,
		 GError **error)
{
	gssize n_read;
	n_read = g_socket_receive (socket,
				   buffer + *read_offset,
				   buffer_length - *read_offset,
				   cancellable,
				   error);
	if (n_read < 0)
		return FALSE;
	*read_offset += (gsize) n_read;
	buffer[*read_offset] = '\0';

	return TRUE;
}

static gboolean
send_request (const gchar  *method,
	      const gchar  *path,
	      const gchar  *content,
	      gboolean      authenticate,
	      const gchar  *macaroon_,
	      gchar       **discharges_,
	      gboolean      retry_after_login,
	      gchar       **out_macaroon,
	      gchar      ***out_discharges,
	      guint        *status_code,
	      gchar       **reason_phrase,
	      gchar       **response_type,
	      gchar       **response,
	      gsize        *response_length,
	      GCancellable *cancellable,
	      GError      **error)
{
	g_autoptr (GSocket) socket = NULL;
	g_autoptr (GString) request = NULL;
	gssize n_written;
	gsize max_data_length = 65535, data_length = 0, header_length;
	gchar data[max_data_length + 1], *body = NULL;
	g_autoptr (SoupMessageHeaders) headers = NULL;
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;
	gsize chunk_length, n_required;
	gchar *chunk_start = NULL;
	guint code;
	gboolean ret;

	macaroon = g_strdup (macaroon_);
	discharges = g_strdupv (discharges_);
	if (macaroon == NULL && authenticate) {
		gs_ubuntuone_get_macaroon (TRUE, FALSE, &macaroon, &discharges, NULL);
	}

	// NOTE: Would love to use libsoup but it doesn't support unix sockets
	// https://bugzilla.gnome.org/show_bug.cgi?id=727563

	socket = open_snapd_socket (cancellable, error);
	if (socket == NULL)
		return FALSE;

	request = g_string_new ("");
	g_string_append_printf (request, "%s %s HTTP/1.1\r\n", method, path);
	g_string_append (request, "Host:\r\n");
	if (macaroon != NULL) {
		gint i;

		g_string_append_printf (request, "Authorization: Macaroon root=\"%s\"", macaroon);
		for (i = 0; discharges[i] != NULL; i++)
			g_string_append_printf (request, ",discharge=\"%s\"", discharges[i]);
		g_string_append (request, "\r\n");
	}
	if (content)
		g_string_append_printf (request, "Content-Length: %zu\r\n", strlen (content));
	g_string_append (request, "\r\n");
	if (content)
		g_string_append (request, content);

	if (g_strcmp0 (g_getenv ("GNOME_SOFTWARE_SNAPPY"), "debug") == 0)
		g_print ("===== begin snapd request =====\n%s\n===== end snapd request =====\n", request->str);

	/* send HTTP request */
	n_written = g_socket_send (socket, request->str, request->len, cancellable, error);
	if (n_written < 0)
		return FALSE;

	/* read HTTP headers */
	while (data_length < max_data_length && !body) {
		if (!read_from_snapd (socket,
				      data,
				      max_data_length,
				      &data_length,
				      cancellable,
				      error))
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

	/* body starts after header divider */
	body += 4;
	header_length = (gsize) (body - data);

	/* parse headers */
	headers = soup_message_headers_new (SOUP_MESSAGE_HEADERS_RESPONSE);
	if (!soup_headers_parse_response (data, header_length, headers,
					  NULL, &code, reason_phrase)) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "snapd response HTTP headers not parseable");
		return FALSE;
	}

	if (status_code != NULL)
		*status_code = code;

	if ((code == 401 || code == 403) && retry_after_login) {
		g_socket_close (socket, NULL);

		gs_ubuntuone_clear_macaroon ();

		g_clear_pointer (&macaroon, g_free);
		g_clear_pointer (&discharges, g_strfreev);
		gs_ubuntuone_get_macaroon (FALSE, TRUE, &macaroon, &discharges, NULL);

		if (macaroon == NULL) {
			g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
					     "failed to authenticate");
			return FALSE;
		}

		ret = send_request (method,
				    path,
				    content,
				    TRUE,
				    macaroon, discharges,
				    FALSE,
				    NULL, NULL,
				    status_code,
				    reason_phrase,
				    response_type,
				    response,
				    response_length,
				    cancellable,
				    error);

		if (ret && out_macaroon != NULL) {
			*out_macaroon = g_steal_pointer (&macaroon);
			*out_discharges = g_steal_pointer (&discharges);
		}

		return ret;
	}

	/* Work out how much data to follow */
	if (g_strcmp0 (soup_message_headers_get_one (headers, "Transfer-Encoding"), "chunked") == 0) {
		while (data_length < max_data_length) {
			chunk_start = strstr (body, "\r\n");
			if (chunk_start)
				break;
			if (!read_from_snapd (socket, data, max_data_length, &data_length, cancellable, error))
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
		if (!read_from_snapd (socket, data, n_required - data_length, &data_length, cancellable, error))
			return FALSE;

	if (out_macaroon != NULL) {
		*out_macaroon = g_steal_pointer (&macaroon);
		*out_discharges = g_steal_pointer (&discharges);
	}
	if (response_type)
		*response_type = g_strdup (soup_message_headers_get_one (headers, "Content-Type"));
	if (response) {
		*response = g_malloc (chunk_length + 2);
		memcpy (*response, chunk_start, chunk_length + 1);
		(*response)[chunk_length + 1] = '\0';

		if (g_strcmp0 (g_getenv ("GNOME_SOFTWARE_SNAPPY"), "debug") == 0)
			g_print ("===== begin snapd response =====\nStatus %u\n%s\n===== end snapd response =====\n", code, *response);
	}
	if (response_length)
		*response_length = chunk_length;

	return TRUE;
}

static JsonParser *
parse_result (const gchar *response, const gchar *response_type, GError **error)
{
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) error_local = NULL;

	if (response_type == NULL) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "snapd returned no content type");
		return NULL;
	}
	if (g_strcmp0 (response_type, "application/json") != 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned unexpected content type %s", response_type);
		return NULL;
	}

	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, response, -1, &error_local)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to parse snapd response: %s",
			     error_local->message);
		return NULL;
	}
	if (!JSON_NODE_HOLDS_OBJECT (json_parser_get_root (parser))) {
		g_set_error_literal (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "snapd response does is not a valid JSON object");
		return NULL;
	}

	return g_object_ref (parser);
}

gchar *
gs_snapd_login (const gchar *username, const gchar *password, const gchar *otp,
		guint *status_code,
		GCancellable *cancellable, GError **error)
{
	g_autofree gchar *escaped_username = NULL;
	g_autofree gchar *escaped_password = NULL;
	g_autofree gchar *escaped_otp = NULL;
	g_autofree gchar *content = NULL;
	g_autofree gchar *response = NULL;

	escaped_username = g_strescape (username, NULL);
	escaped_password = g_strescape (password, NULL);

	if (otp != NULL) {
		escaped_otp = g_strescape (otp, NULL);

		content = g_strdup_printf ("{"
					   "  \"username\" : \"%s\","
					   "  \"password\" : \"%s\","
					   "  \"otp\" : \"%s\""
					   "}",
					   escaped_username,
					   escaped_password,
					   escaped_otp);
	} else {
		content = g_strdup_printf ("{"
					   "  \"username\" : \"%s\","
					   "  \"password\" : \"%s\""
					   "}",
					   escaped_username,
					   escaped_password);
	}

	if (!send_request ("POST",
			   "/v2/login",
			   content,
			   FALSE, NULL, NULL,
			   FALSE, NULL, NULL,
			   status_code,
			   NULL,
			   NULL,
			   &response,
			   NULL,
			   NULL,
			   error))
		return NULL;

	return g_steal_pointer (&response);
}

JsonObject *
gs_snapd_list_one (const gchar *name,
		   GCancellable *cancellable, GError **error)
{
	g_autofree gchar *path = NULL;
	guint status_code;
	g_autofree gchar *reason_phrase = NULL;
	g_autofree gchar *response_type = NULL;
	g_autofree gchar *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root, *result;

	path = g_strdup_printf ("/v2/snaps/%s", name);
	if (!send_request ("GET", path, NULL,
			   TRUE, NULL, NULL,
			   TRUE, NULL, NULL,
			   &status_code, &reason_phrase,
			   &response_type, &response, NULL,
			   cancellable, error))
		return NULL;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %u: %s",
			     status_code, reason_phrase);
		return NULL;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return NULL;
	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_object_member (root, "result");
	if (result == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned no results for %s", name);
		return NULL;
	}

	return json_object_ref (result);
}

JsonArray *
gs_snapd_list (GCancellable *cancellable, GError **error)
{
	guint status_code;
	g_autofree gchar *reason_phrase = NULL;
	g_autofree gchar *response_type = NULL;
	g_autofree gchar *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;
	JsonArray *result;

	if (!send_request ("GET", "/v2/snaps", NULL,
			   TRUE, NULL, NULL,
			   TRUE, NULL, NULL,
			   &status_code, &reason_phrase,
			   &response_type, &response, NULL,
			   cancellable, error))
		return NULL;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %u: %s",
			     status_code, reason_phrase);
		return NULL;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return NULL;
	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_array_member (root, "result");
	if (result == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned no result");
		return NULL;
	}

	return json_array_ref (result);
}

JsonArray *
gs_snapd_find (gchar **values,
	       GCancellable *cancellable, GError **error)
{
	g_autoptr(GString) path = NULL;
	g_autofree gchar *query = NULL;
	g_autofree gchar *escaped = NULL;
	guint status_code;
	g_autofree gchar *reason_phrase = NULL;
	g_autofree gchar *response_type = NULL;
	g_autofree gchar *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;
	JsonArray *result;

	path = g_string_new ("/v2/find?q=");
	query = g_strjoinv (" ", values);
	escaped = soup_uri_encode (query, NULL);
	g_string_append (path, escaped);
	if (!send_request ("GET", path->str, NULL,
			   TRUE, NULL, NULL,
			   TRUE, NULL, NULL,
			   &status_code, &reason_phrase,
			   &response_type, &response, NULL,
			   cancellable, error))
		return NULL;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %u: %s",
			     status_code, reason_phrase);
		return NULL;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return NULL;
	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_array_member (root, "result");
	if (result == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned no result");
		return NULL;
	}

	return json_array_ref (result);
}

JsonObject *
gs_snapd_get_interfaces (GCancellable *cancellable, GError **error)
{
	guint status_code;
	g_autofree gchar *reason_phrase = NULL;
	g_autofree gchar *response_type = NULL;
	g_autofree gchar *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;
	JsonObject *result;

	if (!send_request ("GET", "/v2/interfaces", NULL,
			   TRUE, NULL, NULL,
			   TRUE, NULL, NULL,
			   &status_code, &reason_phrase,
			   &response_type, &response, NULL,
			   cancellable, error))
		return NULL;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %u: %s",
			     status_code, reason_phrase);
		return NULL;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return NULL;
	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_object_member (root, "result");
	if (result == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned no result");
		return NULL;
	}

	return json_object_ref (result);
}

static JsonObject *
get_changes (const gchar *macaroon, gchar **discharges,
	     const gchar *change_id,
	     GCancellable *cancellable, GError **error)
{
	g_autofree gchar *path = NULL;
	guint status_code;
	g_autofree gchar *reason_phrase = NULL;
	g_autofree gchar *response_type = NULL;
	g_autofree gchar *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root, *result;

	path = g_strdup_printf ("/v2/changes/%s", change_id);
	if (!send_request ("GET", path, NULL,
			   TRUE, macaroon, discharges,
			   TRUE, NULL, NULL,
			   &status_code, &reason_phrase,
			   &response_type, &response, NULL,
			   cancellable, error))
		return NULL;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %u: %s",
			     status_code, reason_phrase);
		return NULL;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return NULL;
	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_object_member (root, "result");
	if (result == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned no result");
		return NULL;
	}

	return json_object_ref (result);
}

static gboolean
send_package_action (const gchar *name,
		     const gchar *action,
		     GsSnapdProgressCallback callback,
		     gpointer user_data,
		     GCancellable *cancellable,
		     GError **error)
{
	g_autofree gchar *content = NULL, *path = NULL;
	guint status_code;
	g_autofree gchar *macaroon = NULL;
	g_auto(GStrv) discharges = NULL;
	g_autofree gchar *reason_phrase = NULL;
	g_autofree gchar *response_type = NULL;
	g_autofree gchar *response = NULL;
	g_autofree gchar *status = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root, *result;
	const gchar *type;

	content = g_strdup_printf ("{\"action\": \"%s\"}", action);
	path = g_strdup_printf ("/v2/snaps/%s", name);
	if (!send_request ("POST", path, content,
			   TRUE, NULL, NULL,
			   TRUE, &macaroon, &discharges,
			   &status_code, &reason_phrase,
			   &response_type, &response, NULL,
			   cancellable, error))
		return FALSE;

	if (status_code != SOUP_STATUS_ACCEPTED) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %u: %s",
			     status_code, reason_phrase);
		return FALSE;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return FALSE;

	root = json_node_get_object (json_parser_get_root (parser));
	type = json_object_get_string_member (root, "type");

	if (g_strcmp0 (type, "async") == 0) {
		const gchar *change_id;

		change_id = json_object_get_string_member (root, "change");

		while (TRUE) {
			/* Wait for a little bit before polling */
			g_usleep (100 * 1000);

			result = get_changes (macaroon, discharges, change_id, cancellable, error);
			if (result == NULL)
				return FALSE;

			status = g_strdup (json_object_get_string_member (result, "status"));

			if (g_strcmp0 (status, "Done") == 0)
				break;

			callback (result, user_data);
		}
	}

	if (g_strcmp0 (status, "Done") != 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd operation finished with status %s", status);
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_snapd_install (const gchar *name,
		  GsSnapdProgressCallback callback, gpointer user_data,
		  GCancellable *cancellable,
		  GError **error)
{
	return send_package_action (name, "install", callback, user_data, cancellable, error);
}

gboolean
gs_snapd_remove (const gchar *name,
		 GsSnapdProgressCallback callback, gpointer user_data,
		 GCancellable *cancellable, GError **error)
{
	return send_package_action (name, "remove", callback, user_data, cancellable, error);
}

gchar *
gs_snapd_get_resource (const gchar *path,
		       gsize *data_length,
		       GCancellable *cancellable, GError **error)
{
	guint status_code;
	g_autofree gchar *reason_phrase = NULL;
	g_autofree gchar *response_type = NULL;
	g_autofree gchar *data = NULL;

	if (!send_request ("GET", path, NULL,
			   TRUE, NULL, NULL,
			   TRUE, NULL, NULL,
			   &status_code, &reason_phrase,
			   NULL, &data, data_length,
			   cancellable, error))
		return NULL;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %u: %s",
			     status_code, reason_phrase);
		return NULL;
	}

	return g_steal_pointer (&data);
}
