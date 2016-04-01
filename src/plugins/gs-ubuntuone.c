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

#include <libsecret/secret.h>

#include <gs-plugin.h>

#include "gs-ubuntuone.h"
#include "gs-ubuntu-login-dialog.h"

#define SCHEMA_NAME     "com.ubuntu.UbuntuOne.GnomeSoftware"
#define CONSUMER_KEY    "consumer-key"
#define CONSUMER_SECRET "consumer-secret"
#define TOKEN_KEY       "token-key"
#define TOKEN_SECRET    "token-secret"

typedef struct
{
	GCancellable *cancellable;
	GCond cond;
	GMutex mutex;

	gint waiting;

	gchar *consumer_key;
	gchar *consumer_secret;
	gchar *token_key;
	gchar *token_secret;
} SecretContext;

static void
lookup_consumer_key (GObject      *source_object,
		     GAsyncResult *result,
		     gpointer      user_data)
{
	SecretContext *context = user_data;

	context->consumer_key = secret_password_lookup_finish (result, NULL);

	g_mutex_lock (&context->mutex);

	if (context->consumer_key != NULL)
		context->waiting--;
	else
		context->waiting = 0;

	g_cond_signal (&context->cond);
	g_mutex_unlock (&context->mutex);
}

static void
lookup_consumer_secret (GObject      *source_object,
			GAsyncResult *result,
			gpointer      user_data)
{
	SecretContext *context = user_data;

	context->consumer_secret = secret_password_lookup_finish (result, NULL);

	g_mutex_lock (&context->mutex);

	if (context->consumer_secret != NULL)
		context->waiting--;
	else
		context->waiting = 0;

	g_cond_signal (&context->cond);
	g_mutex_unlock (&context->mutex);
}

static void
lookup_token_key (GObject      *source_object,
		  GAsyncResult *result,
		  gpointer      user_data)
{
	SecretContext *context = user_data;

	context->token_key = secret_password_lookup_finish (result, NULL);

	g_mutex_lock (&context->mutex);

	if (context->token_key != NULL)
		context->waiting--;
	else
		context->waiting = 0;

	g_cond_signal (&context->cond);
	g_mutex_unlock (&context->mutex);
}

static void
lookup_token_secret (GObject      *source_object,
		     GAsyncResult *result,
		     gpointer      user_data)
{
	SecretContext *context = user_data;

	context->token_secret = secret_password_lookup_finish (result, NULL);

	g_mutex_lock (&context->mutex);

	if (context->token_secret != NULL)
		context->waiting--;
	else
		context->waiting = 0;

	g_cond_signal (&context->cond);
	g_mutex_unlock (&context->mutex);
}

typedef struct
{
	GError **error;

	GCond cond;
	GMutex mutex;

	gboolean done;
	gboolean success;
	gboolean remember;

	gchar *consumer_key;
	gchar *consumer_secret;
	gchar *token_key;
	gchar *token_secret;
} LoginContext;

static gboolean
show_login_dialog (gpointer user_data)
{
	LoginContext *context = user_data;
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
		context->consumer_key = g_strdup (gs_ubuntu_login_dialog_get_consumer_key (GS_UBUNTU_LOGIN_DIALOG (dialog)));
		context->consumer_secret = g_strdup (gs_ubuntu_login_dialog_get_consumer_secret (GS_UBUNTU_LOGIN_DIALOG (dialog)));
		context->token_key = g_strdup (gs_ubuntu_login_dialog_get_token_key (GS_UBUNTU_LOGIN_DIALOG (dialog)));
		context->token_secret = g_strdup (gs_ubuntu_login_dialog_get_token_secret (GS_UBUNTU_LOGIN_DIALOG (dialog)));
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

gboolean
gs_ubuntuone_sign_in (gchar **consumer_key, gchar **consumer_secret, gchar **token_key, gchar **token_secret, GError **error)
{
	static SecretSchema schema = {
		SCHEMA_NAME,
		SECRET_SCHEMA_NONE,
		{ { "key", SECRET_SCHEMA_ATTRIBUTE_STRING } }
	};

	SecretContext secret_context = { 0 };
	LoginContext login_context = { 0 };

	/* Use credentials from libsecret if available */
	secret_context.waiting = 4;
	secret_context.cancellable = g_cancellable_new ();
	g_cond_init (&secret_context.cond);
	g_mutex_init (&secret_context.mutex);
	g_mutex_lock (&secret_context.mutex);

	secret_password_lookup (&schema,
				secret_context.cancellable,
				lookup_consumer_key,
				&secret_context,
				"key", CONSUMER_KEY,
				NULL);
	secret_password_lookup (&schema,
				secret_context.cancellable,
				lookup_consumer_secret,
				&secret_context,
				"key", CONSUMER_SECRET,
				NULL);
	secret_password_lookup (&schema,
				secret_context.cancellable,
				lookup_token_key,
				&secret_context,
				"key", TOKEN_KEY,
				NULL);
	secret_password_lookup (&schema,
				secret_context.cancellable,
				lookup_token_secret,
				&secret_context,
				"key", TOKEN_SECRET,
				NULL);

	while (secret_context.waiting > 0)
		g_cond_wait (&secret_context.cond, &secret_context.mutex);

	g_mutex_unlock (&secret_context.mutex);
	g_mutex_clear (&secret_context.mutex);
	g_cond_clear (&secret_context.cond);
	g_cancellable_cancel (secret_context.cancellable);
	g_clear_object (&secret_context.cancellable);

	if (secret_context.consumer_key != NULL &&
	    secret_context.consumer_secret != NULL &&
	    secret_context.token_key != NULL &&
	    secret_context.token_secret != NULL) {
		*consumer_key = secret_context.consumer_key;
		*consumer_secret = secret_context.consumer_secret;
		*token_key = secret_context.token_key;
		*token_secret = secret_context.token_secret;
		return TRUE;
	}

	/* Otherwise start with a clean slate */
	g_clear_pointer (&secret_context.token_secret, g_free);
	g_clear_pointer (&secret_context.token_key, g_free);
	g_clear_pointer (&secret_context.consumer_secret, g_free);
	g_clear_pointer (&secret_context.consumer_key, g_free);

	/* Pop up a login dialog */
	login_context.error = error;
	g_cond_init (&login_context.cond);
	g_mutex_init (&login_context.mutex);
	g_mutex_lock (&login_context.mutex);

	gdk_threads_add_idle (show_login_dialog, &login_context);

	while (!login_context.done)
		g_cond_wait (&login_context.cond, &login_context.mutex);

	g_mutex_unlock (&login_context.mutex);
	g_mutex_clear (&login_context.mutex);
	g_cond_clear (&login_context.cond);

	if (login_context.remember) {
		secret_password_store (&schema,
				       NULL,
				       SCHEMA_NAME,
				       login_context.consumer_key,
				       NULL,
				       NULL,
				       NULL,
				       "key", CONSUMER_KEY,
				       NULL);

		secret_password_store (&schema,
				       NULL,
				       SCHEMA_NAME,
				       login_context.consumer_secret,
				       NULL,
				       NULL,
				       NULL,
				       "key", CONSUMER_SECRET,
				       NULL);

		secret_password_store (&schema,
				       NULL,
				       SCHEMA_NAME,
				       login_context.token_key,
				       NULL,
				       NULL,
				       NULL,
				       "key", TOKEN_KEY,
				       NULL);

		secret_password_store (&schema,
				       NULL,
				       SCHEMA_NAME,
				       login_context.token_secret,
				       NULL,
				       NULL,
				       NULL,
				       "key", TOKEN_SECRET,
				       NULL);
	}

	*consumer_key = login_context.consumer_key;
	*consumer_secret = login_context.consumer_secret;
	*token_key = login_context.token_key;
	*token_secret = login_context.token_secret;
	return login_context.success;
}
