#include "notifications.h"
#include "shell-plugin.h"
#include <gio/gio.h>
#include <string.h>
#include <xfconf/xfconf.h>

/* =========================================================================
 * Ultimate Shell notification server (org.freedesktop.Notifications 1.2)
 *  - replaces_id updates in place; "transient" notifications skip history
 *  - NotificationClosed / ActionInvoked signals (spec-compliant)
 *  - action buttons, click-to-open (default action), click × to dismiss
 *  - expire_timeout honoured; critical urgency stays until dismissed
 *  - icons: image-data / image-path hints, app_icon (name, path or file:// URI)
 *  - safe body markup (falls back to plain text)
 *  - stacked toasts, top-right or bottom-right depending on where the panel is
 *  - history grouped by app, max 100, per-item ×, Do Not Disturb
 * ========================================================================= */

#define NOTIF_HISTORY_MAX   100
#define NOTIF_DEFAULT_MS    5000
#define TOAST_WIDTH         340
#define TOAST_GAP           8
#define TOAST_MARGIN        12

typedef struct _NotificationCenter NotificationCenter;

typedef struct {
    NotificationCenter *nc;
    guint32    id;
    gchar     *app_name;
    gchar     *app_icon;
    gchar     *summary;
    gchar     *body;          /* raw body as sent */
    gchar    **actions;       /* key,label,key,label,... */
    guchar     urgency;       /* 0 low, 1 normal, 2 critical */
    gboolean   transient;
    gint       expire_ms;     /* -1 default, 0 never */
    GdkPixbuf *image;         /* from image-data / image-path, may be NULL */
    GDateTime *time;

    gboolean   in_history;
    GtkWidget *row;           /* history row (inside its app group) */
    GtkWidget *toast;         /* visible toast window, or NULL */
    guint      toast_timer;
} Notif;

typedef struct {
    gchar     *app_name;
    GtkWidget *frame;         /* whole group box */
    GtkWidget *rows;          /* vbox of rows */
    gint       count;
} NotifGroup;

struct _NotificationCenter {
    GtkWidget       *bell_btn;
    GtkWidget       *badge_label;
    GtkWidget       *popup_win;
    GtkWidget       *groups_box;
    GtkWidget       *empty_lbl;
    GtkWidget       *dnd_switch;
    guint            unread_count;
    guint32          next_notif_id;
    guint            bus_owner_id;
    GDBusNodeInfo   *notif_node;
    GDBusConnection *conn;
    GHashTable      *by_id;        /* id -> Notif */
    GList           *history;      /* Notif*, newest first (only stored ones) */
    GList           *toasts;       /* Notif* with a visible toast, oldest first */
    GHashTable      *groups;       /* app name -> NotifGroup */
    gboolean         dnd;
};

static const gchar fdo_notifications_xml[] =
    "<node>"
    "  <interface name='org.freedesktop.Notifications'>"
    "    <method name='GetCapabilities'>"
    "      <arg type='as' name='capabilities' direction='out'/>"
    "    </method>"
    "    <method name='Notify'>"
    "      <arg type='s' name='app_name' direction='in'/>"
    "      <arg type='u' name='replaces_id' direction='in'/>"
    "      <arg type='s' name='app_icon' direction='in'/>"
    "      <arg type='s' name='summary' direction='in'/>"
    "      <arg type='s' name='body' direction='in'/>"
    "      <arg type='as' name='actions' direction='in'/>"
    "      <arg type='a{sv}' name='hints' direction='in'/>"
    "      <arg type='i' name='expire_timeout' direction='in'/>"
    "      <arg type='u' name='id' direction='out'/>"
    "    </method>"
    "    <method name='CloseNotification'>"
    "      <arg type='u' name='id' direction='in'/>"
    "    </method>"
    "    <method name='GetServerInformation'>"
    "      <arg type='s' name='name' direction='out'/>"
    "      <arg type='s' name='vendor' direction='out'/>"
    "      <arg type='s' name='version' direction='out'/>"
    "      <arg type='s' name='spec_version' direction='out'/>"
    "    </method>"
    "    <signal name='NotificationClosed'>"
    "      <arg type='u' name='id'/>"
    "      <arg type='u' name='reason'/>"
    "    </signal>"
    "    <signal name='ActionInvoked'>"
    "      <arg type='u' name='id'/>"
    "      <arg type='s' name='action_key'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

enum { CLOSE_EXPIRED = 1, CLOSE_DISMISSED = 2, CLOSE_BY_CALL = 3 };

static void toast_close(Notif *n);
static void reposition_toasts(NotificationCenter *nc);
static void history_remove(Notif *n);

/* ---------------------------------------------------------------- helpers */

static void update_bell_badge(NotificationCenter *nc) {
    shell_set_badge_count(nc->badge_label, nc->unread_count);
}

static void emit_closed(NotificationCenter *nc, guint32 id, guint32 reason) {
    if (!nc->conn) return;
    g_dbus_connection_emit_signal(nc->conn, NULL, "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "NotificationClosed",
        g_variant_new("(uu)", id, reason), NULL);
}

static void emit_action(NotificationCenter *nc, guint32 id, const gchar *key) {
    if (!nc->conn || !key) return;
    g_dbus_connection_emit_signal(nc->conn, NULL, "/org/freedesktop/Notifications",
        "org.freedesktop.Notifications", "ActionInvoked",
        g_variant_new("(us)", id, key), NULL);
}

static gboolean notif_has_action(Notif *n, const gchar *key) {
    for (gint i = 0; n->actions && n->actions[i] && n->actions[i + 1]; i += 2) {
        if (g_strcmp0(n->actions[i], key) == 0) return TRUE;
    }
    return FALSE;
}

/* Body markup: GTK/Pango subset. Drop <a>, <img> tags (unsupported in a label), then
   validate; if it still isn't valid markup, show it as plain text. */
static gchar *safe_body_markup(const gchar *body) {
    if (!body || !body[0]) return g_strdup("");
    GString *s = g_string_new("");
    const gchar *p = body;
    while (*p) {
        if (*p == '<' && (g_ascii_strncasecmp(p, "<a ", 3) == 0 || g_ascii_strncasecmp(p, "<a>", 3) == 0 ||
                          g_ascii_strncasecmp(p, "</a>", 4) == 0 || g_ascii_strncasecmp(p, "<img", 4) == 0)) {
            const gchar *e = strchr(p, '>');
            if (!e) break;
            p = e + 1;
            continue;
        }
        g_string_append_c(s, *p++);
    }
    gchar *cand = g_string_free(s, FALSE);
    if (pango_parse_markup(cand, -1, 0, NULL, NULL, NULL, NULL)) return cand;
    g_free(cand);
    return g_markup_escape_text(body, -1);
}

static GdkPixbuf *pixbuf_from_image_data(GVariant *v) {
    if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE("(iiibiiay)"))) return NULL;
    gint w, h, stride, bps, chans;
    gboolean alpha;
    GVariant *data = NULL;
    g_variant_get(v, "(iiibii@ay)", &w, &h, &stride, &alpha, &bps, &chans, &data);
    GdkPixbuf *pb = NULL;
    gsize len = 0;
    const guchar *raw = g_variant_get_fixed_array(data, &len, 1);
    if (raw && w > 0 && h > 0 && bps == 8 && (chans == 3 || chans == 4) &&
        len >= (gsize)stride * (gsize)(h - 1) + (gsize)w * (gsize)chans) {
        GBytes *bytes = g_bytes_new(raw, len);
        pb = gdk_pixbuf_new_from_bytes(bytes, GDK_COLORSPACE_RGB, alpha, 8, w, h, stride);
        g_bytes_unref(bytes);
    }
    g_variant_unref(data);
    return pb;
}

/* An icon from: our image pixbuf, then app_icon (file path / file:// URI / icon name) */
static GtkWidget *make_notif_icon(Notif *n, gint px) {
    if (n->image) {
        GdkPixbuf *sc = gdk_pixbuf_scale_simple(n->image, px, px, GDK_INTERP_BILINEAR);
        GtkWidget *img = gtk_image_new_from_pixbuf(sc ? sc : n->image);
        if (sc) g_object_unref(sc);
        return img;
    }
    const gchar *ic = n->app_icon;
    if (ic && ic[0]) {
        gchar *path = NULL;
        if (g_str_has_prefix(ic, "file://")) path = g_filename_from_uri(ic, NULL, NULL);
        else if (ic[0] == '/') path = g_strdup(ic);
        if (path) {
            GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, px, px, TRUE, NULL);
            g_free(path);
            if (pb) { GtkWidget *img = gtk_image_new_from_pixbuf(pb); g_object_unref(pb); return img; }
        } else {
            GtkWidget *img = gtk_image_new_from_icon_name(ic, GTK_ICON_SIZE_DIALOG);
            gtk_image_set_pixel_size(GTK_IMAGE(img), px);
            return img;
        }
    }
    GtkWidget *img = gtk_image_new_from_icon_name("dialog-information-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    return img;
}

static void notif_free(Notif *n) {
    if (!n) return;
    if (n->toast_timer) g_source_remove(n->toast_timer);
    if (n->toast) gtk_widget_destroy(n->toast);
    g_free(n->app_name); g_free(n->app_icon); g_free(n->summary); g_free(n->body);
    g_strfreev(n->actions);
    if (n->image) g_object_unref(n->image);
    if (n->time) g_date_time_unref(n->time);
    g_free(n);
}

/* Drop a notification entirely once it is neither shown nor in history */
static void notif_release_if_unused(Notif *n) {
    if (n->toast || n->in_history) return;
    g_hash_table_remove(n->nc->by_id, GUINT_TO_POINTER(n->id));
    notif_free(n);
}

/* ---------------------------------------------------------------- toasts */

static gboolean on_toast_expired(gpointer user_data) {
    Notif *n = (Notif *)user_data;
    n->toast_timer = 0;
    NotificationCenter *nc = n->nc;
    guint32 id = n->id;
    toast_close(n);                        /* may free n */
    emit_closed(nc, id, CLOSE_EXPIRED);
    return G_SOURCE_REMOVE;
}

static void on_toast_close_clicked(GtkButton *btn, Notif *n) {
    NotificationCenter *nc = n->nc;
    guint32 id = n->id;
    toast_close(n);
    emit_closed(nc, id, CLOSE_DISMISSED);
}

static void on_toast_action_clicked(GtkButton *btn, Notif *n) {
    const gchar *key = g_object_get_data(G_OBJECT(btn), "action-key");
    NotificationCenter *nc = n->nc;
    guint32 id = n->id;
    emit_action(nc, id, key);
    toast_close(n);
    emit_closed(nc, id, CLOSE_DISMISSED);
}

/* Click on the toast body: run the "default" action if the app offers one, then dismiss */
static gboolean on_toast_body_clicked(GtkWidget *w, GdkEventButton *ev, Notif *n) {
    if (ev->button != 1) return FALSE;
    NotificationCenter *nc = n->nc;
    guint32 id = n->id;
    if (notif_has_action(n, "default")) emit_action(nc, id, "default");
    toast_close(n);
    emit_closed(nc, id, CLOSE_DISMISSED);
    return TRUE;
}

static gboolean bell_is_low_on_screen(NotificationCenter *nc, GdkRectangle *geom) {
    GdkWindow *gw = nc->bell_btn ? gtk_widget_get_window(nc->bell_btn) : NULL;
    if (!gw) return FALSE;
    gint x = 0, y = 0;
    gdk_window_get_origin(gw, &x, &y);
    return y > geom->y + geom->height / 2;
}

static void reposition_toasts(NotificationCenter *nc) {
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    if (!monitor) return;
    GdkRectangle wa, geom;
    gdk_monitor_get_workarea(monitor, &wa);
    gdk_monitor_get_geometry(monitor, &geom);
    gboolean bottom = bell_is_low_on_screen(nc, &geom);

    /* newest nearest the panel */
    gint offset = TOAST_MARGIN;
    for (GList *l = g_list_last(nc->toasts); l; l = l->prev) {
        Notif *n = (Notif *)l->data;
        if (!n->toast) continue;
        GtkRequisition req;
        gtk_widget_get_preferred_size(n->toast, NULL, &req);
        gint h = MAX(req.height, 40);
        gint x = wa.x + wa.width - TOAST_WIDTH - TOAST_MARGIN;
        gint y = bottom ? (wa.y + wa.height - offset - h) : (wa.y + offset);
        gtk_window_move(GTK_WINDOW(n->toast), x, y);
        offset += h + TOAST_GAP;
        if (offset > wa.height - 60) break;    /* screen full: older ones wait off-stack */
    }
}

static void toast_close(Notif *n) {
    NotificationCenter *nc = n->nc;
    if (n->toast_timer) { g_source_remove(n->toast_timer); n->toast_timer = 0; }
    if (n->toast) {
        gtk_widget_destroy(n->toast);
        n->toast = NULL;
        nc->toasts = g_list_remove(nc->toasts, n);
        reposition_toasts(nc);
    }
    notif_release_if_unused(n);
}

static void toast_build(Notif *n) {
    if (n->toast) { gtk_widget_destroy(n->toast); n->toast = NULL; }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_widget_set_name(win, "ultimate-toast-win");
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_widget_set_size_request(win, TOAST_WIDTH, -1);

    GtkWidget *eb = gtk_event_box_new();
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_toast_body_clicked), n);
    gtk_container_add(GTK_CONTAINER(win), eb);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);
    gtk_container_add(GTK_CONTAINER(eb), outer);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *icon = make_notif_icon(n, 40);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(top), icon, FALSE, FALSE, 0);

    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *title = gtk_label_new(NULL);
    gchar *tm = g_markup_printf_escaped("<b>%s</b>  <span size='small' alpha='60%%'>%s</span>",
        n->summary && n->summary[0] ? n->summary : "Notification",
        n->app_name && n->app_name[0] ? n->app_name : "");
    gtk_label_set_markup(GTK_LABEL(title), tm);
    g_free(tm);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 30);
    gtk_box_pack_start(GTK_BOX(texts), title, FALSE, FALSE, 0);

    if (n->body && n->body[0]) {
        GtkWidget *body = gtk_label_new(NULL);
        gchar *bm = safe_body_markup(n->body);
        gtk_label_set_markup(GTK_LABEL(body), bm);
        g_free(bm);
        gtk_label_set_xalign(GTK_LABEL(body), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(body), 34);
        gtk_label_set_lines(GTK_LABEL(body), 6);
        gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(texts), body, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(top), texts, TRUE, TRUE, 0);

    GtkWidget *x_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(x_btn), GTK_RELIEF_NONE);
    gtk_widget_set_valign(x_btn, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(x_btn, "Dismiss");
    g_signal_connect(x_btn, "clicked", G_CALLBACK(on_toast_close_clicked), n);
    gtk_box_pack_end(GTK_BOX(top), x_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), top, FALSE, FALSE, 0);

    /* Action buttons (the "default" action is the click on the toast itself) */
    GtkWidget *acts = NULL;
    for (gint i = 0; n->actions && n->actions[i] && n->actions[i + 1]; i += 2) {
        if (g_strcmp0(n->actions[i], "default") == 0) continue;
        if (!acts) {
            acts = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            gtk_widget_set_halign(acts, GTK_ALIGN_END);
        }
        GtkWidget *b = gtk_button_new_with_label(n->actions[i + 1][0] ? n->actions[i + 1] : n->actions[i]);
        g_object_set_data_full(G_OBJECT(b), "action-key", g_strdup(n->actions[i]), g_free);
        g_signal_connect(b, "clicked", G_CALLBACK(on_toast_action_clicked), n);
        gtk_box_pack_start(GTK_BOX(acts), b, FALSE, FALSE, 0);
    }
    if (acts) gtk_box_pack_start(GTK_BOX(outer), acts, FALSE, FALSE, 0);

    n->toast = win;
    gtk_widget_show_all(win);
}

static void toast_show(Notif *n) {
    NotificationCenter *nc = n->nc;
    gboolean was_showing = (n->toast != NULL);
    toast_build(n);
    if (!was_showing) nc->toasts = g_list_append(nc->toasts, n);
    reposition_toasts(nc);

    if (n->toast_timer) { g_source_remove(n->toast_timer); n->toast_timer = 0; }
    gint ms = n->expire_ms;
    if (n->urgency == 2) ms = 0;              /* critical: stays until dismissed */
    else if (ms < 0) {
        /* app left it to us: use the duration chosen in Settings ▸ Notifications */
        XfconfChannel *ch = xfconf_channel_get("xfce4-panel");
        gint secs = ch ? xfconf_channel_get_int(ch, "/plugins/ultimateshell/notif_timeout_s", NOTIF_DEFAULT_MS / 1000)
                       : NOTIF_DEFAULT_MS / 1000;
        ms = CLAMP(secs, 2, 60) * 1000;
    }
    if (ms > 0) n->toast_timer = g_timeout_add(ms, on_toast_expired, n);
}

/* ---------------------------------------------------------------- history */

static void update_empty_state(NotificationCenter *nc) {
    if (nc->empty_lbl) gtk_widget_set_visible(nc->empty_lbl, nc->history == NULL);
}

static void on_row_dismiss_clicked(GtkButton *btn, Notif *n) {
    NotificationCenter *nc = n->nc;
    guint32 id = n->id;
    gboolean was_toast = (n->toast != NULL);
    history_remove(n);
    if (was_toast) {
        toast_close(n);
        emit_closed(nc, id, CLOSE_DISMISSED);
    } else {
        notif_release_if_unused(n);
    }
}

static gboolean on_row_clicked(GtkWidget *w, GdkEventButton *ev, Notif *n) {
    if (ev->button != 1) return FALSE;
    if (notif_has_action(n, "default")) {
        emit_action(n->nc, n->id, "default");
        if (n->nc->bell_btn) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(n->nc->bell_btn), FALSE);
    }
    return TRUE;
}

static NotifGroup *get_group(NotificationCenter *nc, const gchar *app) {
    const gchar *key = (app && app[0]) ? app : "System";
    NotifGroup *g = g_hash_table_lookup(nc->groups, key);
    if (g) {
        gtk_box_reorder_child(GTK_BOX(nc->groups_box), g->frame, 0);   /* most recent app on top */
        return g;
    }
    g = g_new0(NotifGroup, 1);
    g->app_name = g_strdup(key);
    g->frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *hdr = gtk_label_new(NULL);
    gchar *hm = g_markup_printf_escaped("<span size='small' weight='bold' alpha='70%%'>%s</span>", key);
    gtk_label_set_markup(GTK_LABEL(hdr), hm);
    g_free(hm);
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
    gtk_widget_set_margin_top(hdr, 6);
    gtk_box_pack_start(GTK_BOX(g->frame), hdr, FALSE, FALSE, 0);
    g->rows = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(g->frame), g->rows, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(nc->groups_box), g->frame, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(nc->groups_box), g->frame, 0);
    gtk_widget_show_all(g->frame);
    g_hash_table_insert(nc->groups, g->app_name, g);
    return g;
}

static void group_row_removed(NotificationCenter *nc, const gchar *app) {
    const gchar *key = (app && app[0]) ? app : "System";
    NotifGroup *g = g_hash_table_lookup(nc->groups, key);
    if (!g) return;
    if (--g->count <= 0) {
        gtk_widget_destroy(g->frame);
        g_hash_table_remove(nc->groups, key);   /* frees g via value destroy */
    }
}

static void free_group(gpointer p) {
    NotifGroup *g = (NotifGroup *)p;
    g_free(g->app_name);
    g_free(g);
}

static GtkWidget *build_history_row(Notif *n) {
    GtkWidget *eb = gtk_event_box_new();
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_row_clicked), n);

    GtkWidget *hb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(hb, 4);
    gtk_widget_set_margin_end(hb, 2);
    gtk_widget_set_margin_top(hb, 4);
    gtk_widget_set_margin_bottom(hb, 4);
    gtk_container_add(GTK_CONTAINER(eb), hb);

    GtkWidget *icon = make_notif_icon(n, 28);
    gtk_widget_set_valign(icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(hb), icon, FALSE, FALSE, 0);

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gchar *ts = g_date_time_format(n->time, "%I:%M %p");
    GtkWidget *hdr = gtk_label_new(NULL);
    gchar *hm = g_markup_printf_escaped("<b>%s</b>  <span size='small' alpha='60%%'>%s</span>",
        n->summary && n->summary[0] ? n->summary : "Notification", ts ? ts : "");
    gtk_label_set_markup(GTK_LABEL(hdr), hm);
    g_free(hm); g_free(ts);
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(hdr), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(hdr), 30);
    gtk_box_pack_start(GTK_BOX(vb), hdr, FALSE, FALSE, 0);

    if (n->body && n->body[0]) {
        GtkWidget *body = gtk_label_new(NULL);
        gchar *bm = safe_body_markup(n->body);
        gtk_label_set_markup(GTK_LABEL(body), bm);
        g_free(bm);
        gtk_label_set_xalign(GTK_LABEL(body), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(body), 32);
        gtk_label_set_lines(GTK_LABEL(body), 4);
        gtk_label_set_ellipsize(GTK_LABEL(body), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(vb), body, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(hb), vb, TRUE, TRUE, 0);

    GtkWidget *x_btn = gtk_button_new_from_icon_name("window-close-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(x_btn), GTK_RELIEF_NONE);
    gtk_widget_set_valign(x_btn, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(x_btn, "Remove");
    g_signal_connect(x_btn, "clicked", G_CALLBACK(on_row_dismiss_clicked), n);
    gtk_box_pack_end(GTK_BOX(hb), x_btn, FALSE, FALSE, 0);
    return eb;
}

static void history_remove(Notif *n) {
    if (!n->in_history) return;
    NotificationCenter *nc = n->nc;
    if (n->row) { gtk_widget_destroy(n->row); n->row = NULL; }
    nc->history = g_list_remove(nc->history, n);
    n->in_history = FALSE;
    group_row_removed(nc, n->app_name);
    update_empty_state(nc);
}

static void history_add_or_update(Notif *n, gboolean is_update) {
    NotificationCenter *nc = n->nc;
    if (n->transient) {                       /* temporary (e.g. volume OSD): never stored */
        if (n->in_history) history_remove(n);
        return;
    }
    if (n->in_history) {
        /* replace the row in place */
        NotifGroup *g = get_group(nc, n->app_name);
        GtkWidget *row = build_history_row(n);
        gtk_box_pack_start(GTK_BOX(g->rows), row, FALSE, FALSE, 0);
        gtk_box_reorder_child(GTK_BOX(g->rows), row, 0);
        if (n->row) gtk_widget_destroy(n->row);
        n->row = row;
        gtk_widget_show_all(row);
        nc->history = g_list_remove(nc->history, n);
        nc->history = g_list_prepend(nc->history, n);
        return;
    }

    NotifGroup *g = get_group(nc, n->app_name);
    n->row = build_history_row(n);
    gtk_box_pack_start(GTK_BOX(g->rows), n->row, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(g->rows), n->row, 0);
    gtk_widget_show_all(n->row);
    g->count++;
    n->in_history = TRUE;
    nc->history = g_list_prepend(nc->history, n);

    /* cap the history */
    while (g_list_length(nc->history) > NOTIF_HISTORY_MAX) {
        Notif *old = (Notif *)g_list_last(nc->history)->data;
        history_remove(old);
        notif_release_if_unused(old);
    }

    if (!is_update && !(nc->popup_win && gtk_widget_get_visible(nc->popup_win))) {
        nc->unread_count++;
        update_bell_badge(nc);
    }
    update_empty_state(nc);
}

static void on_clear_history_clicked(GtkButton *btn, NotificationCenter *nc) {
    GList *copy = g_list_copy(nc->history);
    for (GList *l = copy; l; l = l->next) {
        Notif *n = (Notif *)l->data;
        history_remove(n);
        notif_release_if_unused(n);
    }
    g_list_free(copy);
    nc->unread_count = 0;
    update_bell_badge(nc);
    update_empty_state(nc);
}

static void on_dnd_toggled(GObject *sw, GParamSpec *ps, NotificationCenter *nc) {
    nc->dnd = gtk_switch_get_active(GTK_SWITCH(sw));
    XfconfChannel *ch = xfconf_channel_get("xfce4-panel");
    if (ch && xfconf_channel_get_bool(ch, "/plugins/ultimateshell/notif_dnd", FALSE) != nc->dnd)
        xfconf_channel_set_bool(ch, "/plugins/ultimateshell/notif_dnd", nc->dnd);   /* remembered */
    gtk_widget_set_tooltip_text(nc->bell_btn, nc->dnd ? "Notifications (Do Not Disturb)" : "Notifications");
}

static void on_dnd_setting_changed(XfconfChannel *ch, const gchar *prop, const GValue *val, NotificationCenter *nc) {
    gboolean on = (val && G_VALUE_HOLDS_BOOLEAN(val)) ? g_value_get_boolean(val) : FALSE;
    if (gtk_switch_get_active(GTK_SWITCH(nc->dnd_switch)) != on)
        gtk_switch_set_active(GTK_SWITCH(nc->dnd_switch), on);
}

static void on_bell_toggled(GtkToggleButton *btn, NotificationCenter *nc) {
    if (gtk_toggle_button_get_active(btn)) {
        nc->unread_count = 0;
        update_bell_badge(nc);
    }
    shell_toggle_popup_window(btn, nc->popup_win);
}

/* ---------------------------------------------------------------- D-Bus */

static void handle_notify(NotificationCenter *nc, GVariant *params, GDBusMethodInvocation *inv) {
    const gchar *app_name = NULL, *app_icon = NULL, *summary = NULL, *body = NULL;
    guint32 replaces_id = 0;
    gchar **actions = NULL;
    GVariant *hints = NULL;
    gint32 expire = -1;
    g_variant_get(params, "(&su&s&s&s^as@a{sv}i)",
                  &app_name, &replaces_id, &app_icon, &summary, &body, &actions, &hints, &expire);

    Notif *n = replaces_id ? g_hash_table_lookup(nc->by_id, GUINT_TO_POINTER(replaces_id)) : NULL;
    gboolean is_update = (n != NULL);
    if (!n) {
        n = g_new0(Notif, 1);
        n->nc = nc;
        n->id = ++nc->next_notif_id;
        if (n->id == 0) n->id = ++nc->next_notif_id;
        g_hash_table_insert(nc->by_id, GUINT_TO_POINTER(n->id), n);
    } else {
        g_free(n->app_name); g_free(n->app_icon); g_free(n->summary); g_free(n->body);
        g_strfreev(n->actions);
        if (n->image) { g_object_unref(n->image); n->image = NULL; }
        if (n->time) g_date_time_unref(n->time);
    }

    n->app_name  = g_strdup(app_name ? app_name : "");
    n->app_icon  = g_strdup(app_icon ? app_icon : "");
    n->summary   = g_strdup(summary ? summary : "");
    n->body      = g_strdup(body ? body : "");
    n->actions   = actions;                 /* takes ownership */
    n->expire_ms = expire;
    n->time      = g_date_time_new_now_local();
    n->urgency   = 1;
    n->transient = FALSE;

    guchar urg = 1;
    if (g_variant_lookup(hints, "urgency", "y", &urg)) n->urgency = urg;
    gboolean tr = FALSE;
    if (g_variant_lookup(hints, "transient", "b", &tr)) n->transient = tr;

    GVariant *img = g_variant_lookup_value(hints, "image-data", NULL);
    if (!img) img = g_variant_lookup_value(hints, "image_data", NULL);
    if (!img) img = g_variant_lookup_value(hints, "icon_data", NULL);
    if (img) { n->image = pixbuf_from_image_data(img); g_variant_unref(img); }
    if (!n->image) {
        const gchar *ip = NULL;
        if (g_variant_lookup(hints, "image-path", "&s", &ip) || g_variant_lookup(hints, "image_path", "&s", &ip)) {
            if (ip && ip[0]) {
                gchar *path = g_str_has_prefix(ip, "file://") ? g_filename_from_uri(ip, NULL, NULL)
                            : (ip[0] == '/' ? g_strdup(ip) : NULL);
                if (path) {
                    n->image = gdk_pixbuf_new_from_file_at_scale(path, 64, 64, TRUE, NULL);
                    g_free(path);
                } else {
                    g_free(n->app_icon);
                    n->app_icon = g_strdup(ip);  /* an icon name */
                }
            }
        }
    }
    g_variant_unref(hints);

    history_add_or_update(n, is_update);

    /* Do Not Disturb: keep it in history, only critical ones pop up */
    if (!nc->dnd || n->urgency == 2) toast_show(n);
    else if (n->toast) toast_close(n);

    guint32 id = n->id;
    if (!n->toast && !n->in_history) {
        /* Not shown and not stored (transient under DND): it is closed right away */
        notif_release_if_unused(n);
        emit_closed(nc, id, CLOSE_EXPIRED);
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
}

static void handle_notif_method_call(
    GDBusConnection *conn, const gchar *sender, const gchar *obj_path,
    const gchar *iface_name, const gchar *method_name, GVariant *params,
    GDBusMethodInvocation *invocation, gpointer user_data)
{
    NotificationCenter *nc = (NotificationCenter *)user_data;
    nc->conn = conn;

    if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        const gchar *caps[] = { "body", "body-markup", "actions", "icon-static", "persistence", NULL };
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(^as)", caps));
    } else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        g_dbus_method_invocation_return_value(
            invocation, g_variant_new("(ssss)", "UltimateShellNotify", "Ultimate Shell", "0.6.0", "1.2"));
    } else if (g_strcmp0(method_name, "CloseNotification") == 0) {
        guint32 id = 0;
        g_variant_get(params, "(u)", &id);
        Notif *n = g_hash_table_lookup(nc->by_id, GUINT_TO_POINTER(id));
        if (n) {
            /* Close only this notification's pop-up; it stays in the history */
            toast_close(n);
            emit_closed(nc, id, CLOSE_BY_CALL);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);
    } else if (g_strcmp0(method_name, "Notify") == 0) {
        handle_notify(nc, params, invocation);
    } else {
        g_dbus_method_invocation_return_dbus_error(invocation,
            "org.freedesktop.DBus.Error.UnknownMethod", "Unknown method");
    }
}

static const GDBusInterfaceVTable notif_vtable = {
    handle_notif_method_call, NULL, NULL, { 0 }
};

static void on_notif_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    NotificationCenter *nc = (NotificationCenter *)user_data;
    nc->conn = conn;
    g_dbus_connection_register_object(conn, "/org/freedesktop/Notifications",
                                      nc->notif_node->interfaces[0], &notif_vtable, nc, NULL, NULL);
}

/* ---------------------------------------------------------------- widget */

/* ---------------------------------------------------------------- public API (calendar flyout) */
static NotificationCenter *g_nc = NULL;

GList *notifications_get_history(void) {
    GList *out = NULL;
    if (!g_nc) return NULL;
    for (GList *l = g_nc->history; l; l = l->next) {
        Notif *n = l->data;
        ShellNotifEntry *e = g_new0(ShellNotifEntry, 1);
        e->app_name = g_strdup(n->app_name ? n->app_name : "");
        e->app_icon = g_strdup(n->app_icon ? n->app_icon : "");
        e->summary  = g_strdup(n->summary ? n->summary : "");
        e->body     = g_strdup(n->body ? n->body : "");
        e->time     = n->time ? g_date_time_ref(n->time) : NULL;
        out = g_list_append(out, e);
    }
    return out;
}

void shell_notif_entry_free(gpointer p) {
    ShellNotifEntry *e = p;
    if (!e) return;
    g_free(e->app_name); g_free(e->app_icon); g_free(e->summary); g_free(e->body);
    if (e->time) g_date_time_unref(e->time);
    g_free(e);
}

void notifications_clear_all(void) {
    if (g_nc) on_clear_history_clicked(NULL, g_nc);
}

gboolean notifications_get_dnd(void) { return g_nc ? g_nc->dnd : FALSE; }

void notifications_set_dnd(gboolean on) {
    if (g_nc && g_nc->dnd_switch) gtk_switch_set_active(GTK_SWITCH(g_nc->dnd_switch), on);
}

GtkWidget *notifications_widget_new(void) {
    NotificationCenter *nc = g_new0(NotificationCenter, 1);
    g_nc = nc;
    nc->notif_node = g_dbus_node_info_new_for_xml(fdo_notifications_xml, NULL);
    nc->by_id  = g_hash_table_new(g_direct_hash, g_direct_equal);
    nc->groups = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_group);

    nc->bell_btn = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(nc->bell_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(nc->bell_btn), "ultimate-module-btn");
    gtk_widget_set_tooltip_text(nc->bell_btn, "Notifications");

    GtkWidget *icon = gtk_image_new_from_icon_name("preferences-system-notifications-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 18);
    GtkWidget *overlay = shell_create_badged_icon(icon, &nc->badge_label);
    gtk_container_add(GTK_CONTAINER(nc->bell_btn), overlay);

    nc->popup_win = shell_create_popup_window(nc->bell_btn, 360, 440, FALSE);

    GtkWidget *pop_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(pop_vbox), 12);

    GtkWidget *top_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>Notifications</b>");
    gtk_box_pack_start(GTK_BOX(top_bar), hdr, FALSE, FALSE, 0);

    GtkWidget *clear_btn = gtk_button_new_with_label("Clear All");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_history_clicked), nc);
    gtk_box_pack_end(GTK_BOX(top_bar), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pop_vbox), top_bar, FALSE, FALSE, 0);

    GtkWidget *dnd_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *dnd_lbl = gtk_label_new("Do Not Disturb");
    gtk_widget_set_tooltip_text(dnd_lbl, "Hide pop-ups (urgent ones still show). Everything is kept in this list.");
    gtk_box_pack_start(GTK_BOX(dnd_row), dnd_lbl, FALSE, FALSE, 0);
    nc->dnd_switch = gtk_switch_new();
    gtk_widget_set_valign(nc->dnd_switch, GTK_ALIGN_CENTER);
    g_signal_connect(nc->dnd_switch, "notify::active", G_CALLBACK(on_dnd_toggled), nc);
    gtk_box_pack_end(GTK_BOX(dnd_row), nc->dnd_switch, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pop_vbox), dnd_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pop_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *list_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    nc->empty_lbl = gtk_label_new("No notifications");
    gtk_widget_set_opacity(nc->empty_lbl, 0.6);
    gtk_widget_set_margin_top(nc->empty_lbl, 24);
    gtk_widget_set_no_show_all(nc->empty_lbl, TRUE);
    gtk_widget_show(nc->empty_lbl);
    gtk_box_pack_start(GTK_BOX(list_vbox), nc->empty_lbl, FALSE, FALSE, 0);
    nc->groups_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(list_vbox), nc->groups_box, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(scroll), list_vbox);
    gtk_box_pack_start(GTK_BOX(pop_vbox), scroll, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(nc->popup_win), pop_vbox);
    g_signal_connect(nc->bell_btn, "toggled", G_CALLBACK(on_bell_toggled), nc);

    /* Do Not Disturb is remembered, and follows Settings ▸ Notifications */
    {
        XfconfChannel *ch = xfconf_channel_get("xfce4-panel");
        if (ch) {
            gtk_switch_set_active(GTK_SWITCH(nc->dnd_switch),
                xfconf_channel_get_bool(ch, "/plugins/ultimateshell/notif_dnd", FALSE));
            g_signal_connect(ch, "property-changed::/plugins/ultimateshell/notif_dnd",
                             G_CALLBACK(on_dnd_setting_changed), nc);
        }
    }

    nc->bus_owner_id = g_bus_own_name(
        G_BUS_TYPE_SESSION, "org.freedesktop.Notifications",
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_notif_bus_acquired, NULL, NULL, nc, NULL
    );

    return nc->bell_btn;
}
