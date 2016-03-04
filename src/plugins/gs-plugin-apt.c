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
#include <gs-utils.h>

typedef struct {
	gchar *name;
	gchar *installed_version;
	gchar *update_version;
	gint installed_size;
	gboolean is_official;
	gboolean is_open_source;
} PackageInfo;


#include "ubuntu-unity-launcher-proxy.h"

struct GsPluginPrivate {
	gsize		 loaded;
	GHashTable	*package_info;
	GList 		*installed_packages;
	GList 		*updatable_packages;
};

const gchar *
gs_plugin_get_name (void)
{
	return "apt";
}

const gchar **
gs_plugin_get_deps (GsPlugin *plugin)
{
	static const gchar *deps[] = {
		"appstream",		/* need pkgname */
		NULL };
	return deps;
}

static void
free_package_info (gpointer data)
{
	PackageInfo *info = data;
	g_free (info->name);
	g_free (info->installed_version);
	g_free (info->update_version);
	g_slice_free (PackageInfo, info);
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
	plugin->priv->package_info = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, free_package_info);
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	g_hash_table_unref (plugin->priv->package_info);
	g_list_free (plugin->priv->installed_packages);
	g_list_free (plugin->priv->updatable_packages);
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
			int o0 = order (*v0);
			int o1 = order (*v1);

			if (o0 != o1)
				return o0 - o1;

			v0++;
			v1++;
		}

		// Skip leading zeroes
		while (*v0 == '0')
			v0++;
		while (*v1 == '0')
			v1++;

		// Compare numbers - longest wins, otherwise compare first digit
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

typedef gboolean (*PackageFileFunc) (const gchar *name, gsize name_length, const gchar *value, gsize value_length, gpointer user_data, GError **error);

static void
skip_to_eol (gchar *data, gsize data_length, gsize *offset)
{
	while (*offset < data_length && data[*offset] != '\n')
		(*offset)++;
	if (*offset >= data_length)
		return;
	(*offset)++;
}

static gboolean
parse_package_file_data (gchar *data, gsize data_length, PackageFileFunc callback, gpointer user_data, GError **error)
{
	gsize offset;

	for (offset = 0; offset < data_length; ) {
		gsize name_start, name_end, name_length;
		gsize value_start, value_end, value_length;

		// Entry divided by empty space
		if (data[offset] == '\n') {
			offset++;
			continue;
		}

		// Line continuations start with a space
		if (data[offset] == ' ') {
			skip_to_eol (data, data_length, &offset);
			continue;
		}

		// Find the field in the form "name: value"
		name_start = offset;
		name_end = name_start + 1;
		while (name_end < data_length && !(data[name_end] == ':' || data[name_end] == '\n'))
			name_end++;
		if ((name_end + 1) >= data_length)
			return TRUE;
		if (data[name_end] != ':') {
			offset = name_end;
			skip_to_eol (data, data_length, &offset);
		}
		value_start = name_end + 1;
		while (value_start < data_length && data[value_start] == ' ')
			value_start++;
		value_end = value_start + 1;
		while (value_end < data_length && data[value_end] != '\n')
			value_end++;
		if (value_end >= data_length)
			return TRUE;
		name_length = name_end - name_start;
		value_length = value_end - value_start;

		if (!callback (data + name_start, name_length, data + value_start, value_length, user_data, error))
			return FALSE;
		offset = value_end + 1;
	}

	return TRUE;
}

static gboolean
parse_package_file (const gchar *filename, PackageFileFunc callback, gpointer user_data, GError **error)
{
	g_autoptr(GMappedFile) f = NULL;

	f = g_mapped_file_new (filename, FALSE, NULL);
	if (f == NULL)
		return FALSE;
	return parse_package_file_data (g_mapped_file_get_contents (f), g_mapped_file_get_length (f), callback, user_data, error);
}

typedef struct {
	GsPlugin *plugin;
	PackageInfo *current_info;
	gboolean current_installed;
	gboolean is_official;
	gboolean is_open_source;
} FieldData;

static gboolean
field_cb (const gchar *name, gsize name_length, const gchar *value, gsize value_length, gpointer user_data, GError **error)
{
	FieldData *data = user_data;

	if (strncmp (name, "Package", name_length) == 0) {
		gchar *id = g_strndup (value, value_length);
		data->current_info = g_hash_table_lookup (data->plugin->priv->package_info, id);
		if (data->current_info == NULL) {
			data->current_info = g_slice_new0 (PackageInfo);
			data->current_installed = FALSE;
			data->current_info->name = id;
			g_hash_table_insert (data->plugin->priv->package_info, data->current_info->name, data->current_info);
		} else
			g_free (id);

		// If an official source provides this package, then mark it as such.
		// NOTE: A PPA could re-use the name and make this not true
		if (data->is_official)
			data->current_info->is_official = data->is_official;
		if (data->is_open_source)
			data->current_info->is_open_source = data->is_open_source;
		return TRUE;
	}

	if (data->current_info == NULL)
		return TRUE;

	if (strncmp (name, "Status", name_length) == 0) {
		if (strncmp (value, "install ok installed", value_length) == 0) {
			data->current_installed = TRUE;
			data->plugin->priv->installed_packages = g_list_append (data->plugin->priv->installed_packages, data->current_info);
		}
	} else if (strncmp (name, "Installed-Size", name_length) == 0) {
		data->current_info->installed_size = atoi (value);
	} else if (strncmp (name, "Version", name_length) == 0) {
		gchar *version = g_strndup (value, value_length);
		if (data->current_installed) {
			g_free (data->current_info->installed_version);
			data->current_info->installed_version = version;
		} else if (version_newer (data->current_info->installed_version, version) && version_newer (data->current_info->update_version, version)) {
			if (data->current_info->installed_version && data->current_info->update_version == NULL)
				data->plugin->priv->updatable_packages = g_list_append (data->plugin->priv->updatable_packages, data->current_info);
			g_free (data->current_info->update_version);
			data->current_info->update_version = version;
		} else
			g_free (version);
	}

	return TRUE;
}

static gboolean
load_db (GsPlugin *plugin, GError **error)
{
	GPtrArray *lists;
	GDir *dir;
	guint i;
	gboolean result = FALSE;

	// Find the package lists to load
	lists = g_ptr_array_new ();
	g_ptr_array_set_free_func (lists, g_free);
	g_ptr_array_add (lists, g_strdup ("/var/lib/dpkg/status"));
	dir = g_dir_open ("/var/lib/apt/lists", 0, NULL);
	while (TRUE) {
		const gchar *name = g_dir_read_name (dir);
		if (name == NULL)
			break;
		if (g_str_has_suffix (name, "_Packages"))
			g_ptr_array_add (lists, g_build_filename ("/var/lib/apt/lists", name, NULL));
	}
	g_dir_close (dir);

	for (i = 0; i < lists->len; i++) {
		const gchar *list = lists->pdata[i];
		FieldData data;
		data.is_official = g_str_has_prefix (list, "/var/lib/apt/lists/archive.ubuntu.com_");
		data.is_open_source = data.is_official && (strstr (list, "_main_") != NULL || strstr (list, "_universe_") != NULL);
		data.plugin = plugin;
		data.current_info = NULL;
		data.current_installed = FALSE;
		result = parse_package_file (list, field_cb, &data, error);
		if (!result)
			goto done;
	}

done:
	g_ptr_array_unref (lists);
	return result;
}

static void
get_changelog (GsPlugin *plugin, GsApp *app)
{
	const gchar *source, *current_version, *version;
	g_autofree gchar *source_prefix = NULL, *uri = NULL, *changelog_prefix = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	guint status_code;
	g_auto(GStrv) lines = NULL;
	int i;
	GString *details;

	g_return_if_fail (gs_app_get_source_default (app) != NULL);
	g_return_if_fail (gs_app_get_update_version (app) != NULL);

	source = gs_app_get_source_default (app);
	if (g_str_has_prefix (source, "lib"))
		source_prefix = g_strdup_printf ("lib%c", source[3]);
	else
		source_prefix = g_strdup_printf ("%c", source[0]);
	current_version = gs_app_get_version (app);
	version = gs_app_get_update_version (app);
	uri = g_strdup_printf ("http://changelogs.ubuntu.com/changelogs/binary/%s/%s/%s/changelog", source_prefix, source, version);
	msg = soup_message_new (SOUP_METHOD_GET, uri);

	status_code = soup_session_send_message (plugin->soup_session, msg);
	if (status_code != SOUP_STATUS_OK) {
		g_warning ("Failed to get changelog for %s version %s from changelogs.ubuntu.com: %s", source, version, soup_status_get_phrase (status_code));
		return;
	}

	// Extract changelog entries newer than our current version
	changelog_prefix = g_strdup_printf ("%s (", source);
	lines = g_strsplit (msg->response_body->data, "\n", -1);
	details = g_string_new ("");
	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = lines[i];
		int version_start, version_end;
		g_autofree gchar *v = NULL;

		// First line is in the form "package (version) distribution(s); urgency=urgency"
		if (!g_str_has_prefix (line, changelog_prefix))
			continue;
		version_start = strlen (changelog_prefix);
		for (version_end = version_start; line[version_end] != '\0' && line[version_end] != ')'; version_end++);
		v = g_strdup_printf ("%.*s", version_end - version_start, line + version_start);

		// We're only interested in new versions
		if (!version_newer (current_version, v))
			break;

		g_string_append_printf (details, "# %s\n\n", v);
		for (i++; lines[i] != NULL; i++) {
			// Last line is in the form " -- maintainer name <email address>  date"
			if (g_str_has_prefix (lines[i], " -- "))
				break;
			if (g_str_has_prefix (lines[i], "  "))
				g_string_append_printf (details, "%s\n\n", lines[i] + 2);
			else
				g_string_append_printf (details, "%s\n\n", lines[i]);
		}
	}

	gs_app_set_update_details (app, details->str);
	g_string_free (details, TRUE);
}

gboolean
gs_plugin_refine (GsPlugin *plugin,
		  GList **list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	GList *link;

	/* Load database once */
	if (g_once_init_enter (&plugin->priv->loaded)) {
		gboolean ret;

		ret = load_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	for (link = *list; link; link = link->next) {
		GsApp *app = link->data;
		PackageInfo *info;

		if (gs_app_get_source_default (app) == NULL)
			continue;

		info = g_hash_table_lookup (plugin->priv->package_info, gs_app_get_source_default (app));
		if (info != NULL) {
			if (gs_app_get_state (app) == AS_APP_STATE_UNKNOWN) {
				if (info->installed_version != NULL) {
					if (info->update_version != NULL) {
						gs_app_set_state (app, AS_APP_STATE_UPDATABLE_LIVE);
					} else {
						gs_app_set_state (app, AS_APP_STATE_INSTALLED);
					}
				} else {
					gs_app_set_state (app, AS_APP_STATE_AVAILABLE);
				}
			}
			if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_SIZE) != 0 && gs_app_get_size (app) == 0) {
				gs_app_set_size (app, info->installed_size * 1024);
			}
			if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_VERSION) != 0) {
				if (info->installed_version != NULL) {
					gs_app_set_version (app, info->installed_version);
				}
				if (info->update_version != NULL) {
					gs_app_set_update_version (app, info->update_version);
				}
			}
			if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_PROVENANCE) != 0 && info->is_official) {
				gs_app_add_quirk (app, AS_APP_QUIRK_PROVENANCE);
			}
			if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENCE) != 0 && info->is_open_source) {
				gs_app_set_licence (app, GS_APP_QUALITY_HIGHEST, "@LicenseRef-ubuntu");
			}
		}

		if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS) != 0) {
			get_changelog (plugin, app);
		}
	}

	return TRUE;
}

gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	GList *link;

	/* Load database once */
	if (g_once_init_enter (&plugin->priv->loaded)) {
		gboolean ret;

		ret = load_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	for (link = plugin->priv->installed_packages; link; link = link->next) {
		PackageInfo *info = link->data;
		g_autoptr(GsApp) app = NULL;

		app = gs_app_new (info->name);
		// FIXME: Since appstream marks all packages as owned by PackageKit and we are replacing PackageKit we need to accept those packages
		gs_app_set_management_plugin (app, "PackageKit");
		gs_app_add_source (app, info->name);
		gs_plugin_add_app (list, app);
	}

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

	if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(sv)"))) {
		g_variant_get (parameters, "(&sv)", &name, &value);
		if (data->app && strcmp (name, "Progress") == 0)
			gs_plugin_progress_update (data->plugin, data->app, g_variant_get_int32 (value));
	} else {
		g_warning ("Unknown parameters in %s.%s: %s", interface_name, signal_name, g_variant_get_type_string (parameters));
	}
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

	if (g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(s)")))
		g_variant_get (parameters, "(s)", data->result);
	else
		g_warning ("Unknown parameters in %s.%s: %s", interface_name, signal_name, g_variant_get_type_string (parameters));

	g_main_loop_quit (data->loop);
}

static void
notify_unity_launcher (GsApp *app, const gchar *transaction_path)
{
	UbuntuUnityLauncher *launcher = NULL;

	g_return_if_fail (GS_IS_APP (app));
	g_return_if_fail (transaction_path);

	launcher = ubuntu_unity_launcher_proxy_new_for_bus_sync (G_BUS_TYPE_SESSION,
		G_DBUS_PROXY_FLAGS_NONE,
		"com.canonical.Unity.Launcher",
		"/com/canonical/Unity/Launcher",
		NULL, NULL);

	g_return_if_fail (launcher);

	ubuntu_unity_launcher_call_add_launcher_item (launcher,
		gs_app_get_id (app),
		transaction_path,
		NULL, NULL, NULL);

	g_object_unref (launcher);
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

	if (g_strcmp0 (method, "InstallFile") == 0)
		parameters = g_variant_new ("(sb)", gs_app_get_origin (app), TRUE);
	else if (app != NULL) {
		GVariantBuilder builder;
		g_variant_builder_init (&builder, G_VARIANT_TYPE ("as")),
		g_variant_builder_add (&builder, "s", gs_app_get_source_default (app));
		parameters = g_variant_new ("(as)", &builder);
	} else
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

	if (!g_strcmp0(method, "InstallPackages"))
		notify_unity_launcher (app, transaction_path);

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
	const gchar *management_plugin = gs_app_get_management_plugin (app);

	// FIXME: Since appstream marks all packages as owned by PackageKit and we are replacing PackageKit we need to accept those packages
	return g_strcmp0 (management_plugin, "PackageKit") == 0 ||
	       g_strcmp0 (management_plugin, "dpkg") == 0;
}

gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	gboolean success = FALSE;

	if (!app_is_ours (app))
		return TRUE;

	if (gs_app_get_source_default (app) == NULL)
		return TRUE;

	gs_app_set_state (app, AS_APP_STATE_INSTALLING);

	if (g_strcmp0 (gs_app_get_management_plugin (app), "PackageKit") == 0)
		success = aptd_transaction (plugin, "InstallPackages", app, error);
	else if (g_strcmp0 (gs_app_get_management_plugin (app), "dpkg") == 0)
		success = aptd_transaction (plugin, "InstallFile", app, error);

	if (success)
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
	else
		gs_app_set_state (app, AS_APP_STATE_AVAILABLE);

	return success;
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
	GList *link;

	/* Load database once */
	if (g_once_init_enter (&plugin->priv->loaded)) {
		gboolean ret;

		ret = load_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	for (link = plugin->priv->updatable_packages; link; link = link->next) {
		PackageInfo *info = link->data;
		g_autoptr(GsApp) app = NULL;

		app = gs_app_new (info->name);
		// FIXME: Since appstream marks all packages as owned by PackageKit and we are replacing PackageKit we need to accept those packages
		gs_app_set_management_plugin (app, "PackageKit");
		gs_app_add_source (app, info->name);
		gs_plugin_add_app (list, app);
	}

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

gboolean
gs_plugin_filename_to_app (GsPlugin      *plugin,
			   GList        **list,
			   const gchar   *filename,
			   GCancellable  *cancellable,
			   GError       **error)
{
	GsApp *app;
	g_autoptr(GFile) file = NULL;
	gchar *argv[5] = { NULL };
	g_autofree gchar *argv0 = NULL;
	g_autofree gchar *argv1 = NULL;
	g_autofree gchar *argv2 = NULL;
	g_autofree gchar *argv3 = NULL;
	g_autofree gchar *output = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *path = NULL;
	g_auto(GStrv) tokens = NULL;

	argv[0] = argv0 = g_strdup ("dpkg-deb");
	argv[1] = argv1 = g_strdup ("--showformat=${Package}\\n"
				    "${Version}\\n"
				    "${Installed-Size}\\n"
				    "${Homepage}\\n"
				    "${Description}");
	argv[2] = argv2 = g_strdup ("-W");
	argv[3] = argv3 = g_strdup (filename);

	if (!g_spawn_sync (NULL,
			   argv,
			   NULL,
			   G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
			   NULL,
			   NULL,
			   &output,
			   NULL,
			   NULL,
			   error))
		return FALSE;

	tokens = g_strsplit (output, "\n", 0);

	if (g_strv_length (tokens) < 5) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "dpkg-deb output format incorrect:\n\"%s\"\n", output);
		return FALSE;
	}

	description = g_strjoinv (NULL, tokens + 5);

	app = gs_app_new (tokens[0]);
	file = g_file_new_for_commandline_arg (filename);
	path = g_file_get_path (file);

	gs_app_set_name (app, GS_APP_QUALITY_HIGHEST, tokens[0]);
	gs_app_set_version (app, tokens[1]);
	gs_app_set_size (app, 1024 * atoi (tokens[2]));
	gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, tokens[3]);
	gs_app_set_summary (app, GS_APP_QUALITY_HIGHEST, tokens[4]);
	gs_app_set_description (app, GS_APP_QUALITY_HIGHEST, description + 1);
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE_LOCAL);
	gs_app_set_management_plugin (app, "dpkg");
	gs_app_add_source (app, tokens[0]);
	gs_app_set_origin (app, path);

	gs_plugin_add_app (list, app);

	return TRUE;
}
