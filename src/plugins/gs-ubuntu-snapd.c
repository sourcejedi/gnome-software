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
#include "gs-ubuntu-snapd.h"
#include "gs-ubuntuone.h"

#define SNAPD_SOCKET_PATH "/run/snapd.socket"

// snapd API documentation is at https://github.com/ubuntu-core/snappy/blob/master/docs/rest.md

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

gboolean
send_snapd_request (const gchar  *method,
		    const gchar  *path,
		    const gchar  *content,
		    gboolean      authenticate,
		    GVariant     *macaroon,
		    gboolean      retry_after_login,
		    GVariant    **out_macaroon,
		    guint        *status_code,
		    gchar       **reason_phrase,
		    gchar       **response_type,
		    gchar       **response,
		    gsize        *response_length,
		    GError      **error)
{
	g_autoptr (GSocket) socket = NULL;
	g_autoptr (GString) request = NULL;
	gssize n_written;
	gsize max_data_length = 65535, data_length = 0, header_length;
	gchar data[max_data_length + 1], *body = NULL;
	g_autoptr (SoupMessageHeaders) headers = NULL;
	g_autoptr(GVariant) auto_macaroon = NULL;
	gsize chunk_length, n_required;
	gchar *chunk_start = NULL;
	const gchar *root;
	const gchar *discharge;
	GVariantIter *iter;
	guint code;
	gboolean ret;

	if (macaroon == NULL && authenticate) {
		auto_macaroon = gs_ubuntuone_get_macaroon (TRUE, FALSE, NULL);
		macaroon = auto_macaroon;
	}

	// NOTE: Would love to use libsoup but it doesn't support unix sockets
	// https://bugzilla.gnome.org/show_bug.cgi?id=727563

	socket = open_snapd_socket (error);

	if (socket == NULL)
		return FALSE;

	request = g_string_new ("");
	g_string_append_printf (request, "%s %s HTTP/1.1\r\n", method, path);
	g_string_append (request, "Host:\r\n");
	if (macaroon != NULL) {
		g_variant_get (macaroon, "(&sas)", &root, &iter);
		g_string_append_printf (request, "Authorization: Macaroon root=\"%s\"", root);

		while (g_variant_iter_next (iter, "&s", &discharge))
			g_string_append_printf (request, ",discharge=\"%s\"", discharge);

		g_variant_iter_free (iter);
		g_string_append (request, "\r\n");
	}
	if (content)
		g_string_append_printf (request, "Content-Length: %zi\r\n", strlen (content));
	g_string_append (request, "\r\n");
	if (content)
		g_string_append (request, content);

	if (g_strcmp0 (g_getenv ("GNOME_SOFTWARE_SNAPPY"), "debug") == 0)
		g_print ("===== begin snapd request =====\n%s\n===== end snapd request =====\n", request->str);

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
	if (!soup_headers_parse_response (data, header_length, headers, NULL, &code, reason_phrase)) {
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

		macaroon = gs_ubuntuone_get_macaroon (FALSE, TRUE, NULL);

		if (macaroon == NULL) {
			g_set_error_literal (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_FAILED,
					     "failed to authenticate");
			return FALSE;
		}

		ret = send_snapd_request (method,
					  path,
					  content,
					  TRUE,
					  macaroon,
					  FALSE,
					  NULL,
					  status_code,
					  reason_phrase,
					  response_type,
					  response,
					  response_length,
					  error);

		if (ret && out_macaroon != NULL) {
			*out_macaroon = macaroon;
		} else {
			g_variant_unref (macaroon);
		}

		return ret;
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

	if (out_macaroon != NULL)
		*out_macaroon = g_variant_ref (macaroon);
	if (response_type)
		*response_type = g_strdup (soup_message_headers_get_one (headers, "Content-Type"));
	if (response) {
		*response = g_malloc (chunk_length + 2);
		memcpy (*response, chunk_start, chunk_length + 1);
		(*response)[chunk_length + 1] = '\0';

		if (g_strcmp0 (g_getenv ("GNOME_SOFTWARE_SNAPPY"), "debug") == 0)
			g_info ("===== begin snapd response =====\nStatus %u\n%s\n===== end snapd response =====\n", code, *response);
	}
	if (response_length)
		*response_length = chunk_length;

	return TRUE;
}
