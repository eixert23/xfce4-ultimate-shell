#include "launcher-entry.h"
#include <gio/gio.h>
#include <string.h>

/* One entry per app, keyed by normalized desktop id. The D-Bus sender is kept
   so the entry can be dropped when that app quits without resetting it. */
typedef struct {
    LauncherEntryState state;
    gchar             *sender;
} Entry;

static GDBusConnection         *g_le_bus = NULL;
static GHashTable              *g_le_entries = NULL;
static LauncherEntryChangedFunc g_le_cb = NULL;
static gpointer                 g_le_cb_data = NULL;
static guint                    g_le_notify_id = 0;

static void entry_free(gpointer p) {
    Entry *e = p;
    if (!e) return;
    g_free(e->sender);
    g_free(e);
}

gchar *launcher_entry_normalize_key(const gchar *raw) {
    if (!raw || !raw[0]) return NULL;
    if (g_str_has_prefix(raw, "application://")) raw += strlen("application://");
    const gchar *slash = strrchr(raw, '/');          /* file:///usr/share/applications/x.desktop */
    if (slash && slash[1]) raw = slash + 1;
    gchar *lower = g_utf8_strdown(raw, -1);
    if (g_str_has_suffix(lower, ".desktop")) lower[strlen(lower) - 8] = '\0';
    const gchar *dot = strrchr(lower, '.');
    if (dot && dot[1]) {
        gchar *s = g_strdup(dot + 1);
        g_free(lower);
        return s;
    }
    return lower;
}

/* Batch bursts of updates (progress can arrive many times a second) into one redraw */
static gboolean notify_idle(gpointer data) {
    g_le_notify_id = 0;
    if (g_le_cb) g_le_cb(g_le_cb_data);
    return G_SOURCE_REMOVE;
}

static void schedule_notify(void) {
    if (!g_le_notify_id) g_le_notify_id = g_timeout_add(120, notify_idle, NULL);
}

static void on_update(GDBusConnection *c, const gchar *sender, const gchar *path,
                      const gchar *iface, const gchar *signal, GVariant *params, gpointer data) {
    (void)c; (void)path; (void)iface; (void)signal; (void)data;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(sa{sv})"))) return;

    const gchar *uri = NULL;
    GVariant *props = NULL;
    g_variant_get(params, "(&s@a{sv})", &uri, &props);
    gchar *key = launcher_entry_normalize_key(uri);
    if (!key) { g_variant_unref(props); return; }

    Entry *e = g_hash_table_lookup(g_le_entries, key);
    if (!e) {
        e = g_new0(Entry, 1);
        g_hash_table_insert(g_le_entries, g_strdup(key), e);
    }
    if (g_strcmp0(e->sender, sender) != 0) {
        g_free(e->sender);
        e->sender = g_strdup(sender);
    }

    /* Each signal carries only what changed; keep the rest */
    GVariantIter it;
    const gchar *k;
    GVariant *v;
    g_variant_iter_init(&it, props);
    while (g_variant_iter_next(&it, "{&sv}", &k, &v)) {
        if (g_strcmp0(k, "count") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_INT64))
            e->state.count = g_variant_get_int64(v);
        else if (g_strcmp0(k, "count-visible") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
            e->state.count_visible = g_variant_get_boolean(v);
        else if (g_strcmp0(k, "progress") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_DOUBLE))
            e->state.progress = CLAMP(g_variant_get_double(v), 0.0, 1.0);
        else if (g_strcmp0(k, "progress-visible") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
            e->state.progress_visible = g_variant_get_boolean(v);
        else if (g_strcmp0(k, "urgent") == 0 && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN))
            e->state.urgent = g_variant_get_boolean(v);
        g_variant_unref(v);
    }
    g_variant_unref(props);
    g_free(key);
    schedule_notify();
}

/* App quit (or crashed): forget everything it published */
static void on_name_owner_changed(GDBusConnection *c, const gchar *sender, const gchar *path,
                                  const gchar *iface, const gchar *signal, GVariant *params, gpointer data) {
    (void)c; (void)sender; (void)path; (void)iface; (void)signal; (void)data;
    const gchar *name = NULL, *old_owner = NULL, *new_owner = NULL;
    g_variant_get(params, "(&s&s&s)", &name, &old_owner, &new_owner);
    if (!name || name[0] != ':' || (new_owner && new_owner[0])) return;   /* only unique names going away */

    gboolean removed = FALSE;
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_le_entries);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Entry *e = v;
        if (g_strcmp0(e->sender, name) == 0) { g_hash_table_iter_remove(&it); removed = TRUE; }
    }
    if (removed) schedule_notify();
}

void launcher_entry_init(LauncherEntryChangedFunc on_changed, gpointer user_data) {
    g_le_cb = on_changed;
    g_le_cb_data = user_data;
    if (g_le_bus) return;

    GError *err = NULL;
    g_le_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (!g_le_bus) {
        g_warning("LauncherEntry: no session bus: %s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    g_le_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, entry_free);

    g_dbus_connection_signal_subscribe(g_le_bus, NULL, "com.canonical.Unity.LauncherEntry", "Update",
                                       NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_update, NULL, NULL);
    g_dbus_connection_signal_subscribe(g_le_bus, "org.freedesktop.DBus", "org.freedesktop.DBus",
                                       "NameOwnerChanged", "/org/freedesktop/DBus", NULL,
                                       G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, NULL, NULL);
}

gboolean launcher_entry_lookup(const gchar *norm_key, LauncherEntryState *out) {
    if (!g_le_entries || !norm_key) return FALSE;
    Entry *e = g_hash_table_lookup(g_le_entries, norm_key);
    if (!e) return FALSE;
    if (out) *out = e->state;
    return TRUE;
}
