/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2013-2016 Richard Hughes <richard@hughsie.com>
 * Copyright (C) 2013 Matthias Clasen <mclasen@redhat.com>
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

#include "config.h"

#include <string.h>
#include <glib/gi18n.h>
#include <gsettings-desktop-schemas/gdesktop-enums.h>

#include "gs-update-monitor.h"
#include "gs-plugin-loader.h"
#include "gs-common.h"
#include "gs-utils.h"

struct _GsUpdateMonitor {
	GObject		 parent;

	GApplication	*application;
	GCancellable    *cancellable;
	GSettings	*settings;
	GsPluginLoader	*plugin_loader;
	GDBusProxy	*proxy_upower;
	GError		*last_offline_error;

	guint		 cleanup_notifications_id;	/* at startup */
	guint		 check_startup_id;		/* 60s after startup */
	guint		 check_hourly_id;		/* and then every hour */
	guint		 check_daily_id;		/* every 3rd day */
	GNetworkMonitor	*network_monitor;		/* network type detection */
	guint		 notification_blocked_id;	/* rate limit notifications */
};

G_DEFINE_TYPE (GsUpdateMonitor, gs_update_monitor, G_TYPE_OBJECT)

static gboolean
reenable_offline_update_notification (gpointer data)
{
	GsUpdateMonitor *monitor = data;
	monitor->notification_blocked_id = 0;
	return G_SOURCE_REMOVE;
}

static void
notify_offline_update_available (GsUpdateMonitor *monitor)
{
	const gchar *title;
	const gchar *body;
	guint64 elapsed_security = 0;
	guint64 security_timestamp = 0;
	g_autoptr(GNotification) n = NULL;

	if (gs_application_has_active_window (GS_APPLICATION (monitor->application)))
		return;
	if (monitor->notification_blocked_id > 0)
		return;

	/* rate limit update notifications to once per hour */
	monitor->notification_blocked_id = g_timeout_add_seconds (3600, reenable_offline_update_notification, monitor);

	/* get time in days since we saw the first unapplied security update */
	g_settings_get (monitor->settings,
			"security-timestamp", "x", &security_timestamp);
	if (security_timestamp > 0) {
		elapsed_security = (guint64) g_get_monotonic_time () - security_timestamp;
		elapsed_security /= G_USEC_PER_SEC;
		elapsed_security /= 60 * 60 * 24;
	}

	/* only show the scary warning after the user has ignored
	 * security updates for a full day */
	if (elapsed_security > 1) {
		title = _("Security Updates Pending");
		body = _("It is recommended that you install important updates now");
		n = g_notification_new (title);
		g_notification_set_body (n, body);
		g_notification_add_button (n, _("Restart & Install"), "app.reboot-and-install");
		g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
		g_application_send_notification (monitor->application, "updates-available", n);
	} else {
		title = _("Software Updates Available");
		body = _("Important OS and application updates are ready to be installed");
		n = g_notification_new (title);
		g_notification_set_body (n, body);
		g_notification_add_button (n, _("Not Now"), "app.nop");
		g_notification_add_button_with_target (n, _("View"), "app.set-mode", "s", "updates");
		g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
		g_application_send_notification (monitor->application, "updates-available", n);
	}
}

static gboolean
has_important_updates (GsAppList *apps)
{
	guint i;
	GsApp *app;

	for (i = 0; i < gs_app_list_length (apps); i++) {
		app = gs_app_list_index (apps, i);
		if (gs_app_get_update_urgency (app) == AS_URGENCY_KIND_CRITICAL ||
		    gs_app_get_update_urgency (app) == AS_URGENCY_KIND_HIGH)
			return TRUE;
	}

	return FALSE;
}

static gboolean
no_updates_for_a_week (GsUpdateMonitor *monitor)
{
	GTimeSpan d;
	gint64 tmp;
	g_autoptr(GDateTime) last_update = NULL;
	g_autoptr(GDateTime) now = NULL;

	g_settings_get (monitor->settings, "install-timestamp", "x", &tmp);
	if (tmp == 0)
		return TRUE;

	last_update = g_date_time_new_from_unix_local (tmp);
	if (last_update == NULL) {
		g_warning ("failed to set timestamp %" G_GINT64_FORMAT, tmp);
		return TRUE;
	}

	now = g_date_time_new_now_local ();
	d = g_date_time_difference (now, last_update);
	if (d >= 7 * G_TIME_SPAN_DAY)
		return TRUE;

	return FALSE;
}

static void
get_updates_finished_cb (GObject *object,
			 GAsyncResult *res,
			 gpointer data)
{
	GsUpdateMonitor *monitor = data;
	guint i;
	GsApp *app;
	guint64 security_timestamp = 0;
	guint64 security_timestamp_old = 0;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) apps = NULL;

	/* get result */
	apps = gs_plugin_loader_get_updates_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to get updates: %s", error->message);
		return;
	}

	/* no updates */
	if (gs_app_list_length (apps) == 0) {
		g_debug ("no updates; withdrawing updates-available notification");
		g_application_withdraw_notification (monitor->application,
						     "updates-available");
		return;
	}

	/* find security updates, or clear timestamp if there are now none */
	g_settings_get (monitor->settings,
			"security-timestamp", "x", &security_timestamp_old);
	for (i = 0; i < gs_app_list_length (apps); i++) {
		app = gs_app_list_index (apps, i);
		if (gs_app_get_metadata_item (app, "is-security") != NULL) {
			security_timestamp = (guint64) g_get_monotonic_time ();
			break;
		}
	}
	if (security_timestamp_old != security_timestamp) {
		g_settings_set (monitor->settings,
				"security-timestamp", "x", security_timestamp);
	}

	g_debug ("got %u updates", gs_app_list_length (apps));

	if (has_important_updates (apps) ||
	    no_updates_for_a_week (monitor)) {
		notify_offline_update_available (monitor);
	}
}

static gboolean
should_show_upgrade_notification (GsUpdateMonitor *monitor)
{
	GTimeSpan d;
	gint64 tmp;
	g_autoptr(GDateTime) now = NULL;
	g_autoptr(GDateTime) then = NULL;

	g_settings_get (monitor->settings, "upgrade-notification-timestamp", "x", &tmp);
	if (tmp == 0)
		return TRUE;
	then = g_date_time_new_from_unix_local (tmp);
	if (then == NULL) {
		g_warning ("failed to parse timestamp %" G_GINT64_FORMAT, tmp);
		return TRUE;
	}

	now = g_date_time_new_now_local ();
	d = g_date_time_difference (now, then);
	if (d >= 30 * G_TIME_SPAN_DAY)
		return TRUE;

	return FALSE;
}

static void
get_upgrades_finished_cb (GObject *object,
			  GAsyncResult *res,
			  gpointer data)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (data);
	GsApp *app;
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GNotification) n = NULL;
	g_autoptr(GsAppList) apps = NULL;

	/* get result */
	apps = gs_plugin_loader_get_distro_upgrades_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED)) {
			g_warning ("failed to get upgrades: %s",
				   error->message);
		}
		return;
	}

	/* no results */
	if (gs_app_list_length (apps) == 0) {
		g_debug ("no upgrades; withdrawing upgrades-available notification");
		g_application_withdraw_notification (monitor->application,
						     "upgrades-available");
		return;
	}

	/* do not show if gnome-software is already open */
	if (gs_application_has_active_window (GS_APPLICATION (monitor->application)))
		return;

	/* only nag about upgrades once per month */
	if (!should_show_upgrade_notification (monitor))
		return;

	/* just get the first result : FIXME, do we sort these by date? */
	app = gs_app_list_index (apps, 0);

	/* TRANSLATORS: this is a distro upgrade, the replacement would be the
	 * distro name, e.g. 'Fedora' */
	body = g_strdup_printf (_("A new version of %s is available to install"),
				gs_app_get_name (app));

	/* TRANSLATORS: this is a distro upgrade */
	n = g_notification_new (_("Software Upgrade Available"));
	g_notification_set_body (n, body);
	g_notification_set_default_action_and_target (n, "app.set-mode", "s", "updates");
	g_application_send_notification (monitor->application, "upgrades-available", n);
}

static void
get_updates (GsUpdateMonitor *monitor)
{
	/* NOTE: this doesn't actually do any network access, instead it just
	 * returns already downloaded-and-depsolved packages */
	g_debug ("Getting updates");
	gs_plugin_loader_get_updates_async (monitor->plugin_loader,
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_DETAILS |
					    GS_PLUGIN_REFINE_FLAGS_REQUIRE_UPDATE_SEVERITY,
					    monitor->cancellable,
					    get_updates_finished_cb,
					    monitor);
}

static void
get_upgrades (GsUpdateMonitor *monitor)
{
	/* NOTE: this doesn't actually do any network access, it relies on the
	 * AppStream data being up to date, either by the appstream-data
	 * package being up-to-date, or the metadata being auto-downloaded */
	g_debug ("Getting upgrades");
	gs_plugin_loader_get_distro_upgrades_async (monitor->plugin_loader,
						    GS_PLUGIN_REFINE_FLAGS_DEFAULT,
						    monitor->cancellable,
						    get_upgrades_finished_cb,
						    monitor);
}

static void
refresh_cache_finished_cb (GObject *object,
			   GAsyncResult *res,
			   gpointer data)
{
	GsUpdateMonitor *monitor = data;
	g_autoptr(GError) error = NULL;

	if (!gs_plugin_loader_refresh_finish (GS_PLUGIN_LOADER (object), res, &error)) {
		if (!g_error_matches (error, GS_PLUGIN_ERROR, GS_PLUGIN_ERROR_CANCELLED))
			g_warning ("failed to refresh the cache: %s", error->message);
		return;
	}
	get_updates (monitor);
}

typedef enum {
	UP_DEVICE_LEVEL_UNKNOWN,
	UP_DEVICE_LEVEL_NONE,
	UP_DEVICE_LEVEL_DISCHARGING,
	UP_DEVICE_LEVEL_LOW,
	UP_DEVICE_LEVEL_CRITICAL,
	UP_DEVICE_LEVEL_ACTION,
	UP_DEVICE_LEVEL_LAST
} UpDeviceLevel;

static void
check_updates (GsUpdateMonitor *monitor)
{
	gint64 tmp;
	g_autoptr(GDateTime) last_refreshed = NULL;
	g_autoptr(GDateTime) now_refreshed = NULL;

	/* we don't know the network state */
	if (monitor->network_monitor == NULL)
		return;

	/* never refresh when offline or on mobile connections */
	if (!g_network_monitor_get_network_available (monitor->network_monitor) ||
	    g_network_monitor_get_network_metered (monitor->network_monitor))
		return;

	/* never refresh when the battery is low */
	if (monitor->proxy_upower != NULL) {
		g_autoptr(GVariant) val = NULL;
		val = g_dbus_proxy_get_cached_property (monitor->proxy_upower,
							"WarningLevel");
		if (val != NULL) {
			guint32 level = g_variant_get_uint32 (val);
			if (level >= UP_DEVICE_LEVEL_LOW) {
				g_debug ("not getting updates on low power");
				return;
			}
		}
	} else {
		g_debug ("no UPower support, so not doing power level checks");
	}

	g_settings_get (monitor->settings, "check-timestamp", "x", &tmp);
	last_refreshed = g_date_time_new_from_unix_local (tmp);
	if (last_refreshed != NULL) {
		gint now_year, now_month, now_day, now_hour;
		gint year, month, day;
		g_autoptr(GDateTime) now = NULL;

		now = g_date_time_new_now_local ();

		g_date_time_get_ymd (now, &now_year, &now_month, &now_day);
		now_hour = g_date_time_get_hour (now);

		g_date_time_get_ymd (last_refreshed, &year, &month, &day);

		/* check that it is the next day */
		if (!((now_year > year) ||
		      (now_year == year && now_month > month) ||
		      (now_year == year && now_month == month && now_day > day)))
			return;

		/* ...and past 6am */
		if (!(now_hour >= 6))
			return;
	}

	g_debug ("Daily update check due");
	now_refreshed = g_date_time_new_now_local ();
	g_settings_set (monitor->settings, "check-timestamp", "x",
			g_date_time_to_unix (now_refreshed));

	/* NOTE: this doesn't actually refresh the cache, it actually just checks
	 * for updates (which might happen to also refresh the cache as a side
	 * effect) and then downloads new packages */
	g_debug ("Refreshing cache");
	gs_plugin_loader_refresh_async (monitor->plugin_loader,
					60 * 60 * 24,
					GS_PLUGIN_REFRESH_FLAGS_METADATA |
					GS_PLUGIN_REFRESH_FLAGS_PAYLOAD,
					monitor->cancellable,
					refresh_cache_finished_cb,
					monitor);
}

static gboolean
check_hourly_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("Hourly updates check");
	check_updates (monitor);

	return G_SOURCE_CONTINUE;
}

static gboolean
check_thrice_daily_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("Daily upgrades check");
	get_upgrades (monitor);

	return G_SOURCE_CONTINUE;
}

static gboolean
check_updates_on_startup_cb (gpointer data)
{
	GsUpdateMonitor *monitor = data;

	g_debug ("First hourly updates check");
	check_updates (monitor);
	get_upgrades (monitor);

	monitor->check_hourly_id =
		g_timeout_add_seconds (3600, check_hourly_cb, monitor);
	monitor->check_daily_id =
		g_timeout_add_seconds (3 * 86400, check_thrice_daily_cb, monitor);

	monitor->check_startup_id = 0;
	return G_SOURCE_REMOVE;
}

static void
check_updates_upower_changed_cb (GDBusProxy *proxy,
				 GParamSpec *pspec,
				 GsUpdateMonitor *monitor)
{
	g_debug ("upower changed updates check");
	check_updates (monitor);
}

static void
notify_network_state_cb (GNetworkMonitor *network_monitor,
			 gboolean active,
			 GsUpdateMonitor *monitor)
{
	check_updates (monitor);
}

static void
updates_changed_cb (GsPluginLoader *plugin_loader, GsUpdateMonitor *monitor)
{
	/* when the list of downloaded-and-ready-to-go updates changes get the
	 * new list and perhaps show/hide the notification */
	get_updates (monitor);
}

static void
get_updates_historical_cb (GObject *object, GAsyncResult *res, gpointer data)
{
	GsUpdateMonitor *monitor = data;
	GsApp *app;
	const gchar *message;
	const gchar *title;
	guint64 time_last_notified;
	g_autoptr(GError) error = NULL;
	g_autoptr(GsAppList) apps = NULL;
	g_autoptr(GNotification) notification = NULL;

	/* get result */
	apps = gs_plugin_loader_get_updates_finish (GS_PLUGIN_LOADER (object), res, &error);
	if (apps == NULL) {

		/* save this in case the user clicks the
		 * 'Show Details' button from the notification below */
		g_clear_error (&monitor->last_offline_error);
		monitor->last_offline_error = g_error_copy (error);

		/* TRANSLATORS: title when we offline updates have failed */
		notification = g_notification_new (_("Software Updates Failed"));
		/* TRANSLATORS: message when we offline updates have failed */
		g_notification_set_body (notification, _("An important OS update failed to be installed."));
		g_notification_add_button (notification, _("Show Details"), "app.show-offline-update-error");
		g_notification_set_default_action (notification, "app.show-offline-update-error");
		g_application_send_notification (monitor->application, "offline-updates", notification);
		return;
	}

	/* no results */
	if (gs_app_list_length (apps) == 0) {
		g_debug ("no historical updates; withdrawing notification");
		g_application_withdraw_notification (monitor->application,
						     "updates-available");
		return;
	}

	/* have we notified about this before */
	app = gs_app_list_index (apps, 0);
	g_settings_get (monitor->settings,
			"install-timestamp", "x", &time_last_notified);
	if (time_last_notified >= gs_app_get_install_date (app))
		return;

	/* TRANSLATORS: title when we've done offline updates */
	title = ngettext ("Software Update Installed",
			  "Software Updates Installed",
			  gs_app_list_length (apps));
	/* TRANSLATORS: message when we've done offline updates */
	message = ngettext ("An important OS update has been installed.",
			    "Important OS updates have been installed.",
			    gs_app_list_length (apps));

	notification = g_notification_new (title);
	g_notification_set_body (notification, message);
	/* TRANSLATORS: Button to look at the updates that were installed.
	 * Note that it has nothing to do with the application reviews, the
	 * users can't express their opinions here. In some languages
	 * "Review (evaluate) something" is a different translation than
	 * "Review (browse) something." */
	g_notification_add_button_with_target (notification, C_("updates", "Review"), "app.set-mode", "s", "updated");
	g_notification_set_default_action_and_target (notification, "app.set-mode", "s", "updated");
	g_application_send_notification (monitor->application, "offline-updates", notification);

	/* update the timestamp so we don't show again */
	g_settings_set (monitor->settings,
			"install-timestamp", "x", gs_app_get_install_date (app));


}

static gboolean
cleanup_notifications_cb (gpointer user_data)
{
	GsUpdateMonitor *monitor = user_data;

	/* this doesn't do any network access */
	g_debug ("getting historical updates for fresh session");
	gs_plugin_loader_get_updates_async (monitor->plugin_loader,
					    GS_PLUGIN_REFINE_FLAGS_USE_HISTORY,
					    monitor->cancellable,
					    get_updates_historical_cb,
					    monitor);

	/* wait until first check to show */
	g_application_withdraw_notification (monitor->application,
					     "updates-available");

	monitor->cleanup_notifications_id = 0;
	return G_SOURCE_REMOVE;
}

void
gs_update_monitor_show_error (GsUpdateMonitor *monitor, GsShell *shell)
{
	const gchar *title;
	const gchar *msg;
	gboolean show_detailed_error;

	/* can this happen in reality? */
	if (monitor->last_offline_error == NULL)
		return;

	/* TRANSLATORS: this is when the offline update failed */
	title = _("Failed To Update");

	switch (monitor->last_offline_error->code) {
	case GS_PLUGIN_ERROR_NOT_SUPPORTED:
		/* TRANSLATORS: the user must have updated manually after
		 * the updates were prepared */
		msg = _("The system was already up to date.");
		show_detailed_error = TRUE;
		break;
	case GS_PLUGIN_ERROR_CANCELLED:
		/* TRANSLATORS: the user aborted the update manually */
		msg = _("The update was cancelled.");
		show_detailed_error = FALSE;
		break;
	case GS_PLUGIN_ERROR_NO_NETWORK:
		/* TRANSLATORS: the package manager needed to download
		 * something with no network available */
		msg = _("Internet access was required but wasn’t available. "
			"Please make sure that you have internet access and try again.");
		show_detailed_error = FALSE;
		break;
	case GS_PLUGIN_ERROR_NO_SECURITY:
		/* TRANSLATORS: if the package is not signed correctly */
		msg = _("There were security issues with the update. "
			"Please consult your software provider for more details.");
		show_detailed_error = TRUE;
		break;
	case GS_PLUGIN_ERROR_NO_SPACE:
		/* TRANSLATORS: we ran out of disk space */
		msg = _("There wasn’t enough disk space. Please free up some space and try again.");
		show_detailed_error = FALSE;
		break;
	default:
		/* TRANSLATORS: We didn't handle the error type */
		msg = _("We’re sorry: the update failed to install. "
			"Please wait for another update and try again. "
			"If the problem persists, contact your software provider.");
		show_detailed_error = TRUE;
		break;
	}

	gs_utils_show_error_dialog (gs_shell_get_window (shell),
	                            title,
	                            msg,
	                            show_detailed_error ? monitor->last_offline_error->message : NULL);
}

static void
gs_update_monitor_init (GsUpdateMonitor *monitor)
{
	g_autoptr(GError) error = NULL;
	monitor->settings = g_settings_new ("org.gnome.software");

	/* cleanup at startup */
	monitor->cleanup_notifications_id =
		g_idle_add (cleanup_notifications_cb, monitor);

	/* do a first check 60 seconds after login, and then every hour */
	monitor->check_startup_id =
		g_timeout_add_seconds (60, check_updates_on_startup_cb, monitor);

	monitor->cancellable = g_cancellable_new ();
	monitor->network_monitor = g_network_monitor_get_default ();
	if (monitor->network_monitor != NULL) {
		g_signal_connect (monitor->network_monitor, "network-changed",
				  G_CALLBACK (notify_network_state_cb), monitor);
	}

	/* connect to UPower to get the system power state */
	monitor->proxy_upower = g_dbus_proxy_new_for_bus_sync (G_BUS_TYPE_SYSTEM,
					G_DBUS_PROXY_FLAGS_NONE,
					NULL,
					"org.freedesktop.UPower",
					"/org/freedesktop/UPower/devices/DisplayDevice",
					"org.freedesktop.UPower.Device",
					NULL,
					&error);
	if (monitor->proxy_upower != NULL) {
		g_signal_connect (monitor->proxy_upower, "notify",
				  G_CALLBACK (check_updates_upower_changed_cb),
				  monitor);
	} else {
		g_warning ("failed to connect to upower: %s", error->message);
	}
}

static void
gs_update_monitor_dispose (GObject *object)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (object);

	if (monitor->cancellable) {
		g_cancellable_cancel (monitor->cancellable);
		g_clear_object (&monitor->cancellable);
	}
	if (monitor->check_hourly_id != 0) {
		g_source_remove (monitor->check_hourly_id);
		monitor->check_hourly_id = 0;
	}
	if (monitor->check_daily_id != 0) {
		g_source_remove (monitor->check_daily_id);
		monitor->check_daily_id = 0;
	}
	if (monitor->check_startup_id != 0) {
		g_source_remove (monitor->check_startup_id);
		monitor->check_startup_id = 0;
	}
	if (monitor->notification_blocked_id != 0) {
		g_source_remove (monitor->notification_blocked_id);
		monitor->notification_blocked_id = 0;
	}
	if (monitor->cleanup_notifications_id != 0) {
		g_source_remove (monitor->cleanup_notifications_id);
		monitor->cleanup_notifications_id = 0;
	}
	if (monitor->network_monitor != NULL) {
		g_signal_handlers_disconnect_by_func (monitor->network_monitor,
						      notify_network_state_cb,
						      monitor);
		monitor->network_monitor = NULL;
	}
	if (monitor->plugin_loader != NULL) {
		g_signal_handlers_disconnect_by_func (monitor->plugin_loader,
		                                      updates_changed_cb,
		                                      monitor);
		monitor->plugin_loader = NULL;
	}
	g_clear_object (&monitor->settings);
	g_clear_object (&monitor->proxy_upower);

	G_OBJECT_CLASS (gs_update_monitor_parent_class)->dispose (object);
}

static void
gs_update_monitor_finalize (GObject *object)
{
	GsUpdateMonitor *monitor = GS_UPDATE_MONITOR (object);

	g_application_release (monitor->application);
	g_clear_error (&monitor->last_offline_error);

	G_OBJECT_CLASS (gs_update_monitor_parent_class)->finalize (object);
}

static void
gs_update_monitor_class_init (GsUpdateMonitorClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->dispose = gs_update_monitor_dispose;
	object_class->finalize = gs_update_monitor_finalize;
}

GsUpdateMonitor *
gs_update_monitor_new (GsApplication *application)
{
	GsUpdateMonitor *monitor;

	monitor = GS_UPDATE_MONITOR (g_object_new (GS_TYPE_UPDATE_MONITOR, NULL));
	monitor->application = G_APPLICATION (application);
	g_application_hold (monitor->application);

	monitor->plugin_loader = gs_application_get_plugin_loader (application);
	g_signal_connect (monitor->plugin_loader, "updates-changed",
			  G_CALLBACK (updates_changed_cb), monitor);

	return monitor;
}

GPermission *
gs_update_monitor_permission_get (void)
{
	static GPermission *permission = NULL;
#ifdef HAVE_PACKAGEKIT
	if (permission == NULL)
		permission = gs_utils_get_permission ("org.freedesktop.packagekit.trigger-offline-update");
#endif
	return permission;
}

gboolean
gs_update_monitor_is_managed (void)
{
	GPermission *permission;
	gboolean managed;

	permission = gs_update_monitor_permission_get ();
	if (permission == NULL)
		return FALSE;
	managed = !g_permission_get_allowed (permission) &&
                  !g_permission_get_can_acquire (permission);

	return managed;
}

/* vim: set noexpandtab: */
