/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2016 Canonical Ltd.
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

#include <string.h>
#include <math.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <oauth.h>
#include <sqlite3.h>

#include <gs-plugin.h>
#include <gs-utils.h>

#include "gs-ubuntu-login-dialog.h"
#include "gs-os-release.h"

struct GsPluginPrivate {
	gchar		*db_path;
	sqlite3		*db;
	gsize		 db_loaded;
	SoupSession	*session;
	gchar		*consumer_key;
	gchar		*consumer_secret;
	gchar		*token_key;
	gchar		*token_secret;
};

typedef struct {
	gint64		 one_star_count;
	gint64		 two_star_count;
	gint64		 three_star_count;
	gint64		 four_star_count;
	gint64		 five_star_count;
} Histogram;

const gchar *
gs_plugin_get_name (void)
{
	return "ubuntu-reviews";
}

#define UBUNTU_REVIEWS_SERVER		"https://reviews.ubuntu.com/reviews"

/* Download new stats every three months */
// FIXME: Much shorter time?
#define REVIEW_STATS_AGE_MAX		(60 * 60 * 24 * 7 * 4 * 3)

void
gs_plugin_initialize (GsPlugin *plugin)
{
	/* create private area */
	plugin->priv = GS_PLUGIN_GET_PRIVATE (GsPluginPrivate);

	/* check that we are running on Ubuntu */
	if (!gs_plugin_check_distro_id (plugin, "ubuntu")) {
		gs_plugin_set_enabled (plugin, FALSE);
		g_debug ("disabling '%s' as we're not Ubuntu", plugin->name);
		return;
	}

	plugin->priv->db_path = g_build_filename (g_get_user_data_dir (),
						  "gnome-software",
						  "ubuntu-reviews.db",
						  NULL);
}

const gchar **
gs_plugin_get_deps (GsPlugin *plugin)
{
	static const gchar *deps[] = { NULL };
	return deps;
}

void
gs_plugin_destroy (GsPlugin *plugin)
{
	GsPluginPrivate *priv = plugin->priv;

	g_clear_pointer (&priv->token_secret, g_free);
	g_clear_pointer (&priv->token_key, g_free);
	g_clear_pointer (&priv->consumer_secret, g_free);
	g_clear_pointer (&priv->consumer_key, g_free);
	g_clear_pointer (&priv->db, sqlite3_close);
	g_clear_object (&priv->session);
}

static gboolean
setup_networking (GsPlugin *plugin, GError **error)
{
	/* already set up */
	if (plugin->priv->session != NULL)
		return TRUE;

	/* set up a session */
	plugin->priv->session = soup_session_new_with_options (SOUP_SESSION_USER_AGENT,
							       gs_user_agent (),
							       NULL);
	if (plugin->priv->session == NULL) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "%s: failed to setup networking",
			     plugin->name);
		return FALSE;
	}
	return TRUE;
}

static gint
get_timestamp_sqlite_cb (void *data, gint argc,
			 gchar **argv, gchar **col_name)
{
	gint64 *timestamp = (gint64 *) data;
	*timestamp = g_ascii_strtoll (argv[0], NULL, 10);
	return 0;
}

static gboolean
set_package_stats (GsPlugin *plugin,
		   const gchar *package_name,
		   Histogram *histogram,
		   GError **error)
{
	char *error_msg = NULL;
	gint result;
	g_autofree gchar *statement = NULL;

	statement = g_strdup_printf ("INSERT OR REPLACE INTO review_stats (package_name, "
				     "one_star_count, two_star_count, three_star_count, "
				     "four_star_count, five_star_count) "
				     "VALUES ('%s', '%" G_GINT64_FORMAT "', '%" G_GINT64_FORMAT"', '%" G_GINT64_FORMAT "', '%" G_GINT64_FORMAT "', '%" G_GINT64_FORMAT "');",
				     package_name, histogram->one_star_count, histogram->two_star_count,
				     histogram->three_star_count, histogram->four_star_count, histogram->five_star_count);
	result = sqlite3_exec (plugin->priv->db, statement, NULL, NULL, &error_msg);
	if (result != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		return FALSE;
	}

	return TRUE;
}

static gboolean
set_timestamp (GsPlugin *plugin,
	       const gchar *type,
	       GError **error)
{
	char *error_msg = NULL;
	gint result;
	g_autofree gchar *statement = NULL;

	statement = g_strdup_printf ("INSERT OR REPLACE INTO timestamps (key, value) "
				     "VALUES ('%s', '%" G_GINT64_FORMAT "');",
				     type,
				     g_get_real_time () / G_USEC_PER_SEC);
	result = sqlite3_exec (plugin->priv->db, statement, NULL, NULL, &error_msg);
	if (result != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		return FALSE;
	}
	return TRUE;
}

static gint
get_review_stats_sqlite_cb (void *data,
			    gint argc,
			    gchar **argv,
			    gchar **col_name)
{
	Histogram *histogram = (Histogram *) data;
	histogram->one_star_count = g_ascii_strtoll (argv[0], NULL, 10);
	histogram->two_star_count = g_ascii_strtoll (argv[1], NULL, 10);
	histogram->three_star_count = g_ascii_strtoll (argv[2], NULL, 10);
	histogram->four_star_count = g_ascii_strtoll (argv[3], NULL, 10);
	histogram->five_star_count = g_ascii_strtoll (argv[4], NULL, 10);
	return 0;
}

static gboolean
get_review_stats (GsPlugin *plugin,
		  const gchar *package_name,
		  gint *rating,
		  gint *review_ratings,
		  GError **error)
{
	Histogram histogram = { 0, 0, 0, 0, 0 };
	gchar *error_msg = NULL;
	gint result, n_ratings;
	g_autofree gchar *statement = NULL;

	/* Get histogram from the database */
	statement = g_strdup_printf ("SELECT one_star_count, two_star_count, three_star_count, four_star_count, five_star_count FROM review_stats "
				     "WHERE package_name = '%s'", package_name);
	result = sqlite3_exec (plugin->priv->db,
			       statement,
			       get_review_stats_sqlite_cb,
			       &histogram,
			       &error_msg);
	if (result != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "SQL error: %s", error_msg);
		sqlite3_free (error_msg);
		return FALSE;
	}

	/* Convert to a rating */
	// FIXME: Convert to a Wilson score
	n_ratings = histogram.one_star_count + histogram.two_star_count + histogram.three_star_count + histogram.four_star_count + histogram.five_star_count;
	if (n_ratings == 0)
		*rating = -1;
	else
		*rating = ((histogram.one_star_count * 20) + (histogram.two_star_count * 40) + (histogram.three_star_count * 60) + (histogram.four_star_count * 80) + (histogram.five_star_count * 100)) / n_ratings;
	review_ratings[0] = 0;
	review_ratings[1] = histogram.one_star_count;
	review_ratings[2] = histogram.two_star_count;
	review_ratings[3] = histogram.three_star_count;
	review_ratings[4] = histogram.four_star_count;
	review_ratings[5] = histogram.five_star_count;

	return TRUE;
}

static gboolean
parse_histogram (const gchar *text, Histogram *histogram)
{
	JsonParser *parser = NULL;
	JsonArray *array;
	gboolean result = FALSE;

	/* Histogram is a five element JSON array, e.g. "[1, 3, 5, 8, 4]" */
	parser = json_parser_new ();
	if (!json_parser_load_from_data (parser, text, -1, NULL))
		goto out;
	if (!JSON_NODE_HOLDS_ARRAY (json_parser_get_root (parser)))
		goto out;
	array = json_node_get_array (json_parser_get_root (parser));
	if (json_array_get_length (array) != 5)
		goto out;
	histogram->one_star_count = json_array_get_int_element (array, 0);
	histogram->two_star_count = json_array_get_int_element (array, 1);
	histogram->three_star_count = json_array_get_int_element (array, 2);
	histogram->four_star_count = json_array_get_int_element (array, 3);
	histogram->five_star_count = json_array_get_int_element (array, 4);
	result = TRUE;

out:
	g_clear_object (&parser);

	return result;
}

static gboolean
parse_review_entry (JsonNode *node, const gchar **package_name, Histogram *histogram)
{
	JsonObject *object;
	const gchar *name = NULL, *histogram_text = NULL;

	if (!JSON_NODE_HOLDS_OBJECT (node))
		return FALSE;

	object = json_node_get_object (node);

	name = json_object_get_string_member (object, "package_name");
	histogram_text = json_object_get_string_member (object, "histogram");
	if (!name || !histogram_text)
		return FALSE;

	if (!parse_histogram (histogram_text, histogram))
		return FALSE;
	*package_name = name;

	return TRUE;
}

static gboolean
parse_review_entries (GsPlugin *plugin, JsonParser *parser, GError **error)
{
	JsonArray *array;
	gint i;

	if (!JSON_NODE_HOLDS_ARRAY (json_parser_get_root (parser)))
		return FALSE;
	array = json_node_get_array (json_parser_get_root (parser));
	for (i = 0; i < json_array_get_length (array); i++) {
		const gchar *package_name;
		Histogram histogram;

		/* Read in from JSON... (skip bad entries) */
		if (!parse_review_entry (json_array_get_element (array, i), &package_name, &histogram))
			continue;

		/* ...write into the database (abort everything if can't write) */
		if (!set_package_stats (plugin, package_name, &histogram, error))
			return FALSE;
	}

	return TRUE;
}

static void
sign_message (SoupMessage *message, OAuthMethod method,
	      const gchar *consumer_key, const gchar *consumer_secret,
	      const gchar *token_key, const gchar *token_secret)
{
	g_autofree gchar *url = NULL, *oauth_authorization_parameters = NULL, *authorization_text = NULL;
	gchar **url_parameters = NULL;
	int url_parameters_length;

	url = soup_uri_to_string (soup_message_get_uri (message), FALSE);

	url_parameters_length = oauth_split_url_parameters(url, &url_parameters);
	oauth_sign_array2_process (&url_parameters_length, &url_parameters,
				   NULL,
				   method,
				   message->method,
				   consumer_key, consumer_secret,
				   token_key, token_secret);
	oauth_authorization_parameters = oauth_serialize_url_sep (url_parameters_length, 1, url_parameters, ", ", 6);
	oauth_free_array (&url_parameters_length, &url_parameters);
	authorization_text = g_strdup_printf ("OAuth realm=\"Ratings and Reviews\", %s", oauth_authorization_parameters);
	soup_message_headers_append (message->request_headers, "Authorization", authorization_text);
}

static gboolean
send_review_request (GsPlugin *plugin, const gchar *method, const gchar *path, JsonBuilder *request, gboolean do_sign, JsonParser **result, GError **error)
{
	GsPluginPrivate *priv = plugin->priv;
	g_autofree gchar *uri = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	guint status_code;

	if (!setup_networking (plugin, error))
		return FALSE;

	uri = g_strdup_printf ("%s%s",
			       UBUNTU_REVIEWS_SERVER, path);
	msg = soup_message_new (method, uri);

	if (request != NULL) {
		JsonGenerator *generator;
		gchar *data;
		gsize length;

		generator = json_generator_new ();
		json_generator_set_root (generator, json_builder_get_root (request));
		data = json_generator_to_data (generator, &length);
		soup_message_set_request (msg, "application/json", SOUP_MEMORY_TAKE, data, length);
		g_object_unref (generator);
	}

	if (do_sign)
		sign_message (msg,
			      OA_PLAINTEXT,
			      priv->consumer_key,
			      priv->consumer_secret,
			      priv->token_key,
			      priv->token_secret);

	status_code = soup_session_send_message (plugin->priv->session, msg);
	if (status_code != SOUP_STATUS_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Got status code %s from reviews.ubuntu.com",
			     soup_status_get_phrase (status_code));
		return FALSE;
	}

	if (result != NULL) {
		JsonParser *parser;
		const gchar *content_type;

		content_type = soup_message_headers_get_content_type (msg->response_headers, NULL);
		if (g_strcmp0 (content_type, "application/json") != 0) {
			g_set_error (error,
				     GS_PLUGIN_ERROR,
				     GS_PLUGIN_ERROR_FAILED,
				     "Got unknown content type %s from reviews.ubuntu.com",
				     content_type);
			return FALSE;
		}

		parser = json_parser_new ();
		if (!json_parser_load_from_data (parser, msg->response_body->data, -1, error)) {
			g_object_unref (parser);
			return FALSE;
		}
		*result = parser;
	}

	return TRUE;
}

static gboolean
download_review_stats (GsPlugin *plugin, GError **error)
{
	guint status_code;
	g_autofree gchar *uri = NULL;
	g_autoptr(SoupMessage) msg = NULL;
	JsonParser *result;
	gboolean ret;

	if (!send_review_request (plugin, SOUP_METHOD_GET, "/api/1.0/review-stats/any/any/", NULL, FALSE, &result, error))
		return FALSE;

	/* Extract the stats from the data */
	ret = parse_review_entries (plugin, result, error);
	g_object_unref (result);
	if (!ret)
		return FALSE;

	/* Record the time we downloaded it */
	return set_timestamp (plugin, "stats_mtime", error);
}

static gboolean
load_database (GsPlugin *plugin, GError **error)
{
	const gchar *statement;
	gboolean rebuild_ratings = FALSE;
	char *error_msg = NULL;
	gint result;
	gint64 stats_mtime = 0;
	gint64 now;
	g_autoptr(GError) error_local = NULL;

	g_debug ("trying to open database '%s'", plugin->priv->db_path);
	if (!gs_mkdir_parent (plugin->priv->db_path, error))
		return FALSE;
	result = sqlite3_open (plugin->priv->db_path, &plugin->priv->db);
	if (result != SQLITE_OK) {
		g_set_error (error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Can't open Ubuntu review statistics database: %s",
			     sqlite3_errmsg (plugin->priv->db));
		return FALSE;
	}

	/* We don't need to keep doing fsync */
	sqlite3_exec (plugin->priv->db, "PRAGMA synchronous=OFF",
		      NULL, NULL, NULL);

	/* Create a table to store the stats */
	result = sqlite3_exec (plugin->priv->db, "SELECT * FROM review_stats LIMIT 1", NULL, NULL, &error_msg);
	if (result != SQLITE_OK) {
		g_debug ("creating table to repair: %s", error_msg);
		sqlite3_free (error_msg);
		statement = "CREATE TABLE review_stats ("
			    "package_name TEXT PRIMARY KEY,"
			    "one_star_count INTEGER DEFAULT 0,"
			    "two_star_count INTEGER DEFAULT 0,"
			    "three_star_count INTEGER DEFAULT 0,"
			    "four_star_count INTEGER DEFAULT 0,"
			    "five_star_count INTEGER DEFAULT 0);";
		sqlite3_exec (plugin->priv->db, statement, NULL, NULL, NULL);
		rebuild_ratings = TRUE;
	}

	/* Create a table to store local reviews */
	result = sqlite3_exec (plugin->priv->db, "SELECT * FROM reviews LIMIT 1", NULL, NULL, &error_msg);
	if (result != SQLITE_OK) {
		g_debug ("creating table to repair: %s", error_msg);
		sqlite3_free (error_msg);
		statement = "CREATE TABLE reviews ("
			    "package_name TEXT PRIMARY KEY,"
			    "id TEXT,"
			    "version TEXT,"
			    "date TEXT,"
			    "rating INTEGER,"
			    "summary TEXT,"
			    "text TEXT);";
		sqlite3_exec (plugin->priv->db, statement, NULL, NULL, NULL);
		rebuild_ratings = TRUE;
	}

	/* Create a table to store timestamps */
	result = sqlite3_exec (plugin->priv->db,
			       "SELECT value FROM timestamps WHERE key = 'stats_mtime' LIMIT 1",
			       get_timestamp_sqlite_cb, &stats_mtime,
			       &error_msg);
	if (result != SQLITE_OK) {
		g_debug ("creating table to repair: %s", error_msg);
		sqlite3_free (error_msg);
		statement = "CREATE TABLE timestamps ("
			    "key TEXT PRIMARY KEY,"
			    "value INTEGER DEFAULT 0);";
		sqlite3_exec (plugin->priv->db, statement, NULL, NULL, NULL);

		/* Set the time of database creation */
		if (!set_timestamp (plugin, "stats_ctime", error))
			return FALSE;
	}

	/* Download data if we have none or it is out of date */
	now = g_get_real_time () / G_USEC_PER_SEC;
	if (stats_mtime == 0 || rebuild_ratings) {
		g_debug ("No Ubuntu review statistics");
		if (!download_review_stats (plugin, &error_local)) {
			g_warning ("Failed to get Ubuntu review statistics: %s",
				   error_local->message);
			return TRUE;
		}
	} else if (now - stats_mtime > REVIEW_STATS_AGE_MAX) {
		g_debug ("Ubuntu review statistics was %" G_GINT64_FORMAT
			 " days old, so regetting",
			 (now - stats_mtime) / ( 60 * 60 * 24));
		if (!download_review_stats (plugin, error))
			return FALSE;
	} else {
		g_debug ("Ubuntu review statistics %" G_GINT64_FORMAT
			 " days old, so no need to redownload",
			 (now - stats_mtime) / ( 60 * 60 * 24));
	}
	return TRUE;
}

static GDateTime *
parse_date_time (const gchar *text)
{
	const gchar *format = "YYYY-MM-DD HH:MM:SS";
	int i, value_index, values[6] = { 0, 0, 0, 0, 0, 0 };

	if (!text)
		return NULL;

	/* Extract the numbers as shown in the format */
	for (i = 0, value_index = 0; text[i] && format[i] && value_index < 6; i++) {
		char c = text[i];

		if (c == '-' || c == ' ' || c == ':') {
			if (format[i] != c)
				return NULL;
			value_index++;
		} else {
			int d = c - '0';
			if (d < 0 || d > 9)
				return NULL;
			values[value_index] = values[value_index] * 10 + d;
		}
	}

	/* We didn't match the format */
	if (format[i] != '\0' || text[i] != '\0' || value_index != 5)
		return NULL;

	/* Use the numbers to create a GDateTime object */
	return g_date_time_new_utc (values[0], values[1], values[2], values[3], values[4], values[5]);
}

static GsReview *
parse_review (JsonNode *node)
{
	GsReview *review;
	JsonObject *object;
	gint64 star_rating;

	if (!JSON_NODE_HOLDS_OBJECT (node))
		return NULL;

	object = json_node_get_object (node);

	review = gs_review_new ();
	gs_review_set_reviewer (review, json_object_get_string_member (object, "reviewer_displayname"));
	gs_review_set_summary (review, json_object_get_string_member (object, "summary"));
	gs_review_set_text (review, json_object_get_string_member (object, "review_text"));
	gs_review_set_version (review, json_object_get_string_member (object, "version"));
	star_rating = json_object_get_int_member (object, "rating");
	if (star_rating > 0)
		gs_review_set_rating (review, star_rating * 20);
	gs_review_set_date (review, parse_date_time (json_object_get_string_member (object, "date_created")));
	gs_review_add_metadata (review, "ubuntu-id", json_object_get_string_member (object, "id"));

	return review;
}

static gboolean
parse_reviews (GsPlugin *plugin, JsonParser *parser, GsApp *app, GError **error)
{
	JsonArray *array;
	gint i;

	if (!JSON_NODE_HOLDS_ARRAY (json_parser_get_root (parser)))
		return FALSE;
	array = json_node_get_array (json_parser_get_root (parser));
	for (i = 0; i < json_array_get_length (array); i++) {
		GsReview *review;

		/* Read in from JSON... (skip bad entries) */
		review = parse_review (json_array_get_element (array, i));
		if (!review)
			continue;

		gs_app_add_review (app, review);
		g_object_unref (review);
	}

	return TRUE;
}

static gboolean
download_reviews (GsPlugin *plugin, GsApp *app, const gchar *package_name, GError **error)
{
	g_autofree gchar *path = NULL;
	JsonParser *result;
	gboolean ret;

	/* Get the review stats using HTTP */
	// FIXME: This will only get the first page of reviews
	path = g_strdup_printf ("/api/1.0/reviews/filter/any/any/any/any/%s/", package_name);
	if (!send_review_request (plugin, SOUP_METHOD_GET, path, NULL, FALSE, &result, error))
		return FALSE;

	/* Extract the stats from the data */
	ret = parse_reviews (plugin, result, app, error);
	g_object_unref (result);

	return ret;
}

static gboolean
refine_rating (GsPlugin *plugin, GsApp *app, GError **error)
{
	GPtrArray *sources;
	guint i;

	/* Load database once */
	if (g_once_init_enter (&plugin->priv->db_loaded)) {
		gboolean ret = load_database (plugin, error);
		g_once_init_leave (&plugin->priv->db_loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	/* Skip if already has a rating */
	if (gs_app_get_rating (app) != -1)
		return TRUE;

	sources = gs_app_get_sources (app);
	for (i = 0; i < sources->len; i++) {
		const gchar *package_name;
		gint rating;
		gint review_ratings[6];
		gboolean ret;

		/* If we have a local review, use that as the rating */
		// FIXME

		/* Otherwise use the statistics */
		package_name = g_ptr_array_index (sources, i);
		ret = get_review_stats (plugin, package_name, &rating, review_ratings, error);
		if (!ret)
			return FALSE;
		if (rating != -1) {
			g_autoptr(GArray) ratings = NULL;

			g_debug ("ubuntu-reviews setting rating on %s to %i%%",
				 package_name, rating);
			gs_app_set_rating (app, rating);
			ratings = g_array_sized_new (FALSE, FALSE, sizeof (gint), 6);
			g_array_append_vals (ratings, review_ratings, 6);
			gs_app_set_review_ratings (app, ratings);
			if (rating > 80)
				gs_app_add_kudo (app, GS_APP_KUDO_POPULAR);
		}
	}

	return TRUE;
}

static gboolean
refine_reviews (GsPlugin *plugin, GsApp *app, GError **error)
{
	GPtrArray *sources;
	guint i;

	/* Skip if already has reviews */
	if (gs_app_get_reviews (app)->len > 0)
		return TRUE;

	sources = gs_app_get_sources (app);
	for (i = 0; i < sources->len; i++) {
		const gchar *package_name;
		gboolean ret;

		package_name = g_ptr_array_index (sources, i);
		ret = download_reviews (plugin, app, package_name, error);
		if (!ret)
			return FALSE;
	}

	return TRUE;
}

gboolean
gs_plugin_refine (GsPlugin *plugin,
		  GList **list,
		  GsPluginRefineFlags flags,
		  GCancellable *cancellable,
		  GError **error)
{
	GList *l;
	gboolean ret = TRUE;

	for (l = *list; l != NULL; l = l->next) {
		GsApp *app = GS_APP (l->data);

		if ((flags & (GS_PLUGIN_REFINE_FLAGS_REQUIRE_RATING | GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEW_RATINGS)) != 0) {
			if (!refine_rating (plugin, app, error))
				return FALSE;
		}
		if ((flags & GS_PLUGIN_REFINE_FLAGS_REQUIRE_REVIEWS) != 0) {
			if (!refine_reviews (plugin, app, error))
				return FALSE;
		}
	}

	return ret;
}

static void
add_string_member (JsonBuilder *builder, const gchar *name, const gchar *value)
{
	json_builder_set_member_name (builder, name);
	json_builder_add_string_value (builder, value);
}

static void
add_int_member (JsonBuilder *builder, const gchar *name, gint64 value)
{
	json_builder_set_member_name (builder, name);
	json_builder_add_int_value (builder, value);
}

static gboolean
set_package_review (GsPlugin *plugin,
		    GsReview *review,
		    const gchar *package_name,
		    GError **error)
{
	GsPluginPrivate *priv = plugin->priv;
	gint rating;
	gint n_stars;
	g_autofree gchar *os_id = NULL, *os_ubuntu_codename = NULL, *language = NULL, *architecture = NULL;
	gchar *c;
	JsonBuilder *request;
	gboolean ret;

	/* Ubuntu reviews require a summary and description - just make one up for now */
	rating = gs_review_get_rating (review);
	if (rating > 80)
		n_stars = 5;
	else if (rating > 60)
		n_stars = 4;
	else if (rating > 40)
		n_stars = 3;
	else if (rating > 20)
		n_stars = 2;
	else
		n_stars = 1;

	os_id = gs_os_release_get_id (error);
	if (os_id == NULL)
		return FALSE;
	os_ubuntu_codename = gs_os_release_get ("UBUNTU_CODENAME", error);
	if (os_ubuntu_codename == NULL)
		return FALSE;

	/* Convert locale into language */
	language = g_strdup (plugin->locale);
	c = strchr (language, '_');
	if (c)
		*c = '\0';

	// FIXME: Need to get Apt::Architecture configuration value from APT
	architecture = g_strdup ("amd64");

	/* Create message for reviews.ubuntu.com */
	request = json_builder_new ();
	json_builder_begin_object (request);
	add_string_member (request, "package_name", package_name);
	add_string_member (request, "summary", gs_review_get_summary (review));
	add_string_member (request, "review_text", gs_review_get_text (review));
	add_string_member (request, "language", language);
	add_string_member (request, "origin", os_id);
	add_string_member (request, "distroseries", os_ubuntu_codename);
	add_string_member (request, "version", gs_review_get_version (review));
	add_int_member (request, "rating", n_stars);
	add_string_member (request, "arch_tag", architecture);
	json_builder_end_object (request);
	ret = send_review_request (plugin, SOUP_METHOD_POST, "/api/1.0/reviews/", request, TRUE, NULL, error);
	g_object_unref (request);

	return ret;
}

static gboolean
set_review_usefulness (GsPlugin *plugin,
		       const gchar *review_id,
		       gboolean is_useful,
		       GError **error)
{
	g_autofree gchar *path;

	/* Create message for reviews.ubuntu.com */
	path = g_strdup_printf ("/api/1.0/reviews/%s/recommendations/?useful=%s", review_id, is_useful ? "True" : "False");
	return send_review_request (plugin, SOUP_METHOD_POST, path, NULL, TRUE, NULL, error);
}

static gboolean
report_review (GsPlugin *plugin,
	       const gchar *review_id,
	       const gchar *reason,
	       const gchar *text,
	       GError **error)
{
	g_autofree gchar *path;

	/* Create message for reviews.ubuntu.com */
	// FIXME: escape reason / text properly
	path = g_strdup_printf ("/api/1.0/reviews/%s/recommendations/?reason=%s&text=%s", review_id, reason, text);
	return send_review_request (plugin, SOUP_METHOD_POST, path, NULL, TRUE, NULL, error);
}

static gboolean
remove_review (GsPlugin *plugin,
	       const gchar *review_id,
	       GError **error)
{
	g_autofree gchar *path;

	/* Create message for reviews.ubuntu.com */
	path = g_strdup_printf ("/api/1.0/reviews/delete/%s/", review_id);
	return send_review_request (plugin, SOUP_METHOD_POST, path, NULL, TRUE, NULL, error);
}

typedef struct
{
	GsPlugin *plugin;
	GError **error;

	GCond cond;
	GMutex mutex;
	gboolean done;
	gboolean success;
	gboolean remember;
} LoginContext;

static gboolean
show_login_dialog (gpointer user_data)
{
	LoginContext *context = user_data;
	GsPluginPrivate *priv = context->plugin->priv;
	GtkWidget *dialog;

	dialog = gs_ubuntu_login_dialog_new ();
	switch (gtk_dialog_run (GTK_DIALOG (dialog))) {
	case GTK_RESPONSE_DELETE_EVENT:
	case GTK_RESPONSE_CANCEL:
		g_set_error (context->error,
			     GS_PLUGIN_ERROR,
			     GS_PLUGIN_ERROR_FAILED,
			     "Unable to sign into Ubuntu One");

		context->success = FALSE;
		break;

	case GTK_RESPONSE_OK:
		context->remember = gs_ubuntu_login_dialog_get_do_remember (GS_UBUNTU_LOGIN_DIALOG (dialog));
		priv->consumer_key = g_strdup (gs_ubuntu_login_dialog_get_consumer_key (GS_UBUNTU_LOGIN_DIALOG (dialog)));
		priv->consumer_secret = g_strdup (gs_ubuntu_login_dialog_get_consumer_secret (GS_UBUNTU_LOGIN_DIALOG (dialog)));
		priv->token_key = g_strdup (gs_ubuntu_login_dialog_get_token_key (GS_UBUNTU_LOGIN_DIALOG (dialog)));
		priv->token_secret = g_strdup (gs_ubuntu_login_dialog_get_token_secret (GS_UBUNTU_LOGIN_DIALOG (dialog)));
		context->success = TRUE;
		break;
	}

	gtk_widget_destroy (dialog);

	g_mutex_lock (&context->mutex);
	context->done = TRUE;
	g_cond_signal (&context->cond);
	g_mutex_unlock (&context->mutex);

	return G_SOURCE_REMOVE;
}

static gboolean
sign_into_ubuntu (GsPlugin  *plugin,
		  GError   **error)
{
	GsPluginPrivate *priv = plugin->priv;
	LoginContext context = { 0 };

	if (priv->consumer_key != NULL &&
	    priv->consumer_secret != NULL &&
	    priv->token_key != NULL &&
	    priv->token_secret != NULL)
		return TRUE;

	g_clear_pointer (&priv->token_secret, g_free);
	g_clear_pointer (&priv->token_key, g_free);
	g_clear_pointer (&priv->consumer_secret, g_free);
	g_clear_pointer (&priv->consumer_key, g_free);

	context.plugin = plugin;
	context.error = error;
	g_cond_init (&context.cond);
	g_mutex_init (&context.mutex);
	g_mutex_lock (&context.mutex);

	gdk_threads_add_idle (show_login_dialog, &context);

	while (!context.done)
		g_cond_wait (&context.cond, &context.mutex);

	g_mutex_unlock (&context.mutex);
	g_mutex_clear (&context.mutex);
	g_cond_clear (&context.cond);

	return context.success;
}

gboolean
gs_plugin_review_submit (GsPlugin *plugin,
			 GsApp *app,
			 GsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	/* Load database once */
	if (g_once_init_enter (&plugin->priv->db_loaded)) {
		gboolean ret = load_database (plugin, error);
		g_once_init_leave (&plugin->priv->db_loaded, TRUE);
		if (!ret)
			return FALSE;
	}

	if (!setup_networking (plugin, error))
		return FALSE;

	if (!sign_into_ubuntu (plugin, error))
		return FALSE;

	return set_package_review (plugin,
				   review,
				   gs_app_get_source_default (app),
				   error);
}

gboolean
gs_plugin_review_report (GsPlugin *plugin,
			 GsApp *app,
			 GsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	const gchar *review_id;

	/* Can only modify Ubuntu reviews */
	review_id = gs_review_get_metadata_item (review, "ubuntu-id");
	if (review_id == NULL)
		return TRUE;

	if (!report_review (plugin, review_id, "FIXME: gnome-software", "FIXME: gnome-software", error))
		return FALSE;
	gs_review_add_flags (review, GS_REVIEW_FLAG_VOTED);
	return TRUE;
}

gboolean
gs_plugin_review_upvote (GsPlugin *plugin,
			 GsApp *app,
			 GsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	const gchar *review_id;

	/* Can only modify Ubuntu reviews */
	review_id = gs_review_get_metadata_item (review, "ubuntu-id");
	if (review_id == NULL)
		return TRUE;

	if (!set_review_usefulness (plugin, review_id, TRUE, error))
		return FALSE;
	gs_review_add_flags (review, GS_REVIEW_FLAG_VOTED);
	return TRUE;
}

gboolean
gs_plugin_review_downvote (GsPlugin *plugin,
			   GsApp *app,
			   GsReview *review,
			   GCancellable *cancellable,
			   GError **error)
{
	const gchar *review_id;

	/* Can only modify Ubuntu reviews */
	review_id = gs_review_get_metadata_item (review, "ubuntu-id");
	if (review_id == NULL)
		return TRUE;

	if (!set_review_usefulness (plugin, review_id, FALSE, error))
		return FALSE;
	gs_review_add_flags (review, GS_REVIEW_FLAG_VOTED);
	return TRUE;
}

gboolean
gs_plugin_review_remove (GsPlugin *plugin,
			 GsApp *app,
			 GsReview *review,
			 GCancellable *cancellable,
			 GError **error)
{
	const gchar *review_id;

	/* Can only modify Ubuntu reviews */
	review_id = gs_review_get_metadata_item (review, "ubuntu-id");
	if (review_id == NULL)
		return TRUE;

	return remove_review (plugin, review_id, error);
}
