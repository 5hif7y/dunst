/* SPDX-License-Identifier: BSD-3-Clause */
/**
 * @file
 * @brief Keep dbus.c's method implementations untouched on Windows.
 *
 * GDBus itself builds on Windows, but there is no session bus to own a name
 * on. dbus.c only touches the *transport* through a handful of calls; this
 * header redirects exactly those to the wnotify named pipe:
 *
 *   GDBusMethodInvocation                -> struct wn_invocation (one pipe request)
 *   g_dbus_method_invocation_return_*    -> reply line on that request
 *   g_dbus_connection_emit_signal        -> broadcast line to Subscribe'd clients
 *   g_dbus_connection_flush              -> no-op (replies are written synchronously)
 *
 * Everything that builds or reads GVariants (dbus_message_to_notification,
 * history listing, rules) is compiled unchanged.
 */
#ifndef DUNST_GDBUS_SHIM_H
#define DUNST_GDBUS_SHIM_H

#include <gio/gio.h>

typedef struct wn_invocation WnInvocation;

void wn_inv_return_value(WnInvocation *inv, GVariant *value);
void wn_inv_return_error_literal(WnInvocation *inv, const char *message);
void wn_inv_return_error(WnInvocation *inv, GQuark domain, gint code, const char *format, ...);
gboolean wn_emit_signal(const char *iface, const char *name, GVariant *body, GError **error);

#define GDBusMethodInvocation WnInvocation
#define g_dbus_method_invocation_return_value(inv, v) wn_inv_return_value((inv), (v))
#define g_dbus_method_invocation_return_dbus_error(inv, name, msg) wn_inv_return_error_literal((inv), (msg))
#define g_dbus_method_invocation_return_error wn_inv_return_error
#define g_dbus_connection_flush(...) ((void)0)
#define g_dbus_connection_emit_signal(conn, dest, path, iface, name, body, err) \
        wn_emit_signal((iface), (name), (body), (err))

#endif
