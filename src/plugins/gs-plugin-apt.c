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
#include <ctype.h>
#include <gs-plugin.h>

struct GsPluginPrivate {
	gboolean cache_loaded;
	GList *dpkg_cache;
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

// Ordering of symbols in dpkg ~, 0-9, A-Z, a-z, everything else (ASCII ordering)
static int
order (int c)
{
	if (isdigit(c))
		return 0;
	else if (isalpha(c))
		return c;
	else if (c == '~')
		return -1;
	else if (c)
		return c + 256;
	else
		return 0;
}

// Check if this is a valid dpkg character ('-' terminates version for revision)
static gboolean
valid_char (char c)
{
	return c != '\0' && c != '-';
}

static int
compare_version (const char *v0, const char *v1)
{
	while (valid_char (*v0) || valid_char (*v1))
	{
		int first_diff = 0;

		// Compare non-digits based on ordering rules
		while ((valid_char (*v0) && !isdigit (*v0)) || (valid_char (*v1) && !isdigit (*v1))) {
			int ac = order (*v0);
			int bc = order (*v1);

			if (ac != bc)
				return ac - bc;

			v0++;
			v1++;
		}

		// Skip leading zeroes
		while (*v0 == '0')
			v0++;
		while (*v1 == '0')
			v1++;

		// Compare numbers - longest wins, otherwise compare next digit
		while (isdigit (*v0) && isdigit (*v1)) {
			if (first_diff == 0)
				first_diff = *v0 - *v1;
			v0++;
			v1++;
		}
		if (isdigit (*v0))
			return 1;
		if (isdigit (*v1))
			return -1;
		if (first_diff)
			return first_diff;
	}

	return 0;
}

// Get the epoch, i.e. 1:2.3-4 -> '1'
static int
get_epoch (const gchar *version)
{
	if (strchr (version, ':') == NULL)
		return 0;
	return atoi (version);
}

// Get the version after the epoch, i.e. 1:2.3-4 -> '2.3'
static const gchar *
get_version (const gchar *version)
{
	const gchar *v = strchr (version, ':');
	return v ? v : version;
}

// Get the debian revision, i.e. 1:2.3-4 -> '4'
static const gchar *
get_revision (const gchar *version)
{
	const gchar *v = strchr (version, '-');
	return v ? v + 1: version + strlen (version);
}

static int
compare_dpkg_version (const gchar *v0, const gchar *v1)
{
	int r;

	r = get_epoch (v0) - get_epoch (v1);
	if (r != 0)
		return r;

	r = compare_version (get_version (v0), get_version (v1));
	if (r != 0)
		return r;

	return compare_version (get_revision (v0), get_revision (v1));
}

static gboolean
version_newer (const gchar *v0, const gchar *v1)
{
	return v0 ? compare_dpkg_version (v0, v1) < 0 : TRUE;
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
		if (app == NULL)
			continue;

		if (g_str_has_prefix (lines[i], status_prefix)) {
			if (g_str_has_suffix (lines[i] + strlen (status_prefix), " installed") && gs_app_get_state (app) == AS_APP_STATE_UNKNOWN)
				gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		} else if (g_str_has_prefix (lines[i], installed_size_prefix) && (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) != 0) {
			if (gs_app_get_size (app) == 0)
				gs_app_set_size (app, atoi (lines[i] + strlen (installed_size_prefix)) * 1024);
		} else if (g_str_has_prefix (lines[i], version_prefix) && (flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION) != 0) {
			const gchar *version = lines[i] + strlen (version_prefix);

			if (gs_app_get_version (app) == NULL)
				gs_app_set_version (app, version);
			if ((gs_app_get_state (app) == AS_APP_STATE_INSTALLED ||gs_app_get_state (app) == AS_APP_STATE_UPDATABLE_LIVE) && version_newer (gs_app_get_update_version (app), version)) {
				if (gs_app_get_state (app) == AS_APP_STATE_INSTALLED)
					gs_app_set_state (app, AS_APP_STATE_UNKNOWN);
				gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
				gs_app_set_update_version (app, version);
			}
		}
	}

	g_strfreev (lines);
}

static gboolean
refine (GsPlugin *plugin,
        GList **list,
        GsPluginRefineFlags flags,
        GCancellable *cancellable,
        GError **error)
{
	GList *link;
	GPtrArray *dpkg_argv_array, *cache_argv_array;
	gboolean known_apps = FALSE;
	g_autofree gchar **dpkg_argv = NULL, **cache_argv = NULL, *dpkg_stdout = NULL, *cache_stdout = NULL, *dpkg_stderr = NULL, *cache_stderr = NULL;

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
		if (source == NULL)
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
gs_plugin_refine (GsPlugin *plugin,
		  GList **list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	// NOTE: Had to put into a static function so can be called from inside plugin - not sure why
	return refine (plugin, list, flags, cancellable, error);
}

static gchar **
get_installed (GError **error)
{
	g_autofree gchar *dpkg_stdout = NULL, *dpkg_stderr = NULL;
	gint exit_status;
	gchar *argv[3] = { (gchar *) "dpkg", (gchar *) "--get-selections", NULL }, **lines;
	int i;
	GPtrArray *array;

	if (!g_spawn_sync (NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &dpkg_stdout, &dpkg_stderr, &exit_status, error))
		return FALSE;
	if (exit_status != EXIT_SUCCESS) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "dpkg --get-selections returned status %d", exit_status);
		return FALSE;
	}

	array = g_ptr_array_new ();
	lines = g_strsplit (dpkg_stdout, "\n", -1);
	for (i = 0; lines[i]; i++) {
		g_autoptr(GsApp) app = NULL;
		gchar *status, *c;

		// Line is the form <name>\t<status> - find the status and only use installed packages
		status = strrchr (lines[i], '\t');
		if (status == NULL)
			continue;
		status++;
		if (strcmp (status, "install") != 0)
			continue;

		// Split out name
		c = strchr (lines[i], '\t');
		*c = '\0';
		g_ptr_array_add (array, (gpointer) g_strdup (lines[i]));
	}
	g_strfreev (lines);
	g_ptr_array_add (array, NULL);

	return (gchar **) g_ptr_array_free (array, FALSE);
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	gchar **installed;
	int i;

	installed = get_installed (error);
	if (installed == NULL)
		return FALSE;

	for (i = 0; installed[i] != NULL; i++) {
		g_autoptr(GsApp) app = NULL;

		app = gs_app_new (installed[i]);
		// FIXME: Since appstream marks all packages as owned by PackageKit and we are replacing PackageKit we need to accept those packages
		gs_app_set_management_plugin (app, "PackageKit");
		gs_app_add_source (app, installed[i]);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
		gs_plugin_add_app (list, app);
	}
	g_strfreev (installed);

	return TRUE;
}

typedef struct
{
	GsPlugin *plugin;
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
		gs_plugin_progress_update (data->plugin, data->app, g_variant_get_int32 (value));
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
aptd_transaction (GsPlugin *plugin, const gchar *method, GsApp *app, GError **error)
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
	if (result == NULL)
		return FALSE;
	g_variant_get (result, "(s)", &transaction_path);
	g_variant_unref (result);

	loop = g_main_loop_new (NULL, FALSE);

	data.plugin = plugin;
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
	if (result == NULL)
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

static gboolean
app_is_ours (GsApp *app)
{
	// FIXME: Since appstream marks all packages as owned by PackageKit and we are replacing PackageKit we need to accept those packages
	return g_strcmp0 (gs_app_get_management_plugin (app), "PackageKit") == 0;
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	if (!app_is_ours (app))
		return TRUE;

	if (gs_app_get_source_default (app) == NULL)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	if (aptd_transaction (plugin, "InstallPackages", app, error))
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
	if (!app_is_ours (app))
		return TRUE;

	if (gs_app_get_source_default (app) == NULL)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_REMOVING);
	if (aptd_transaction (plugin, "RemovePackages", app, error))
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
	if ((flags & GS_PLUGIN_REFRESH_FLAGS_UPDATES) == 0)
		return TRUE;

	return aptd_transaction (plugin, "UpdateCache", NULL, error);
}

gboolean
gs_plugin_add_updates (GsPlugin *plugin,
                       GList **list,
                       GCancellable *cancellable,
                       GError **error)
{
	GList *installed = NULL, *link;

	// Get the version of everything installed
	// FIXME: Checks all the packages we don't have appstream data for (so inefficient)
	if (!gs_plugin_add_installed (plugin, &installed, NULL, error) ||
	    !refine (plugin, &installed, GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION, NULL, error)) {
		g_list_free_full (installed, g_object_unref);
		return FALSE;
	}

	for (link = installed; link; link = link->next) {
		GsApp *app = GS_APP (link->data);
		const gchar *v0, *v1;

		v0 = gs_app_get_version (app);
		v1 = gs_app_get_update_version (app);
		if (v0 != NULL && v1 != NULL && version_newer (v0, v1))
			gs_plugin_add_app (list, app);
	}
	g_list_free_full (installed, g_object_unref);

	return TRUE;
}

gboolean
gs_plugin_app_update (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	if (!app_is_ours (app))
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);
	if (aptd_transaction (plugin, "UpgradePackages", app, error))
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	else {
		gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
		return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_launch (GsPlugin *plugin,
		  GsApp *app,
		  GCancellable *cancellable,
		  GError **error)
{
	if (!app_is_ours (app))
		return TRUE;

	return gs_plugin_app_launch (plugin, app, error);
}
