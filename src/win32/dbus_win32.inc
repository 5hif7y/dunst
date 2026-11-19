/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Windows transport for dbus.c (textually included at the end of dbus.c so it
 * can reach the static introspection data and method tables).
 *
 * Wire format: LowLevel-Katas N4-notifications/wnotify/PROTOCOL.md
 *   org.freedesktop.Notifications methods: native wnotify lines, so any
 *     wnotify client (notify-send, tiramisu clients) works unchanged.
 *   org.dunstproject.cmd0 methods (dunstctl):
 *     "org.dunstproject.cmd0.<Method>\t<GVariant text of the in-args tuple>"
 *     reply "OK\t<GVariant text>" (typed, g_variant_parse() round-trips it)
 *   properties:
 *     "org.freedesktop.DBus.Properties.Get\t<name>"
 *     "org.freedesktop.DBus.Properties.Set\t<name>\t<GVariant text>"
 *
 * Threading: wnotify calls us on a per-client thread. dbus.c (queues, draw,
 * settings) is single-threaded GLib main-loop code, so every request is
 * marshalled to the main context and the client thread waits for the reply,
 * which is what GDBus does for us on Linux.
 */
#include "win32/wnotify.h"

struct wn_invocation {
        gboolean flatten;   /* fdn replies: plain fields; dunst replies: GVariant text */
        gboolean json;      /* request prefixed with "json:" -> reply value as JSON */
        char *reply;        /* set exactly once */
};

struct wn_call {
        char **fields;
        size_t n_fields;
        char *sender;
        WnInvocation inv;
        GMutex lock;
        GCond cond;
        gboolean done;
};

static wn_server *wn_srv;
static volatile gint wn_shutting_down; /* main loop gone: queued calls will never run */
static int wn_dummy_connection; /* dbus.c checks dbus_conn != NULL before signalling */

/* ---- GVariant <-> wnotify ------------------------------------------------ */

static void flatten_into(wn_buf *b, GVariant *v)
{
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
                wn_buf_field(b, g_variant_get_string(v, NULL));
        } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)
                   || g_variant_is_of_type(v, G_VARIANT_TYPE_TUPLE)) {
                GVariantIter it;
                GVariant *child;
                g_variant_iter_init(&it, v);
                while ((child = g_variant_iter_next_value(&it))) {
                        flatten_into(b, child);
                        g_variant_unref(child);
                }
        } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) {
                wn_buf_fieldf(b, "%u", g_variant_get_uint32(v));
        } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT32)) {
                wn_buf_fieldf(b, "%d", g_variant_get_int32(v));
        } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
                wn_buf_field(b, g_variant_get_boolean(v) ? "true" : "false");
        } else {
                gchar *s = g_variant_print(v, FALSE);
                wn_buf_field(b, s);
                g_free(s);
        }
}

/* GVariant -> JSON, for `dunstctl history` / `rules` (busctl --json on Linux).
 * Variants are unwrapped, dicts become objects, byte strings arrays of ints. */
static void json_string(GString *out, const char *str)
{
        g_string_append_c(out, '"');
        for (const unsigned char *p = (const unsigned char *)str; *p; p++) {
                if (*p == '"' || *p == '\\')
                        g_string_append_printf(out, "\\%c", *p);
                else if (*p < 0x20)
                        g_string_append_printf(out, "\\u%04x", *p);
                else
                        g_string_append_c(out, (char)*p);
        }
        g_string_append_c(out, '"');
}

static void variant_to_json(GString *out, GVariant *v)
{
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
                GVariant *inner = g_variant_get_variant(v);
                variant_to_json(out, inner);
                g_variant_unref(inner);
        } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                   || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)) {
                json_string(out, g_variant_get_string(v, NULL));
        } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) {
                g_string_append(out, g_variant_get_boolean(v) ? "true" : "false");
        } else if (g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE)) {
                g_string_append_printf(out, "%.17g", g_variant_get_double(v));
        } else if (g_variant_type_is_basic(g_variant_get_type(v))) {
                gchar *s = g_variant_print(v, FALSE); /* integers */
                g_string_append(out, s);
                g_free(s);
        } else if (g_variant_type_is_dict_entry(g_variant_type_element(g_variant_get_type(v)))
                   && g_variant_type_is_array(g_variant_get_type(v))) {
                GVariantIter it;
                GVariant *entry;
                gboolean first = TRUE;
                g_string_append_c(out, '{');
                g_variant_iter_init(&it, v);
                while ((entry = g_variant_iter_next_value(&it))) {
                        GVariant *k = g_variant_get_child_value(entry, 0);
                        GVariant *val = g_variant_get_child_value(entry, 1);
                        if (!first)
                                g_string_append_c(out, ',');
                        first = FALSE;
                        if (g_variant_is_of_type(k, G_VARIANT_TYPE_STRING)) {
                                json_string(out, g_variant_get_string(k, NULL));
                        } else {
                                gchar *ks = g_variant_print(k, FALSE);
                                json_string(out, ks);
                                g_free(ks);
                        }
                        g_string_append_c(out, ':');
                        variant_to_json(out, val);
                        g_variant_unref(k);
                        g_variant_unref(val);
                        g_variant_unref(entry);
                }
                g_string_append_c(out, '}');
        } else { /* arrays, tuples, maybes */
                GVariantIter it;
                GVariant *child;
                gboolean first = TRUE;
                g_string_append_c(out, '[');
                g_variant_iter_init(&it, v);
                while ((child = g_variant_iter_next_value(&it))) {
                        if (!first)
                                g_string_append_c(out, ',');
                        first = FALSE;
                        variant_to_json(out, child);
                        g_variant_unref(child);
                }
                g_string_append_c(out, ']');
        }
}

/* One hint from the wire to the GVariant type dunst expects (D-Bus a{sv}). */
static GVariant *hint_to_variant(const wn_hint *h)
{
        const char *v = h->value;
        if (g_str_equal(h->type, "s")) return g_variant_new_string(v);
        if (g_str_equal(h->type, "b")) return g_variant_new_boolean(g_str_equal(v, "true") || g_str_equal(v, "1"));
        if (g_str_equal(h->type, "y")) return g_variant_new_byte((guchar)g_ascii_strtoull(v, NULL, 10));
        if (g_str_equal(h->type, "n")) return g_variant_new_int16((gint16)g_ascii_strtoll(v, NULL, 10));
        if (g_str_equal(h->type, "q")) return g_variant_new_uint16((guint16)g_ascii_strtoull(v, NULL, 10));
        if (g_str_equal(h->type, "i")) return g_variant_new_int32((gint32)g_ascii_strtoll(v, NULL, 10));
        if (g_str_equal(h->type, "u")) return g_variant_new_uint32((guint32)g_ascii_strtoull(v, NULL, 10));
        if (g_str_equal(h->type, "x")) return g_variant_new_int64(g_ascii_strtoll(v, NULL, 10));
        if (g_str_equal(h->type, "t")) return g_variant_new_uint64(g_ascii_strtoull(v, NULL, 10));
        if (g_str_equal(h->type, "d")) return g_variant_new_double(g_ascii_strtod(v, NULL));
        if (g_str_equal(h->type, "image")) {
                int w, hh, stride, alpha, bps, ch, used = 0;
                if (sscanf(v, "%d:%d:%d:%d:%d:%d:%n", &w, &hh, &stride, &alpha, &bps, &ch, &used) != 6 || !used)
                        return NULL;
                size_t len;
                unsigned char *px = wn_base64_decode(v + used, &len);
                GVariant *data = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, px, len, 1);
                free(px);
                return g_variant_new("(iiibii@ay)", w, hh, stride, (gboolean)alpha, bps, ch, data);
        }
        return NULL;
}

static GVariant *notify_to_variant(const wn_notify *n)
{
        GVariantBuilder actions, hints;
        g_variant_builder_init(&actions, G_VARIANT_TYPE_STRING_ARRAY);
        for (size_t i = 0; i < n->n_actions; i++)
                g_variant_builder_add(&actions, "s", n->actions[i]);
        g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
        for (size_t i = 0; i < n->n_hints; i++) {
                GVariant *v = hint_to_variant(&n->hints[i]);
                if (v)
                        g_variant_builder_add(&hints, "{sv}", n->hints[i].name, v);
                else
                        LOG_W("wnotify: ignoring hint '%s' of type '%s'", n->hints[i].name, n->hints[i].type);
        }
        return g_variant_new("(susssasa{sv}i)", n->app_name, n->replaces_id, n->app_icon,
                             n->summary, n->body, &actions, &hints, n->expire_timeout);
}

/* Expected in-args tuple type of a dunst method, from the introspection XML. */
static GVariantType *in_args_type(const char *method)
{
        GDBusMethodInfo *mi = g_dbus_interface_info_lookup_method(introspection_data->interfaces[1], method);
        if (!mi)
                return NULL;
        GString *sig = g_string_new("(");
        for (GDBusArgInfo **a = mi->in_args; a && *a; a++)
                g_string_append(sig, (*a)->signature);
        g_string_append_c(sig, ')');
        GVariantType *t = g_variant_type_new(sig->str);
        g_string_free(sig, TRUE);
        return t;
}

/* ---- invocation (what dbus.c calls through gdbus_shim.h) ----------------- */

void wn_inv_return_value(WnInvocation *inv, GVariant *value)
{
        if (inv->reply)
                return;
        wn_buf b = { 0 };
        wn_buf_field(&b, "OK");
        if (value) {
                g_variant_ref_sink(value);
                if (inv->json) {
                        GString *js = g_string_new(NULL);
                        /* unwrap the reply tuple when it holds one value */
                        if (g_variant_is_of_type(value, G_VARIANT_TYPE_TUPLE) && g_variant_n_children(value) == 1) {
                                GVariant *only = g_variant_get_child_value(value, 0);
                                variant_to_json(js, only);
                                g_variant_unref(only);
                        } else {
                                variant_to_json(js, value);
                        }
                        wn_buf_field(&b, js->str);
                        g_string_free(js, TRUE);
                } else if (inv->flatten) {
                        flatten_into(&b, value);
                } else {
                        gchar *s = g_variant_print(value, TRUE);
                        wn_buf_field(&b, s);
                        g_free(s);
                }
                g_variant_unref(value);
        }
        inv->reply = b.data;
}

void wn_inv_return_error_literal(WnInvocation *inv, const char *message)
{
        if (inv->reply)
                return;
        wn_buf b = { 0 };
        wn_buf_field(&b, "ERR");
        wn_buf_field(&b, message);
        inv->reply = b.data;
}

void wn_inv_return_error(WnInvocation *inv, GQuark domain, gint code, const char *format, ...)
{
        (void)domain; (void)code;
        va_list ap;
        va_start(ap, format);
        gchar *msg = g_strdup_vprintf(format, ap);
        va_end(ap);
        wn_inv_return_error_literal(inv, msg);
        g_free(msg);
}

gboolean wn_emit_signal(const char *iface, const char *name, GVariant *body, GError **error)
{
        (void)error;
        if (!wn_srv) {
                if (body)
                        g_variant_unref(g_variant_ref_sink(body));
                return FALSE;
        }
        wn_buf b = { 0 };
        g_variant_ref_sink(body);
        if (g_str_equal(iface, FDN_IFAC)) {
                /* NotificationClosed / ActionInvoked / ActivationToken: plain wnotify signals */
                wn_buf_field(&b, name);
                flatten_into(&b, body);
        } else {
                gchar *full = g_strdup_printf("%s.%s", iface, name);
                gchar *s = g_variant_print(body, TRUE);
                wn_buf_field(&b, full);
                wn_buf_field(&b, s);
                g_free(full);
                g_free(s);
        }
        g_variant_unref(body);
        wn_server_emit(wn_srv, b.data);
        wn_buf_free(&b);
        return TRUE;
}

/* ---- dispatch (main thread) ------------------------------------------------ */

static void dispatch(struct wn_call *call)
{
        char **f = call->fields;
        const char *method = f[0];
        WnInvocation *inv = &call->inv;
        if (g_str_has_prefix(method, "json:")) {
                inv->json = TRUE;
                method += 5;
        }
        GVariant *params = NULL;
        static const char dunst_prefix[] = DUNST_IFAC ".";
        static const char prop_get[] = PROPERTIES_IFAC ".Get";
        static const char prop_set[] = PROPERTIES_IFAC ".Set";

        if (g_str_equal(method, "Notify")) {
                wn_notify n;
                if (wn_parse_notify(f, call->n_fields, &n)) {
                        wn_inv_return_error_literal(inv, "malformed Notify");
                        return;
                }
                params = g_variant_ref_sink(notify_to_variant(&n));
                wn_notify_free(&n);
        } else if (g_str_equal(method, "CloseNotification")) {
                guint32 id = call->n_fields > 1 ? (guint32)g_ascii_strtoull(f[1], NULL, 10) : 0;
                params = g_variant_ref_sink(g_variant_new("(u)", id));
        }

        if (g_str_equal(method, "Notify") || g_str_equal(method, "CloseNotification")
            || g_str_equal(method, "GetCapabilities") || g_str_equal(method, "GetServerInformation")) {
                inv->flatten = TRUE;
                dbus_cb_fdn_methods((GDBusConnection *)&wn_dummy_connection, call->sender, FDN_PATH,
                                    FDN_IFAC, method, params, inv, NULL);
        } else if (g_str_has_prefix(method, dunst_prefix)) {
                const char *name = method + strlen(dunst_prefix);
                GVariantType *type = in_args_type(name);
                GError *err = NULL;
                if (!type) {
                        wn_inv_return_error_literal(inv, "unknown method");
                        return;
                }
                const char *text = call->n_fields > 1 ? f[1] : "()";
                params = g_variant_parse(type, text, NULL, NULL, &err);
                g_variant_type_free(type);
                if (!params) {
                        wn_inv_return_error(inv, 0, 0, "bad arguments for %s: %s", name, err->message);
                        g_error_free(err);
                        return;
                }
                g_variant_ref_sink(params);
                dbus_cb_dunst_methods((GDBusConnection *)&wn_dummy_connection, call->sender, DUNST_PATH,
                                      DUNST_IFAC, name, params, inv, NULL);
        } else if (g_str_equal(method, prop_get) && call->n_fields > 1) {
                GError *err = NULL;
                GVariant *v = dbus_cb_dunst_Properties_Get(NULL, call->sender, DUNST_PATH, DUNST_IFAC,
                                                           f[1], &err, NULL);
                if (v) {
                        wn_inv_return_value(inv, v);
                } else {
                        wn_inv_return_error_literal(inv, err ? err->message : "unknown property");
                        g_clear_error(&err);
                }
        } else if (g_str_equal(method, prop_set) && call->n_fields > 2) {
                GError *err = NULL;
                GVariant *v = g_variant_parse(NULL, f[2], NULL, NULL, &err);
                if (v && dbus_cb_dunst_Properties_Set(NULL, call->sender, DUNST_PATH, DUNST_IFAC,
                                                      f[1], g_variant_ref_sink(v), &err, NULL))
                        wn_inv_return_value(inv, NULL);
                else
                        wn_inv_return_error_literal(inv, err ? err->message : "cannot set property");
                g_clear_error(&err);
                if (v)
                        g_variant_unref(v);
        } else {
                wn_inv_return_error_literal(inv, "unknown method");
        }

        if (params)
                g_variant_unref(params);
        /* methods like CloseNotification answer with NULL -> "OK"; a method
         * that did not answer at all is a bug, but never leave the client hanging */
        if (!inv->reply)
                wn_inv_return_value(inv, NULL);
}

static gboolean dispatch_in_main(gpointer data)
{
        struct wn_call *call = data;
        dispatch(call);
        g_mutex_lock(&call->lock);
        call->done = TRUE;
        g_cond_signal(&call->cond);
        g_mutex_unlock(&call->lock);
        return G_SOURCE_REMOVE;
}

/* wnotify client thread */
static void on_request(wn_conn *conn, char **fields, size_t n_fields, void *ud)
{
        (void)ud;
        struct wn_call call = { .fields = fields, .n_fields = n_fields };
        call.sender = g_strdup_printf("wnotify:%p", (void *)conn);
        g_mutex_init(&call.lock);
        g_cond_init(&call.cond);

        g_main_context_invoke(NULL, dispatch_in_main, &call);

        g_mutex_lock(&call.lock);
        while (!call.done && !g_atomic_int_get(&wn_shutting_down))
                g_cond_wait_until(&call.cond, &call.lock, g_get_monotonic_time() + 100 * G_TIME_SPAN_MILLISECOND);
        gboolean done = call.done;
        g_mutex_unlock(&call.lock);

        if (done)
                wn_reply(conn, call.inv.reply);
        else
                wn_reply_err(conn, "dunst is shutting down");
        free(call.inv.reply);
        g_free(call.sender);
        g_mutex_clear(&call.lock);
        g_cond_clear(&call.cond);
}

int dbus_init(void)
{
        char err[512];
        introspection_data = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
        wn_srv = wn_server_start(on_request, NULL, err, sizeof err);
        if (!wn_srv)
                DIE("Cannot acquire '" FDN_NAME "': %s", err);
        dbus_conn = (GDBusConnection *)&wn_dummy_connection;
        return 1;
}

void dbus_teardown(int owner_id)
{
        (void)owner_id;
        g_atomic_int_set(&wn_shutting_down, 1);
        wn_server_stop(wn_srv);
        wn_srv = NULL;
        dbus_conn = NULL;
        g_clear_pointer(&introspection_data, g_dbus_node_info_unref);
}
