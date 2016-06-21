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

#include <gs-plugin.h>
#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include "gs-snapd.h"
#include "gs-ubuntuone.h"

// snapd API documentation is at https://github.com/snapcore/snapd/blob/master/docs/rest.md

#define SNAPD_SOCKET "/run/snapd.socket"

struct GsPluginPrivate {
};

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

	if (!g_file_test (SNAPD_SOCKET, G_FILE_TEST_EXISTS)) {
		g_debug ("disabling '%s' as no %s available",
			 plugin->name, SNAPD_SOCKET);
		gs_plugin_set_enabled (plugin, FALSE);
	}
}

static JsonParser *
parse_result (const gchar *response, const gchar *response_type, GError **error)
{
	g_autoptr(JsonParser) parser = NULL;
	g_autoptr(GError) sub_error = NULL;

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
	if (!json_parser_load_from_data (parser, response, -1, &sub_error)) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to parse snapd response: %s", sub_error->message);
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

static void
refine_app (GsPlugin *plugin, GsApp *app, JsonObject *package)
{
	const gchar *status, *icon_url;
	g_autoptr(GdkPixbuf) icon_pixbuf = NULL;
	gint64 size = -1;

	status = json_object_get_string_member (package, "status");
	if (g_strcmp0 (status, "installed") == 0 || g_strcmp0 (status, "active") == 0) {
		const gchar *update_available;

		update_available = json_object_has_member (package, "update_available") ? json_object_get_string_member (package, "update_available") : NULL;
		if (update_available)
			gs_app_set_state (app, AS_APP_STATE_UPDATABLE);
		else
			gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		size = json_object_get_int_member (package, "installed-size");
	}
	else if (g_strcmp0 (status, "removed") == 0) {
		// A removed app is only available if it can be downloaded (it might have been sideloaded)
		size = json_object_get_int_member (package, "download-size");
		if (size > 0)
			gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	} else if (g_strcmp0 (status, "not installed") == 0 || g_strcmp0 (status, "available") == 0) {
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		size = json_object_get_int_member (package, "download-size");
	}
	gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, json_object_get_string_member (package, "name"));
	gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST, json_object_get_string_member (package, "description"));
	gs_app_set_version (app, json_object_get_string_member (package, "version"));
	if (size > 0)
		gs_app_set_size (app, size);
	gs_app_add_quirk (app, AS_APP_QUIRK_PROVENANCE);
	icon_url = json_object_get_string_member (package, "icon");
	if (g_str_has_prefix (icon_url, "/")) {
		g_autofree gchar *icon_response = NULL;
		gsize icon_response_length;

		if (send_snapd_request ("GET", icon_url, NULL, TRUE, NULL, TRUE, NULL, NULL, NULL, NULL, &icon_response, &icon_response_length, NULL)) {
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
}

static gboolean
get_apps (GsPlugin *plugin, const gchar *sources, gchar **search_terms, GList **list, AppFilterFunc filter_func, gpointer user_data, GError **error)
{
	guint status_code;
	GPtrArray *query_fields;
	g_autoptr (GString) path = NULL;
	g_autofree gchar *reason_phrase = NULL, *response_type = NULL, *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;
	JsonArray *result;
	GList *snaps;
	GList *i;

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
	path = g_string_new ("/v2/snaps");
	if (query_fields->len > 1) {
		g_autofree gchar *fields = NULL;
		g_string_append (path, "?");
		fields = g_strjoinv ("&", (gchar **) query_fields->pdata);
		g_string_append (path, fields);
	}
	g_ptr_array_free (query_fields, TRUE);
	if (!send_snapd_request ("GET", path->str, NULL, TRUE, NULL, TRUE, NULL, &status_code, &reason_phrase, &response_type, &response, NULL, error))
		return FALSE;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %d: %s", status_code, reason_phrase);
		return FALSE;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return FALSE;

	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_array_member (root, "result");
	snaps = json_array_get_elements (result);

	for (i = snaps; i != NULL; i = i->next) {
		JsonObject *package = json_node_get_object (i->data);
		g_autoptr(GsApp) app = NULL;
		const gchar *id;

		id = json_object_get_string_member (package, "name");

		if (filter_func != NULL && !filter_func (id, package, user_data))
			continue;

		app = gs_app_new (id);
		gs_app_set_management_plugin (app, "snappy");
		gs_app_set_origin (app, _("Snappy Store"));
		gs_app_set_kind (app, AS_APP_KIND_DESKTOP);
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_REVIEWABLE);
		gs_app_add_quirk (app, AS_APP_QUIRK_NOT_LAUNCHABLE);
		refine_app (plugin, app, package);
		gs_plugin_add_app (list, app);
	}

	g_list_free (snaps);

	return TRUE;
}

static gboolean
get_app (GsPlugin *plugin, GsApp *app, GError **error)
{
	guint status_code;
	g_autofree gchar *path = NULL, *reason_phrase = NULL, *response_type = NULL, *response = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root, *result;

	path = g_strdup_printf ("/v2/snaps/%s", gs_app_get_id (app));
	if (!send_snapd_request ("GET", path, NULL, TRUE, NULL, TRUE, NULL, &status_code, &reason_phrase, &response_type, &response, NULL, error))
		return FALSE;

	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %d: %s", status_code, reason_phrase);
		return FALSE;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return FALSE;
	root = json_node_get_object (json_parser_get_root (parser));
	result = json_object_get_object_member (root, "result");
	if (result == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned no results for %s", gs_app_get_id (app));
		return FALSE;
	}

	refine_app (plugin, app, result);

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

gboolean
gs_plugin_refine (GsPlugin *plugin,
		  GList **list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	GList *link;

	for (link = *list; link; link = link->next) {
		GsApp *app = link->data;

		if (g_strcmp0 (gs_app_get_management_plugin (app), "snappy") != 0)
			continue;

		// Get info from snapd
		if (!get_app (plugin, app, error))
			return FALSE;
	}

	return TRUE;
}

static gboolean
send_package_action (GsPlugin *plugin, GsApp *app, const char *id, const gchar *action, GError **error)
{
	g_autofree gchar *content = NULL, *path = NULL;
	guint status_code;
	g_autofree gchar *reason_phrase = NULL, *response_type = NULL, *response = NULL, *status = NULL;
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root, *result, *task, *progress;
	JsonArray *tasks;
	GList *task_list, *l;
	gint64 done, total, task_done, task_total;
        const gchar *resource_path;
	const gchar *type;
	const gchar *change_id;
	g_autoptr(GVariant) macaroon = NULL;

	content = g_strdup_printf ("{\"action\": \"%s\"}", action);
	path = g_strdup_printf ("/v2/snaps/%s", id);
	if (!send_snapd_request ("POST", path, content, TRUE, NULL, TRUE, &macaroon, &status_code, &reason_phrase, &response_type, &response, NULL, error))
		return FALSE;

	if (status_code != SOUP_STATUS_ACCEPTED) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "snapd returned status code %d: %s", status_code, reason_phrase);
		return FALSE;
	}

	parser = parse_result (response, response_type, error);
	if (parser == NULL)
		return FALSE;

	root = json_node_get_object (json_parser_get_root (parser));
	type = json_object_get_string_member (root, "type");

	if (g_strcmp0 (type, "async") == 0) {
		change_id = json_object_get_string_member (root, "change");
		resource_path = g_strdup_printf ("/v2/changes/%s", change_id);

		while (TRUE) {
			g_autofree gchar *status_reason_phrase = NULL, *status_response_type = NULL, *status_response = NULL;
			g_autoptr(JsonParser) status_parser = NULL;

			/* Wait for a little bit before polling */
			g_usleep (100 * 1000);

			if (!send_snapd_request ("GET", resource_path, NULL, TRUE, macaroon, TRUE, NULL,
						 &status_code, &status_reason_phrase, &status_response_type,
						 &status_response, NULL, error)) {
				return FALSE;
			}

			if (status_code != SOUP_STATUS_OK) {
				g_set_error (error,
					     GS_PLUGIN_ERROR,
					     GS_PLUGIN_ERROR_FAILED,
					     "snapd returned status code %d: %s", status_code, status_reason_phrase);
				return FALSE;
			}

			status_parser = parse_result (status_response, status_response_type, error);
			if (status_parser == NULL)
				return FALSE;

			root = json_node_get_object (json_parser_get_root (status_parser));
			result = json_object_get_object_member (root, "result");

			g_free (status);
			status = g_strdup (json_object_get_string_member (result, "status"));

			if (g_strcmp0 (status, "Done") == 0)
				break;

			tasks = json_object_get_array_member (result, "tasks");
			task_list = json_array_get_elements (tasks);

			done = 0;
			total = 0;

			for (l = task_list; l != NULL; l = l->next) {
				task = json_node_get_object (l->data);
				progress = json_object_get_object_member (task, "progress");
				task_done = json_object_get_int_member (progress, "done");
				task_total = json_object_get_int_member (progress, "total");

				done += task_done;
				total += task_total;
			}

			gs_plugin_progress_update (plugin, app, 100 * done / total);

			g_list_free (task_list);
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
	result = send_package_action (plugin, app, gs_app_get_id (app), "install", error);
	if (result)
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	else
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

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
	result = send_package_action (plugin, app, gs_app_get_id (app), "remove", error);
	if (result)
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	else
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);

	return result;
}
