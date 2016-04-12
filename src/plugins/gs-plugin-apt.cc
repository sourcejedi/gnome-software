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

#include <apt-pkg/init.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/cmndline.h>
#include <apt-pkg/version.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <set>
#include <vector>
#include <fstream>
#include <iomanip>
#include <algorithm>
#include <stdexcept>

#include <gs-plugin.h>
#include <gs-utils.h>

#define LICENSE_URL "http://www.ubuntu.com/about/about-ubuntu/licensing"

typedef struct {
	gchar		*name;
	gchar		*section;
	gchar		*architecture;
	gchar		*installed_version;
	gchar		*update_version;
	gchar		*origin;
	gchar		*release;
	gchar		*component;
	gint		 installed_size;
	gint		 package_size;
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
gs_plugin_order_after (GsPlugin *plugin)
{
	static const gchar *deps[] = {
		"appstream",		/* need pkgname */
		NULL };
	return deps;
}

/**
 * gs_plugin_get_conflicts:
 */
const gchar **
gs_plugin_get_conflicts (GsPlugin *plugin)
{

	static const gchar *deps[] = {
		"packagekit",
		"packagekit-history",
		"packagekit-offline",
		"packagekit-origin",
		"packagekit-proxy",
		"packagekit-refine",
		"packagekit-refresh",
		"systemd-updates",
		NULL };
	return deps;
}

static void
free_package_info (gpointer data)
{
	PackageInfo *info = (PackageInfo *) data;
	g_free (info->section);
	g_free (info->installed_version);
	g_free (info->update_version);
	g_free (info->origin);
	g_free (info->release);
	g_free (info->component);
	g_free (info);
}

void
gs_plugin_initialize (GsPlugin *plugin)
{
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
	plugin->priv->package_info = g_hash_table_new_full (g_str_hash,
							    g_str_equal,
							    NULL,
							    free_package_info);

	pkgInitConfig (*_config);
	pkgInitSystem (*_config, _system);
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	g_hash_table_unref (plugin->priv->package_info);
	g_list_free (plugin->priv->installed_packages);
	g_list_free (plugin->priv->updatable_packages);
}

static gboolean
version_newer (const gchar *v0, const gchar *v1)
{
	return v0 ? _system->VS->CmpVersion(v0, v1) < 0 : TRUE;
}

static gboolean
look_at_pkg (const pkgCache::PkgIterator &P,
	     pkgSourceList *list,
	     pkgPolicy *policy,
	     GsPlugin *plugin)
{
	pkgCache::VerIterator current = P.CurrentVer();
	pkgCache::VerIterator candidate = policy->GetCandidateVer(P);
	pkgCache::VerFileIterator VF;
	FileFd PkgF;
	pkgTagSection Tags;
	gchar *name;

	PackageInfo *info;

	if (!candidate || !candidate.FileList ())
		return false;

	name = g_strdup (P.Name ());
	info = (PackageInfo *) g_hash_table_lookup (plugin->priv->package_info, name);
	if (info == NULL) {
		info = g_new0 (PackageInfo, 1);
		info->name = name;
		g_hash_table_insert (plugin->priv->package_info, name, info);
	}

	for (VF = candidate.FileList (); VF.IsGood (); VF++) {
		// see InRelease for the fields
		if (VF.File ().Archive ())
			info->release = g_strdup (VF.File ().Archive ());
		if (VF.File ().Origin ())
			info->origin = g_strdup (VF.File ().Origin ());
		if (VF.File ().Component ())
			info->component = g_strdup (VF.File ().Component ());
		// also available: Codename, Label
		break;
	}


	pkgCache::PkgFileIterator I = VF.File ();

	if (I.IsOk () == false)
		return _error->Error (("Package file %s is out of sync."),I.FileName ());

	PkgF.Open (I.FileName (), FileFd::ReadOnly, FileFd::Extension);

	pkgTagFile TagF (&PkgF);

	if (TagF.Jump (Tags, current.FileList ()->Offset) == false)
		return _error->Error ("Internal Error, Unable to parse a package record");

	if (Tags.FindI ("Installed-Size") > 0)
		info->installed_size = Tags.FindI ("Installed-Size")*1024;
	else
		info->installed_size = 0;

	if (Tags.FindI ("Size") > 0)
		info->package_size = Tags.FindI ("Size");
	else
		info->package_size = 0;

	if (current)
		info->installed_version = g_strdup (current.VerStr ());

	info->section = g_strdup (candidate.Section ());
	info->architecture = g_strdup (P.Arch ());
	if (info->installed_version) {
		info->update_version = g_strdup (candidate.VerStr ());
		plugin->priv->installed_packages =
			g_list_append (plugin->priv->installed_packages, info);
	}

	/* no upgrade */
	if (g_strcmp0 (info->installed_version, info->update_version) == 0)
		info->update_version = NULL;

	if (info->update_version)
		plugin->priv->updatable_packages =
			g_list_append (plugin->priv->updatable_packages, info);

	return TRUE;
}

static gboolean
load_apt_db (GsPlugin *plugin, GError **error)
{
	pkgSourceList *list;
	pkgPolicy *policy;
	pkgCacheFile cachefile;
	pkgCache *cache = cachefile.GetPkgCache();
	pkgCache::PkgIterator P;

	list = cachefile.GetSourceList();
	policy = cachefile.GetPolicy();
	if (cache == NULL || _error->PendingError()) {
		_error->DumpErrors();
		return FALSE;
	}

	for (pkgCache::GrpIterator grp = cache->GrpBegin(); grp != cache->GrpEnd(); grp++) {
		P = grp.FindPreferredPkg();
		if (P.end())
			continue;
		look_at_pkg (P, list, policy, plugin);
	}

	return TRUE;
}

static void
get_changelog (GsPlugin *plugin, GsApp *app)
{
	g_autofree gchar *source_prefix = NULL,
			 *uri = NULL,
			 *changelog_prefix = NULL,
			 *binary_source = NULL,
			 *current_version = NULL,
			 *update_version = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	guint status_code;
	g_auto(GStrv) lines = NULL;
	int i;
	GString *details;

	// Need to know the source and version to download changelog
	binary_source = g_strdup (gs_app_get_source_default (app));
	current_version = g_strdup (gs_app_get_version (app));
	update_version = g_strdup (gs_app_get_update_version (app));
	if (binary_source == NULL || update_version == NULL)
		return;

	if (g_str_has_prefix (binary_source, "lib"))
		source_prefix = g_strdup_printf ("lib%c", binary_source[3]);
	else
		source_prefix = g_strdup_printf ("%c", binary_source[0]);
	uri = g_strdup_printf ("http://changelogs.ubuntu.com/changelogs/binary/%s/%s/%s/changelog", source_prefix, binary_source, update_version);
	msg = soup_message_new (SOUP_METHOD_GET, uri);

	status_code = soup_session_send_message (plugin->soup_session, msg);
	if (status_code != SOUP_STATUS_OK) {
		g_warning ("Failed to get changelog for %s version %s from changelogs.ubuntu.com: %s", binary_source, update_version, soup_status_get_phrase (status_code));
		return;
	}

	// Extract changelog entries newer than our current version
	lines = g_strsplit (msg->response_body->data, "\n", -1);
	details = g_string_new ("");
	for (i = 0; lines[i] != NULL; i++) {
		gchar *line = lines[i];
		const gchar *version_start, *version_end;
		g_autofree gchar *v = NULL;

		// First line is in the form "package (version) distribution(s); urgency=urgency"
                version_start = strchr (line, '(');
                version_end = strchr (line, ')');
		if (line[0] == ' ' || version_start == NULL || version_end == NULL || version_end < version_start)
			continue;
		v = g_strdup_printf ("%.*s", (int) (version_end - version_start - 1), version_start + 1);

		// We're only interested in new versions
		if (!version_newer (current_version, v))
			break;

		g_string_append_printf (details, "%s\n", v);
		for (i++; lines[i] != NULL; i++) {
			// Last line is in the form " -- maintainer name <email address>  date"
			if (g_str_has_prefix (lines[i], " -- "))
				break;
			g_string_append_printf (details, "%s\n", lines[i]);
		}
	}

	gs_app_set_update_details (app, details->str);
	g_string_free (details, TRUE);
}

static gboolean
is_official (PackageInfo *info)
{
	return g_strcmp0 (info->origin, "Ubuntu") == 0;
}

static gboolean
is_open_source (PackageInfo *info)
{
	const gchar *open_source_components[] = { "main", "universe", NULL };

	/* There's no valid apps in the libs section */
	return info->component != NULL && g_strv_contains (open_source_components, info->component);
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

		ret = load_apt_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	for (link = *list; link; link = link->next) {
		GsApp *app = (GsApp *) link->data;
		PackageInfo *info;

		if (gs_app_get_source_default (app) == NULL)
			continue;

		info = (PackageInfo *) g_hash_table_lookup (plugin->priv->package_info, gs_app_get_source_default (app));
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
				gs_app_set_size (app, info->installed_size);
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
				gs_app_set_provenance (app, TRUE);
			}
			if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_LICENCE) != 0 && info->is_open_source) {
				gs_app_set_licence (app, "@LicenseRef-ubuntu", GS_APP_QUALITY_HIGHEST);
			}
		}

		if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS) != 0) {
			get_changelog (plugin, app);
		}
	}

	return TRUE;
}

static gboolean
is_allowed_section (PackageInfo *info)
{
	const gchar *section_blacklist[] = { "libs", NULL };

	/* There's no valid apps in the libs section */
	return info->section == NULL || !g_strv_contains (section_blacklist, info->section);
}

static gchar *
get_origin (PackageInfo *info)
{
	if (!info->origin)
		return NULL;

	g_autofree gchar *origin_lower = g_strdup (info->origin);
	gchar *out;
	for (int i = 0; origin_lower[i]; ++i)
		origin_lower[i] = g_ascii_tolower (origin_lower[i]);

	out = g_strdup_printf ("%s-%s-%s", origin_lower, info->release, info->component);

	return out;
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

		ret = load_apt_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			return FALSE;
	}


	for (link = plugin->priv->installed_packages; link; link = link->next) {
		PackageInfo *info = (PackageInfo *) link->data;
		g_autofree gchar *origin = get_origin (info);
		g_autoptr(GsApp) app = NULL;

		if (!is_allowed_section (info))
			continue;

		app = gs_app_new (NULL);
		// FIXME: Since appstream marks all packages as owned by
		// PackageKit and we are replacing PackageKit we need to accept
		// those packages
		gs_app_set_management_plugin (app, "PackageKit");
		gs_app_set_name (app, GS_APP_QUALITY_LOWEST, info->name);
		gs_app_add_source (app, info->name);
		gs_app_set_origin (app, origin);
		gs_app_set_origin_ui (app, info->origin);
		gs_app_set_state (app, AS_APP_STATE_INSTALLED);
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
	TransactionData *data = (TransactionData *) user_data;
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
	TransactionData *data = (TransactionData *) user_data;

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
	if (result != NULL)
		g_main_loop_run (loop);
	g_dbus_connection_signal_unsubscribe (conn, property_signal);
	g_dbus_connection_signal_unsubscribe (conn, finished_signal);
	if (result == NULL)
		return FALSE;

	if (g_strcmp0 (transaction_result, "exit-success") != 0) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "apt transaction returned result %s", transaction_result);
		return FALSE;
	}

	return TRUE;
}

static gboolean
app_is_ours (GsApp *app)
{
	const gchar *management_plugin = gs_app_get_management_plugin (app);

	// FIXME: Since appstream marks all packages as owned by PackageKit and
	// we are replacing PackageKit we need to accept those packages
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

		ret = load_apt_db (plugin, error);
		g_once_init_leave (&plugin->priv->loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	for (link = plugin->priv->updatable_packages; link; link = link->next) {
		PackageInfo *info = (PackageInfo *) link->data;
		g_autoptr(GsApp) app = NULL;

		if (!is_allowed_section (info))
			continue;

		app = gs_app_new (NULL);
		// FIXME: Since appstream marks all packages as owned by
		// PackageKit and we are replacing PackageKit we need to accept
		// those packages
		gs_app_set_management_plugin (app, "PackageKit");
		gs_app_set_name (app, GS_APP_QUALITY_LOWEST, info->name);
		gs_app_set_kind (app, AS_APP_KIND_GENERIC);
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

static gboolean
content_type_matches (const gchar *filename,
		      gboolean *matches,
		      GCancellable *cancellable,
		      GError **error)
{
	const gchar *content_type;
	g_autoptr(GFile) file = NULL;
	g_autoptr(GFileInfo) info = NULL;
	const gchar *supported_types[] = {
		"application/vnd.debian.binary-package",
		NULL };

	/* get content type */
	file = g_file_new_for_path (filename);
	info = g_file_query_info (file,
				  G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE,
				  G_FILE_QUERY_INFO_NONE,
				  cancellable,
				  error);
	if (info == NULL)
		return FALSE;

	content_type = g_file_info_get_attribute_string (info, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
	*matches = g_strv_contains (supported_types, content_type);

	return TRUE;
}

gboolean
gs_plugin_filename_to_app (GsPlugin      *plugin,
			   GList        **list,
			   const gchar   *filename,
			   GCancellable  *cancellable,
			   GError       **error)
{
	gboolean supported;
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

	if (!content_type_matches (filename,
				   &supported,
				   cancellable,
				   error))
		return FALSE;
	if (!supported)
		return TRUE;

	argv[0] = argv0 = g_strdup ("dpkg-deb");
	argv[1] = argv1 = g_strdup ("--showformat=${Package}\\n"
				    "${Version}\\n"
				    "${Installed-Size}\\n"
				    "${Homepage}\\n"
				    "${Description}");
	argv[2] = argv2 = g_strdup ("-W");
	argv[3] = argv3 = g_strdup (filename);

	if (!g_spawn_sync (NULL, /* working_directory */
			   argv,
			   NULL, /* envp */
			   (GSpawnFlags) (G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL),
			   NULL, /* child_setup */
			   NULL, /* user_data */
			   &output,
			   NULL, /* standard_error */
			   NULL, /* exit_status */
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

	app = gs_app_new (NULL);
	file = g_file_new_for_commandline_arg (filename);
	path = g_file_get_path (file);

	gs_app_set_name (app, GS_APP_QUALITY_LOWEST, tokens[0]);
	gs_app_set_version (app, tokens[1]);
	gs_app_set_size (app, 1024 * atoi (tokens[2]));
	gs_app_set_url (app, AS_URL_KIND_HOMEPAGE, tokens[3]);
	gs_app_set_summary (app, GS_APP_QUALITY_LOWEST, tokens[4]);
	gs_app_set_description (app, GS_APP_QUALITY_LOWEST, description + 1);
	gs_app_set_state (app, AS_APP_STATE_AVAILABLE_LOCAL);
	gs_app_set_management_plugin (app, "dpkg");
	gs_app_add_source (app, tokens[0]);
	gs_app_set_origin (app, path);
	gs_app_set_kind (app, AS_APP_KIND_GENERIC);

	gs_plugin_add_app (list, app);

	return TRUE;
}

/* vim: set noexpandtab ts=8 sw=8: */
