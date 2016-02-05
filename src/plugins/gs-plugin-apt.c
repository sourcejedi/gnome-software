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

#include <config.h>

#include <stdlib.h>
#include <string.h>
#include <gs-plugin.h>

struct GsPluginPrivate {
};

const gchar *
gs_plugin_get_name (void)
{
	return "apt";
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
}

static GsApp *
find_app (GList **list, const gchar *source)
{
	GList *link;

	for (link = *list; link; link = link->next) {
		GsApp *app = GS_APP (link->data);
		if (g_strcmp0 (gs_app_get_source_default (app), source) == 0)
			return app;
	}

	return NULL;
}

static void
parse_package_info (const gchar *info, GsPluginRefineFlags flags, GList **list, gboolean mark_available)
{
	gchar **lines;
	gint i;
	GsApp *app = NULL;
	const gchar *package_prefix = "Package: ";
	const gchar *status_prefix = "Status: ";
	const gchar *installed_size_prefix = "Installed-Size: ";
	const gchar *version_prefix = "Version: ";

	lines = g_strsplit (info, "\n", -1);
	for (app = NULL, i = 0; lines[i]; i++) {
		if (g_str_has_prefix (lines[i], package_prefix)) {
			app = find_app (list, lines[i] + strlen (package_prefix));
			if (app && mark_available && gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
				gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		}

		// Skip other fields until we find an app we know
		if (!app)
			continue;

		if (g_str_has_prefix (lines[i], status_prefix)) {
			if (g_str_has_suffix (lines[i] + strlen (status_prefix), " installed"))
				gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		} else if (g_str_has_prefix (lines[i], installed_size_prefix)) {
			if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) != 0 && gs_app_get_size (app) == 0)
				gs_app_set_size (app, atoi (lines[i] + strlen (installed_size_prefix)) * 1024);
		} else if (g_str_has_prefix (lines[i], version_prefix)) {
			// FIXME: apt-cache contains multiple versions - pick the best
			if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION) != 0 && gs_app_get_version (app) == NULL)
				gs_app_set_version (app, lines[i] + strlen (version_prefix));
		}
	}

	g_strfreev (lines);
}

gboolean
gs_plugin_refine (GsPlugin *plugin,
		  GList **list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	GList *link;
	GPtrArray *dpkg_argv_array, *cache_argv_array;
	gboolean known_apps = FALSE;
	g_autofree gchar **dpkg_argv = NULL, **cache_argv = NULL, *dpkg_stdout = NULL, *cache_stdout = NULL, *dpkg_stderr = NULL, *cache_stderr = NULL;

	/*g_printerr ("APT: gs_plugin_refine");
	for (link = *list; link; link = link->next) {
		GsApp *app = GS_APP (link->data);
		g_printerr (" %s", gs_app_get_id (app));
	}
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENCE) != 0)
		g_printerr (" LICENCE");
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) != 0)
		g_printerr (" SIZE");
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION) != 0)
		g_printerr (" VERSION");
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_HISTORY) != 0)
		g_printerr (" HISTORY");
	if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS) != 0)
		g_printerr (" UPDATE_DETAILS");
	g_printerr ("\n");*/

	// Get the information from the cache
	dpkg_argv_array = g_ptr_array_new ();
	g_ptr_array_add (dpkg_argv_array, (gpointer) "dpkg");
	g_ptr_array_add (dpkg_argv_array, (gpointer) "--status");
	cache_argv_array = g_ptr_array_new ();
	g_ptr_array_add (cache_argv_array, (gpointer) "apt-cache");
	g_ptr_array_add (cache_argv_array, (gpointer) "show");
	for (link = *list; link; link = link->next) {
		GsApp *app = GS_APP (link->data);
		const gchar *source;

		source = gs_app_get_source_default (app);
		if (!source)
			continue;

		known_apps = TRUE;
		g_ptr_array_add (dpkg_argv_array, (gpointer) source);
		g_ptr_array_add (cache_argv_array, (gpointer) source);
	}
	g_ptr_array_add (dpkg_argv_array, NULL);
	dpkg_argv = (gchar **) g_ptr_array_free (dpkg_argv_array, FALSE);
	g_ptr_array_add (cache_argv_array, NULL);
	cache_argv = (gchar **) g_ptr_array_free (cache_argv_array, FALSE);

	if (!known_apps)
		return TRUE;

	if (!g_spawn_sync (NULL, dpkg_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &dpkg_stdout, &dpkg_stderr, NULL, error))
		return FALSE;
	if (!g_spawn_sync (NULL, cache_argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &cache_stdout, &cache_stderr, NULL, error))
		return FALSE;

	parse_package_info (dpkg_stdout, flags, list, FALSE);
	parse_package_info (cache_stdout, flags, list, TRUE);

	return TRUE;
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	g_autofree gchar *output = NULL;
	gint exit_status;
	gchar **lines = NULL;
	int i;

	g_printerr ("APT: gs_plugin_add_installed\n");

	if (!g_spawn_command_line_sync ("dpkg --get-selections", &output, NULL, &exit_status, error))
		return FALSE;
	if (exit_status != EXIT_SUCCESS) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "dpkg --get-selections returned status %d", exit_status);
		return FALSE;
	}

	lines = g_strsplit (output, "\n", -1);
	for (i = 0; lines[i]; i++) {
		g_autoptr(GsApp) app = NULL;
		gchar *id, *status, *c;

		// Line is the form <name>\t<status> - find the status and only use installed packages
		status = strrchr (lines[i], '\t');
		if (!status)
			continue;
		status++;
		if (strcmp (status, "install") != 0)
			continue;

		// Split out name
		c = strchr (lines[i], '\t');
		*c = '\0';
		id = lines[i];

		app = gs_app_new (id);
		gs_app_add_source (app, id);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		gs_plugin_add_app (list, app);
	}
	g_strfreev (lines);

	return TRUE;
}

typedef struct
{
	GMainLoop *loop;
	GsApp *app;
	gchar **result;
} TransactionData;

static void
transaction_property_changed_cb (GDBusConnection *connection,
				 const gchar *sender_name,
				 const gchar *object_path,
				 const gchar *interface_name,
				 const gchar *signal_name,
				 GVariant *parameters,
				 gpointer user_data)
{
	TransactionData *data = user_data;
	const gchar *name;
	g_autoptr(GVariant) value = NULL;

	g_variant_get (parameters, "(&sv)", &name, &value);
	if (data->app && strcmp (name, "Progress") == 0)
		gs_app_set_progress (data->app, g_variant_get_int32 (value));
}

static void
transaction_finished_cb (GDBusConnection *connection,
			 const gchar *sender_name,
			 const gchar *object_path,
			 const gchar *interface_name,
			 const gchar *signal_name,
			 GVariant *parameters,
			 gpointer user_data)
{
	TransactionData *data = user_data;

	g_variant_get (parameters, "(s)", data->result);

	g_main_loop_quit (data->loop);
}

static gboolean
aptd_transaction (const gchar *method, GsApp *app, GError **error)
{
	g_autoptr(GDBusConnection) conn = NULL;
	GVariant *parameters;
	g_autoptr(GVariant) result = NULL;
	g_autofree gchar *transaction_path = NULL, *transaction_result = NULL;
	g_autoptr(GMainLoop) loop = NULL;
	guint property_signal, finished_signal;
	TransactionData data;

	conn = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
	if (conn == NULL)
		return FALSE;

	if (app) {
		GVariantBuilder builder;
		g_variant_builder_init (&builder, G_VARIANT_TYPE ("as")),
		g_variant_builder_add (&builder, "s", gs_app_get_source_default (app));
		parameters = g_variant_new ("(as)", &builder);
	}
	else
		parameters = g_variant_new ("()");
	result = g_dbus_connection_call_sync (conn,
					      "org.debian.apt",
					      "/org/debian/apt",
					      "org.debian.apt",
					      method,
					      parameters,
					      G_VARIANT_TYPE ("(s)"),
					      G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      NULL,
					      error);
	if (!result)
		return FALSE;
	g_variant_get (result, "(s)", &transaction_path);
	g_variant_unref (result);

	loop = g_main_loop_new (NULL, FALSE);

	data.app = app;
	data.loop = loop;
	data.result = &transaction_result;
	property_signal = g_dbus_connection_signal_subscribe (conn,
	                                                      "org.debian.apt",
	                                                      "org.debian.apt.transaction",
	                                                      "PropertyChanged",
	                                                      transaction_path,
	                                                      NULL,
	                                                      G_DBUS_SIGNAL_FLAGS_NONE,
	                                                      transaction_property_changed_cb,
	                                                      &data,
	                                                      NULL);
	finished_signal = g_dbus_connection_signal_subscribe (conn,
	                                                      "org.debian.apt",
	                                                      "org.debian.apt.transaction",
	                                                      "Finished",
	                                                      transaction_path,
	                                                      NULL,
	                                                      G_DBUS_SIGNAL_FLAGS_NONE,
	                                                      transaction_finished_cb,
	                                                      &data,
	                                                      NULL);
	result = g_dbus_connection_call_sync (conn,
					      "org.debian.apt",
					      transaction_path,
					      "org.debian.apt.transaction",
					      "Run",
					      g_variant_new ("()"),
					      G_VARIANT_TYPE ("()"),
					      G_DBUS_CALL_FLAGS_NONE,
					      -1,
					      NULL,
					      error);
	if (!result)
		return FALSE;

	g_main_loop_run (loop);
	g_dbus_connection_signal_unsubscribe (conn, property_signal);
	g_dbus_connection_signal_unsubscribe (conn, finished_signal);

	if (g_strcmp0 (transaction_result, "exit-success") != 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "apt trasaction returned result %s", transaction_result);
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
	//g_printerr ("APT: gs_plugin_app_install\n");

	if (!gs_app_get_source_default (app))
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	if (aptd_transaction ("InstallPackages", app, error))
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	else {
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	//g_printerr ("APT: gs_plugin_app_remove\n");

	if (!gs_app_get_source_default (app))
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	if (aptd_transaction ("RemovePackages", app, error))
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
	else {
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_refresh (GsPlugin *plugin,
		   guint cache_age,
		   GsPluginRefreshFlags flags,
		   GCancellable *cancellable,
		   GError **error)
{
	//g_printerr ("APT: gs_plugin_refresh\n");

	if ((flags & GS_PLUGIN_REFRESH_FLAGS_UPDATES) == 0)
		return TRUE;

	return aptd_transaction ("UpdateCache", NULL, error);
}
