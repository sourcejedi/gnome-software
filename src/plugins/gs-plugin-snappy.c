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

#include <glib/gi18n.h>

#include <gs-plugin.h>

struct GsPluginPrivate {
};

/**
 * gs_plugin_get_name:
 */
const gchar *
gs_plugin_get_name (void)
{
	return "snappy";
}

/**
 * gs_plugin_initialize:
 */
void
gs_plugin_initialize (GsPlugin *plugin)
{
	/* create private area */
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);
}

/**
 * gs_plugin_destroy:
 */
void
gs_plugin_destroy (GsPlugin *plugin)
{
}

/**
 * gs_plugin_add_installed:
 */
gboolean
gs_plugin_add_installed (GsPlugin *plugin,
			 GList **list,
			 GCancellable *cancellable,
			 GError **error)
{
	return FALSE;
}

/**
 * gs_plugin_add_sources:
 */
gboolean
gs_plugin_add_sources (GsPlugin *plugin,
		       GList **list,
		       GCancellable *cancellable,
		       GError **error)
{
	return FALSE;
}

/**
 * gs_plugin_app_install:
 */
gboolean
gs_plugin_app_install (GsPlugin *plugin,
		       GsApp *app,
		       GCancellable *cancellable,
		       GError **error)
{
	return FALSE;
}

/**
 * gs_plugin_app_remove:
 */
gboolean
gs_plugin_app_remove (GsPlugin *plugin,
		      GsApp *app,
		      GCancellable *cancellable,
		      GError **error)
{
	return FALSE;
}

/**
 * gs_plugin_add_search_files:
 */
gboolean
gs_plugin_add_search_files (GsPlugin *plugin,
                            gchar **search,
                            GList **list,
                            GCancellable *cancellable,
                            GError **error)
{
	return FALSE;
}

/**
 * gs_plugin_add_search_what_provides:
 */
gboolean
gs_plugin_add_search_what_provides (GsPlugin *plugin,
                                    gchar **search,
                                    GList **list,
                                    GCancellable *cancellable,
                                    GError **error)
{
	return FALSE;
}
