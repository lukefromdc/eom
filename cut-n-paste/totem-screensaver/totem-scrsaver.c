/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-

   Copyright (C) 2004-2006 Bastien Nocera <hadess@hadess.net>
   Copyright © 2010 Christian Persch
   Copyright © 2010 Carlos Garcia Campos

   The Mate Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   The Mate Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with the Mate Library; see the file COPYING.LIB.  If not,
   write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301  USA.

   Authors: Bastien Nocera <hadess@hadess.net>
            Christian Persch
            Carlos Garcia Campos
 */

#include "config.h"

#include <glib/gi18n.h>

#include <gdk/gdk.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/keysym.h>

#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif /* HAVE_XTEST */
#endif /* GDK_WINDOWING_X11 */

#include "totem-scrsaver.h"

#define GS_SERVICE   "org.gnome.SessionManager"
#define GS_PATH      "/org/gnome/SessionManager"
#define GS_INTERFACE "org.gnome.SessionManager"

#define GSM_INHIBITOR_FLAG_IDLE 1 << 3
#define XSCREENSAVER_MIN_TIMEOUT 60

enum {
	PROP_0,
	PROP_REASON
};

static void totem_scrsaver_finalize   (GObject *object);

struct TotemScrsaverPrivate {
	/* Whether the screensaver is disabled */
	gboolean disabled;
	/* The reason for the inhibition */
	char *reason;

	GDBusProxy *gs_proxy;
	gboolean have_session_dbus;
	guint32 cookie;

	/* To save the screensaver info */
	int timeout;
	int interval;
	int prefer_blanking;
	int allow_exposures;

	/* For use with XTest */
	int keycode1, keycode2;
	int *keycode;
	gboolean have_xtest;
};

G_DEFINE_TYPE(TotemScrsaver, totem_scrsaver, G_TYPE_OBJECT)

static gboolean
screensaver_is_running_dbus (TotemScrsaver *scr)
{
	return scr->priv->have_session_dbus;
}

static void
on_inhibit_cb (GObject      *source_object,
	       GAsyncResult *res,
	       gpointer      user_data)
{
	GDBusProxy    *proxy = G_DBUS_PROXY (source_object);
	TotemScrsaver *scr = TOTEM_SCRSAVER (user_data);
	GVariant      *value;
	GError        *error = NULL;

	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (!value) {
		g_warning ("Problem inhibiting the screensaver: %s", error->message);
		g_object_unref (scr);
		g_error_free (error);

		return;
	}

	/* save the cookie */
	if (g_variant_is_of_type (value, G_VARIANT_TYPE ("(u)")))
		g_variant_get (value, "(u)", &scr->priv->cookie);
	else
		scr->priv->cookie = 0;
	g_variant_unref (value);
	g_object_unref (scr);
}

static void
on_uninhibit_cb (GObject      *source_object,
		 GAsyncResult *res,
		 gpointer      user_data)
{
	GDBusProxy    *proxy = G_DBUS_PROXY (source_object);
	TotemScrsaver *scr = TOTEM_SCRSAVER (user_data);
	GVariant      *value;
	GError        *error = NULL;

	value = g_dbus_proxy_call_finish (proxy, res, &error);
	if (!value) {
		g_warning ("Problem uninhibiting the screensaver: %s", error->message);
		g_object_unref (scr);
		g_error_free (error);

		return;
	}

	/* clear the cookie */
	scr->priv->cookie = 0;
	g_variant_unref (value);

	g_object_unref (scr);
}

static void
screensaver_inhibit_dbus (TotemScrsaver *scr,
			  gboolean	 inhibit)
{
	TotemScrsaverPrivate *priv = scr->priv;

	if (!priv->have_session_dbus)
		return;

	g_object_ref (scr);

	if (inhibit) {
		g_return_if_fail (scr->priv->reason != NULL);
		g_dbus_proxy_call (priv->gs_proxy,
				   "Inhibit",
				   g_variant_new ("(susu)",
						  g_get_application_name (),
						  0,
						  scr->priv->reason,
						  GSM_INHIBITOR_FLAG_IDLE),
				   G_DBUS_CALL_FLAGS_NO_AUTO_START,
				   -1,
				   NULL,
				   on_inhibit_cb,
				   scr);
	} else {
		g_dbus_proxy_call (priv->gs_proxy,
				   "Uninhibit",
				   g_variant_new ("(u)", priv->cookie),
				   G_DBUS_CALL_FLAGS_NO_AUTO_START,
				   -1,
				   NULL,
				   on_uninhibit_cb,
				   scr);
	}
}

static void
screensaver_enable_dbus (TotemScrsaver *scr)
{
	screensaver_inhibit_dbus (scr, FALSE);
}

static void
screensaver_disable_dbus (TotemScrsaver *scr)
{
	screensaver_inhibit_dbus (scr, TRUE);
}

static void
screensaver_update_dbus_presence (TotemScrsaver *scr)
{
	TotemScrsaverPrivate *priv = scr->priv;
	gchar *name_owner;

	name_owner = g_dbus_proxy_get_name_owner (priv->gs_proxy);
	if (name_owner) {
		priv->have_session_dbus = TRUE;
		g_free (name_owner);
	} else {
		priv->have_session_dbus = FALSE;
	}
}

static void
screensaver_dbus_owner_changed_cb (GObject    *object,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
	TotemScrsaver *scr = TOTEM_SCRSAVER (user_data);

	screensaver_update_dbus_presence (scr);
}

static void
screensaver_dbus_proxy_new_cb (GObject      *source,
                               GAsyncResult *result,
                               gpointer      user_data)
{
	TotemScrsaver *scr = TOTEM_SCRSAVER (user_data);
	TotemScrsaverPrivate *priv = scr->priv;

	priv->gs_proxy = g_dbus_proxy_new_for_bus_finish (result, NULL);
	if (!priv->gs_proxy)
		return;

	screensaver_update_dbus_presence (scr);

	g_signal_connect (priv->gs_proxy, "notify::g-name-owner",
	                  G_CALLBACK (screensaver_dbus_owner_changed_cb),
	                  scr);
}

static void
screensaver_init_dbus (TotemScrsaver *scr)
{
	g_dbus_proxy_new_for_bus (G_BUS_TYPE_SESSION,
	                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
	                          NULL,
	                          GS_SERVICE,
	                          GS_PATH,
	                          GS_INTERFACE,
	                          NULL,
	                          screensaver_dbus_proxy_new_cb,
	                          scr);
}

static void
screensaver_finalize_dbus (TotemScrsaver *scr)
{
	if (scr->priv->gs_proxy) {
		g_object_unref (scr->priv->gs_proxy);
	}
}

#ifdef GDK_WINDOWING_X11
static void
screensaver_enable_x11 (TotemScrsaver *scr)
{
	Display *display;

#ifdef HAVE_XTEST
	if (scr->priv->have_xtest != FALSE)
	{
		g_source_remove_by_user_data (scr);
		return;
	}
#endif /* HAVE_XTEST */

	display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
	XLockDisplay (display);
	XSetScreenSaver (display,
			scr->priv->timeout,
			scr->priv->interval,
			scr->priv->prefer_blanking,
			scr->priv->allow_exposures);
	XUnlockDisplay (display);
}

#ifdef HAVE_XTEST
static gboolean
fake_event (TotemScrsaver *scr)
{
	if (scr->priv->disabled)
	{
		Display *display;

		display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());
		XLockDisplay (display);
		XTestFakeKeyEvent (display, *scr->priv->keycode,
				True, CurrentTime);
		XTestFakeKeyEvent (display, *scr->priv->keycode,
				False, CurrentTime);
		XUnlockDisplay (display);
		/* Swap the keycode */
		if (scr->priv->keycode == &scr->priv->keycode1)
			scr->priv->keycode = &scr->priv->keycode2;
		else
			scr->priv->keycode = &scr->priv->keycode1;
	}

	return TRUE;
}
#endif /* HAVE_XTEST */

static void
screensaver_disable_x11 (TotemScrsaver *scr)
{
	Display *display;

	display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());

#ifdef HAVE_XTEST
	if (scr->priv->have_xtest != FALSE)
	{
		XLockDisplay (display);
		XGetScreenSaver(display, &scr->priv->timeout,
				&scr->priv->interval,
				&scr->priv->prefer_blanking,
				&scr->priv->allow_exposures);
		XUnlockDisplay (display);

		if (scr->priv->timeout != 0) {
			g_timeout_add_seconds (scr->priv->timeout / 2,
					       (GSourceFunc) fake_event, scr);
		} else {
			g_timeout_add_seconds (XSCREENSAVER_MIN_TIMEOUT / 2,
					       (GSourceFunc) fake_event, scr);
		}

		return;
	}
#endif /* HAVE_XTEST */

	XLockDisplay (display);
	XGetScreenSaver(display, &scr->priv->timeout,
			&scr->priv->interval,
			&scr->priv->prefer_blanking,
			&scr->priv->allow_exposures);
	XSetScreenSaver(display, 0, 0,
			DontPreferBlanking, DontAllowExposures);
	XUnlockDisplay (display);
}

static void
screensaver_init_x11 (TotemScrsaver *scr)
{
#ifdef HAVE_XTEST
	int a, b, c, d;
	Display *display;

	display = GDK_DISPLAY_XDISPLAY (gdk_display_get_default ());

	XLockDisplay (display);
	scr->priv->have_xtest = (XTestQueryExtension (display, &a, &b, &c, &d) == True);
	if (scr->priv->have_xtest != FALSE)
	{
		scr->priv->keycode1 = XKeysymToKeycode (display, XK_Alt_L);
		if (scr->priv->keycode1 == 0) {
			g_warning ("scr->priv->keycode1 not existent");
		}
		scr->priv->keycode2 = XKeysymToKeycode (display, XK_Alt_R);
		if (scr->priv->keycode2 == 0) {
			scr->priv->keycode2 = XKeysymToKeycode (display, XK_Alt_L);
			if (scr->priv->keycode2 == 0) {
				g_warning ("scr->priv->keycode2 not existent");
			}
		}
		scr->priv->keycode = &scr->priv->keycode1;
	}
	XUnlockDisplay (display);
#endif /* HAVE_XTEST */
}

static void
screensaver_finalize_x11 (TotemScrsaver *scr)
{
	g_source_remove_by_user_data (scr);
}
#endif

static void
totem_scrsaver_get_property (GObject *object,
			     guint property_id,
			     GValue *value,
			     GParamSpec *pspec)
{
	TotemScrsaver *scr;

	scr = TOTEM_SCRSAVER (object);

	switch (property_id)
	{
	case PROP_REASON:
		g_value_set_string (value, scr->priv->reason);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static void
totem_scrsaver_set_property (GObject *object,
			     guint property_id,
			     const GValue *value,
			     GParamSpec *pspec)
{
	TotemScrsaver *scr;

	scr = TOTEM_SCRSAVER (object);

	switch (property_id)
	{
	case PROP_REASON:
		g_free (scr->priv->reason);
		scr->priv->reason = g_value_dup_string (value);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
	}
}

static void
totem_scrsaver_class_init (TotemScrsaverClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	g_type_class_add_private (klass, sizeof (TotemScrsaverPrivate));

	object_class->set_property = totem_scrsaver_set_property;
	object_class->get_property = totem_scrsaver_get_property;
	object_class->finalize = totem_scrsaver_finalize;

	g_object_class_install_property (object_class, PROP_REASON,
					 g_param_spec_string ("reason", NULL, NULL,
							      NULL, G_PARAM_READWRITE));

}

/**
 * totem_scrsaver_new:
 *
 * Creates a #TotemScrsaver object.
 * If the MATE screen saver is running, it uses its DBUS interface to
 * inhibit the screensaver; otherwise it falls back to using the X
 * screensaver functionality for this.
 *
 * Returns: a newly created #TotemScrsaver
 */
TotemScrsaver *
totem_scrsaver_new (void)
{
	return TOTEM_SCRSAVER (g_object_new (TOTEM_TYPE_SCRSAVER, NULL));
}

static void
totem_scrsaver_init (TotemScrsaver *scr)
{
	scr->priv = G_TYPE_INSTANCE_GET_PRIVATE (scr,
						 TOTEM_TYPE_SCRSAVER,
						 TotemScrsaverPrivate);

	screensaver_init_dbus (scr);
#ifdef GDK_WINDOWING_X11
	screensaver_init_x11 (scr);
#else
#warning Unimplemented
#endif
}

void
totem_scrsaver_disable (TotemScrsaver *scr)
{
	g_return_if_fail (TOTEM_IS_SCRSAVER (scr));

	if (scr->priv->disabled != FALSE)
		return;

	scr->priv->disabled = TRUE;

	if (screensaver_is_running_dbus (scr) != FALSE)
		screensaver_disable_dbus (scr);
	else 
#ifdef GDK_WINDOWING_X11
		screensaver_disable_x11 (scr);
#else
#warning Unimplemented
	{}
#endif
}

void
totem_scrsaver_enable (TotemScrsaver *scr)
{
	g_return_if_fail (TOTEM_IS_SCRSAVER (scr));

	if (scr->priv->disabled == FALSE)
		return;

	scr->priv->disabled = FALSE;

	if (screensaver_is_running_dbus (scr) != FALSE)
		screensaver_enable_dbus (scr);
	else
#ifdef GDK_WINDOWING_X11
		screensaver_enable_x11 (scr);
#else
#warning Unimplemented
	{}
#endif
}

void
totem_scrsaver_set_state (TotemScrsaver *scr, gboolean enable)
{
	g_return_if_fail (TOTEM_IS_SCRSAVER (scr));

	if (scr->priv->disabled == !enable)
		return;

	if (enable == FALSE)
		totem_scrsaver_disable (scr);
	else
		totem_scrsaver_enable (scr);
}

static void
totem_scrsaver_finalize (GObject *object)
{
	TotemScrsaver *scr = TOTEM_SCRSAVER (object);

	g_free (scr->priv->reason);

	screensaver_finalize_dbus (scr);
#ifdef GDK_WINDOWING_X11
	screensaver_finalize_x11 (scr);
#else
#warning Unimplemented
	{}
#endif

	G_OBJECT_CLASS (totem_scrsaver_parent_class)->finalize (object);
}
