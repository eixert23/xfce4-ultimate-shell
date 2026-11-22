#include <unistd.h>
#include <sys/stat.h>
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "systray.h"
#include "notifications.h"
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/extensions/XTest.h>
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <libdbusmenu-gtk/menu.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _SystrayWidget SystrayWidget;

typedef struct {
    SystrayWidget *tray;
    gchar         *id;
    gchar         *display_name;
    gchar         *key;
    gchar         *bus_name;
    gchar         *object_path;
    gchar         *menu_path;
    gboolean       is_builtin;
    gboolean       is_hidden;
    GtkWidget     *button;
    GtkWidget     *image;
    GtkWidget     *badge_lbl;
    GtkWidget     *dbus_menu;
    GDBusProxy    *item_proxy;
    guint          watch_id;
    gint           pid;               /* owning process (SNI items), 0 if unknown */
    gboolean       is_network_applet; /* nm-applet & co. — hidden while the built-in Network item is on */
    gboolean       is_xembed;         /* legacy XEmbed icon (IBus etc.) living in a GtkSocket */
    gboolean       xembed_dragged;    /* the press became a drag: don't forward the release */
    GtkWidget     *ov_wrap, *ov_socket; /* its slot in the ▲ popup (XEmbed, while hidden) */
    GtkWidget     *plug_in;           /* the socket that holds the app's icon window now */
    gboolean       migrating;         /* moving between panel and popup: "removed" is not "quit" */
    GtkWidget     *socket;            /* the GtkSocket inside item->button (XEmbed only) */
    gulong         plug_xid;          /* embedded X window (XEmbed only) */
    GCancellable  *cancel;            /* cancels in-flight async D-Bus work when the item goes away */
} TrayItem;

typedef struct {
    gchar    *sink_or_source_name;
    gchar    *port_name;
    gchar    *label;
    gboolean  active;
} AudioPortChoice;

struct _SystrayWidget {
    UltimateShell   *shell;
    GtkWidget       *root_box;
    GtkWidget       *visible_box;
    GtkWidget       *xembed_box;
    GtkWidget       *xembed_Owner_win;
    GtkWidget       *qs_btn, *qs_icons;   /* Quick Settings group button (one button, several icons) */

    /* Overflow */
    GtkWidget       *chevron_btn;
    GtkWidget       *chevron_img;
    GtkWidget       *overflow_popup;
    GtkWidget       *overflow_grid;

    /* Network */
    TrayItem        *net_item;
    GtkWidget       *net_popup;
    GtkWidget       *net_status_lbl;
    GtkWidget       *wifi_list_box;

    /* Battery & Clipboard (Clipman) */
    TrayItem        *bat_item;
    GtkWidget       *bat_popup;
    GtkWidget       *bat_status_lbl;
    GtkWidget       *bat_detail_lbl;
    gboolean         bat_present;        /* battery found (or user forces the icon) */
    guint            pwr_sysfs_timer;
    gboolean         pwr_updating;
    GtkWidget       *pwr_profile_box;
    GtkWidget       *pwr_profile_btns[3];
    gchar           *pwr_ppd_service;
    gchar           *pwr_ppd_path;
    gchar           *pwr_ppd_iface;
    gboolean         pwr_ppd_tried_new;
    gchar           *pwr_backlight;
    GtkWidget       *pwr_bright_scale;
    TrayItem        *clip_item;
    TrayItem        *mic_item;        /* "microphone in use" indicator, its own tray icon */
    gchar           *pa_rec_apps;     /* names of apps using the mic */
    gboolean         mic_shown;
    GtkWidget       *clip_popup;
    GtkWidget       *clip_list_box;
    GList           *clip_history;
    /* Removable Drives (GIO GVolumeMonitor) */
    TrayItem        *rem_item;
    GtkWidget       *rem_popup;
    GtkWidget       *rem_status_lbl;
    gchar           *rem_msg;            /* last result / error shown under the header */
    gboolean         rem_msg_is_error;
    guint            rem_msg_timer;
    GtkWidget       *rem_list_box;
    GVolumeMonitor  *vol_mon;

    /* Bluetooth (BlueZ) */
    TrayItem        *bt_item;
    GtkWidget       *bt_popup;
    GtkWidget       *bt_status_lbl;
    GtkWidget       *bt_power_sw;
    GtkWidget       *bt_scan_btn;
    GtkWidget       *bt_list_box;
    gboolean         bt_scanning;
    gboolean         updating_bt_ui;
    gchar           *bt_adapter_path;
    GDBusConnection *sys_bus;
    GtkWidget       *bt_adapter_combo;
    gchar           *bt_selected_adapter;
    gboolean         bt_live_ready;
    guint            bt_agent_obj_id;
    gboolean         bt_agent_registered;
    guint            bt_bluez_watch_id;
    guint            bt_sig_props_id;
    guint            bt_sig_added_id;
    guint            bt_sig_removed_id;
    guint            bt_refresh_id;
    GHashTable      *bt_busy;            /* device path -> "Pairing…" etc. */
    gchar           *bt_error_msg;
    guint            bt_error_timer_id;
    GDBusMethodInvocation *bt_agent_inv; /* pending agent request awaiting the user */
    gchar           *bt_agent_method;
    GtkWidget       *bt_agent_dialog;
    GtkWidget       *bt_agent_entry;
    GtkWidget       *bt_agent_display;

    /* Upstream XFCE PulseAudio Plugin Menu */
    TrayItem        *vol_item;
    GtkWidget       *vol_menu;
    GtkWidget       *vol_scale;
    GtkWidget       *vol_switch;
    GtkWidget       *vol_row_img;
    GtkWidget       *mic_scale;
    GtkWidget       *mic_switch;
    GtkWidget       *mic_row_img;
    GtkWidget       *output_submenu_item;
    GtkWidget       *input_submenu_item;
    GtkWidget       *mpris_separator;
    GList           *mpris_items;
    gboolean         updating_vol_ui;
    gboolean         scale_grabbed;
    GtkWidget       *grabbed_scale;
    /* audio core (cached, event-driven) */
    gint             pa_vol, pa_mic;
    gboolean         pa_muted, pa_mic_muted, pa_have_state, pa_recording;
    gchar           *pa_txt_sinks, *pa_txt_sources, *pa_txt_inputs, *pa_def_sink, *pa_def_source;
    GSubprocess     *pa_sub;
    GDataInputStream *pa_sub_in;
    gboolean         pa_sub_alive;
    guint            pa_sub_retry_id;
    guint            pa_refresh_id;
    gboolean         pa_refreshing, pa_refresh_again;
    gint             pa_pending_vol, pa_sent_vol, pa_pending_mic, pa_sent_mic;
    guint            pa_vol_throttle_id, pa_mic_throttle_id;
    gint64           pa_ui_change_until;
    gdouble          pa_scroll_accum;
    guint32          pa_notif_id;
    GtkWidget       *pa_rec_img;
    GList           *pa_app_items;
    gboolean         pa_keys_grabbed, pa_key_filter_added;
    guint            pa_poll_tick;

    GList           *all_items;
    GHashTable      *sni_by_key;
    GtkWidget       *settings_list_box;

    guint            hw_timer_id;
    GDBusNodeInfo   *watcher_node;
    GDBusConnection *session_bus;
    guint            kde_owner_id;
    guint            fdo_owner_id;
};

static SystrayWidget *g_systray = NULL;

static const GtkTargetEntry tray_dnd_targets[] = {
    { "ULTIMATE_SYSTRAY_ITEM", GTK_TARGET_SAME_APP, 0 }
};

static const gchar sni_watcher_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierWatcher'>"
    "    <method name='RegisterStatusNotifierItem'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <method name='RegisterStatusNotifierHost'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "    <property name='ProtocolVersion' type='i' access='read'/>"
    "    <signal name='StatusNotifierItemRegistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierItemUnregistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierHostRegistered'/>"
    "  </interface>"
    "  <interface name='org.freedesktop.StatusNotifierWatcher'>"
    "    <method name='RegisterStatusNotifierItem'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <method name='RegisterStatusNotifierHost'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <property name='RegisteredStatusNotifierItems' type='as' access='read'/>"
    "    <property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "    <property name='ProtocolVersion' type='i' access='read'/>"
    "    <signal name='StatusNotifierItemRegistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierItemUnregistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierHostRegistered'/>"
    "  </interface>"
    "</node>";

static void rebuild_settings_list_ui(SystrayWidget *tray);
static gint compare_tray_items_by_saved_order(gconstpointer a, gconstpointer b);
static void refresh_tray_item_properties(TrayItem *item);
static void xembed_refresh_visibility(SystrayWidget *tray);
static void xembed_place(TrayItem *item, gboolean in_popup, gint col, gint row, gint icon_px);
static void qs_on_tray_click(SystrayWidget *t, GtkToggleButton *btn);
static gboolean qs_enabled(SystrayWidget *t);
static gboolean qs_is_member(SystrayWidget *t, TrayItem *it);
static void qs_group_sync(SystrayWidget *t);
static GdkWindow *xembed_shield_of(GtkWidget *wrap);
static gboolean xembed_restore_shield(gpointer data);
static gboolean on_xembed_plug_removed(GtkSocket *socket, gpointer user_data);
static gchar *pa_cached_output(const gchar *cmd);
static void pa_rebuild_app_items(SystrayWidget *tray);
static void on_sni_getall_done(GObject *src, GAsyncResult *res, gpointer user_data);

static void on_systray_root_destroy(GtkWidget *w, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    if (tray && tray->hw_timer_id) { g_source_remove(tray->hw_timer_id); tray->hw_timer_id = 0; }
}

/* -------------------------------------------------------------------------
 * Duplicate-icon helpers
 * Some apps (nm-applet, blueman, ...) show up twice: once through the
 * XEmbed tray we now own for IBus, and once through StatusNotifierItem.
 * nm-applet also duplicates our own built-in Network item.
 * ------------------------------------------------------------------------- */
static gboolean is_network_applet_name(const gchar *name) {
    if (!name || !*name) return FALSE;
    static const gchar *const known[] = {
        "nm-applet", "nm-tray", "network-manager-applet", "connman-gtk", "cmst", NULL
    };
    for (gint i = 0; known[i]; i++) {
        if (g_ascii_strcasecmp(name, known[i]) == 0) return TRUE;
    }
    return FALSE;
}

static gchar *get_comm_for_pid(gint pid) {
    if (pid <= 0) return NULL;
    gchar *path = g_strdup_printf("/proc/%d/comm", pid);
    gchar *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL) && contents) g_strstrip(contents);
    g_free(path);
    return contents;
}

static gint get_pid_for_bus_name(GDBusConnection *bus, const gchar *name) {
    if (!bus || !name) return 0;
    GVariant *res = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetConnectionUnixProcessID", g_variant_new("(s)", name),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    if (!res) return 0;
    guint32 pid = 0;
    g_variant_get(res, "(u)", &pid);
    g_variant_unref(res);
    return (gint)pid;
}

/* Resolve a well-known name to its unique ":1.x" owner so the same item
 * registered once by path and once by service name gets a single key. */
static gchar *get_unique_bus_name(GDBusConnection *bus, const gchar *name) {
    if (!bus || !name) return NULL;
    if (name[0] == ':') return g_strdup(name);
    GVariant *res = g_dbus_connection_call_sync(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", name),
        G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    if (!res) return NULL;
    gchar *owner = NULL;
    g_variant_get(res, "(s)", &owner);
    g_variant_unref(res);
    return owner;
}

static gboolean network_applet_suppressed(SystrayWidget *tray) {
    return tray && tray->shell && tray->shell->tray_enable_network;
}

static gboolean sni_item_has_pid(SystrayWidget *tray, gint pid) {
    if (!tray || pid <= 0) return FALSE;
    for (GList *l = tray->all_items; l; l = l->next) {
        TrayItem *it = (TrayItem *)l->data;
        if (it && !it->is_builtin && !it->is_xembed && it->pid == pid) return TRUE;
    }
    return FALSE;
}
static void refresh_audio_devices_and_mpris(SystrayWidget *tray);
static void update_network_and_audio(SystrayWidget *tray);

static void free_port_choice(gpointer data) {
    AudioPortChoice *c = (AudioPortChoice *)data;
    if (!c) return;
    g_free(c->sink_or_source_name);
    g_free(c->port_name);
    g_free(c->label);
    g_free(c);
}

/* =========================================================================
 * SNI IconPixmap (a(iiay)) ARGB32 -> GdkPixbuf Decoder
 * ========================================================================= */
static void free_pixel_data(guchar *pixels, gpointer data)
{
    (void)data;
    g_free(pixels);
}

static GdkPixbuf *extract_pixbuf_from_sni_variant(GVariant *v_pix, gint target_px) {
    if (!v_pix || !g_variant_is_of_type(v_pix, G_VARIANT_TYPE("a(iiay)"))) return NULL;

    GVariantIter iter;
    g_variant_iter_init(&iter, v_pix);

    gint best_w = 0, best_h = 0;
    GVariant *best_bytes = NULL;
    gint width = 0, height = 0;
    GVariant *bytes_var = NULL;

    while (g_variant_iter_next(&iter, "(ii@ay)", &width, &height, &bytes_var)) {
        if (width > 0 && height > 0 && bytes_var) {
            gsize n_bytes = g_variant_get_size(bytes_var);
            if (n_bytes >= (gsize)(width * height * 4)) {
                if (!best_bytes ||
                    (best_w < target_px && width > best_w) ||
                    (width >= target_px && best_w >= target_px && width < best_w)) {
                    if (best_bytes) g_variant_unref(best_bytes);
                    best_w = width;
                    best_h = height;
                    best_bytes = g_variant_ref(bytes_var);
                }
            }
        }
        if (bytes_var) g_variant_unref(bytes_var);
    }

    if (!best_bytes || best_w <= 0 || best_h <= 0) {
        if (best_bytes) g_variant_unref(best_bytes);
        return NULL;
    }

    gsize n_len = 0;
    const guchar *src = (const guchar *)g_variant_get_fixed_array(best_bytes, &n_len, sizeof(guchar));
    if (!src || n_len < (gsize)(best_w * best_h * 4)) {
        g_variant_unref(best_bytes);
        return NULL;
    }

    guchar *rgba = g_malloc(best_w * best_h * 4);
    for (gint i = 0; i < best_w * best_h; i++) {
        /* SNI specification stores pixels in network byte order: A, R, G, B */
        guchar a = src[i * 4 + 0];
        guchar r = src[i * 4 + 1];
        guchar g = src[i * 4 + 2];
        guchar b = src[i * 4 + 3];
        rgba[i * 4 + 0] = r;
        rgba[i * 4 + 1] = g;
        rgba[i * 4 + 2] = b;
        rgba[i * 4 + 3] = a;
    }
    g_variant_unref(best_bytes);

    GdkPixbuf *raw_pb = gdk_pixbuf_new_from_data(
        rgba, GDK_COLORSPACE_RGB, TRUE, 8, best_w, best_h, best_w * 4,
        free_pixel_data, NULL);

    if (!raw_pb) return NULL;

    if (best_w != target_px || best_h != target_px) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(raw_pb, target_px, target_px, GDK_INTERP_BILINEAR);
        g_object_unref(raw_pb);
        return scaled;
    }
    return raw_pb;
}

/* =========================================================================
 * Upstream ScaleMenuItem Event Routing + Magnetic Mark Snapping
 * ========================================================================= */
static gboolean is_point_in_widget(GtkWidget *ref_widget, gint rx, gint ry, GtkWidget *target) {
    if (!target || !gtk_widget_get_visible(target)) return FALSE;
    gint tx = 0, ty = 0;
    if (!gtk_widget_translate_coordinates(ref_widget, target, rx, ry, &tx, &ty)) return FALSE;
    GtkAllocation alloc;
    gtk_widget_get_allocation(target, &alloc);
    return (tx >= 0 && tx < alloc.width && ty >= 0 && ty < alloc.height);
}

static void update_scale_from_coord(GtkWidget *item, gdouble ex, gdouble ey, GtkWidget *scale) {
    gint sx = 0, sy = 0;
    gtk_widget_translate_coordinates(item, scale, (gint)ex, (gint)ey, &sx, &sy);
    GtkAllocation alloc;
    gtk_widget_get_allocation(scale, &alloc);
    if (alloc.width <= 16) return;

    GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(scale));
    gdouble lower = gtk_adjustment_get_lower(adj);
    gdouble upper = gtk_adjustment_get_upper(adj);
    gdouble frac = CLAMP((sx - 8.0) / (gdouble)(alloc.width - 16.0), 0.0, 1.0);
    gdouble raw_val = lower + frac * (upper - lower);

    gboolean is_mic = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(scale), "is-mic-scale"));
    const gdouble snap_threshold = 4.5;

    if (fabs(raw_val - 100.0) <= snap_threshold) {
        raw_val = 100.0;
    } else if (is_mic && fabs(raw_val - 42.0) <= snap_threshold) {
        raw_val = 42.0;
    } else {
        raw_val = floor(raw_val + 0.5);
    }

    gtk_range_set_value(GTK_RANGE(scale), CLAMP(raw_val, lower, upper));
}

static gboolean on_scale_menuitem_button_press(GtkWidget *item, GdkEventButton *event, SystrayWidget *tray) {
    GtkWidget *scale = g_object_get_data(G_OBJECT(item), "scale-widget");
    GtkWidget *sw    = g_object_get_data(G_OBJECT(item), "switch-widget");

    if (sw && is_point_in_widget(item, (gint)event->x, (gint)event->y, sw)) {
        if (event->button == 1) {
            gboolean cur = gtk_switch_get_active(GTK_SWITCH(sw));
            gtk_switch_set_active(GTK_SWITCH(sw), !cur);
        }
        return TRUE;
    }

    if (scale && gtk_widget_get_sensitive(scale) && is_point_in_widget(item, (gint)event->x, (gint)event->y, scale)) {
        tray->scale_grabbed = TRUE;
        tray->grabbed_scale = scale;
        update_scale_from_coord(item, event->x, event->y, scale);
        return TRUE;
    }
    return TRUE;
}

static gboolean on_scale_menuitem_button_release(GtkWidget *item, GdkEventButton *event, SystrayWidget *tray) {
    if (tray->scale_grabbed) {
        tray->scale_grabbed = FALSE;
        tray->grabbed_scale = NULL;
    }
    return TRUE;
}

static gboolean on_scale_menuitem_motion(GtkWidget *item, GdkEventMotion *event, SystrayWidget *tray) {
    GtkWidget *scale = g_object_get_data(G_OBJECT(item), "scale-widget");
    if (tray->scale_grabbed && tray->grabbed_scale == scale && (event->state & GDK_BUTTON1_MASK)) {
        update_scale_from_coord(item, event->x, event->y, scale);
        return TRUE;
    }
    return TRUE;
}

static gboolean on_scale_menuitem_scroll(GtkWidget *item, GdkEventScroll *event, SystrayWidget *tray) {
    GtkWidget *scale = g_object_get_data(G_OBJECT(item), "scale-widget");
    if (!scale || !gtk_widget_get_sensitive(scale)) return FALSE;
    gdouble step = (tray->shell && tray->shell->audio_vol_step > 0) ? tray->shell->audio_vol_step : 5.0;
    gdouble max_v = (tray->shell && tray->shell->audio_max_vol > 0) ? tray->shell->audio_max_vol : 150.0;
    gdouble val = gtk_range_get_value(GTK_RANGE(scale));
    if (event->direction == GDK_SCROLL_UP) {
        gtk_range_set_value(GTK_RANGE(scale), CLAMP(val + step, 0.0, max_v));
        return TRUE;
    } else if (event->direction == GDK_SCROLL_DOWN) {
        gtk_range_set_value(GTK_RANGE(scale), CLAMP(val - step, 0.0, max_v));
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *create_upstream_scale_menu_item(SystrayWidget *tray, const gchar *icon_name,
                                                  gboolean is_mic,
                                                  GtkWidget **out_scale, GtkWidget **out_switch,
                                                  GtkWidget **out_img)
{
    GtkWidget *item = gtk_image_menu_item_new();
    GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_MENU);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), img);
    gtk_image_menu_item_set_always_show_image(GTK_IMAGE_MENU_ITEM(item), TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 150.0, 1.0);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_widget_set_size_request(scale, 320, -1);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_widget_set_valign(scale, GTK_ALIGN_CENTER);

    g_object_set_data(G_OBJECT(scale), "is-mic-scale", GINT_TO_POINTER(is_mic ? 1 : 0));

    if (is_mic) {
        gtk_scale_add_mark(GTK_SCALE(scale), 42.0, GTK_POS_BOTTOM, NULL);
        gtk_scale_add_mark(GTK_SCALE(scale), 100.0, GTK_POS_BOTTOM, NULL);
    } else {
        gtk_scale_add_mark(GTK_SCALE(scale), 100.0, GTK_POS_BOTTOM, NULL);
    }

    GtkWidget *sw = gtk_switch_new();
    gtk_widget_set_valign(sw, GTK_ALIGN_CENTER);

    gtk_box_pack_start(GTK_BOX(hbox), scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), sw, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(item), hbox);

    g_object_set_data(G_OBJECT(item), "scale-widget", scale);
    g_object_set_data(G_OBJECT(item), "switch-widget", sw);

    gtk_widget_add_events(item, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    g_signal_connect(item, "button-press-event", G_CALLBACK(on_scale_menuitem_button_press), tray);
    g_signal_connect(item, "button-release-event", G_CALLBACK(on_scale_menuitem_button_release), tray);
    g_signal_connect(item, "motion-notify-event", G_CALLBACK(on_scale_menuitem_motion), tray);
    g_signal_connect(item, "scroll-event", G_CALLBACK(on_scale_menuitem_scroll), tray);

    if (out_scale)  *out_scale  = scale;
    if (out_switch) *out_switch = sw;
    if (out_img)    *out_img    = img;

    return item;
}

/* =========================================================================
 * Upstream MprisMenuItem + org.mpris.MediaPlayer2.Playlists Submenu
 * ========================================================================= */
static gboolean on_mpris_menuitem_button_press(GtkWidget *item, GdkEventButton *event, SystrayWidget *tray) {
    GtkWidget *btn_prev = g_object_get_data(G_OBJECT(item), "btn-prev");
    GtkWidget *btn_play = g_object_get_data(G_OBJECT(item), "btn-play");
    GtkWidget *btn_next = g_object_get_data(G_OBJECT(item), "btn-next");
    const gchar *bus_name   = g_object_get_data(G_OBJECT(item), "mpris-bus");
    const gchar *launch_cmd = g_object_get_data(G_OBJECT(item), "mpris-launch");
    gboolean is_running = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item), "mpris-running"));

    if (!is_running) {
        if (launch_cmd && launch_cmd[0] != '\0') {
            /* launch_cmd goes in as $1: word-split for arguments, but never shell-parsed */
            gchar *argv[] = { "sh", "-c", "gtk-launch \"$1\" 2>/dev/null || $1",
                              "sh", (gchar *)launch_cmd, NULL };
            g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        }
        gtk_menu_shell_deactivate(GTK_MENU_SHELL(tray->vol_menu));
        return TRUE;
    }
    if (!bus_name || !tray->session_bus) return TRUE;

    const gchar *method = NULL;
    if (btn_prev && is_point_in_widget(item, (gint)event->x, (gint)event->y, btn_prev)) {
        method = "Previous";
    } else if (btn_play && is_point_in_widget(item, (gint)event->x, (gint)event->y, btn_play)) {
        method = "PlayPause";
    } else if (btn_next && is_point_in_widget(item, (gint)event->x, (gint)event->y, btn_next)) {
        method = "Next";
    }

    if (method) {
        g_dbus_connection_call(tray->session_bus, bus_name, "/org/mpris/MediaPlayer2",
                               "org.mpris.MediaPlayer2.Player", method, NULL, NULL,
                               G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        return TRUE;
    }

    g_dbus_connection_call(tray->session_bus, bus_name, "/org/mpris/MediaPlayer2",
                           "org.mpris.MediaPlayer2", "Raise", NULL, NULL,
                           G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    gtk_menu_shell_deactivate(GTK_MENU_SHELL(tray->vol_menu));
    return TRUE;
}

static gboolean on_mpris_menuitem_button_release(GtkWidget *item, GdkEventButton *event, gpointer user_data) {
    return TRUE;
}

static void on_mpris_playlist_activated(GtkMenuItem *mi, SystrayWidget *tray) {
    const gchar *bus_name = g_object_get_data(G_OBJECT(mi), "mpris-bus");
    const gchar *pl_path  = g_object_get_data(G_OBJECT(mi), "playlist-path");
    if (!bus_name || !pl_path || !tray->session_bus || !g_variant_is_object_path(pl_path)) return;

    g_dbus_connection_call(
        tray->session_bus, bus_name, "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Playlists", "ActivatePlaylist",
        g_variant_new("(o)", pl_path), NULL,
        G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static GtkWidget *create_mpris_playlist_submenu_item(SystrayWidget *tray, const gchar *bus_name) {
    if (!tray->session_bus || !bus_name) return NULL;

    guint32 pl_count = 0;
    GVariant *cnt_res = g_dbus_connection_call_sync(
        tray->session_bus, bus_name, "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Playlists", "PlaylistCount"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 400, NULL, NULL);

    if (cnt_res) {
        GVariant *val = NULL;
        g_variant_get(cnt_res, "(v)", &val);
        if (val) {
            if (g_variant_is_of_type(val, G_VARIANT_TYPE_UINT32))
                pl_count = g_variant_get_uint32(val);
            g_variant_unref(val);
        }
        g_variant_unref(cnt_res);
    }

    if (pl_count == 0) return NULL;

    GVariant *pl_res = g_dbus_connection_call_sync(
        tray->session_bus, bus_name, "/org/mpris/MediaPlayer2",
        "org.mpris.MediaPlayer2.Playlists", "GetPlaylists",
        g_variant_new("(uusb)", 0, 100, "Alphabetical", FALSE),
        G_VARIANT_TYPE("(a(oss))"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);

    if (!pl_res) return NULL;

    GVariantIter *iter = NULL;
    g_variant_get(pl_res, "(a(oss))", &iter);
    if (!iter) {
        g_variant_unref(pl_res);
        return NULL;
    }

    GtkWidget *submenu = gtk_menu_new();
    gint added = 0;
    const gchar *obj_path = NULL;
    const gchar *pl_name  = NULL;
    const gchar *pl_icon  = NULL;

    while (g_variant_iter_next(iter, "(&o&s&s)", &obj_path, &pl_name, &pl_icon)) {
        if (obj_path && pl_name && pl_name[0] != '\0') {
            GtkWidget *pl_mi = gtk_menu_item_new_with_label(pl_name);
            g_object_set_data_full(G_OBJECT(pl_mi), "mpris-bus", g_strdup(bus_name), g_free);
            g_object_set_data_full(G_OBJECT(pl_mi), "playlist-path", g_strdup(obj_path), g_free);
            g_signal_connect(pl_mi, "activate", G_CALLBACK(on_mpris_playlist_activated), tray);
            gtk_menu_shell_append(GTK_MENU_SHELL(submenu), pl_mi);
            added++;
        }
    }

    g_variant_iter_free(iter);
    g_variant_unref(pl_res);

    if (added == 0) {
        gtk_widget_destroy(submenu);
        return NULL;
    }

    GtkWidget *choose_item = gtk_menu_item_new_with_label("Choose Playlist");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(choose_item), submenu);
    gtk_widget_show_all(choose_item);
    return choose_item;
}

/* "org.mpris.MediaPlayer2.firefox.instance_1_67" -> "firefox",
   "org.mpris.MediaPlayer2.io.github.celluloid_player.Celluloid" -> same id kept whole */
static gchar *mpris_player_id(const gchar *bus) {
    const gchar *p = bus + strlen("org.mpris.MediaPlayer2.");
    gchar *id = g_strdup(p);
    gchar *inst = strstr(id, ".instance");
    if (inst) *inst = '\0';
    return id;
}

/* services that expose MPRIS but are not players */
static gboolean mpris_is_helper(const gchar *id) {
    static const gchar *const skip[] = { "playerctld", "kdeconnect", "plasma-browser-integration", NULL };
    for (gint i = 0; skip[i]; i++) if (g_ascii_strncasecmp(id, skip[i], strlen(skip[i])) == 0) return TRUE;
    return FALSE;
}

/* Has this player actually played something? (Playing or Paused) */
static gboolean mpris_has_played(GDBusConnection *bus, const gchar *name) {
    GVariant *r = g_dbus_connection_call_sync(bus, name, "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "PlaybackStatus"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 300, NULL, NULL);
    if (!r) return FALSE;
    GVariant *v = NULL;
    g_variant_get(r, "(v)", &v);
    gboolean played = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) &&
                      g_strcmp0(g_variant_get_string(v, NULL), "Stopped") != 0;
    if (v) g_variant_unref(v);
    g_variant_unref(r);
    return played;
}

/* Media Players tab list, refreshed live while the settings window is open */
static GtkWidget *g_known_players_lb = NULL;
static void populate_known_players_listbox(GtkWidget *lb, UltimateShell *sh);

static gboolean is_token_in_semicolon_list(const gchar *csv, const gchar *tok) {
    if (!csv || !tok || !tok[0]) return FALSE;
    gboolean found = FALSE;
    gchar **parts = g_strsplit(csv, ";", -1);
    for (int i = 0; parts && parts[i]; i++) {
        if (g_ascii_strcasecmp(g_strstrip(parts[i]), tok) == 0) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    return found;
}

static GtkWidget *create_offline_persistent_mpris_item(SystrayWidget *tray, const gchar *short_name) {
    if (!short_name || !short_name[0]) return NULL;

    gchar *identity = NULL;
    gchar *icon_name = NULL;
    gchar *desktop_id = g_strdup_printf("%s.desktop", short_name);
    GDesktopAppInfo *app_info = g_desktop_app_info_new(desktop_id);

    if (app_info) {
        identity = g_strdup(g_app_info_get_display_name(G_APP_INFO(app_info)));
        GIcon *gicon = g_app_info_get_icon(G_APP_INFO(app_info));
        if (gicon) icon_name = g_icon_to_string(gicon);
        g_object_unref(app_info);
    }
    g_free(desktop_id);

    if (!identity) {
        if (g_ascii_strcasecmp(short_name, "vlc") == 0) identity = g_strdup("VLC media player");
        else if (g_ascii_strcasecmp(short_name, "strawberry") == 0) identity = g_strdup("Strawberry");
        else {
            identity = g_strdup(short_name);
            identity[0] = g_ascii_toupper(identity[0]);
        }
    }
    if (!icon_name) {
        icon_name = g_ascii_strdown(short_name, -1);
    }

    GtkWidget *item = gtk_image_menu_item_new();
    GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), img);
    gtk_image_menu_item_set_always_show_image(GTK_IMAGE_MENU_ITEM(item), TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *title_lbl = gtk_label_new(NULL);
    gchar *title_markup = g_markup_printf_escaped("<b>%s</b>", identity);
    gtk_label_set_markup(GTK_LABEL(title_lbl), title_markup);
    g_free(title_markup);
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_lbl), 36);

    GtkWidget *status_lbl = gtk_label_new("Not currently playing");
    gtk_label_set_xalign(GTK_LABEL(status_lbl), 0.0);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_lbl), "dim-label");

    gtk_box_pack_start(GTK_BOX(vbox), title_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_lbl, FALSE, FALSE, 0);

    GtkWidget *ctl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_valign(ctl_box, GTK_ALIGN_CENTER);

    GtkWidget *btn_prev = gtk_button_new_from_icon_name("media-skip-backward-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *btn_play = gtk_button_new_from_icon_name("media-playback-start-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *btn_next = gtk_button_new_from_icon_name("media-skip-forward-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(btn_prev), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(btn_play), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(btn_next), GTK_RELIEF_NONE);
    gtk_widget_set_sensitive(btn_prev, FALSE);
    gtk_widget_set_sensitive(btn_next, FALSE);

    gtk_box_pack_start(GTK_BOX(ctl_box), btn_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl_box), btn_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl_box), btn_next, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), ctl_box, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(item), hbox);

    g_object_set_data(G_OBJECT(item), "mpris-running", GINT_TO_POINTER(0));
    g_object_set_data_full(G_OBJECT(item), "mpris-launch", g_strdup(short_name), g_free);
    g_object_set_data(G_OBJECT(item), "btn-prev", btn_prev);
    g_object_set_data(G_OBJECT(item), "btn-play", btn_play);
    g_object_set_data(G_OBJECT(item), "btn-next", btn_next);

    gtk_widget_add_events(item, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(item, "button-press-event", G_CALLBACK(on_mpris_menuitem_button_press), tray);
    g_signal_connect(item, "button-release-event", G_CALLBACK(on_mpris_menuitem_button_release), NULL);

    g_free(identity);
    g_free(icon_name);
    return item;
}

static GtkWidget *create_mpris_menu_item_from_bus(SystrayWidget *tray, const gchar *bus_name) {
    if (!tray->session_bus) return NULL;

    GVariant *root_props = g_dbus_connection_call_sync(
        tray->session_bus, bus_name, "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.mpris.MediaPlayer2"),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);

    GVariant *player_props = g_dbus_connection_call_sync(
        tray->session_bus, bus_name, "/org/mpris/MediaPlayer2",
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.mpris.MediaPlayer2.Player"),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);

    gchar *identity = NULL;
    gchar *desktop_entry = NULL;
    gchar *playback_status = NULL;
    gchar *track_title = NULL;

    if (root_props) {
        GVariant *dict = g_variant_get_child_value(root_props, 0);
        g_variant_lookup(dict, "Identity", "s", &identity);
        g_variant_lookup(dict, "DesktopEntry", "s", &desktop_entry);
        g_variant_unref(dict);
        g_variant_unref(root_props);
    }

    if (player_props) {
        GVariant *dict = g_variant_get_child_value(player_props, 0);
        g_variant_lookup(dict, "PlaybackStatus", "s", &playback_status);
        GVariant *meta = g_variant_lookup_value(dict, "Metadata", G_VARIANT_TYPE("a{sv}"));
        if (meta) {
            g_variant_lookup(meta, "xesam:title", "s", &track_title);
            g_variant_unref(meta);
        }
        g_variant_unref(dict);
        g_variant_unref(player_props);
    }

    if (!identity) {
        const gchar *suffix = strrchr(bus_name, '.');
        identity = g_strdup(suffix ? suffix + 1 : bus_name);
    }

    gchar *icon_name = NULL;
    if (desktop_entry) {
        gchar *desktop_id = g_strdup_printf("%s.desktop", desktop_entry);
        GDesktopAppInfo *app_info = g_desktop_app_info_new(desktop_id);
        if (app_info) {
            GIcon *gicon = g_app_info_get_icon(G_APP_INFO(app_info));
            if (gicon) icon_name = g_icon_to_string(gicon);
            g_object_unref(app_info);
        }
        g_free(desktop_id);
        if (!icon_name) icon_name = g_strdup(desktop_entry);
    }
    if (!icon_name) {
        if (g_ascii_strcasecmp(identity, "VLC media player") == 0 || strstr(bus_name, "vlc"))
            icon_name = g_strdup("vlc");
        else
            icon_name = g_ascii_strdown(identity, -1);
    }

    gboolean is_playing = (g_strcmp0(playback_status, "Playing") == 0);
    const gchar *sub_text = "Not currently playing";
    if (track_title && track_title[0] != '\0' && (is_playing || g_strcmp0(playback_status, "Paused") == 0)) {
        sub_text = track_title;
    } else if (is_playing) {
        sub_text = "Playing";
    }

    GtkWidget *item = gtk_image_menu_item_new();
    GtkWidget *img = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_image_menu_item_set_image(GTK_IMAGE_MENU_ITEM(item), img);
    gtk_image_menu_item_set_always_show_image(GTK_IMAGE_MENU_ITEM(item), TRUE);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);

    GtkWidget *title_lbl = gtk_label_new(NULL);
    gchar *title_markup = g_markup_printf_escaped("<b>%s</b>", identity);
    gtk_label_set_markup(GTK_LABEL(title_lbl), title_markup);
    g_free(title_markup);
    gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title_lbl), 36);

    GtkWidget *status_lbl = gtk_label_new(sub_text);
    gtk_label_set_xalign(GTK_LABEL(status_lbl), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(status_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(status_lbl), 36);
    gtk_style_context_add_class(gtk_widget_get_style_context(status_lbl), "dim-label");

    gtk_box_pack_start(GTK_BOX(vbox), title_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), status_lbl, FALSE, FALSE, 0);

    GtkWidget *ctl_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_valign(ctl_box, GTK_ALIGN_CENTER);

    GtkWidget *btn_prev = gtk_button_new_from_icon_name("media-skip-backward-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *btn_play = gtk_button_new_from_icon_name(
        is_playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic", GTK_ICON_SIZE_MENU);
    GtkWidget *btn_next = gtk_button_new_from_icon_name("media-skip-forward-symbolic", GTK_ICON_SIZE_MENU);

    gtk_button_set_relief(GTK_BUTTON(btn_prev), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(btn_play), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(btn_next), GTK_RELIEF_NONE);

    gtk_box_pack_start(GTK_BOX(ctl_box), btn_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl_box), btn_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl_box), btn_next, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(hbox), ctl_box, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(item), hbox);

    g_object_set_data(G_OBJECT(item), "mpris-running", GINT_TO_POINTER(1));
    g_object_set_data_full(G_OBJECT(item), "mpris-bus", g_strdup(bus_name), g_free);
    g_object_set_data(G_OBJECT(item), "btn-prev", btn_prev);
    g_object_set_data(G_OBJECT(item), "btn-play", btn_play);
    g_object_set_data(G_OBJECT(item), "btn-next", btn_next);

    gtk_widget_add_events(item, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(item, "button-press-event", G_CALLBACK(on_mpris_menuitem_button_press), tray);
    g_signal_connect(item, "button-release-event", G_CALLBACK(on_mpris_menuitem_button_release), NULL);

    g_free(identity);
    g_free(desktop_entry);
    g_free(playback_status);
    g_free(track_title);
    g_free(icon_name);

    return item;
}

/* =========================================================================
 * Upstream PulseAudio Port & Device Submenu Parser
 * ========================================================================= */
static gboolean is_monitor_source_block(const gchar *block) {
    if (!block) return FALSE;
    if (strstr(block, "Monitor of Sink:")) {
        gchar *m = strstr(block, "Monitor of Sink:");
        gchar *nl = strchr(m, '\n');
        gchar *val = nl ? g_strndup(m + 16, nl - (m + 16)) : g_strdup(m + 16);
        g_strstrip(val);
        gboolean is_mon = (g_strcmp0(val, "n/a") != 0 && val[0] != '\0');
        g_free(val);
        if (is_mon) return TRUE;
    }
    if (strstr(block, ".monitor")) return TRUE;
    return FALSE;
}

static GList *parse_pulse_port_choices(const gchar *pactl_cmd, const gchar *block_prefix,
                                       const gchar *default_dev_cmd, gboolean is_source,
                                       gchar **out_active_label)
{
    GList *choices = NULL;
    gchar *def_dev = NULL;
    def_dev = pa_cached_output(default_dev_cmd);
    if (def_dev) g_strstrip(def_dev);

    gchar *out = NULL;
    out = pa_cached_output(pactl_cmd);
    if (!out) {
        g_free(def_dev);
        return NULL;
    }

    gchar **blocks = g_strsplit(out, block_prefix, -1);
    gint valid_devs = 0;

    for (int i = 1; blocks && blocks[i]; i++) {
        if (is_source && is_monitor_source_block(blocks[i])) continue;
        valid_devs++;
    }

    for (int i = 1; blocks && blocks[i]; i++) {
        if (is_source && is_monitor_source_block(blocks[i])) continue;

        gchar *dev_name = NULL;
        gchar *dev_desc = NULL;
        gchar *active_port = NULL;
        gboolean in_ports = FALSE;
        GList *block_ports = NULL;

        gchar **lines = g_strsplit(blocks[i], "\n", -1);
        for (int l = 0; lines && lines[l]; l++) {
            gchar *raw = lines[l];
            gchar *trimmed = g_strstrip(g_strdup(raw));

            if (g_str_has_prefix(trimmed, "Name: ")) {
                g_free(dev_name);
                dev_name = g_strdup(g_strstrip(trimmed + 6));
                in_ports = FALSE;
            } else if (g_str_has_prefix(trimmed, "Description: ") && !dev_desc) {
                dev_desc = g_strdup(g_strstrip(trimmed + 13));
            } else if (g_strcmp0(trimmed, "Ports:") == 0) {
                in_ports = TRUE;
            } else if (g_str_has_prefix(trimmed, "Active Port: ")) {
                in_ports = FALSE;
                g_free(active_port);
                active_port = g_strdup(g_strstrip(trimmed + 13));
            } else if (in_ports) {
                gint tab_count = 0;
                while (raw[tab_count] == '\t') tab_count++;
                if (tab_count == 2 || (tab_count == 0 && g_str_has_prefix(raw, "                ") && !g_str_has_prefix(raw, "                        "))) {
                    gchar *colon = strchr(trimmed, ':');
                    if (colon && !g_str_has_prefix(trimmed, "Part of profile") && !g_str_has_prefix(trimmed, "Properties")) {
                        gchar *p_id = g_strndup(trimmed, colon - trimmed);
                        gchar *rest = g_strstrip(colon + 1);
                        gchar *paren = strstr(rest, " (");
                        gchar *p_desc = paren ? g_strndup(rest, paren - rest) : g_strdup(rest);

                        AudioPortChoice *pc = g_new0(AudioPortChoice, 1);
                        pc->sink_or_source_name = g_strdup(dev_name ? dev_name : "");
                        pc->port_name = p_id;
                        pc->label = p_desc;
                        block_ports = g_list_append(block_ports, pc);
                    }
                } else if (tab_count == 1 && trimmed[0] != '\0') {
                    in_ports = FALSE;
                }
            }
            g_free(trimmed);
        }
        g_strfreev(lines);

        gboolean is_def_dev = (def_dev && dev_name && g_strcmp0(def_dev, dev_name) == 0) || (valid_devs == 1);

        if (block_ports) {
            for (GList *p = block_ports; p != NULL; p = p->next) {
                AudioPortChoice *pc = (AudioPortChoice *)p->data;
                pc->active = (is_def_dev && active_port && g_strcmp0(pc->port_name, active_port) == 0);
                if (pc->active && out_active_label && !*out_active_label) {
                    *out_active_label = g_strdup(pc->label);
                }
                choices = g_list_append(choices, pc);
            }
            g_list_free(block_ports);
        } else if (dev_name) {
            AudioPortChoice *pc = g_new0(AudioPortChoice, 1);
            pc->sink_or_source_name = g_strdup(dev_name);
            pc->port_name = NULL;
            pc->label = g_strdup(dev_desc ? dev_desc : dev_name);
            pc->active = is_def_dev;
            if (pc->active && out_active_label && !*out_active_label) {
                *out_active_label = g_strdup(pc->label);
            }
            choices = g_list_append(choices, pc);
        }

        g_free(dev_name);
        g_free(dev_desc);
        g_free(active_port);
    }

    g_strfreev(blocks);
    g_free(out);
    g_free(def_dev);

    if (out_active_label && !*out_active_label && choices) {
        AudioPortChoice *first = (AudioPortChoice *)choices->data;
        first->active = TRUE;
        *out_active_label = g_strdup(first->label);
    }
    return choices;
}

static void on_output_port_toggled(GtkCheckMenuItem *mi, SystrayWidget *tray) {
    if (!gtk_check_menu_item_get_active(mi)) return;
    const gchar *dev  = g_object_get_data(G_OBJECT(mi), "dev-name");
    const gchar *port = g_object_get_data(G_OBJECT(mi), "port-name");
    const gchar *lbl  = g_object_get_data(G_OBJECT(mi), "port-label");

    if (dev && dev[0]) {
        gchar *cmd = NULL;
        if (port && port[0]) {
            cmd = g_strdup_printf(
                "sh -c \"pactl set-default-sink '%s' 2>/dev/null; pactl set-sink-port '%s' '%s' 2>/dev/null\"",
                dev, dev, port);
        } else {
            cmd = g_strdup_printf("sh -c \"pactl set-default-sink '%s' 2>/dev/null\"", dev);
        }
        g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
    }
    if (lbl && GTK_IS_MENU_ITEM(tray->output_submenu_item)) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(tray->output_submenu_item), lbl);
    }
}

static void on_input_port_toggled(GtkCheckMenuItem *mi, SystrayWidget *tray) {
    if (!gtk_check_menu_item_get_active(mi)) return;
    const gchar *dev  = g_object_get_data(G_OBJECT(mi), "dev-name");
    const gchar *port = g_object_get_data(G_OBJECT(mi), "port-name");
    const gchar *lbl  = g_object_get_data(G_OBJECT(mi), "port-label");

    if (dev && dev[0]) {
        gchar *cmd = NULL;
        if (port && port[0]) {
            cmd = g_strdup_printf(
                "sh -c \"pactl set-default-source '%s' 2>/dev/null; pactl set-source-port '%s' '%s' 2>/dev/null\"",
                dev, dev, port);
        } else {
            cmd = g_strdup_printf("sh -c \"pactl set-default-source '%s' 2>/dev/null\"", dev);
        }
        g_spawn_command_line_async(cmd, NULL);
        g_free(cmd);
    }
    if (lbl && GTK_IS_MENU_ITEM(tray->input_submenu_item)) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(tray->input_submenu_item), lbl);
    }
}

static gint compare_mpris_bus_names(gconstpointer a, gconstpointer b) {
    return g_ascii_strcasecmp((const gchar *)a, (const gchar *)b);
}

static void refresh_audio_devices_and_mpris(SystrayWidget *tray) {
    if (!tray || !GTK_IS_MENU(tray->vol_menu)) return;

    pa_rebuild_app_items(tray);

    /* 1. Populate Output Ports Radio Submenu */
    gchar *active_out_label = NULL;
    GList *out_choices = parse_pulse_port_choices(
        "sh -c \"LC_ALL=C pactl list sinks\"", "Sink #",
        "sh -c \"LC_ALL=C pactl get-default-sink\"", FALSE, &active_out_label);

    if (active_out_label) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(tray->output_submenu_item), active_out_label);
        g_free(active_out_label);
    }

    GtkWidget *out_sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(tray->output_submenu_item));
    if (out_sub) {
        GList *old = gtk_container_get_children(GTK_CONTAINER(out_sub));
        for (GList *l = old; l != NULL; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(old);

        GSList *out_group = NULL;
        for (GList *l = out_choices; l != NULL; l = l->next) {
            AudioPortChoice *pc = (AudioPortChoice *)l->data;
            GtkWidget *rmi = gtk_radio_menu_item_new_with_label(out_group, pc->label);
            out_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(rmi));
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(rmi), pc->active);
            g_object_set_data_full(G_OBJECT(rmi), "dev-name", g_strdup(pc->sink_or_source_name), g_free);
            g_object_set_data_full(G_OBJECT(rmi), "port-name", g_strdup(pc->port_name ? pc->port_name : ""), g_free);
            g_object_set_data_full(G_OBJECT(rmi), "port-label", g_strdup(pc->label), g_free);
            g_signal_connect(rmi, "toggled", G_CALLBACK(on_output_port_toggled), tray);
            gtk_menu_shell_append(GTK_MENU_SHELL(out_sub), rmi);
        }
        gtk_widget_show_all(out_sub);
    }
    g_list_free_full(out_choices, free_port_choice);

    /* 2. Populate Input Ports Radio Submenu */
    gchar *active_in_label = NULL;
    GList *in_choices = parse_pulse_port_choices(
        "sh -c \"LC_ALL=C pactl list sources\"", "Source #",
        "sh -c \"LC_ALL=C pactl get-default-source\"", TRUE, &active_in_label);

    if (active_in_label) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(tray->input_submenu_item), active_in_label);
        g_free(active_in_label);
    }

    GtkWidget *in_sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(tray->input_submenu_item));
    if (in_sub) {
        GList *old = gtk_container_get_children(GTK_CONTAINER(in_sub));
        for (GList *l = old; l != NULL; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(old);

        GSList *in_group = NULL;
        for (GList *l = in_choices; l != NULL; l = l->next) {
            AudioPortChoice *pc = (AudioPortChoice *)l->data;
            GtkWidget *rmi = gtk_radio_menu_item_new_with_label(in_group, pc->label);
            in_group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(rmi));
            gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(rmi), pc->active);
            g_object_set_data_full(G_OBJECT(rmi), "dev-name", g_strdup(pc->sink_or_source_name), g_free);
            g_object_set_data_full(G_OBJECT(rmi), "port-name", g_strdup(pc->port_name ? pc->port_name : ""), g_free);
            g_object_set_data_full(G_OBJECT(rmi), "port-label", g_strdup(pc->label), g_free);
            g_signal_connect(rmi, "toggled", G_CALLBACK(on_input_port_toggled), tray);
            gtk_menu_shell_append(GTK_MENU_SHELL(in_sub), rmi);
        }
        gtk_widget_show_all(in_sub);
    }
    g_list_free_full(in_choices, free_port_choice);

    /* 3. Populate MPRIS2 Media Players (Running + Persistent Offline Players) */
    for (GList *l = tray->mpris_items; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(tray->mpris_items);
    tray->mpris_items = NULL;

    if (!tray->shell || tray->shell->audio_mpris_enabled) {
        GList *mpris_buses = NULL;
        GList *running_short_names = NULL;

        if (tray->session_bus) {
            GVariant *names_var = g_dbus_connection_call_sync(
                tray->session_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                "org.freedesktop.DBus", "ListNames", NULL,
                G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);

            if (names_var) {
                GVariantIter *iter = NULL;
                gchar *bname = NULL;
                g_variant_get(names_var, "(as)", &iter);

                while (g_variant_iter_loop(iter, "s", &bname)) {
                    if (g_str_has_prefix(bname, "org.mpris.MediaPlayer2.")) {
                        gchar *clean_short = mpris_player_id(bname);
                        if (mpris_is_helper(clean_short)) { g_free(clean_short); continue; }

                        if (tray->shell) {
                            /* remember only real players: ones that have played something */
                            if (!is_token_in_semicolon_list(tray->shell->audio_known_players, clean_short) &&
                                mpris_has_played(tray->session_bus, bname)) {
                                gchar *joined = (tray->shell->audio_known_players && tray->shell->audio_known_players[0])
                                    ? g_strdup_printf("%s;%s", tray->shell->audio_known_players, clean_short)
                                    : g_strdup(clean_short);
                                g_free(tray->shell->audio_known_players);
                                tray->shell->audio_known_players = joined;
                                if (tray->shell->channel) {
                                    xfconf_channel_set_string(tray->shell->channel, "/plugins/ultimateshell/audio_known_players", joined);
                                }
                                if (g_known_players_lb) populate_known_players_listbox(g_known_players_lb, tray->shell);
                            }
                            if (is_token_in_semicolon_list(tray->shell->audio_ignored_players, clean_short)) {
                                g_free(clean_short);
                                continue;
                            }
                        }
                        running_short_names = g_list_append(running_short_names, clean_short);
                        mpris_buses = g_list_append(mpris_buses, g_strdup(bname));
                    }
                }
                g_variant_iter_free(iter);
                g_variant_unref(names_var);
            }
        }

        mpris_buses = g_list_sort(mpris_buses, compare_mpris_bus_names);
        gint insert_pos = 6;
        gint player_idx = 0;

        /* 3A. Render currently running MPRIS players */
        for (GList *b = mpris_buses; b != NULL; b = b->next) {
            const gchar *bus_str = (const gchar *)b->data;
            GtkWidget *mpris_row = create_mpris_menu_item_from_bus(tray, bus_str);
            if (mpris_row) {
                if (player_idx > 0) {
                    GtkWidget *inter_sep = gtk_separator_menu_item_new();
                    gtk_widget_show(inter_sep);
                    gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), inter_sep, insert_pos++);
                    tray->mpris_items = g_list_append(tray->mpris_items, inter_sep);
                }

                gtk_widget_show_all(mpris_row);
                gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), mpris_row, insert_pos++);
                tray->mpris_items = g_list_append(tray->mpris_items, mpris_row);

                GtkWidget *pl_item = create_mpris_playlist_submenu_item(tray, bus_str);
                if (pl_item) {
                    gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), pl_item, insert_pos++);
                    tray->mpris_items = g_list_append(tray->mpris_items, pl_item);
                }
                player_idx++;
            }
        }

        /* 3B. Render offline Persistent players that are checked as Persistent and not Ignored */
        if (tray->shell) {
            const gchar *pers_csv = tray->shell->audio_persistent_players ? tray->shell->audio_persistent_players : "";
            gchar **ptoks = g_strsplit(pers_csv, ";", -1);
            for (int i = 0; ptoks && ptoks[i]; i++) {
                gchar *p = g_strstrip(ptoks[i]);
                if (!p[0]) continue;
                if (is_token_in_semicolon_list(tray->shell->audio_ignored_players, p)) continue;

                gboolean already_running = FALSE;
                for (GList *r = running_short_names; r != NULL; r = r->next) {
                    if (g_ascii_strcasecmp((const gchar *)r->data, p) == 0) {
                        already_running = TRUE;
                        break;
                    }
                }
                if (already_running) continue;

                GtkWidget *off_row = create_offline_persistent_mpris_item(tray, p);
                if (off_row) {
                    if (player_idx > 0) {
                        GtkWidget *inter_sep = gtk_separator_menu_item_new();
                        gtk_widget_show(inter_sep);
                        gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), inter_sep, insert_pos++);
                        tray->mpris_items = g_list_append(tray->mpris_items, inter_sep);
                    }
                    gtk_widget_show_all(off_row);
                    gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), off_row, insert_pos++);
                    tray->mpris_items = g_list_append(tray->mpris_items, off_row);
                    player_idx++;
                }
            }
            g_strfreev(ptoks);
        }

        g_list_free_full(mpris_buses, g_free);
        g_list_free_full(running_short_names, g_free);
    }

    if (tray->mpris_separator) {
        if (tray->mpris_items != NULL)
            gtk_widget_show(tray->mpris_separator);
        else
            gtk_widget_hide(tray->mpris_separator);
    }
}

/* =========================================================================
 * Tray Persistence, DND & Hardware Helpers
 * ========================================================================= */
static gboolean is_id_in_csv(const gchar *csv, const gchar *id) {
    if (!csv || !id || csv[0] == '\0') return FALSE;
    gboolean found = FALSE;
    gchar **tokens = g_strsplit(csv, ";", -1);
    for (int i = 0; tokens && tokens[i]; i++) {
        if (g_strcmp0(tokens[i], id) == 0) { found = TRUE; break; }
    }
    g_strfreev(tokens);
    return found;
}

static gint get_id_order_index(SystrayWidget *tray, const gchar *id) {
    if (!tray->shell || !tray->shell->channel || !id) return 9999;
    gchar *csv = xfconf_channel_get_string(tray->shell->channel, "/plugins/ultimateshell/tray_icon_order",
        "builtin:removable;builtin:bluetooth;builtin:clipboard;builtin:network;builtin:audio;builtin:battery");
    gint idx = 9999;
    if (csv) {
        gchar **tokens = g_strsplit(csv, ";", -1);
        for (int i = 0; tokens && tokens[i]; i++) {
            if (g_strcmp0(tokens[i], id) == 0) { idx = i; break; }
        }
        g_strfreev(tokens);
        g_free(csv);
    }
    return idx;
}

static gboolean load_hidden_state_for_id(SystrayWidget *tray, const gchar *id, gboolean is_builtin) {
    if (!tray->shell || !tray->shell->channel) return FALSE;
    gchar *hidden_csv = xfconf_channel_get_string(tray->shell->channel, "/plugins/ultimateshell/tray_hidden_ids", "");
    gchar *shown_csv  = xfconf_channel_get_string(tray->shell->channel, "/plugins/ultimateshell/tray_shown_ids", "builtin:clipboard;builtin:network;builtin:audio;builtin:battery");

    gboolean hidden = FALSE;
    if (is_id_in_csv(hidden_csv, id)) {
        hidden = TRUE;
    } else if (is_id_in_csv(shown_csv, id)) {
        hidden = FALSE;
    } else {
        hidden = (!is_builtin && tray->shell->tray_hide_new_default);
    }

    g_free(hidden_csv);
    g_free(shown_csv);
    return hidden;
}

static void save_tray_layout_to_xfconf(SystrayWidget *tray) {
    if (!tray->shell || !tray->shell->channel) return;
    gchar *old_order  = xfconf_channel_get_string(tray->shell->channel, "/plugins/ultimateshell/tray_icon_order", "");
    gchar *old_hidden = xfconf_channel_get_string(tray->shell->channel, "/plugins/ultimateshell/tray_hidden_ids", "");
    gchar *old_shown  = xfconf_channel_get_string(tray->shell->channel, "/plugins/ultimateshell/tray_shown_ids", "");

    GString *order_str  = g_string_new("");
    GString *hidden_str = g_string_new("");
    GString *shown_str  = g_string_new("");

    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *item = (TrayItem *)l->data;
        if (!item->id || item->id[0] == ':') continue; /* Ignore temporary D-Bus unique names like :1.84 */

        if (order_str->len > 0) g_string_append_c(order_str, ';');
        g_string_append(order_str, item->id);

        if (item->is_hidden) {
            if (hidden_str->len > 0) g_string_append_c(hidden_str, ';');
            g_string_append(hidden_str, item->id);
        } else {
            if (shown_str->len > 0) g_string_append_c(shown_str, ';');
            g_string_append(shown_str, item->id);
        }
    }

    /* Preserve any previously saved app IDs that are not currently running */
    if (old_order && old_order[0]) {
        gchar **toks = g_strsplit(old_order, ";", -1);
        for (int i = 0; toks && toks[i]; i++) {
            if (toks[i][0] && toks[i][0] != ':' && !is_id_in_csv(order_str->str, toks[i])) {
                if (order_str->len > 0) g_string_append_c(order_str, ';');
                g_string_append(order_str, toks[i]);
            }
        }
        g_strfreev(toks);
    }
    if (old_hidden && old_hidden[0]) {
        gchar **toks = g_strsplit(old_hidden, ";", -1);
        for (int i = 0; toks && toks[i]; i++) {
            if (toks[i][0] && toks[i][0] != ':' &&
                !is_id_in_csv(hidden_str->str, toks[i]) && !is_id_in_csv(shown_str->str, toks[i])) {
                if (hidden_str->len > 0) g_string_append_c(hidden_str, ';');
                g_string_append(hidden_str, toks[i]);
            }
        }
        g_strfreev(toks);
    }
    if (old_shown && old_shown[0]) {
        gchar **toks = g_strsplit(old_shown, ";", -1);
        for (int i = 0; toks && toks[i]; i++) {
            if (toks[i][0] && toks[i][0] != ':' &&
                !is_id_in_csv(shown_str->str, toks[i]) && !is_id_in_csv(hidden_str->str, toks[i])) {
                if (shown_str->len > 0) g_string_append_c(shown_str, ';');
                g_string_append(shown_str, toks[i]);
            }
        }
        g_strfreev(toks);
    }

    xfconf_channel_set_string(tray->shell->channel, "/plugins/ultimateshell/tray_icon_order", order_str->str);
    xfconf_channel_set_string(tray->shell->channel, "/plugins/ultimateshell/tray_hidden_ids", hidden_str->str);
    xfconf_channel_set_string(tray->shell->channel, "/plugins/ultimateshell/tray_shown_ids", shown_str->str);

    g_free(old_order);
    g_free(old_hidden);
    g_free(old_shown);
    g_string_free(order_str, TRUE);
    g_string_free(hidden_str, TRUE);
    g_string_free(shown_str, TRUE);
}

/* an empty box still takes the box spacing on both sides - hide it then */
static void tray_hide_empty_boxes(SystrayWidget *tray) {
    {
        GtkWidget *boxes[2] = { tray->visible_box, tray->xembed_box };
        for (gint b = 0; b < 2; b++) {
            if (!GTK_IS_CONTAINER(boxes[b])) continue;
            gboolean any = FALSE;
            GList *kids = gtk_container_get_children(GTK_CONTAINER(boxes[b]));
            for (GList *k = kids; k && !any; k = k->next) any = gtk_widget_get_visible(k->data);
            g_list_free(kids);
            gtk_widget_set_visible(boxes[b], any);
        }
    }
}

static void relayout_tray_containers(SystrayWidget *tray) {
    if (!tray) return;

    gint icon_px = tray->shell ? CLAMP(tray->shell->tray_icon_size, 14, 28) : 16;
    gint spacing = tray->shell ? CLAMP(tray->shell->tray_icon_spacing, 0, 16) : 2;
    gboolean show_all = tray->shell ? tray->shell->tray_show_all : FALSE;

    /* Status updates (power, network, audio, app icons) asked for this about once a second although
       nothing about the layout had changed - and every pass takes all icons off the panel and puts
       them back. Skip when everything the layout depends on is the same as last time. */
    {
        UltimateShell *sh = tray->shell;
        GString *sig = g_string_new(NULL);
        g_string_append_printf(sig, "qs%d%d|", sh ? sh->tray_quick_settings : 0, sh ? sh->tray_qs_bluetooth : 0);
        g_string_append_printf(sig, "%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|", icon_px, spacing, show_all,
            sh ? (gint)sh->mode : -1, sh ? sh->tray_enable_network : -1, sh ? sh->tray_enable_audio : -1,
            sh ? sh->tray_enable_battery : -1, sh ? sh->tray_enable_clipboard : -1,
            tray->bat_present, tray->mic_shown, network_applet_suppressed(tray), g_list_length(tray->all_items));
        for (GList *l = tray->all_items; l; l = l->next) {
            TrayItem *it = l->data;
            if (!it) continue;
            gboolean dup = it->is_xembed && sni_item_has_pid(tray, it->pid);
            g_string_append_printf(sig, "%s:%d:%p:%d;", it->id ? it->id : "?", it->is_hidden, (void *)it->button, dup);
        }
        static gchar *last_sig = NULL;
        if (last_sig && g_strcmp0(last_sig, sig->str) == 0) {
            g_string_free(sig, TRUE);
            /* a changed icon still gets the tray size (that part is cheap) */
            for (GList *l = tray->all_items; l; l = l->next) {
                TrayItem *it = l->data;
                if (it && GTK_IS_IMAGE(it->image) && gtk_image_get_storage_type(GTK_IMAGE(it->image)) == GTK_IMAGE_ICON_NAME)
                    gtk_image_set_pixel_size(GTK_IMAGE(it->image), icon_px);
            }
            return;
        }
        g_free(last_sig);
        last_sig = g_string_free(sig, FALSE);
    }

    gtk_box_set_spacing(GTK_BOX(tray->root_box), spacing);
    gtk_box_set_spacing(GTK_BOX(tray->visible_box), spacing);
    if (GTK_IS_IMAGE(tray->chevron_img)) {
        gtk_image_set_pixel_size(GTK_IMAGE(tray->chevron_img), icon_px);
    }

    /* First, safely detach all tray item buttons while holding a reference so GTK never destroys them! */
    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *item = (TrayItem *)l->data;
        if (!item || !GTK_IS_WIDGET(item->button)) continue;
        if (item->is_xembed) continue;
        g_object_ref(item->button);
        GtkWidget *parent = gtk_widget_get_parent(item->button);
        if (parent) {
            gtk_container_remove(GTK_CONTAINER(parent), item->button);
        }
    }

    guint h_idx = 0;
    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *item = (TrayItem *)l->data;
        if (!item || !GTK_IS_WIDGET(item->button)) continue;

        gboolean disabled_builtin =
            (g_strcmp0(item->id, "builtin:network") == 0 && tray->shell && !tray->shell->tray_enable_network) ||
            (g_strcmp0(item->id, "builtin:audio") == 0 && tray->shell && !tray->shell->tray_enable_audio) ||
            (g_strcmp0(item->id, "builtin:battery") == 0 && tray->shell && !tray->shell->tray_enable_battery) ||
            (g_strcmp0(item->id, "builtin:battery") == 0 && !tray->bat_present) ||
            (g_strcmp0(item->id, "builtin:clipboard") == 0 && tray->shell && !tray->shell->tray_enable_clipboard) ||
            (g_strcmp0(item->id, "builtin:mic") == 0 && (!tray->mic_shown || (tray->shell && !tray->shell->tray_enable_audio))) ||
            (item->is_network_applet && network_applet_suppressed(tray)) ||
            qs_is_member(tray, item);                  /* lives in the Quick Settings group button */

        if (item->is_xembed) {
            /* A hidden XEmbed icon lives in the ▲ popup like the others: its app window is moved
               into a slot there (and back) only when hide / show changes. */
            if (!gtk_widget_get_parent(item->button)) {
                gtk_box_pack_start(GTK_BOX(tray->visible_box), item->button, FALSE, FALSE, 0);
            }
            if (GTK_IS_WIDGET(item->ov_socket)) gtk_widget_set_size_request(item->ov_socket, icon_px, icon_px);
            if (GTK_IS_WIDGET(item->socket)) gtk_widget_set_size_request(item->socket, icon_px, icon_px);   /* follows Icon Size */
            gboolean in_popup = item->is_hidden && !show_all && !disabled_builtin;
            gint col = 0, row = 0;
            if (in_popup) { col = h_idx % 5; row = h_idx / 5; h_idx++; }
            xembed_place(item, in_popup, col, row, icon_px);
            if (disabled_builtin) { gtk_widget_hide(item->button); if (item->ov_wrap) gtk_widget_hide(item->ov_wrap); }
            continue;
        }

        if (disabled_builtin) {
            gtk_widget_hide(item->button);
            g_object_unref(item->button);
            continue;
        }

        if (GTK_IS_IMAGE(item->image)) {
            gtk_widget_set_margin_top(item->image, 0);
            gtk_widget_set_margin_bottom(item->image, 0);
            gtk_widget_set_margin_start(item->image, 0);
            gtk_widget_set_margin_end(item->image, 0);
            if (gtk_image_get_storage_type(GTK_IMAGE(item->image)) == GTK_IMAGE_ICON_NAME) {
                gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
            }
        }

        if (!show_all && item->is_hidden) {
            gint col = h_idx % 5;      /* 5 icons per row */
            gint row = h_idx / 5;
            h_idx++;

            gtk_style_context_add_class(gtk_widget_get_style_context(item->button), "overflow-tray-btn");
            gtk_widget_set_size_request(item->button, 36, 36);
            gtk_widget_set_valign(item->button, GTK_ALIGN_CENTER);
            gtk_widget_set_halign(item->button, GTK_ALIGN_CENTER);
            gtk_grid_attach(GTK_GRID(tray->overflow_grid), item->button, col, row, 1, 1);
            gtk_widget_show_all(item->button);
        } else {
            gtk_style_context_remove_class(gtk_widget_get_style_context(item->button), "overflow-tray-btn");
            gtk_widget_set_size_request(item->button, -1, -1);
            gtk_widget_set_valign(item->button, GTK_ALIGN_FILL);
            gtk_widget_set_halign(item->button, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(tray->visible_box), item->button, FALSE, FALSE, 0);
            gtk_widget_show_all(item->button);
        }
        g_object_unref(item->button);
    }

    xembed_refresh_visibility(tray);
    qs_group_sync(tray);
    tray_hide_empty_boxes(tray);

    /* Respect the exact user-arranged order in tray->all_items 1:1 */
    gint pos_idx = 0;
    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *it = (TrayItem *)l->data;
        if (it && it->button && gtk_widget_get_parent(it->button) == tray->visible_box) {
            gtk_box_reorder_child(GTK_BOX(tray->visible_box), it->button, pos_idx++);
        }
    }

    gint hidden_count = 0;
    if (!show_all) {
        for (GList *l = tray->all_items; l != NULL; l = l->next) {
            TrayItem *it = (TrayItem *)l->data;
            if (!it->button || !it->is_hidden) continue;
            if (qs_is_member(tray, it)) continue;          /* in Quick Settings, not in ▲ */
            if (g_strcmp0(it->id, "builtin:network") == 0 && tray->shell && !tray->shell->tray_enable_network) continue;
            if (g_strcmp0(it->id, "builtin:audio") == 0 && tray->shell && !tray->shell->tray_enable_audio) continue;
            if (g_strcmp0(it->id, "builtin:battery") == 0 && tray->shell && !tray->shell->tray_enable_battery) continue;
            if (g_strcmp0(it->id, "builtin:battery") == 0 && !tray->bat_present) continue;
            if (g_strcmp0(it->id, "builtin:clipboard") == 0 && tray->shell && !tray->shell->tray_enable_clipboard) continue;
            if (g_strcmp0(it->id, "builtin:mic") == 0 && !tray->mic_shown) continue;
            if (it->is_network_applet && network_applet_suppressed(tray)) continue;
            hidden_count++;
        }
    }

    if (GTK_IS_WIDGET(tray->chevron_btn)) {
        gboolean is_mac = (tray->shell && tray->shell->mode == LAYOUT_MACOS);
        if (GTK_IS_IMAGE(tray->chevron_img)) {
            gtk_image_set_from_icon_name(GTK_IMAGE(tray->chevron_img),
                is_mac ? "pan-down-symbolic" : "pan-up-symbolic", GTK_ICON_SIZE_MENU);
        } else {
            GtkWidget *ch = gtk_bin_get_child(GTK_BIN(tray->chevron_btn));
            if (GTK_IS_IMAGE(ch)) {
                gtk_image_set_from_icon_name(GTK_IMAGE(ch),
                    is_mac ? "pan-down-symbolic" : "pan-up-symbolic", GTK_ICON_SIZE_MENU);
            } else if (GTK_IS_LABEL(ch)) {
                gtk_label_set_text(GTK_LABEL(ch), is_mac ? "▾" : "▴");
            }
        }
        if (hidden_count > 0) {
            gtk_widget_show(tray->chevron_btn);
        } else {
            if (GTK_IS_WINDOW(tray->overflow_popup) && gtk_widget_get_visible(tray->overflow_popup)) {
                gtk_widget_hide(tray->overflow_popup);
            }
            if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(tray->chevron_btn))) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->chevron_btn), FALSE);
            }
            gtk_widget_hide(tray->chevron_btn);
        }
    }

    if (GTK_IS_WINDOW(tray->overflow_popup) && hidden_count > 0) {
        gtk_window_resize(GTK_WINDOW(tray->overflow_popup), 1, 1);
        if (gtk_widget_get_visible(tray->overflow_popup)) {
            shell_reposition_popup_window(GTK_TOGGLE_BUTTON(tray->chevron_btn), tray->overflow_popup);
        }
    }
}

static void refresh_removable_ui(SystrayWidget *tray);

void systray_apply_settings(UltimateShell *shell) {
    if (!g_systray) return;
    for (GList *l = g_systray->all_items; l != NULL; l = l->next) {
        TrayItem *it = (TrayItem *)l->data;
        if (it && !it->is_builtin && it->item_proxy) {
            refresh_tray_item_properties(it);
        }
    }
    refresh_removable_ui(g_systray);
    relayout_tray_containers(g_systray);
    rebuild_settings_list_ui(g_systray);
}

static void on_tray_drag_data_get(GtkWidget *widget, GdkDragContext *ctx, GtkSelectionData *sel,
                                  guint info, guint time, TrayItem *item) {
    if (!item || !item->id) return;
    gtk_selection_data_set(sel, gdk_atom_intern_static_string("ULTIMATE_SYSTRAY_ITEM"),
                           8, (const guchar *)item->id, strlen(item->id));
}

static TrayItem *find_tray_item_by_id(SystrayWidget *tray, const gchar *id) {
    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *it = (TrayItem *)l->data;
        if (g_strcmp0(it->id, id) == 0) return it;
    }
    return NULL;
}

static void on_tray_item_drag_received(GtkWidget *dest_btn, GdkDragContext *ctx, gint x, gint y,
                                       GtkSelectionData *sel, guint info, guint time, TrayItem *dest_item) {
    const guchar *raw = gtk_selection_data_get_data(sel);
    gint len = gtk_selection_data_get_length(sel);
    if (!raw || len <= 0) { gtk_drag_finish(ctx, FALSE, FALSE, time); return; }

    gchar *src_id = g_strndup((const gchar *)raw, len);
    SystrayWidget *tray = dest_item->tray;
    TrayItem *src_item = find_tray_item_by_id(tray, src_id);
    g_free(src_id);

    if (src_item && src_item != dest_item) {
        src_item->is_hidden = dest_item->is_hidden;
        tray->all_items = g_list_remove(tray->all_items, src_item);
        gint dest_idx = g_list_index(tray->all_items, dest_item);
        tray->all_items = g_list_insert(tray->all_items, src_item, dest_idx);

        save_tray_layout_to_xfconf(tray);
        relayout_tray_containers(tray);
        rebuild_settings_list_ui(tray);
        gtk_drag_finish(ctx, TRUE, FALSE, time);
        return;
    }
    gtk_drag_finish(ctx, FALSE, FALSE, time);
}

static void on_chevron_drag_received(GtkWidget *chev_btn, GdkDragContext *ctx, gint x, gint y,
                                     GtkSelectionData *sel, guint info, guint time, SystrayWidget *tray) {
    const guchar *raw = gtk_selection_data_get_data(sel);
    gint len = gtk_selection_data_get_length(sel);
    if (!raw || len <= 0) { gtk_drag_finish(ctx, FALSE, FALSE, time); return; }

    gchar *src_id = g_strndup((const gchar *)raw, len);
    TrayItem *src_item = find_tray_item_by_id(tray, src_id);
    g_free(src_id);

    if (src_item) {
        src_item->is_hidden = !src_item->is_hidden;
        save_tray_layout_to_xfconf(tray);
        relayout_tray_containers(tray);
        rebuild_settings_list_ui(tray);
        gtk_drag_finish(ctx, TRUE, FALSE, time);
        return;
    }
    gtk_drag_finish(ctx, FALSE, FALSE, time);
}

static TrayItem *g_tray_drag_item = NULL;
static gdouble   g_tray_drag_start_x = 0.0;
static gdouble   g_tray_drag_start_y = 0.0;
static gboolean  g_tray_drag_moved = FALSE;

static gboolean on_tray_item_btn_press(GtkWidget *btn, GdkEventButton *ev, TrayItem *item) {
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 1) {
        g_tray_drag_item = item;
        g_tray_drag_start_x = ev->x_root;
        g_tray_drag_start_y = ev->y_root;
        g_tray_drag_moved = FALSE;
    }
    return FALSE;
}

static gboolean on_tray_item_motion(GtkWidget *btn, GdkEventMotion *ev, TrayItem *item) {
    if (!item || !item->tray || g_tray_drag_item != item || !(ev->state & GDK_BUTTON1_MASK)) return FALSE;
    if (gtk_widget_get_parent(btn) == item->tray->overflow_grid) {
        /* overflow menu: pick up the icon; it is placed when the button is released */
        if (!g_tray_drag_moved && ABS(ev->x_root - g_tray_drag_start_x) < 6.0 && ABS(ev->y_root - g_tray_drag_start_y) < 6.0)
            return FALSE;
        g_tray_drag_moved = TRUE;
        gtk_widget_set_opacity(btn, 0.45);
        return TRUE;
    }
    if (gtk_widget_get_parent(btn) != item->tray->visible_box) return FALSE;

    gdouble dx = ABS(ev->x_root - g_tray_drag_start_x);
    gdouble dy = ABS(ev->y_root - g_tray_drag_start_y);
    if (!g_tray_drag_moved && dx < 6.0 && dy < 6.0) return FALSE;
    g_tray_drag_moved = TRUE;
    if (item->is_xembed) item->xembed_dragged = TRUE;    /* its release must not click the app */

    SystrayWidget *tray = item->tray;
    /* Find which visible item (within the same group: app icons vs built-in icons) the cursor is over */
    TrayItem *target_item = NULL;
    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *cand = (TrayItem *)l->data;
        if (!cand || cand == item) continue;
        if (!cand->button || gtk_widget_get_parent(cand->button) != tray->visible_box) continue;
        if (!gtk_widget_get_visible(cand->button) || !gtk_widget_get_window(cand->button)) continue;

        gint wx = 0, wy = 0;
        gdk_window_get_origin(gtk_widget_get_window(cand->button), &wx, &wy);
        GtkAllocation ca;
        gtk_widget_get_allocation(cand->button, &ca);
        gint left = wx + ca.x;
        gint right = left + ca.width;
        if ((gint)ev->x_root >= left && (gint)ev->x_root <= right) {
            target_item = cand;
            break;
        }
    }

    if (target_item && target_item != item) {
        gint dest_idx = g_list_index(tray->all_items, target_item);
        tray->all_items = g_list_remove(tray->all_items, item);
        tray->all_items = g_list_insert(tray->all_items, item, dest_idx);
        save_tray_layout_to_xfconf(tray);
        relayout_tray_containers(tray);
        rebuild_settings_list_ui(tray);
    }
    return TRUE;
}

static gboolean on_tray_item_btn_release(GtkWidget *btn, GdkEventButton *ev, TrayItem *item) {
    if (ev->button == 1 && g_tray_drag_item == item) {
        gboolean moved = g_tray_drag_moved;
        g_tray_drag_item = NULL;
        g_tray_drag_moved = FALSE;
        if (moved && item && item->tray && gtk_widget_get_parent(btn) == item->tray->overflow_grid) {
            SystrayWidget *tray = item->tray;
            gtk_widget_set_opacity(btn, 1.0);
            TrayItem *target = NULL;
            for (GList *l = tray->all_items; l && !target; l = l->next) {
                TrayItem *c = l->data;
                if (!c || c == item || !c->button || gtk_widget_get_parent(c->button) != tray->overflow_grid) continue;
                if (!gtk_widget_get_window(c->button)) continue;
                gint wx = 0, wy = 0;
                gdk_window_get_origin(gtk_widget_get_window(c->button), &wx, &wy);
                GtkAllocation a;
                gtk_widget_get_allocation(c->button, &a);
                if (ev->x_root >= wx + a.x && ev->x_root <= wx + a.x + a.width &&
                    ev->y_root >= wy + a.y && ev->y_root <= wy + a.y + a.height) target = c;
            }
            if (target) {
                gint si = g_list_index(tray->all_items, item), ti = g_list_index(tray->all_items, target);
                tray->all_items = g_list_remove(tray->all_items, item);
                ti = g_list_index(tray->all_items, target);
                tray->all_items = g_list_insert(tray->all_items, item, (si < ti + 1) ? ti + 1 : ti);
                save_tray_layout_to_xfconf(tray);
                relayout_tray_containers(tray);
                rebuild_settings_list_ui(tray);
            }
            gtk_widget_unset_state_flags(btn, GTK_STATE_FLAG_ACTIVE);
            return TRUE;
        }
        if (moved) {
            if (item && item->tray) save_tray_layout_to_xfconf(item->tray);
            gtk_widget_unset_state_flags(btn, GTK_STATE_FLAG_ACTIVE);
            return TRUE;
        }
    }
    return FALSE;
}

static void setup_tray_item_dnd_on(TrayItem *item, GtkWidget *w);
static void setup_tray_item_dnd(TrayItem *item) { setup_tray_item_dnd_on(item, item->button); }
static void setup_tray_item_dnd_on(TrayItem *item, GtkWidget *w) {
    gtk_widget_add_events(w, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON1_MOTION_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(w, "button-press-event", G_CALLBACK(on_tray_item_btn_press), item);
    g_signal_connect(w, "motion-notify-event", G_CALLBACK(on_tray_item_motion), item);
    g_signal_connect(w, "button-release-event", G_CALLBACK(on_tray_item_btn_release), item);

    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, tray_dnd_targets, G_N_ELEMENTS(tray_dnd_targets), GDK_ACTION_MOVE);
    g_signal_connect(w, "drag-data-get", G_CALLBACK(on_tray_drag_data_get), item);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_tray_item_drag_received), item);
}

static void rebuild_clip_history_ui(SystrayWidget *tray);

static void on_clip_row_activated(GtkListBox *box, GtkListBoxRow *row, SystrayWidget *tray) {
    if (!row) return;
    const gchar *txt = g_object_get_data(G_OBJECT(row), "clip-text");
    if (txt && txt[0] != '\0') {
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        gtk_clipboard_set_text(cb, txt, -1);
        GtkClipboard *prim = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
        gtk_clipboard_set_text(prim, txt, -1);
    }
    if (tray->clip_item && GTK_IS_TOGGLE_BUTTON(tray->clip_item->button)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->clip_item->button), FALSE);
    }
}

static void on_clip_clear_clicked(GtkButton *btn, SystrayWidget *tray) {
    g_list_free_full(tray->clip_history, g_free);
    tray->clip_history = NULL;
    GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(cb, "", 0);
    rebuild_clip_history_ui(tray);
}

static void on_clip_settings_clicked(GtkButton *btn, SystrayWidget *tray) {
    g_spawn_command_line_async("sh -c \"xfce4-clipman-settings || xfce4-clipman-history\"", NULL);
    if (tray->clip_item && GTK_IS_TOGGLE_BUTTON(tray->clip_item->button)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->clip_item->button), FALSE);
    }
}

static void rebuild_clip_history_ui(SystrayWidget *tray) {
    if (!tray || !GTK_IS_LIST_BOX(tray->clip_list_box)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(tray->clip_list_box));
    for (GList *l = children; l != NULL; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);

    if (!tray->clip_history) {
        GtkWidget *empty_lbl = gtk_label_new("Clipboard is empty.\nCopy any text to see it here.");
        gtk_label_set_justify(GTK_LABEL(empty_lbl), GTK_JUSTIFY_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(empty_lbl), "winck-subtitle");
        gtk_widget_set_margin_top(empty_lbl, 28);
        gtk_widget_set_margin_bottom(empty_lbl, 28);
        gtk_container_add(GTK_CONTAINER(tray->clip_list_box), empty_lbl);
        gtk_widget_show_all(tray->clip_list_box);
        return;
    }

    for (GList *l = tray->clip_history; l != NULL; l = l->next) {
        const gchar *raw = (const gchar *)l->data;
        if (!raw) continue;

        /* Create clean single/double line preview */
        gchar **lines = g_strsplit(raw, "\n", 3);
        gchar *preview = g_strjoinv(" ↵ ", lines);
        g_strfreev(lines);
        g_strstrip(preview);

        GtkWidget *row = gtk_list_box_row_new();
        gtk_style_context_add_class(gtk_widget_get_style_context(row), "app-grid-tile");
        g_object_set_data_full(G_OBJECT(row), "clip-text", g_strdup(raw), g_free);

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_top(hbox, 6);
        gtk_widget_set_margin_bottom(hbox, 6);
        gtk_widget_set_margin_start(hbox, 8);
        gtk_widget_set_margin_end(hbox, 8);

        GtkWidget *ic = gtk_image_new_from_icon_name("edit-copy-symbolic", GTK_ICON_SIZE_MENU);
        GtkWidget *lbl = gtk_label_new(preview);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 32);
        gtk_label_set_lines(GTK_LABEL(lbl), 2);
        gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
        gtk_widget_set_tooltip_text(row, raw);

        gtk_box_pack_start(GTK_BOX(hbox), ic, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_container_add(GTK_CONTAINER(tray->clip_list_box), row);
        g_free(preview);
    }
    gtk_widget_show_all(tray->clip_list_box);
}

static void on_clipboard_text_received(GtkClipboard *clipboard, const gchar *text, gpointer data) {
    SystrayWidget *tray = (SystrayWidget *)data;
    if (!tray || !text) return;
    gchar *trimmed = g_strstrip(g_strdup(text));
    if (trimmed[0] == '\0') {
        g_free(trimmed);
        return;
    }
    /* Avoid duplicates if already at top of history */
    if (tray->clip_history && g_strcmp0((const gchar *)tray->clip_history->data, trimmed) == 0) {
        g_free(trimmed);
        return;
    }
    /* Remove existing duplicate lower in the list */
    for (GList *l = tray->clip_history; l != NULL; l = l->next) {
        if (g_strcmp0((const gchar *)l->data, trimmed) == 0) {
            g_free(l->data);
            tray->clip_history = g_list_delete_link(tray->clip_history, l);
            break;
        }
    }
    tray->clip_history = g_list_prepend(tray->clip_history, trimmed);
    while (g_list_length(tray->clip_history) > 15) {
        GList *last = g_list_last(tray->clip_history);
        g_free(last->data);
        tray->clip_history = g_list_delete_link(tray->clip_history, last);
    }
    if (tray->clip_popup && gtk_widget_get_visible(tray->clip_popup)) {
        rebuild_clip_history_ui(tray);
    }
}

/* =========================================================================
 * Battery & Power
 * Source of truth: UPower (org.freedesktop.UPower) — combined "DisplayDevice",
 * live via PropertiesChanged. Fallback when UPower is absent: /sys/class/power_supply,
 * classified by each supply's real "type"/"scope" (not by name), re-read every 30 s.
 * ========================================================================= */
typedef enum {
    PWR_STATE_UNKNOWN = 0,
    PWR_STATE_CHARGING,
    PWR_STATE_DISCHARGING,
    PWR_STATE_FULL,
    PWR_STATE_NOT_CHARGING      /* plugged in but held (charge limit / pending) */
} PwrState;

typedef struct {
    gboolean has_battery;
    gdouble  percent;
    PwrState state;
    gint64   secs_to_empty;
    gint64   secs_to_full;
    gdouble  health;            /* 0 = unknown, else % of design capacity */
} PwrInfo;

static PwrInfo  g_pwr = { FALSE, 100.0, PWR_STATE_UNKNOWN, 0, 0, 0.0 };
static gboolean g_pwr_upower = FALSE;       /* UPower answered at least once */
static gboolean g_pwr_warned_low = FALSE;
static gboolean g_pwr_warned_crit = FALSE;

static void power_render(SystrayWidget *tray);

/* ---- sysfs fallback ---- */
static gchar *pwr_read_str(const gchar *dir, const gchar *file) {
    gchar *p = g_build_filename(dir, file, NULL);
    gchar *buf = NULL;
    if (!g_file_get_contents(p, &buf, NULL, NULL)) buf = NULL;
    g_free(p);
    if (buf) g_strstrip(buf);
    return buf;
}

static gint64 pwr_read_num(const gchar *dir, const gchar *file, gint64 dflt) {
    gchar *s = pwr_read_str(dir, file);
    gint64 v = dflt;
    if (s && s[0]) v = g_ascii_strtoll(s, NULL, 10);
    g_free(s);
    return v;
}

static void power_read_sysfs(PwrInfo *out) {
    PwrInfo info = { FALSE, 100.0, PWR_STATE_UNKNOWN, 0, 0, 0.0 };
    GDir *dir = g_dir_open("/sys/class/power_supply", 0, NULL);
    if (!dir) { *out = info; return; }

    gboolean ac_online = FALSE;
    gint64 sum_now = 0, sum_full = 0, sum_design = 0, sum_power = 0;
    gdouble cap_sum = 0; gint cap_n = 0;
    gboolean any_charging = FALSE, any_discharging = FALSE, any_not_charging = FALSE, all_full = TRUE;

    const gchar *entry;
    while ((entry = g_dir_read_name(dir)) != NULL) {
        gchar *d = g_build_filename("/sys/class/power_supply", entry, NULL);
        gchar *type  = pwr_read_str(d, "type");
        gchar *scope = pwr_read_str(d, "scope");

        if (type && (g_ascii_strcasecmp(type, "Mains") == 0 || g_ascii_strncasecmp(type, "USB", 3) == 0)) {
            if (pwr_read_num(d, "online", 0) == 1) ac_online = TRUE;
        } else if (type && g_ascii_strcasecmp(type, "Battery") == 0 &&
                   !(scope && g_ascii_strcasecmp(scope, "Device") == 0)) {   /* skip mouse/keyboard batteries */
            if (pwr_read_num(d, "present", 1) == 1) {
                info.has_battery = TRUE;
                /* energy_* (µWh) or charge_* (µAh) — both work for ratios */
                gint64 now  = pwr_read_num(d, "energy_now", -1);
                gint64 full = pwr_read_num(d, "energy_full", -1);
                gint64 des  = pwr_read_num(d, "energy_full_design", -1);
                gint64 pw   = pwr_read_num(d, "power_now", -1);
                if (now < 0 || full <= 0) {
                    now  = pwr_read_num(d, "charge_now", -1);
                    full = pwr_read_num(d, "charge_full", -1);
                    des  = pwr_read_num(d, "charge_full_design", -1);
                    pw   = pwr_read_num(d, "current_now", -1);
                }
                if (now >= 0 && full > 0) {
                    sum_now += now; sum_full += full;
                    if (des > 0) sum_design += des;
                    if (pw > 0) sum_power += pw;
                }
                gint64 cap = pwr_read_num(d, "capacity", -1);
                if (cap >= 0) { cap_sum += CLAMP(cap, 0, 100); cap_n++; }

                gchar *st = pwr_read_str(d, "status");
                if (st) {
                    if (g_ascii_strcasecmp(st, "Charging") == 0) { any_charging = TRUE; all_full = FALSE; }
                    else if (g_ascii_strcasecmp(st, "Discharging") == 0) { any_discharging = TRUE; all_full = FALSE; }
                    else if (g_ascii_strcasecmp(st, "Not charging") == 0) { any_not_charging = TRUE; all_full = FALSE; }
                    else if (g_ascii_strcasecmp(st, "Full") != 0) all_full = FALSE;
                }
                g_free(st);
            }
        }
        g_free(type); g_free(scope); g_free(d);
    }
    g_dir_close(dir);

    if (info.has_battery) {
        if (sum_full > 0) info.percent = CLAMP(100.0 * (gdouble)sum_now / (gdouble)sum_full, 0.0, 100.0);
        else if (cap_n > 0) info.percent = cap_sum / cap_n;
        if (sum_design > 0 && sum_full > 0) info.health = CLAMP(100.0 * (gdouble)sum_full / (gdouble)sum_design, 0.0, 100.0);

        if (any_charging) info.state = PWR_STATE_CHARGING;
        else if (any_discharging && !ac_online) info.state = PWR_STATE_DISCHARGING;
        else if (all_full) info.state = PWR_STATE_FULL;
        else if (any_not_charging || ac_online) info.state = PWR_STATE_NOT_CHARGING;
        else info.state = PWR_STATE_DISCHARGING;

        if (sum_power > 0 && sum_full > 0) {
            if (info.state == PWR_STATE_DISCHARGING)
                info.secs_to_empty = (gint64)(3600.0 * (gdouble)sum_now / (gdouble)sum_power);
            else if (info.state == PWR_STATE_CHARGING)
                info.secs_to_full = (gint64)(3600.0 * (gdouble)(sum_full - sum_now) / (gdouble)sum_power);
        }
    }
    *out = info;
}

static gboolean on_pwr_sysfs_tick(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    if (g_pwr_upower) { tray->pwr_sysfs_timer = 0; return G_SOURCE_REMOVE; }
    power_read_sysfs(&g_pwr);
    power_render(tray);
    return G_SOURCE_CONTINUE;
}

/* ---- UPower ---- */
static void pwr_apply_upower_props(GVariant *dict) {
    gboolean present = g_pwr.has_battery;
    guint32 type = 0, state = 0;
    gdouble pct = g_pwr.percent;
    gint64 tte = g_pwr.secs_to_empty, ttf = g_pwr.secs_to_full;

    if (g_variant_lookup(dict, "Type", "u", &type)) { /* 2 = battery */ }
    g_variant_lookup(dict, "IsPresent", "b", &present);
    g_variant_lookup(dict, "Percentage", "d", &pct);
    g_variant_lookup(dict, "TimeToEmpty", "x", &tte);
    g_variant_lookup(dict, "TimeToFull", "x", &ttf);
    if (g_variant_lookup(dict, "State", "u", &state)) {
        switch (state) {
            case 1:  g_pwr.state = PWR_STATE_CHARGING;     break;
            case 2:
            case 3:
            case 6:  g_pwr.state = PWR_STATE_DISCHARGING;  break;
            case 4:  g_pwr.state = PWR_STATE_FULL;         break;
            case 5:  g_pwr.state = PWR_STATE_NOT_CHARGING; break;
            default: g_pwr.state = PWR_STATE_UNKNOWN;      break;
        }
    }
    g_pwr.has_battery   = present;
    g_pwr.percent       = CLAMP(pct, 0.0, 100.0);
    g_pwr.secs_to_empty = MAX(tte, 0);
    g_pwr.secs_to_full  = MAX(ttf, 0);
}

/* Health = sum(energy_full) / sum(energy_full_design) of laptop batteries; read once via sysfs */
static void pwr_update_health_from_sysfs(void) {
    PwrInfo tmp;
    power_read_sysfs(&tmp);
    g_pwr.health = tmp.health;
}

static void on_upower_display_props(GObject *src, GAsyncResult *res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        /* No UPower: fall back to sysfs, re-read every 30 s */
        g_clear_error(&err);
        g_pwr_upower = FALSE;
        power_read_sysfs(&g_pwr);
        if (!tray->pwr_sysfs_timer) tray->pwr_sysfs_timer = g_timeout_add_seconds(30, on_pwr_sysfs_tick, tray);
        power_render(tray);
        return;
    }
    GVariant *dict = NULL;
    g_variant_get(r, "(@a{sv})", &dict);
    g_pwr_upower = TRUE;
    pwr_apply_upower_props(dict);
    pwr_update_health_from_sysfs();
    g_variant_unref(dict);
    g_variant_unref(r);
    power_render(tray);
}

static void power_query_upower(SystrayWidget *tray) {
    if (!tray->sys_bus) return;
    g_dbus_connection_call(tray->sys_bus, "org.freedesktop.UPower",
        "/org/freedesktop/UPower/devices/DisplayDevice",
        "org.freedesktop.DBus.Properties", "GetAll",
        g_variant_new("(s)", "org.freedesktop.UPower.Device"),
        G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL,
        on_upower_display_props, tray);
}

static void on_upower_props_changed(GDBusConnection *conn, const gchar *sender, const gchar *path,
                                    const gchar *iface, const gchar *signal, GVariant *params, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    const gchar *changed_iface = NULL;
    GVariant *changed = NULL;
    g_variant_get(params, "(&s@a{sv}@as)", &changed_iface, &changed, NULL);
    if (g_strcmp0(changed_iface, "org.freedesktop.UPower.Device") == 0) {
        g_pwr_upower = TRUE;
        pwr_apply_upower_props(changed);
        power_render(tray);
    }
    g_variant_unref(changed);
}

/* ---- low / critical battery warnings ---- */
static void pwr_notify(SystrayWidget *tray, const gchar *summary, const gchar *body, gboolean critical) {
    if (!tray->session_bus) return;
    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(critical ? 2 : 1));
    g_dbus_connection_call(tray->session_bus, "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", "Ultimate Shell", (guint32)0,
                      critical ? "battery-caution-symbolic" : "battery-low-symbolic",
                      summary, body, &actions, &hints, (gint32)(critical ? 0 : 10000)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
}

static gboolean pwr_xfpm_running(SystrayWidget *tray) {
    if (!tray->session_bus) return FALSE;
    GVariant *r = g_dbus_connection_call_sync(tray->session_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "NameHasOwner", g_variant_new("(s)", "org.xfce.PowerManager"),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    gboolean has = FALSE;
    if (r) { g_variant_get(r, "(b)", &has); g_variant_unref(r); }
    return has;
}

static void pwr_check_warnings(SystrayWidget *tray) {
    if (!g_pwr.has_battery || g_pwr.state != PWR_STATE_DISCHARGING) {
        g_pwr_warned_low = g_pwr_warned_crit = FALSE;   /* re-arm once charging */
        return;
    }
    gint pct = (gint)(g_pwr.percent + 0.5);
    if (pct <= 5 && !g_pwr_warned_crit) {
        g_pwr_warned_crit = g_pwr_warned_low = TRUE;
        if (!pwr_xfpm_running(tray)) {
            gchar *body = g_strdup_printf("Battery at %d%%. Plug in now or save your work — the computer will shut down soon.", pct);
            pwr_notify(tray, "Battery critically low", body, TRUE);
            g_free(body);
        }
    } else if (pct <= 10 && !g_pwr_warned_low) {
        g_pwr_warned_low = TRUE;
        if (!pwr_xfpm_running(tray)) {
            gchar *body = g_strdup_printf("Battery at %d%%. Plug in your charger soon.", pct);
            pwr_notify(tray, "Battery low", body, FALSE);
            g_free(body);
        }
    }
}

/* ---- power profiles (power-profiles-daemon) ---- */
static void on_pwr_profile_clicked(GtkToggleButton *btn, SystrayWidget *tray) {
    if (tray->pwr_updating || !gtk_toggle_button_get_active(btn) || !tray->sys_bus) return;
    const gchar *prof = g_object_get_data(G_OBJECT(btn), "pwr-profile");
    const gchar *svc  = tray->pwr_ppd_service ? tray->pwr_ppd_service : "net.hadess.PowerProfiles";
    const gchar *path = tray->pwr_ppd_path ? tray->pwr_ppd_path : "/net/hadess/PowerProfiles";
    const gchar *ifc  = tray->pwr_ppd_iface ? tray->pwr_ppd_iface : "net.hadess.PowerProfiles";
    g_dbus_connection_call(tray->sys_bus, svc, path, "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", ifc, "ActiveProfile", g_variant_new_string(prof)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
}

static void pwr_show_profile(SystrayWidget *tray, const gchar *active) {
    tray->pwr_updating = TRUE;
    for (gint i = 0; i < 3; i++) {
        GtkWidget *b = tray->pwr_profile_btns[i];
        if (!GTK_IS_TOGGLE_BUTTON(b)) continue;
        const gchar *p = g_object_get_data(G_OBJECT(b), "pwr-profile");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(b), g_strcmp0(p, active) == 0);
    }
    tray->pwr_updating = FALSE;
}

static void on_ppd_props(GObject *src, GAsyncResult *res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!r) {
        g_clear_error(&err);
        if (!tray->pwr_ppd_tried_new) {
            /* Newer power-profiles-daemon name */
            tray->pwr_ppd_tried_new = TRUE;
            g_free(tray->pwr_ppd_service); tray->pwr_ppd_service = g_strdup("org.freedesktop.UPower.PowerProfiles");
            g_free(tray->pwr_ppd_path);    tray->pwr_ppd_path    = g_strdup("/org/freedesktop/UPower/PowerProfiles");
            g_free(tray->pwr_ppd_iface);   tray->pwr_ppd_iface   = g_strdup("org.freedesktop.UPower.PowerProfiles");
            g_dbus_connection_call(tray->sys_bus, tray->pwr_ppd_service, tray->pwr_ppd_path,
                "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", tray->pwr_ppd_iface, "ActiveProfile"),
                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_ppd_props, tray);
            return;
        }
        if (tray->pwr_profile_box) gtk_widget_hide(tray->pwr_profile_box);   /* service not installed */
        return;
    }
    GVariant *v = NULL;
    g_variant_get(r, "(v)", &v);
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) {
        if (tray->pwr_profile_box) gtk_widget_show_all(tray->pwr_profile_box);
        pwr_show_profile(tray, g_variant_get_string(v, NULL));
    }
    if (v) g_variant_unref(v);
    g_variant_unref(r);
}

static void power_query_profile(SystrayWidget *tray) {
    if (!tray->sys_bus) return;
    if (!tray->pwr_ppd_service) {
        tray->pwr_ppd_service = g_strdup("net.hadess.PowerProfiles");
        tray->pwr_ppd_path    = g_strdup("/net/hadess/PowerProfiles");
        tray->pwr_ppd_iface   = g_strdup("net.hadess.PowerProfiles");
        tray->pwr_ppd_tried_new = FALSE;
    }
    g_dbus_connection_call(tray->sys_bus, tray->pwr_ppd_service, tray->pwr_ppd_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", tray->pwr_ppd_iface, "ActiveProfile"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_ppd_props, tray);
}

/* ---- screen brightness (sysfs read, logind SetBrightness write: no root needed) ---- */
static gchar *pwr_find_backlight(void) {
    GDir *dir = g_dir_open("/sys/class/backlight", 0, NULL);
    if (!dir) return NULL;
    gchar *best = NULL;
    gint best_rank = -1;
    const gchar *e;
    while ((e = g_dir_read_name(dir)) != NULL) {
        gchar *d = g_build_filename("/sys/class/backlight", e, NULL);
        gchar *t = pwr_read_str(d, "type");
        gint rank = (t && g_strcmp0(t, "firmware") == 0) ? 3 : (t && g_strcmp0(t, "platform") == 0) ? 2 : 1;
        if (rank > best_rank) { g_free(best); best = g_strdup(e); best_rank = rank; }
        g_free(t); g_free(d);
    }
    g_dir_close(dir);
    return best;
}

static void pwr_sync_brightness_slider(SystrayWidget *tray) {
    if (!GTK_IS_RANGE(tray->pwr_bright_scale) || !tray->pwr_backlight) return;
    gchar *d = g_build_filename("/sys/class/backlight", tray->pwr_backlight, NULL);
    gint64 cur = pwr_read_num(d, "brightness", -1);
    gint64 max = pwr_read_num(d, "max_brightness", -1);
    g_free(d);
    if (cur < 0 || max <= 0) return;
    tray->pwr_updating = TRUE;
    gtk_range_set_value(GTK_RANGE(tray->pwr_bright_scale), 100.0 * (gdouble)cur / (gdouble)max);
    tray->pwr_updating = FALSE;
}

static void on_pwr_brightness_changed(GtkRange *range, SystrayWidget *tray) {
    if (tray->pwr_updating || !tray->pwr_backlight || !tray->sys_bus) return;
    gchar *d = g_build_filename("/sys/class/backlight", tray->pwr_backlight, NULL);
    gint64 max = pwr_read_num(d, "max_brightness", -1);
    g_free(d);
    if (max <= 0) return;
    gdouble pct = CLAMP(gtk_range_get_value(range), 1.0, 100.0);   /* never fully black */
    guint32 val = (guint32)MAX(1, (gint64)(pct * (gdouble)max / 100.0 + 0.5));
    g_dbus_connection_call(tray->sys_bus, "org.freedesktop.login1", "/org/freedesktop/login1/session/auto",
        "org.freedesktop.login1.Session", "SetBrightness",
        g_variant_new("(ssu)", "backlight", tray->pwr_backlight, val),
        NULL, G_DBUS_CALL_FLAGS_NONE, 2000, NULL, NULL, NULL);
}

/* ---- rendering: icon, tooltip, flyout, auto-hide ---- */
static gchar *pwr_format_time(gint64 secs) {
    if (secs <= 0) return NULL;
    gint64 mins = (secs + 30) / 60;
    if (mins < 60) return g_strdup_printf("%" G_GINT64_FORMAT " min", mins);
    return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", mins / 60, mins % 60);
}

static gboolean pwr_battery_always_show(SystrayWidget *tray) {
    return (tray->shell && tray->shell->channel)
        ? xfconf_channel_get_bool(tray->shell->channel, "/plugins/ultimateshell/tray_battery_always_show", FALSE)
        : FALSE;
}

static void power_render(SystrayWidget *tray) {
    if (!tray || !tray->bat_item || !GTK_IS_IMAGE(tray->bat_item->image)) return;

    /* Auto-hide on computers without a battery (unless the user wants it shown) */
    gboolean present = g_pwr.has_battery || pwr_battery_always_show(tray);
    if (present != tray->bat_present) {
        tray->bat_present = present;
        relayout_tray_containers(tray);
    }

    gint pct = (gint)(g_pwr.percent + 0.5);
    const gchar *icon;
    gchar *line1 = NULL;   /* "73%  •  Charging" */
    gchar *line2 = NULL;   /* "1 h 20 min until full" */

    if (!g_pwr.has_battery) {
        icon  = "ac-adapter-symbolic";
        line1 = g_strdup("Plugged in (no battery)");
    } else {
        const gchar *lvl = pct >= 90 ? "full" : pct >= 50 ? "good" : pct >= 20 ? "low" : "caution";
        gchar *t = NULL;
        switch (g_pwr.state) {
            case PWR_STATE_CHARGING:
                icon  = pct >= 90 ? "battery-full-charging-symbolic" : pct >= 50 ? "battery-good-charging-symbolic"
                      : pct >= 20 ? "battery-low-charging-symbolic" : "battery-caution-charging-symbolic";
                line1 = g_strdup_printf("%d%%  •  Charging", pct);
                t = pwr_format_time(g_pwr.secs_to_full);
                if (t) line2 = g_strdup_printf("%s until full", t);
                break;
            case PWR_STATE_FULL:
                icon  = "battery-full-charged-symbolic";
                line1 = g_strdup_printf("%d%%  •  Fully charged", pct);
                break;
            case PWR_STATE_NOT_CHARGING:
                icon  = pct >= 90 ? "battery-full-charged-symbolic"
                      : pct >= 50 ? "battery-good-symbolic" : "battery-low-symbolic";
                line1 = g_strdup_printf("%d%%  •  Plugged in, not charging", pct);
                break;
            default: {
                static gchar ibuf[64];
                g_snprintf(ibuf, sizeof ibuf, "battery-%s-symbolic", lvl);
                icon  = ibuf;
                line1 = g_strdup_printf("%d%%  •  On battery", pct);
                t = pwr_format_time(g_pwr.secs_to_empty);
                if (t) line2 = g_strdup_printf("%s remaining", t);
                break;
            }
        }
        g_free(t);
    }

    gint icon_px = tray->shell ? CLAMP(tray->shell->tray_icon_size, 14, 28) : 16;
    gtk_image_set_from_icon_name(GTK_IMAGE(tray->bat_item->image), icon, GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(tray->bat_item->image), icon_px);

    gchar *tip = line2 ? g_strdup_printf("Battery: %s\n%s", line1, line2) : g_strdup_printf("Battery: %s", line1);
    gtk_widget_set_tooltip_text(tray->bat_item->button, tip);
    g_free(tip);

    if (GTK_IS_LABEL(tray->bat_status_lbl)) {
        gchar *mk = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", line1);
        gtk_label_set_markup(GTK_LABEL(tray->bat_status_lbl), mk);
        g_free(mk);
    }
    if (GTK_IS_LABEL(tray->bat_detail_lbl)) {
        GString *det = g_string_new(line2 ? line2 : "");
        if (g_pwr.health > 0.0) {
            if (det->len) g_string_append(det, "\n");
            g_string_append_printf(det, "Battery health: %d%%", (gint)(g_pwr.health + 0.5));
        }
        gtk_label_set_text(GTK_LABEL(tray->bat_detail_lbl), det->str);
        gtk_widget_set_visible(tray->bat_detail_lbl, det->len > 0);
        g_string_free(det, TRUE);
    }

    g_free(line1);
    g_free(line2);
    pwr_check_warnings(tray);
}

/* One-time setup: UPower (live) or sysfs fallback */
static void power_setup(SystrayWidget *tray) {
    if (!tray->sys_bus) tray->sys_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!tray->sys_bus) {
        power_read_sysfs(&g_pwr);
        tray->pwr_sysfs_timer = g_timeout_add_seconds(30, on_pwr_sysfs_tick, tray);
        power_render(tray);
        return;
    }
    g_dbus_connection_signal_subscribe(tray->sys_bus, "org.freedesktop.UPower",
        "org.freedesktop.DBus.Properties", "PropertiesChanged",
        "/org/freedesktop/UPower/devices/DisplayDevice", NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_upower_props_changed, tray, NULL);
    power_query_upower(tray);
}

static void on_bat_toggled(GtkToggleButton *btn, SystrayWidget *tray) {
    if (qs_enabled(tray)) { if (gtk_toggle_button_get_active(btn)) qs_on_tray_click(tray, btn); return; }
    if (gtk_toggle_button_get_active(btn)) {
        if (g_pwr_upower) power_query_upower(tray); else { power_read_sysfs(&g_pwr); power_render(tray); }
        power_query_profile(tray);
        pwr_sync_brightness_slider(tray);
    }
    shell_toggle_popup_window(btn, tray->bat_popup);
}

/* =========================================================================
 * Audio core: cached state, event-driven via `pactl subscribe`, read in the
 * background (never blocks the panel). Falls back to a 3 s background poll
 * when `pactl subscribe` isn't available.
 * ========================================================================= */
#define PA_SCRIPT \
    "LC_ALL=C; export LC_ALL; " \
    "echo '@@ULT_VOL'; pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null || amixer get Master 2>/dev/null; " \
    "echo '@@ULT_MUTE'; pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null || amixer get Master 2>/dev/null; " \
    "echo '@@ULT_MICVOL'; pactl get-source-volume @DEFAULT_SOURCE@ 2>/dev/null; " \
    "echo '@@ULT_MICMUTE'; pactl get-source-mute @DEFAULT_SOURCE@ 2>/dev/null; " \
    "echo '@@ULT_DEFSINK'; pactl get-default-sink 2>/dev/null; " \
    "echo '@@ULT_DEFSRC'; pactl get-default-source 2>/dev/null; " \
    "echo '@@ULT_SINKS'; pactl list sinks 2>/dev/null; " \
    "echo '@@ULT_SOURCES'; pactl list sources 2>/dev/null; " \
    "echo '@@ULT_INPUTS'; pactl list sink-inputs 2>/dev/null; " \
    "echo '@@ULT_RECS'; pactl list source-outputs 2>/dev/null"

static void pa_refresh_async(SystrayWidget *tray);

static gint pa_parse_percent(const gchar *txt, gint dflt) {
    if (!txt) return dflt;
    const char *pct = strchr(txt, '%');
    if (!pct) return dflt;
    const char *p = pct;
    while (p > txt && *(p - 1) >= '0' && *(p - 1) <= '9') p--;
    if (p == pct) return dflt;
    return CLAMP(atoi(p), 0, 200);
}

static gchar *pa_section(const gchar *out, const gchar *name) {
    gchar *marker = g_strdup_printf("@@ULT_%s\n", name);
    const gchar *a = strstr(out, marker);
    gchar *res = NULL;
    if (a) {
        a += strlen(marker);
        const gchar *b = strstr(a, "@@ULT_");
        res = b ? g_strndup(a, b - a) : g_strdup(a);
        g_strstrip(res);
    }
    g_free(marker);
    return res ? res : g_strdup("");
}

/* Run a small shell script with ONE argument passed as $1 (never parsed by the shell). */
static void pa_run(const gchar *script, const gchar *arg) {
    gchar *argv[] = { "sh", "-c", (gchar *)script, "sh", (gchar *)(arg ? arg : ""), NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static void pa_set_sink_volume(gint pct) {
    gchar *v = g_strdup_printf("%d", MAX(pct, 0));
    pa_run("pactl set-sink-volume @DEFAULT_SINK@ \"$1%\" 2>/dev/null || amixer -q set Master \"$1%\"", v);
    g_free(v);
}
static void pa_set_sink_mute(gboolean mute) {
    pa_run("pactl set-sink-mute @DEFAULT_SINK@ \"$1\" 2>/dev/null || "
           "{ if [ \"$1\" = 1 ]; then amixer -q set Master mute; else amixer -q set Master unmute; fi; }",
           mute ? "1" : "0");
}
static void pa_set_source_volume(gint pct) {
    gchar *v = g_strdup_printf("%d", MAX(pct, 0));
    pa_run("pactl set-source-volume @DEFAULT_SOURCE@ \"$1%\"", v);
    g_free(v);
}
static void pa_set_source_mute(gboolean mute) {
    pa_run("pactl set-source-mute @DEFAULT_SOURCE@ \"$1\"", mute ? "1" : "0");
}

static gint pa_max_volume(SystrayWidget *tray) {
    return (tray->shell && tray->shell->audio_max_vol > 0) ? tray->shell->audio_max_vol : 150;
}
static gint pa_step(SystrayWidget *tray) {
    return (tray->shell && tray->shell->audio_vol_step > 0) ? tray->shell->audio_vol_step : 5;
}

/* ---- on-screen volume notification (follows "Show volume notifications") ---- */
static void on_pa_notify_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (r) { g_variant_get(r, "(u)", &tray->pa_notif_id); g_variant_unref(r); }
}

static void pa_show_osd(SystrayWidget *tray, gboolean is_mic) {
    if (!tray->session_bus || !tray->shell) return;
    gint mode = tray->shell->audio_notif_mode;      /* 0 All, 1 Output only, 2 Input only, 3 None */
    if (mode == 3 || (mode == 1 && is_mic) || (mode == 2 && !is_mic)) return;

    gint pct      = is_mic ? tray->pa_mic : tray->pa_vol;
    gboolean mute = is_mic ? tray->pa_mic_muted : tray->pa_muted;
    const gchar *icon = is_mic
        ? (mute || pct == 0 ? "microphone-sensitivity-muted-symbolic"
           : pct < 35 ? "microphone-sensitivity-low-symbolic"
           : pct < 70 ? "microphone-sensitivity-medium-symbolic" : "microphone-sensitivity-high-symbolic")
        : (mute || pct == 0 ? "audio-volume-muted-symbolic"
           : pct < 35 ? "audio-volume-low-symbolic"
           : pct < 70 ? "audio-volume-medium-symbolic" : "audio-volume-high-symbolic");
    gchar *summary = mute ? g_strdup(is_mic ? "Microphone muted" : "Muted")
                          : g_strdup_printf("%s %d%%", is_mic ? "Microphone" : "Volume", pct);

    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "value", g_variant_new_int32(mute ? 0 : MIN(pct, 100)));
    g_variant_builder_add(&hints, "{sv}", "transient", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&hints, "{sv}", "x-canonical-private-synchronous", g_variant_new_string("volume"));
    g_dbus_connection_call(tray->session_bus, "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
        g_variant_new("(susssasa{sv}i)", "Ultimate Shell", tray->pa_notif_id, icon, summary, "",
                      &actions, &hints, (gint32)1500),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, on_pa_notify_done, tray);
    g_free(summary);
}

/* ---- background refresh ---- */
static void ap_on_pa_state(SystrayWidget *tray);
static gchar *pa_prop_value(const gchar *line);

static void on_pa_refresh_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    gchar *out = NULL;
    GError *err = NULL;
    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(src), res, &out, NULL, &err);
    g_clear_error(&err);
    g_object_unref(src);
    tray->pa_refreshing = FALSE;

    if (out) {
        gchar *s_vol = pa_section(out, "VOL"),      *s_mute = pa_section(out, "MUTE");
        gchar *s_mv  = pa_section(out, "MICVOL"),   *s_mm   = pa_section(out, "MICMUTE");
        gchar *s_rec = pa_section(out, "RECS");

        gint old_vol = tray->pa_vol, old_mic = tray->pa_mic;
        gboolean old_m = tray->pa_muted, old_mm = tray->pa_mic_muted;

        /* While the user drags a slider, keep the slider's value as the truth */
        gboolean dragging = (g_get_monotonic_time() < tray->pa_ui_change_until);
        if (!dragging) {
            tray->pa_vol       = pa_parse_percent(s_vol, tray->pa_vol);
            tray->pa_muted     = (strstr(s_mute, "yes") != NULL || strstr(s_mute, "[off]") != NULL);
            tray->pa_mic       = pa_parse_percent(s_mv, tray->pa_mic);
            tray->pa_mic_muted = (strstr(s_mm, "yes") != NULL);
        }

        /* apps using the microphone (monitor sources / peak meters are not recording) */
        gint recs = 0;
        GString *names = g_string_new("");
        gchar **blocks = g_strsplit(s_rec, "Source Output #", -1);
        for (gint i = 1; blocks && blocks[i]; i++) {
            gchar *nm = NULL; gboolean peak = FALSE;
            gchar **lines = g_strsplit(blocks[i], "\n", -1);
            for (gint l = 0; lines && lines[l]; l++) {
                gchar *t = g_strstrip(g_strdup(lines[l]));
                if (g_str_has_prefix(t, "application.name =") && !nm) nm = pa_prop_value(t);
                if (strstr(t, "PEAK_DETECT")) peak = TRUE;
                g_free(t);
            }
            g_strfreev(lines);
            if (!peak) {
                recs++;
                if (nm && !strstr(names->str, nm)) {
                    if (names->len) g_string_append(names, ", ");
                    g_string_append(names, nm);
                }
            }
            g_free(nm);
        }
        g_strfreev(blocks);
        tray->pa_recording = (recs > 0);
        g_free(tray->pa_rec_apps);
        tray->pa_rec_apps = g_string_free(names, FALSE);

        g_free(tray->pa_def_sink);     tray->pa_def_sink    = pa_section(out, "DEFSINK");
        g_free(tray->pa_def_source);   tray->pa_def_source  = pa_section(out, "DEFSRC");
        g_free(tray->pa_txt_sinks);    tray->pa_txt_sinks   = pa_section(out, "SINKS");
        g_free(tray->pa_txt_sources);  tray->pa_txt_sources = pa_section(out, "SOURCES");
        g_free(tray->pa_txt_inputs);   tray->pa_txt_inputs  = pa_section(out, "INPUTS");

        /* OSD for changes made outside the sliders (keys, scrolling, other apps) */
        if (tray->pa_have_state && !dragging) {
            if (tray->pa_vol != old_vol || tray->pa_muted != old_m) pa_show_osd(tray, FALSE);
            else if (tray->pa_mic != old_mic || tray->pa_mic_muted != old_mm) pa_show_osd(tray, TRUE);
        }
        tray->pa_have_state = TRUE;

        g_free(s_vol); g_free(s_mute); g_free(s_mv); g_free(s_mm); g_free(s_rec);
        g_free(out);
        update_network_and_audio(tray);
        ap_on_pa_state(tray);
    }

    if (tray->pa_refresh_again) {
        tray->pa_refresh_again = FALSE;
        pa_refresh_async(tray);
    }
}

static void pa_refresh_async(SystrayWidget *tray) {
    if (tray->pa_refreshing) { tray->pa_refresh_again = TRUE; return; }
    GError *err = NULL;
    GSubprocess *sp = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                       &err, "sh", "-c", PA_SCRIPT, NULL);
    if (!sp) { g_clear_error(&err); return; }
    tray->pa_refreshing = TRUE;
    g_subprocess_communicate_utf8_async(sp, NULL, NULL, on_pa_refresh_done, tray);
}

static gboolean on_pa_refresh_debounce(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    tray->pa_refresh_id = 0;
    pa_refresh_async(tray);
    return G_SOURCE_REMOVE;
}

static void pa_schedule_refresh(SystrayWidget *tray) {
    if (!tray->pa_refresh_id) tray->pa_refresh_id = g_timeout_add(100, on_pa_refresh_debounce, tray);
}

/* ---- `pactl subscribe`: one long-lived process telling us when anything changes ---- */
static void pa_subscribe_start(SystrayWidget *tray);

static gboolean on_pa_sub_retry(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    tray->pa_sub_retry_id = 0;
    pa_subscribe_start(tray);
    return G_SOURCE_REMOVE;
}

static void on_pa_sub_line(GObject *src, GAsyncResult *res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    gsize len = 0;
    GError *err = NULL;
    gchar *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (!line) {
        /* EOF / error: sound server restarted or pactl missing. Retry in 5 s, poll meanwhile. */
        g_clear_error(&err);
        tray->pa_sub_alive = FALSE;
        g_clear_object(&tray->pa_sub_in);
        if (tray->pa_sub) { g_subprocess_force_exit(tray->pa_sub); g_clear_object(&tray->pa_sub); }
        if (!tray->pa_sub_retry_id) tray->pa_sub_retry_id = g_timeout_add_seconds(5, on_pa_sub_retry, tray);
        pa_schedule_refresh(tray);
        return;
    }
    tray->pa_sub_alive = TRUE;
    if (strstr(line, " on sink") || strstr(line, " on source") || strstr(line, " on server") ||
        strstr(line, " on card")) {
        pa_schedule_refresh(tray);
    }
    g_free(line);
    g_data_input_stream_read_line_async(tray->pa_sub_in, G_PRIORITY_DEFAULT, NULL, on_pa_sub_line, tray);
}

static void pa_subscribe_start(SystrayWidget *tray) {
    if (tray->pa_sub) return;
    GSubprocessLauncher *l = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    g_subprocess_launcher_setenv(l, "LC_ALL", "C", TRUE);
    GError *err = NULL;
    tray->pa_sub = g_subprocess_launcher_spawn(l, &err, "pactl", "subscribe", NULL);
    g_object_unref(l);
    if (!tray->pa_sub) {
        g_clear_error(&err);
        tray->pa_sub_alive = FALSE;
        if (!tray->pa_sub_retry_id) tray->pa_sub_retry_id = g_timeout_add_seconds(30, on_pa_sub_retry, tray);
        return;
    }
    tray->pa_sub_alive = TRUE;
    tray->pa_sub_in = g_data_input_stream_new(g_subprocess_get_stdout_pipe(tray->pa_sub));
    g_data_input_stream_read_line_async(tray->pa_sub_in, G_PRIORITY_DEFAULT, NULL, on_pa_sub_line, tray);
    pa_refresh_async(tray);
}

/* Cached `pactl` output for the device menus (falls back to a direct call the very first time) */
static gchar *pa_cached_output(const gchar *cmd) {
    SystrayWidget *t = g_systray;
    const gchar *c = NULL;
    if (t && t->pa_have_state) {
        if (strstr(cmd, "list sinks"))              c = t->pa_txt_sinks;
        else if (strstr(cmd, "list sources"))       c = t->pa_txt_sources;
        else if (strstr(cmd, "get-default-sink"))   c = t->pa_def_sink;
        else if (strstr(cmd, "get-default-source")) c = t->pa_def_source;
    }
    if (c) return g_strdup(c);
    gchar *out = NULL;
    if (!g_spawn_command_line_sync(cmd, &out, NULL, NULL, NULL)) out = NULL;
    return out;
}

/* ---- throttled slider writes: at most one pactl every 60 ms while dragging ---- */
static gboolean on_pa_vol_throttle(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    if (tray->pa_pending_vol != tray->pa_sent_vol) {
        tray->pa_sent_vol = tray->pa_pending_vol;
        pa_set_sink_volume(tray->pa_sent_vol);
        return G_SOURCE_CONTINUE;
    }
    tray->pa_vol_throttle_id = 0;
    return G_SOURCE_REMOVE;
}

static gboolean on_pa_mic_throttle(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    if (tray->pa_pending_mic != tray->pa_sent_mic) {
        tray->pa_sent_mic = tray->pa_pending_mic;
        pa_set_source_volume(tray->pa_sent_mic);
        return G_SOURCE_CONTINUE;
    }
    tray->pa_mic_throttle_id = 0;
    return G_SOURCE_REMOVE;
}

static void pa_queue_sink_volume(SystrayWidget *tray, gint val) {
    tray->pa_vol = val;
    tray->pa_pending_vol = val;
    tray->pa_ui_change_until = g_get_monotonic_time() + 1500000;   /* slider is the truth for 1.5 s */
    if (!tray->pa_vol_throttle_id) {
        tray->pa_sent_vol = val;
        pa_set_sink_volume(val);
        tray->pa_vol_throttle_id = g_timeout_add(60, on_pa_vol_throttle, tray);
    }
}

static void pa_queue_source_volume(SystrayWidget *tray, gint val) {
    tray->pa_mic = val;
    tray->pa_pending_mic = val;
    tray->pa_ui_change_until = g_get_monotonic_time() + 1500000;
    if (!tray->pa_mic_throttle_id) {
        tray->pa_sent_mic = val;
        pa_set_source_volume(val);
        tray->pa_mic_throttle_id = g_timeout_add(60, on_pa_mic_throttle, tray);
    }
}

/* ---- step volume (scroll / keys): instant, never blocks, shows the OSD via the refresh ---- */
static void pa_step_volume(SystrayWidget *tray, gint direction) {
    gint nv = CLAMP(tray->pa_vol + direction * pa_step(tray), 0, pa_max_volume(tray));
    if (nv == tray->pa_vol && !tray->pa_muted) return;
    tray->pa_vol = nv;
    pa_set_sink_volume(nv);
    if (tray->pa_muted && direction > 0) { tray->pa_muted = FALSE; pa_set_sink_mute(FALSE); }
    update_network_and_audio(tray);
    pa_show_osd(tray, FALSE);   /* cache is already updated, so the refresh won't show it again */
}

/* ---- global volume keys (only if nobody else owns them, e.g. xfce4-pulseaudio-plugin) ---- */
#define ULT_XF86AudioLowerVolume 0x1008FF11
#define ULT_XF86AudioMute        0x1008FF12
#define ULT_XF86AudioRaiseVolume 0x1008FF13
#define ULT_XF86AudioMicMute     0x1008FFB2

static const unsigned int pa_key_mods[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };

static GdkFilterReturn pa_key_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data) {
    SystrayWidget *tray = (SystrayWidget *)data;
    XEvent *xev = (XEvent *)xevent;
    if (!tray || !tray->pa_keys_grabbed || xev->type != KeyPress) return GDK_FILTER_CONTINUE;
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return GDK_FILTER_CONTINUE;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    KeyCode kc = (KeyCode)xev->xkey.keycode;

    if (kc == XKeysymToKeycode(dpy, ULT_XF86AudioRaiseVolume)) { pa_step_volume(tray, +1); return GDK_FILTER_REMOVE; }
    if (kc == XKeysymToKeycode(dpy, ULT_XF86AudioLowerVolume)) { pa_step_volume(tray, -1); return GDK_FILTER_REMOVE; }
    if (kc == XKeysymToKeycode(dpy, ULT_XF86AudioMute)) {
        tray->pa_muted = !tray->pa_muted;
        pa_set_sink_mute(tray->pa_muted);
        update_network_and_audio(tray);
        pa_show_osd(tray, FALSE);
        return GDK_FILTER_REMOVE;
    }
    if (kc == XKeysymToKeycode(dpy, ULT_XF86AudioMicMute)) {
        tray->pa_mic_muted = !tray->pa_mic_muted;
        pa_set_source_mute(tray->pa_mic_muted);
        update_network_and_audio(tray);
        pa_show_osd(tray, TRUE);
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static void pa_update_key_grabs(SystrayWidget *tray) {
    if (!tray) return;
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    Window root = DefaultRootWindow(dpy);
    static const KeySym syms[] = { ULT_XF86AudioRaiseVolume, ULT_XF86AudioLowerVolume,
                                   ULT_XF86AudioMute, ULT_XF86AudioMicMute };
    gboolean want = tray->shell && tray->shell->audio_key_shortcuts;

    if (!tray->pa_key_filter_added) {
        gdk_window_add_filter(NULL, pa_key_filter, tray);
        tray->pa_key_filter_added = TRUE;
    }

    /* Always release ours first, then grab again if wanted */
    if (tray->pa_keys_grabbed) {
        gdk_x11_display_error_trap_push(gdk_dpy);
        for (guint k = 0; k < G_N_ELEMENTS(syms); k++) {
            KeyCode kc = XKeysymToKeycode(dpy, syms[k]);
            if (!kc) continue;
            for (guint m = 0; m < G_N_ELEMENTS(pa_key_mods); m++) XUngrabKey(dpy, kc, pa_key_mods[m], root);
        }
        XSync(dpy, False);
        gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
        tray->pa_keys_grabbed = FALSE;
    }
    if (!want) return;

    gboolean any = FALSE;
    for (guint k = 0; k < G_N_ELEMENTS(syms); k++) {
        KeyCode kc = XKeysymToKeycode(dpy, syms[k]);
        if (!kc) continue;
        gdk_x11_display_error_trap_push(gdk_dpy);
        for (guint m = 0; m < G_N_ELEMENTS(pa_key_mods); m++) {
            XGrabKey(dpy, kc, pa_key_mods[m], root, False, GrabModeAsync, GrabModeAsync);
        }
        XSync(dpy, False);
        if (gdk_x11_display_error_trap_pop(gdk_dpy) == 0) {
            any = TRUE;
        } else {
            /* Someone else (e.g. xfce4-pulseaudio-plugin) owns this key: leave it to them */
            gdk_x11_display_error_trap_push(gdk_dpy);
            for (guint m = 0; m < G_N_ELEMENTS(pa_key_mods); m++) XUngrabKey(dpy, kc, pa_key_mods[m], root);
            XSync(dpy, False);
            gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
        }
    }
    tray->pa_keys_grabbed = any;
}

/* ---- per-app volume (sink inputs) ---- */
typedef struct {
    gchar *id;
    gchar *name;
    gchar *icon;
    gint   vol;
    gboolean muted;
} PaAppInfo;

static void pa_app_free(gpointer p) {
    PaAppInfo *a = (PaAppInfo *)p;
    if (!a) return;
    g_free(a->id); g_free(a->name); g_free(a->icon); g_free(a);
}

static gchar *pa_prop_value(const gchar *line) {
    /* application.name = "Firefox"  ->  Firefox */
    const gchar *q1 = strchr(line, '"');
    if (!q1) return NULL;
    const gchar *q2 = strrchr(line, '"');
    if (!q2 || q2 <= q1) return NULL;
    return g_strndup(q1 + 1, q2 - q1 - 1);
}

static GList *pa_parse_apps(const gchar *txt) {
    GList *apps = NULL;
    if (!txt || !txt[0]) return NULL;
    gchar **blocks = g_strsplit(txt, "Sink Input #", -1);
    for (gint i = 1; blocks && blocks[i]; i++) {
        PaAppInfo *a = g_new0(PaAppInfo, 1);
        a->vol = 100;
        a->id = g_strdup_printf("%d", atoi(blocks[i]));
        gchar **lines = g_strsplit(blocks[i], "\n", -1);
        gchar *media = NULL, *binary = NULL;
        for (gint l = 0; lines && lines[l]; l++) {
            gchar *t = g_strstrip(g_strdup(lines[l]));
            if (g_str_has_prefix(t, "Mute:")) a->muted = (strstr(t, "yes") != NULL);
            else if (g_str_has_prefix(t, "Volume:")) a->vol = pa_parse_percent(t, a->vol);
            else if (g_str_has_prefix(t, "application.name =") && !a->name) a->name = pa_prop_value(t);
            else if (g_str_has_prefix(t, "application.icon_name =") && !a->icon) a->icon = pa_prop_value(t);
            else if (g_str_has_prefix(t, "application.process.binary =") && !binary) binary = pa_prop_value(t);
            else if (g_str_has_prefix(t, "media.name =") && !media) media = pa_prop_value(t);
            g_free(t);
        }
        g_strfreev(lines);
        if (!a->name) a->name = g_strdup(binary ? binary : (media ? media : "Application"));
        if (!a->icon) a->icon = g_strdup(binary ? binary : "audio-x-generic");
        g_free(media); g_free(binary);
        apps = g_list_append(apps, a);
    }
    g_strfreev(blocks);
    return apps;
}

/* Firefox & co. close and reopen their audio stream often (new video, ad, seek), so a stream
   number captured when the menu opened can be stale. Look up every current stream of the app
   by name right when the change is made; fall back to the remembered number. */
#define PA_APP_RESOLVE \
    "ids=$(LC_ALL=C pactl list sink-inputs | awk -v n=\"$1\" " \
    "'/^Sink Input #/{id=substr($3,2)} /application.name = /{v=$0; sub(/^[^\"]*\"/,\"\",v); sub(/\"[^\"]*$/,\"\",v); if (v==n) print id}'); " \
    "[ -z \"$ids\" ] && ids=\"$3\"; "

static void pa_app_apply(const gchar *app_name, const gchar *value, const gchar *fallback_id, gboolean is_mute) {
    const gchar *script = is_mute
        ? PA_APP_RESOLVE "for i in $ids; do pactl set-sink-input-mute \"$i\" \"$2\"; done"
        : PA_APP_RESOLVE "for i in $ids; do pactl set-sink-input-volume \"$i\" \"$2%\"; done";
    gchar *argv[] = { "sh", "-c", (gchar *)script, "sh",
                      (gchar *)(app_name ? app_name : ""), (gchar *)value, (gchar *)(fallback_id ? fallback_id : ""), NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
}

static void pa_app_send_volume(GtkWidget *scale, gint val) {
    gchar *v = g_strdup_printf("%d", MAX(val, 0));
    pa_app_apply(g_object_get_data(G_OBJECT(scale), "pa-app-name"), v,
                 g_object_get_data(G_OBJECT(scale), "pa-input-id"), FALSE);
    g_free(v);
}

static gboolean on_pa_app_throttle(gpointer user_data) {
    GtkWidget *scale = GTK_WIDGET(user_data);
    gint pending = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(scale), "pa-pending"));
    gint sent    = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(scale), "pa-sent"));
    if (pending != sent) {
        g_object_set_data(G_OBJECT(scale), "pa-sent", GINT_TO_POINTER(pending));
        pa_app_send_volume(scale, pending);
        return G_SOURCE_CONTINUE;
    }
    g_object_set_data(G_OBJECT(scale), "pa-timer", GINT_TO_POINTER(0));
    g_object_unref(scale);
    return G_SOURCE_REMOVE;
}

static void on_pa_app_scale_changed(GtkRange *range, SystrayWidget *tray) {
    if (tray->updating_vol_ui) return;
    gint val = (gint)gtk_range_get_value(range);
    tray->pa_ui_change_until = g_get_monotonic_time() + 1500000;
    g_object_set_data(G_OBJECT(range), "pa-pending", GINT_TO_POINTER(val));
    if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(range), "pa-timer"))) {
        g_object_set_data(G_OBJECT(range), "pa-sent", GINT_TO_POINTER(val));
        pa_app_send_volume(GTK_WIDGET(range), val);
        g_object_set_data(G_OBJECT(range), "pa-timer", GINT_TO_POINTER(1));
        g_timeout_add(60, on_pa_app_throttle, g_object_ref(range));
    }
}

static void on_pa_app_switch_notify(GObject *obj, GParamSpec *pspec, SystrayWidget *tray) {
    if (tray->updating_vol_ui) return;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(obj));
    GtkWidget *scale = g_object_get_data(obj, "pa-scale");
    if (scale) gtk_widget_set_sensitive(scale, active);
    tray->pa_ui_change_until = g_get_monotonic_time() + 1500000;
    pa_app_apply(g_object_get_data(obj, "pa-app-name"), active ? "0" : "1",
                 g_object_get_data(obj, "pa-input-id"), TRUE);
}

static GtkWidget *create_upstream_scale_menu_item(SystrayWidget *tray, const gchar *icon_name,
                                                  gboolean is_mic,
                                                  GtkWidget **out_scale, GtkWidget **out_switch,
                                                  GtkWidget **out_img);

/* Rebuilds the "Applications" section of the volume menu from the cached sink-input list */
static void pa_rebuild_app_items(SystrayWidget *tray) {
    if (!GTK_IS_MENU(tray->vol_menu)) return;
    for (GList *l = tray->pa_app_items; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(tray->pa_app_items);
    tray->pa_app_items = NULL;

    GList *apps = pa_parse_apps(tray->pa_txt_inputs);
    if (!apps) return;

    /* Insert right after the input-device submenu's separator (menu index 6) */
    gint pos = 6;
    GtkWidget *hdr = gtk_menu_item_new_with_label("Applications");
    gtk_widget_set_sensitive(hdr, FALSE);
    gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), hdr, pos++);
    tray->pa_app_items = g_list_append(tray->pa_app_items, hdr);

    tray->updating_vol_ui = TRUE;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = apps; l; l = l->next) {
        PaAppInfo *a = (PaAppInfo *)l->data;
        if (g_hash_table_contains(seen, a->name)) continue;   /* one row per app; changes go to all its streams */
        g_hash_table_add(seen, a->name);
        GtkWidget *name_item = gtk_menu_item_new_with_label(a->name);
        gtk_widget_set_sensitive(name_item, FALSE);
        gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), name_item, pos++);
        tray->pa_app_items = g_list_append(tray->pa_app_items, name_item);

        GtkWidget *sc = NULL, *sw = NULL, *img = NULL;
        GtkWidget *row = create_upstream_scale_menu_item(tray, a->icon, FALSE, &sc, &sw, &img);
        gtk_range_set_range(GTK_RANGE(sc), 0.0, (gdouble)pa_max_volume(tray));
        gtk_range_set_value(GTK_RANGE(sc), a->vol);
        gtk_widget_set_sensitive(sc, !a->muted);
        gtk_switch_set_active(GTK_SWITCH(sw), !a->muted);
        gtk_widget_set_tooltip_text(row, a->name);
        g_object_set_data_full(G_OBJECT(sc), "pa-input-id", g_strdup(a->id), g_free);
        g_object_set_data_full(G_OBJECT(sw), "pa-input-id", g_strdup(a->id), g_free);
        g_object_set_data(G_OBJECT(sw), "pa-scale", sc);
        g_object_set_data_full(G_OBJECT(sc), "pa-app-name", g_strdup(a->name), g_free);
        g_object_set_data_full(G_OBJECT(sw), "pa-app-name", g_strdup(a->name), g_free);
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_pa_app_scale_changed), tray);
        g_signal_connect(sw, "notify::active", G_CALLBACK(on_pa_app_switch_notify), tray);
        gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), row, pos++);
        tray->pa_app_items = g_list_append(tray->pa_app_items, row);
    }
    tray->updating_vol_ui = FALSE;
    g_hash_table_destroy(seen);

    GtkWidget *sep = gtk_separator_menu_item_new();
    gtk_menu_shell_insert(GTK_MENU_SHELL(tray->vol_menu), sep, pos++);
    tray->pa_app_items = g_list_append(tray->pa_app_items, sep);

    for (GList *l = tray->pa_app_items; l; l = l->next) gtk_widget_show_all(GTK_WIDGET(l->data));
    g_list_free_full(apps, pa_app_free);
}

/* One-time start */
static void pa_audio_setup(SystrayWidget *tray) {
    tray->pa_vol = 75;
    tray->pa_mic = 75;
    pa_subscribe_start(tray);
    if (!tray->pa_sub) pa_refresh_async(tray);
    pa_update_key_grabs(tray);
}

static void refresh_bluetooth_ui(SystrayWidget *tray);

static void update_network_and_audio(SystrayWidget *tray) {
    if (!tray->net_item || !tray->vol_item) return;

    GNetworkMonitor *mon = g_network_monitor_get_default();
    gboolean connected = g_network_monitor_get_network_available(mon);
    gboolean is_wifi = FALSE;

    FILE *fp = fopen("/proc/net/wireless", "r");
    if (fp) {
        char line[256]; int ln = 0;
        while (fgets(line, sizeof(line), fp)) {
            if (++ln > 2 && strlen(line) > 5) { is_wifi = TRUE; break; }
        }
        fclose(fp);
    }

    if (!connected) {
        gtk_image_set_from_icon_name(GTK_IMAGE(tray->net_item->image), "network-offline-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(tray->net_item->button, "Network: Disconnected");
    } else if (is_wifi) {
        gtk_image_set_from_icon_name(GTK_IMAGE(tray->net_item->image), "network-wireless-signal-excellent-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(tray->net_item->button, "Network: Wi-Fi Connected");
    } else {
        gtk_image_set_from_icon_name(GTK_IMAGE(tray->net_item->image), "network-wired-symbolic", GTK_ICON_SIZE_MENU);
        gtk_widget_set_tooltip_text(tray->net_item->button, "Network: Wired Connected");
    }

    /* Cached state (kept current by `pactl subscribe` in the background) */
    gboolean muted     = tray->pa_muted;
    gint vol           = tray->pa_vol;
    gboolean mic_muted = tray->pa_mic_muted;
    gint mic_vol       = tray->pa_mic;

    /* Recording indicator: its own tray icon, shown while an app uses the mic
       (or always, if the user asked for it) */
    if (tray->mic_item && GTK_IS_WIDGET(tray->mic_item->button)) {
        gboolean show_rec = tray->pa_recording || (tray->shell && tray->shell->audio_show_rec_ind);
        GtkWidget *mimg = tray->mic_item->image;
        gtk_widget_set_opacity(mimg, tray->pa_recording ? 1.0 : 0.45);
        GtkStyleContext *rc = gtk_widget_get_style_context(mimg);
        if (tray->pa_recording) gtk_style_context_add_class(rc, "ult-rec-live");
        else gtk_style_context_remove_class(rc, "ult-rec-live");
        gchar *tip = (tray->pa_recording && tray->pa_rec_apps && tray->pa_rec_apps[0])
            ? g_strdup_printf("Microphone in use by %s", tray->pa_rec_apps)
            : g_strdup(tray->pa_recording ? "Microphone in use" : "Microphone not in use");
        gtk_widget_set_tooltip_text(tray->mic_item->button, tip);
        g_free(tip);
        if (show_rec != tray->mic_shown) {
            tray->mic_shown = show_rec;
            relayout_tray_containers(tray);
        }
    }

    const gchar *vol_icon = "audio-volume-high-symbolic";
    if (muted || vol == 0) vol_icon = "audio-volume-muted-symbolic";
    else if (vol < 35)     vol_icon = "audio-volume-low-symbolic";
    else if (vol < 70)     vol_icon = "audio-volume-medium-symbolic";

    const gchar *mic_icon = "microphone-sensitivity-high-symbolic";
    if (mic_muted || mic_vol == 0) mic_icon = "microphone-sensitivity-muted-symbolic";
    else if (mic_vol < 35)         mic_icon = "microphone-sensitivity-low-symbolic";
    else if (mic_vol < 70)         mic_icon = "microphone-sensitivity-medium-symbolic";

    gtk_image_set_from_icon_name(GTK_IMAGE(tray->vol_item->image), vol_icon, GTK_ICON_SIZE_MENU);
    if (tray->vol_row_img) gtk_image_set_from_icon_name(GTK_IMAGE(tray->vol_row_img), vol_icon, GTK_ICON_SIZE_MENU);
    if (tray->mic_row_img) gtk_image_set_from_icon_name(GTK_IMAGE(tray->mic_row_img), mic_icon, GTK_ICON_SIZE_MENU);

    gchar *vtip = g_strdup_printf("Volume: %d%%%s%s", vol, muted ? " (muted)" : "",
                                  tray->pa_recording ? "\nMicrophone in use" : "");
    gtk_widget_set_tooltip_text(tray->vol_item->button, vtip);
    g_free(vtip);

    gint icon_px = tray->shell ? CLAMP(tray->shell->tray_icon_size, 14, 28) : 16;
    gtk_image_set_pixel_size(GTK_IMAGE(tray->net_item->image), icon_px);
    gtk_image_set_pixel_size(GTK_IMAGE(tray->vol_item->image), icon_px);

    power_render(tray);   /* battery state is cached and updated live; this only redraws */
    if (tray->clip_item && GTK_IS_IMAGE(tray->clip_item->image)) {
        gtk_image_set_pixel_size(GTK_IMAGE(tray->clip_item->image), icon_px);
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        if (cb) gtk_clipboard_request_text(cb, on_clipboard_text_received, tray);
    }

    if (!tray->scale_grabbed && tray->vol_scale && GTK_IS_RANGE(tray->vol_scale)) {
        tray->updating_vol_ui = TRUE;
        gtk_range_set_value(GTK_RANGE(tray->vol_scale), vol);
        gtk_widget_set_sensitive(tray->vol_scale, !muted);
        if (tray->vol_switch) {
            gtk_switch_set_active(GTK_SWITCH(tray->vol_switch), !muted);
        }
        if (tray->mic_scale && GTK_IS_RANGE(tray->mic_scale)) {
            gtk_range_set_value(GTK_RANGE(tray->mic_scale), mic_vol);
            gtk_widget_set_sensitive(tray->mic_scale, !mic_muted);
        }
        if (tray->mic_switch) {
            gtk_switch_set_active(GTK_SWITCH(tray->mic_switch), !mic_muted);
        }
        tray->updating_vol_ui = FALSE;
    }
}

static void refresh_network_popup_list(SystrayWidget *tray) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(tray->wifi_list_box));
    for (GList *l = old; l != NULL; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);

    gchar *act_out = NULL;
    if (g_spawn_command_line_sync("nmcli -t -f NAME,TYPE,DEVICE connection show --active", &act_out, NULL, NULL, NULL) && act_out && act_out[0]) {
        gchar **lines = g_strsplit(act_out, "\n", -1);
        gchar *first = (lines && lines[0] && lines[0][0]) ? lines[0] : "Connected";
        /* "Wired connection 1:802-3-ethernet:enp0s3" → "Wired connection 1 · Ethernet" */
        gchar **f = g_strsplit(first, ":", 3);
        const gchar *kind = (f[0] && f[1]) ? (strstr(f[1], "ethernet") ? "Ethernet" : strstr(f[1], "wireless") ? "Wi-Fi" :
                            strstr(f[1], "vpn") ? "VPN" : f[1]) : NULL;
        gchar *nice = kind ? g_strdup_printf("%s · %s", f[0], kind) : g_strdup(first);
        g_strfreev(f);
        gchar *mk = g_markup_printf_escaped("<b>Connected:</b> %s", nice);
        g_free(nice);
        gtk_label_set_markup(GTK_LABEL(tray->net_status_lbl), mk);
        g_free(mk);
        g_strfreev(lines);
    } else {
        gtk_label_set_markup(GTK_LABEL(tray->net_status_lbl), "<b>Not connected</b>");
    }
    g_free(act_out);

    gchar *wifi_out = NULL;
    if (g_spawn_command_line_sync("nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list --rescan no", &wifi_out, NULL, NULL, NULL) && wifi_out) {
        gchar **lines = g_strsplit(wifi_out, "\n", -1);
        for (int i = 0; lines && lines[i]; i++) {
            if (lines[i][0] == '\0') continue;
            gchar **parts = g_strsplit(lines[i], ":", 4);
            if (g_strv_length(parts) >= 3 && parts[1][0] != '\0') {
                gboolean in_use = (parts[0][0] == '*');
                GtkWidget *row = gtk_list_box_row_new();
                GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_widget_set_margin_start(hbox, 6);
                gtk_widget_set_margin_end(hbox, 6);
                gtk_widget_set_margin_top(hbox, 5);
                gtk_widget_set_margin_bottom(hbox, 5);

                gchar *txt = g_strdup_printf("%s %s  (%s%%)  %s",
                    in_use ? "✔" : "📶", parts[1], parts[2], parts[3] ? parts[3] : "");
                GtkWidget *lbl = gtk_label_new(txt);
                gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
                g_free(txt);

                gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);
                gtk_container_add(GTK_CONTAINER(row), hbox);
                g_object_set_data_full(G_OBJECT(row), "wifi-ssid", g_strdup(parts[1]), g_free);
                gtk_container_add(GTK_CONTAINER(tray->wifi_list_box), row);
            }
            g_strfreev(parts);
        }
        g_strfreev(lines);
    }
    g_free(wifi_out);
    gtk_widget_show_all(tray->wifi_list_box);
}

static void on_wifi_row_activated(GtkListBox *box, GtkListBoxRow *row, SystrayWidget *tray) {
    const gchar *ssid = g_object_get_data(G_OBJECT(row), "wifi-ssid");
    if (ssid) {
        /* SSID is passed as a separate argument ($1), never parsed by the shell */
        gchar *argv[] = { "sh", "-c", "nmcli dev wifi connect \"$1\" || nm-connection-editor",
                          "sh", (gchar *)ssid, NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->net_item->button), FALSE);
}

static void on_net_settings_clicked(GtkButton *btn, SystrayWidget *tray) {
    g_spawn_command_line_async("sh -c \"nm-connection-editor || xfce4-terminal -e nmtui\"", NULL);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->net_item->button), FALSE);
}

static void on_net_toggled(GtkToggleButton *btn, SystrayWidget *tray) {
    if (qs_enabled(tray)) { if (gtk_toggle_button_get_active(btn)) qs_on_tray_click(tray, btn); return; }
    if (gtk_toggle_button_get_active(btn)) refresh_network_popup_list(tray);
    shell_toggle_popup_window(btn, tray->net_popup);
}

static void on_vol_scale_changed(GtkRange *range, SystrayWidget *tray) {
    if (tray->updating_vol_ui) return;
    gint val = (gint)gtk_range_get_value(range);
    pa_queue_sink_volume(tray, val);   /* throttled: at most one write every 60 ms */

    const gchar *vol_icon = "audio-volume-high-symbolic";
    if (val == 0)      vol_icon = "audio-volume-muted-symbolic";
    else if (val < 35) vol_icon = "audio-volume-low-symbolic";
    else if (val < 70) vol_icon = "audio-volume-medium-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(tray->vol_item->image), vol_icon, GTK_ICON_SIZE_MENU);
    if (tray->vol_row_img) gtk_image_set_from_icon_name(GTK_IMAGE(tray->vol_row_img), vol_icon, GTK_ICON_SIZE_MENU);
}

static void on_vol_switch_notify(GObject *obj, GParamSpec *pspec, SystrayWidget *tray) {
    if (tray->updating_vol_ui) return;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(obj));
    gboolean mute = !active;
    gtk_widget_set_sensitive(tray->vol_scale, active);
    tray->pa_muted = mute;
    tray->pa_ui_change_until = g_get_monotonic_time() + 1500000;
    pa_set_sink_mute(mute);
}

static void on_mic_scale_changed(GtkRange *range, SystrayWidget *tray) {
    if (tray->updating_vol_ui) return;
    gint val = (gint)gtk_range_get_value(range);
    pa_queue_source_volume(tray, val);

    const gchar *mic_icon = "microphone-sensitivity-high-symbolic";
    if (val == 0)      mic_icon = "microphone-sensitivity-muted-symbolic";
    else if (val < 35) mic_icon = "microphone-sensitivity-low-symbolic";
    else if (val < 70) mic_icon = "microphone-sensitivity-medium-symbolic";
    if (tray->mic_row_img) gtk_image_set_from_icon_name(GTK_IMAGE(tray->mic_row_img), mic_icon, GTK_ICON_SIZE_MENU);
}

static void on_mic_switch_notify(GObject *obj, GParamSpec *pspec, SystrayWidget *tray) {
    if (tray->updating_vol_ui) return;
    gboolean active = gtk_switch_get_active(GTK_SWITCH(obj));
    gboolean mute = !active;
    gtk_widget_set_sensitive(tray->mic_scale, active);
    tray->pa_mic_muted = mute;
    tray->pa_ui_change_until = g_get_monotonic_time() + 1500000;
    pa_set_source_mute(mute);
}

static void on_open_pavucontrol(GtkWidget *w, SystrayWidget *tray) {
    const gchar *mixer = (tray && tray->shell && tray->shell->audio_mixer_cmd && tray->shell->audio_mixer_cmd[0])
                         ? tray->shell->audio_mixer_cmd : "pavucontrol";
    gchar *cmd = g_strdup_printf("sh -c \"%s || pavucontrol || xfce4-terminal -e alsamixer\"", mixer);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    if (tray && tray->vol_item && GTK_IS_TOGGLE_BUTTON(tray->vol_item->button)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->vol_item->button), FALSE);
    }
}

/* =========================================================================
 * Item 4: PulseAudio Panel Plugin 2-Tab Dialog (General & Media Players)
 * ========================================================================= */
static void save_audio_props_to_xfconf(UltimateShell *sh) {
    if (!sh || !sh->channel) return;
    xfconf_channel_set_bool(sh->channel, "/plugins/ultimateshell/audio_key_shortcuts", sh->audio_key_shortcuts);
    xfconf_channel_set_bool(sh->channel, "/plugins/ultimateshell/audio_show_rec_ind", sh->audio_show_rec_ind);
    xfconf_channel_set_int(sh->channel, "/plugins/ultimateshell/audio_vol_step", sh->audio_vol_step);
    xfconf_channel_set_int(sh->channel, "/plugins/ultimateshell/audio_max_vol", sh->audio_max_vol);
    xfconf_channel_set_int(sh->channel, "/plugins/ultimateshell/audio_notif_mode", sh->audio_notif_mode);
    xfconf_channel_set_string(sh->channel, "/plugins/ultimateshell/audio_mixer_cmd", sh->audio_mixer_cmd ? sh->audio_mixer_cmd : "pavucontrol");
    xfconf_channel_set_bool(sh->channel, "/plugins/ultimateshell/audio_mpris_enabled", sh->audio_mpris_enabled);
    xfconf_channel_set_bool(sh->channel, "/plugins/ultimateshell/audio_mpris_keys", sh->audio_mpris_keys);
    xfconf_channel_set_bool(sh->channel, "/plugins/ultimateshell/audio_mpris_all", sh->audio_mpris_all);
    xfconf_channel_set_bool(sh->channel, "/plugins/ultimateshell/audio_mpris_focus", sh->audio_mpris_focus);
    xfconf_channel_set_string(sh->channel, "/plugins/ultimateshell/audio_known_players", sh->audio_known_players ? sh->audio_known_players : "");
    xfconf_channel_set_string(sh->channel, "/plugins/ultimateshell/audio_ignored_players", sh->audio_ignored_players ? sh->audio_ignored_players : "");
    xfconf_channel_set_string(sh->channel, "/plugins/ultimateshell/audio_persistent_players", sh->audio_persistent_players ? sh->audio_persistent_players : "");
}

static void on_pa_bool_toggled(GtkToggleButton *btn, UltimateShell *sh) {
    const gchar *prop = g_object_get_data(G_OBJECT(btn), "pa-prop");
    gboolean val = gtk_toggle_button_get_active(btn);
    if (g_strcmp0(prop, "keys") == 0) sh->audio_key_shortcuts = val;
    else if (g_strcmp0(prop, "rec") == 0) sh->audio_show_rec_ind = val;
    else if (g_strcmp0(prop, "mpris_keys") == 0) sh->audio_mpris_keys = val;
    else if (g_strcmp0(prop, "mpris_all") == 0) sh->audio_mpris_all = val;
    else if (g_strcmp0(prop, "mpris_focus") == 0) sh->audio_mpris_focus = val;
    save_audio_props_to_xfconf(sh);
    if (g_systray && g_strcmp0(prop, "keys") == 0) pa_update_key_grabs(g_systray);
    if (g_systray && g_strcmp0(prop, "rec") == 0) update_network_and_audio(g_systray);
}

static void on_pa_mpris_switch_notify(GObject *sw, GParamSpec *ps, UltimateShell *sh) {
    sh->audio_mpris_enabled = gtk_switch_get_active(GTK_SWITCH(sw));
    save_audio_props_to_xfconf(sh);
}

static void on_pa_step_changed(GtkSpinButton *spin, UltimateShell *sh) {
    sh->audio_vol_step = gtk_spin_button_get_value_as_int(spin);
    save_audio_props_to_xfconf(sh);
}

static void on_pa_max_vol_changed(GtkSpinButton *spin, UltimateShell *sh) {
    sh->audio_max_vol = gtk_spin_button_get_value_as_int(spin);
    if (g_systray && g_systray->vol_scale) {
        gtk_range_set_range(GTK_RANGE(g_systray->vol_scale), 0.0, (gdouble)sh->audio_max_vol);
    }
    if (g_systray && g_systray->mic_scale) {
        gtk_range_set_range(GTK_RANGE(g_systray->mic_scale), 0.0, (gdouble)sh->audio_max_vol);
    }
    save_audio_props_to_xfconf(sh);
}

static void on_pa_notif_changed(GtkComboBox *combo, UltimateShell *sh) {
    sh->audio_notif_mode = gtk_combo_box_get_active(combo);
    save_audio_props_to_xfconf(sh);
}

static void on_pa_mixer_entry_changed(GtkEditable *ed, UltimateShell *sh) {
    g_free(sh->audio_mixer_cmd);
    sh->audio_mixer_cmd = g_strdup(gtk_entry_get_text(GTK_ENTRY(ed)));
    save_audio_props_to_xfconf(sh);
}

static void on_pa_player_persistent_toggled(GtkToggleButton *chk, UltimateShell *sh) {
    const gchar *pname = g_object_get_data(G_OBJECT(chk), "player-id");
    if (!pname) return;
    if (!sh->audio_persistent_players) sh->audio_persistent_players = g_strdup("");
    gboolean pe = gtk_toggle_button_get_active(chk);
    GString *next = g_string_new("");
    gchar **toks = g_strsplit(sh->audio_persistent_players, ";", -1);
    for (int i = 0; toks && toks[i]; i++) {
        if (!toks[i][0] || g_ascii_strcasecmp(toks[i], pname) == 0) continue;
        if (next->len > 0) g_string_append_c(next, ';');
        g_string_append(next, toks[i]);
    }
    g_strfreev(toks);
    if (pe) {
        if (next->len > 0) g_string_append_c(next, ';');
        g_string_append(next, pname);
    }
    g_free(sh->audio_persistent_players);
    sh->audio_persistent_players = g_string_free(next, FALSE);
    save_audio_props_to_xfconf(sh);
}

static void on_pa_player_ignored_toggled(GtkToggleButton *chk, UltimateShell *sh) {
    const gchar *pname = g_object_get_data(G_OBJECT(chk), "player-id");
    if (!pname) return;
    gboolean ig = gtk_toggle_button_get_active(chk);
    GString *next = g_string_new("");
    gchar **toks = g_strsplit(sh->audio_ignored_players ? sh->audio_ignored_players : "", ";", -1);
    for (int i = 0; toks && toks[i]; i++) {
        if (!toks[i][0] || g_ascii_strcasecmp(toks[i], pname) == 0) continue;
        if (next->len > 0) g_string_append_c(next, ';');
        g_string_append(next, toks[i]);
    }
    g_strfreev(toks);
    if (ig) {
        if (next->len > 0) g_string_append_c(next, ';');
        g_string_append(next, pname);
    }
    g_free(sh->audio_ignored_players);
    sh->audio_ignored_players = g_string_free(next, FALSE);
    save_audio_props_to_xfconf(sh);
}

static void populate_known_players_listbox(GtkWidget *lb, UltimateShell *sh) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(lb));
    for (GList *l = old; l != NULL; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);

    if (!sh->audio_known_players) sh->audio_known_players = g_strdup("");

    gchar **toks = g_strsplit(sh->audio_known_players, ";", -1);
    for (int i = 0; toks && toks[i]; i++) {
        const gchar *p = toks[i];
        if (!p[0]) continue;

        /* friendly name: last part of an app id, first letter capitalised */
        const gchar *last = strrchr(p, '.');
        gchar *disp_buf = g_strdup(last ? last + 1 : p);
        if (disp_buf[0]) disp_buf[0] = g_ascii_toupper(disp_buf[0]);
        const gchar *disp = disp_buf;
        const gchar *icon = p;
        if (g_ascii_strcasecmp(p, "vlc") == 0) disp = "VLC media player";
        else if (g_ascii_strcasecmp(p, "strawberry") == 0) disp = "Strawberry";
        else if (g_ascii_strcasecmp(p, "firefox") == 0) disp = "Firefox";

        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(hbox, 8);
        gtk_widget_set_margin_end(hbox, 8);
        gtk_widget_set_margin_top(hbox, 4);
        gtk_widget_set_margin_bottom(hbox, 4);

        GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
        GtkWidget *lbl = gtk_label_new(disp);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        g_free(disp_buf);

        GtkWidget *chk_pers = gtk_check_button_new();
        gboolean is_pers = is_token_in_semicolon_list(sh->audio_persistent_players, p);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_pers), is_pers);
        g_object_set_data_full(G_OBJECT(chk_pers), "player-id", g_strdup(p), g_free);
        g_signal_connect(chk_pers, "toggled", G_CALLBACK(on_pa_player_persistent_toggled), sh);
        gtk_widget_set_margin_end(chk_pers, 28);

        GtkWidget *chk_ign = gtk_check_button_new();
        gboolean is_ig = is_token_in_semicolon_list(sh->audio_ignored_players, p);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_ign), is_ig);
        g_object_set_data_full(G_OBJECT(chk_ign), "player-id", g_strdup(p), g_free);
        g_signal_connect(chk_ign, "toggled", G_CALLBACK(on_pa_player_ignored_toggled), sh);
        gtk_widget_set_margin_end(chk_ign, 12);

        gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), chk_ign, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), chk_pers, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_container_add(GTK_CONTAINER(lb), row);
    }
    g_strfreev(toks);
    gtk_widget_show_all(lb);
}

static void on_pa_clear_known_clicked(GtkButton *btn, GtkWidget *lb) {
    UltimateShell *sh = g_object_get_data(G_OBJECT(lb), "shell-ptr");
    if (!sh) return;
    g_free(sh->audio_known_players);
    sh->audio_known_players = g_strdup("");
    save_audio_props_to_xfconf(sh);
    GList *old = gtk_container_get_children(GTK_CONTAINER(lb));
    for (GList *l = old; l != NULL; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);
}

static void on_pa_props_dialog_destroyed(GtkWidget *dlg, UltimateShell *shell) {
    if (shell && shell->plugin &&
        g_object_get_data(G_OBJECT(shell->plugin), "pa-props-dialog") == (gpointer)dlg) {
        g_object_set_data(G_OBJECT(shell->plugin), "pa-props-dialog", NULL);
    }
}

GtkWidget *systray_build_audio_settings(UltimateShell *shell) {
    GtkWidget *nb = gtk_notebook_new();
    gtk_container_set_border_width(GTK_CONTAINER(nb), 10);

    /* --- TAB 1: GENERAL --- */
    GtkWidget *vbox_gen = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_gen), 14);

    GtkWidget *lbl_beh = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_beh), "<b>Behaviour</b>");
    gtk_label_set_xalign(GTK_LABEL(lbl_beh), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox_gen), lbl_beh, FALSE, FALSE, 0);

    GtkWidget *chk_keys = gtk_check_button_new_with_label("Enable keyboard shortcuts for volume control");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_keys), shell->audio_key_shortcuts);
    g_object_set_data(G_OBJECT(chk_keys), "pa-prop", "keys");
    g_signal_connect(chk_keys, "toggled", G_CALLBACK(on_pa_bool_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_gen), chk_keys, FALSE, FALSE, 0);

    GtkWidget *chk_rec = gtk_check_button_new_with_label("Always display the recording indicator");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_rec), shell->audio_show_rec_ind);
    g_object_set_data(G_OBJECT(chk_rec), "pa-prop", "rec");
    g_signal_connect(chk_rec, "toggled", G_CALLBACK(on_pa_bool_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_gen), chk_rec, FALSE, FALSE, 0);

    GtkWidget *step_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(step_hbox), gtk_label_new("Volume step:"), FALSE, FALSE, 0);
    GtkWidget *step_spin = gtk_spin_button_new_with_range(1, 25, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(step_spin), shell->audio_vol_step > 0 ? shell->audio_vol_step : 5);
    g_signal_connect(step_spin, "value-changed", G_CALLBACK(on_pa_step_changed), shell);
    gtk_box_pack_start(GTK_BOX(step_hbox), step_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_gen), step_hbox, FALSE, FALSE, 0);

    GtkWidget *max_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(max_hbox), gtk_label_new("Maximum volume:"), FALSE, FALSE, 0);
    GtkWidget *max_spin = gtk_spin_button_new_with_range(100, 200, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(max_spin), shell->audio_max_vol > 0 ? shell->audio_max_vol : 150);
    g_signal_connect(max_spin, "value-changed", G_CALLBACK(on_pa_max_vol_changed), shell);
    gtk_box_pack_start(GTK_BOX(max_hbox), max_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_gen), max_hbox, FALSE, FALSE, 0);

    GtkWidget *notif_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(notif_hbox), gtk_label_new("Show volume notifications:"), FALSE, FALSE, 0);
    GtkWidget *notif_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(notif_combo), "All");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(notif_combo), "Output Only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(notif_combo), "Input Only");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(notif_combo), "None");
    gtk_combo_box_set_active(GTK_COMBO_BOX(notif_combo), shell->audio_notif_mode);
    g_signal_connect(notif_combo, "changed", G_CALLBACK(on_pa_notif_changed), shell);
    gtk_box_pack_start(GTK_BOX(notif_hbox), notif_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_gen), notif_hbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox_gen), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 6);
    GtkWidget *lbl_snd = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_snd), "<b>Sound Settings</b>");
    gtk_label_set_xalign(GTK_LABEL(lbl_snd), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox_gen), lbl_snd, FALSE, FALSE, 0);

    GtkWidget *mix_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(mix_hbox), gtk_label_new("Audio Mixer"), FALSE, FALSE, 0);
    GtkWidget *mix_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(mix_entry), shell->audio_mixer_cmd ? shell->audio_mixer_cmd : "pavucontrol");
    g_signal_connect(mix_entry, "changed", G_CALLBACK(on_pa_mixer_entry_changed), shell);
    gtk_box_pack_start(GTK_BOX(mix_hbox), mix_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_gen), mix_hbox, FALSE, FALSE, 0);

    GtkWidget *run_mix_btn = gtk_button_new_with_label("Run Audio Mixer...");
    gtk_widget_set_halign(run_mix_btn, GTK_ALIGN_START);
    g_signal_connect(run_mix_btn, "clicked", G_CALLBACK(on_open_pavucontrol), g_systray);
    gtk_box_pack_start(GTK_BOX(vbox_gen), run_mix_btn, FALSE, FALSE, 4);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), vbox_gen, gtk_label_new("General"));

    /* --- TAB 2: MEDIA PLAYERS --- */
    GtkWidget *vbox_mp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_mp), 14);

    GtkWidget *mp_hdr_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *lbl_mp = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_mp), "<b>Control Playback of Media Players</b>");
    gtk_label_set_xalign(GTK_LABEL(lbl_mp), 0.0);
    GtkWidget *sw_mp = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(sw_mp), shell->audio_mpris_enabled);
    g_signal_connect(sw_mp, "notify::active", G_CALLBACK(on_pa_mpris_switch_notify), shell);
    gtk_box_pack_start(GTK_BOX(mp_hdr_hbox), lbl_mp, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(mp_hdr_hbox), sw_mp, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_mp), mp_hdr_hbox, FALSE, FALSE, 0);

    GtkWidget *chk_mk = gtk_check_button_new_with_label("Enable multimedia keys for playback control");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_mk), shell->audio_mpris_keys);
    g_object_set_data(G_OBJECT(chk_mk), "pa-prop", "mpris_keys");
    g_signal_connect(chk_mk, "toggled", G_CALLBACK(on_pa_bool_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_mp), chk_mk, FALSE, FALSE, 0);

    GtkWidget *chk_ma = gtk_check_button_new_with_label("Send multimedia keys to all players");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_ma), shell->audio_mpris_all);
    g_object_set_data(G_OBJECT(chk_ma), "pa-prop", "mpris_all");
    g_signal_connect(chk_ma, "toggled", G_CALLBACK(on_pa_bool_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_mp), chk_ma, FALSE, FALSE, 0);

    GtkWidget *chk_mf = gtk_check_button_new_with_label("Enable experimental window focus support");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk_mf), shell->audio_mpris_focus);
    g_object_set_data(G_OBJECT(chk_mf), "pa-prop", "mpris_focus");
    g_signal_connect(chk_mf, "toggled", G_CALLBACK(on_pa_bool_toggled), shell);
    gtk_box_pack_start(GTK_BOX(vbox_mp), chk_mf, FALSE, FALSE, 0);

    GtkWidget *lbl_kn = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_kn), "<b>Known Media Players</b>");
    gtk_label_set_xalign(GTK_LABEL(lbl_kn), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox_mp), lbl_kn, FALSE, FALSE, 4);

    GtkWidget *col_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(col_hdr), gtk_label_new("  Name"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(col_hdr), gtk_label_new("Ignored "), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(col_hdr), gtk_label_new("Persistent  "), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox_mp), col_hdr, FALSE, FALSE, 0);

    GtkWidget *kn_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(kn_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(kn_scroll, -1, 140);
    GtkWidget *kn_lb = gtk_list_box_new();
    g_object_set_data(G_OBJECT(kn_lb), "shell-ptr", shell);
    populate_known_players_listbox(kn_lb, shell);
    if (g_known_players_lb) g_object_remove_weak_pointer(G_OBJECT(g_known_players_lb), (gpointer *)&g_known_players_lb);
    g_known_players_lb = kn_lb;
    g_object_add_weak_pointer(G_OBJECT(kn_lb), (gpointer *)&g_known_players_lb);
    gtk_container_add(GTK_CONTAINER(kn_scroll), kn_lb);
    gtk_box_pack_start(GTK_BOX(vbox_mp), kn_scroll, TRUE, TRUE, 0);

    GtkWidget *clr_btn = gtk_button_new_with_label("Clear Known Items");
    gtk_widget_set_halign(clr_btn, GTK_ALIGN_START);
    g_signal_connect(clr_btn, "clicked", G_CALLBACK(on_pa_clear_known_clicked), kn_lb);
    gtk_box_pack_start(GTK_BOX(vbox_mp), clr_btn, FALSE, FALSE, 0);

    gtk_notebook_append_page(GTK_NOTEBOOK(nb), vbox_mp, gtk_label_new("Media Players"));

    return nb;
}

void systray_open_audio_properties_dialog(UltimateShell *shell) {
    if (!shell) return;
    GtkWidget *existing = g_object_get_data(G_OBJECT(shell->plugin), "pa-props-dialog");
    if (GTK_IS_WIDGET(existing)) {
        gtk_window_present(GTK_WINDOW(existing));
        return;
    }

    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        "PulseAudio Panel Plugin", NULL, 0,
        "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_type_hint(GTK_WINDOW(dlg), GDK_WINDOW_TYPE_HINT_NORMAL);
    gtk_window_set_keep_above(GTK_WINDOW(dlg), FALSE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, 520);

    g_object_set_data(G_OBJECT(shell->plugin), "pa-props-dialog", dlg);
    g_signal_connect_swapped(dlg, "response", G_CALLBACK(gtk_widget_destroy), dlg);
    /* Forget the dialog when it closes. (Previously this wrote into a stack variable that
       no longer existed and left a dangling pointer on the plugin, so reopening crashed.) */
    g_signal_connect(dlg, "destroy", G_CALLBACK(on_pa_props_dialog_destroyed), shell);

    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    GtkWidget *nb = systray_build_audio_settings(shell);
    gtk_box_pack_start(GTK_BOX(content), nb, TRUE, TRUE, 0);

    gtk_widget_show_all(dlg);
}

static void on_open_pa_props_menuitem(GtkWidget *w, SystrayWidget *tray) {
    if (tray && tray->shell) systray_open_audio_properties_dialog(tray->shell);
}


static void ensure_menu_rgba_visual(GtkWidget *menu) {
    if (!GTK_IS_WIDGET(menu)) return;
    GdkScreen *scr = gtk_widget_get_screen(menu);
    GdkVisual *vis = scr ? gdk_screen_get_rgba_visual(scr) : NULL;
    if (vis) {
        gtk_widget_set_visual(menu, vis);
        GtkWidget *toplevel = gtk_widget_get_toplevel(menu);
        if (GTK_IS_WINDOW(toplevel)) {
            gtk_widget_set_visual(toplevel, vis);
            gtk_widget_set_app_paintable(toplevel, TRUE);
        }
    }
}

static void ap_toggle(SystrayWidget *tray, GtkToggleButton *btn);

static void on_vol_toggled(GtkToggleButton *btn, SystrayWidget *tray) {
    if (qs_enabled(tray)) { if (gtk_toggle_button_get_active(btn)) qs_on_tray_click(tray, btn); return; }
    /* new sound popup; the old menu below is kept only as a fallback */
    if (tray->vol_item && tray->vol_item->button) { ap_toggle(tray, btn); return; }
    ensure_menu_rgba_visual(tray->vol_menu);
    if (gtk_toggle_button_get_active(btn)) {
        update_network_and_audio(tray);
        refresh_audio_devices_and_mpris(tray);
        gboolean top_bar = (tray->shell && tray->shell->mode == LAYOUT_MACOS);
        gtk_menu_popup_at_widget(GTK_MENU(tray->vol_menu), GTK_WIDGET(btn),
                                 top_bar ? GDK_GRAVITY_SOUTH : GDK_GRAVITY_NORTH,
                                 top_bar ? GDK_GRAVITY_NORTH : GDK_GRAVITY_SOUTH,
                                 NULL);
    } else {
        gtk_menu_popdown(GTK_MENU(tray->vol_menu));
    }
}


static void on_pwr_settings_clicked(GtkButton *btn, SystrayWidget *tray) {
    g_spawn_command_line_async("sh -c \"xfce4-power-manager-settings || xfce4-settings-manager\"", NULL);
    if (tray->bat_item && GTK_IS_TOGGLE_BUTTON(tray->bat_item->button)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->bat_item->button), FALSE);
    }
}

static void on_clip_toggled(GtkToggleButton *btn, SystrayWidget *tray) {
    if (gtk_toggle_button_get_active(btn)) {
        GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
        if (cb) gtk_clipboard_request_text(cb, on_clipboard_text_received, tray);
        rebuild_clip_history_ui(tray);
    }
    shell_toggle_popup_window(btn, tray->clip_popup);
}

static void on_vol_menu_deactivated(GtkMenuShell *shell, SystrayWidget *tray) {
    tray->scale_grabbed = FALSE;
    tray->grabbed_scale = NULL;
    if (tray->vol_item && GTK_IS_TOGGLE_BUTTON(tray->vol_item->button)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->vol_item->button), FALSE);
    }
}

static gboolean on_vol_scroll(GtkWidget *widget, GdkEventScroll *event, SystrayWidget *tray) {
    gint dir = 0;
    if (event->direction == GDK_SCROLL_UP) dir = +1;
    else if (event->direction == GDK_SCROLL_DOWN) dir = -1;
    else if (event->direction == GDK_SCROLL_SMOOTH) {
        /* Touchpads: accumulate small deltas into whole steps */
        gdouble dx = 0, dy = 0;
        gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy);
        tray->pa_scroll_accum -= dy;
        if (tray->pa_scroll_accum >= 1.0)       { dir = +1; tray->pa_scroll_accum = 0.0; }
        else if (tray->pa_scroll_accum <= -1.0) { dir = -1; tray->pa_scroll_accum = 0.0; }
    }
    if (dir != 0) pa_step_volume(tray, dir);   /* never waits; uses your step & max settings */
    return TRUE;
}

/* =========================================================================
 * Sound popup (replaces the old PulseAudio-plugin style menu)
 *   [output level] [input level] (rec notice)
 *   [now-playing cards: art, title, artist, seek, controls, app volume]
 *   [Apps ▸ per-app mixer]          [Sound Settings…  Mixer…]
 *   second page: output / input device chooser
 * Uses the existing PulseAudio state (pa_refresh_async / pactl subscribe).
 * ========================================================================= */
typedef struct _ApPlayer ApPlayer;

typedef struct {
    SystrayWidget *tray;
    GtkWidget *win, *stack;
    GtkWidget *out_btn, *out_img, *out_scale, *out_pct, *out_sub;
    GtkWidget *in_btn,  *in_img,  *in_scale,  *in_pct,  *in_sub;
    GtkWidget *rec_row, *rec_lbl;
    GtkWidget *players_frame, *players_stack, *dots_box;
    GList     *players;
    GtkWidget *apps_exp, *apps_box;
    gchar     *apps_sig;
    GHashTable *app_scales;           /* app name -> GtkScale */
    GtkWidget *dev_title, *dev_list;
    GtkWidget *main_page, *dev_page, *foot, *foot_sep;   /* Quick Settings borrows the pages */
    gboolean   updating;
    guint      pos_timer, rebuild_id, name_sub, panel_icon_id;
} AudioPopup;

struct _ApPlayer {
    AudioPopup *ap;
    gchar      *bus, *id, *identity, *desktop, *trackid, *art_url, *pa_name;
    GDBusProxy *player;
    GtkWidget  *card, *art, *title, *artist, *source, *seek, *t_pos, *t_len, *seek_box;
    GtkWidget  *b_prev, *b_play, *b_next, *vol;
    gint64      length_us, user_seek_until;
    gboolean    playing, updating;
    GCancellable *cancel;
};

static AudioPopup *g_ap = NULL;
static gboolean    g_ap_in_qs = FALSE;          /* its pages are in Quick Settings right now */
static void qs_show_sound_devices(void);
static void qs_back_to_main(void);
static void ap_stack_show(AudioPopup *ap, const gchar *name) {
    if (g_ap_in_qs) { if (g_strcmp0(name, "devices") == 0) qs_show_sound_devices(); else qs_back_to_main(); return; }
    gtk_stack_set_visible_child_name(GTK_STACK(ap->stack), name);
}
static void ap_rebuild_players(AudioPopup *ap);

/* ---------- small widgets ---------- */
static GtkWidget *ap_flat_icon_btn(const gchar *icon, gint px) {
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "ap-icon-btn");
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    gtk_container_add(GTK_CONTAINER(b), img);
    gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
    return b;
}

static void ap_set_btn_icon(GtkWidget *btn, const gchar *icon) {
    GtkWidget *img = gtk_bin_get_child(GTK_BIN(btn));
    if (GTK_IS_IMAGE(img)) {
        gint px = gtk_image_get_pixel_size(GTK_IMAGE(img));
        gtk_image_set_from_icon_name(GTK_IMAGE(img), icon, GTK_ICON_SIZE_BUTTON);
        gtk_image_set_pixel_size(GTK_IMAGE(img), px);
    }
}

static gchar *ap_fmt_time(gint64 us) {
    gint64 s = MAX(us, 0) / 1000000;
    return (s >= 3600) ? g_strdup_printf("%d:%02d:%02d", (gint)(s / 3600), (gint)(s / 60 % 60), (gint)(s % 60))
                       : g_strdup_printf("%d:%02d", (gint)(s / 60), (gint)(s % 60));
}

static const gchar *ap_out_icon(gint vol, gboolean muted) {
    if (muted || vol <= 0) return "audio-volume-muted-symbolic";
    if (vol < 34) return "audio-volume-low-symbolic";
    if (vol < 67) return "audio-volume-medium-symbolic";
    return "audio-volume-high-symbolic";
}
static const gchar *ap_in_icon(gint vol, gboolean muted) {
    if (muted || vol <= 0) return "microphone-sensitivity-muted-symbolic";
    if (vol < 34) return "microphone-sensitivity-low-symbolic";
    if (vol < 67) return "microphone-sensitivity-medium-symbolic";
    return "microphone-sensitivity-high-symbolic";
}

static const gchar *ap_device_icon(const gchar *label, gboolean input) {
    gchar *l = g_ascii_strdown(label ? label : "", -1);
    const gchar *ic = input ? "audio-input-microphone-symbolic" : "audio-speakers-symbolic";
    if (strstr(l, "headphone") || strstr(l, "headset")) ic = "audio-headphones-symbolic";
    else if (strstr(l, "hdmi") || strstr(l, "displayport") || strstr(l, "display")) ic = "video-display-symbolic";
    else if (strstr(l, "bluetooth") || strstr(l, "bluez")) ic = "bluetooth-active-symbolic";
    else if (strstr(l, "usb")) ic = input ? "audio-input-microphone-symbolic" : "audio-card-symbolic";
    g_free(l);
    return ic;
}

/* ---------- output / input levels ---------- */
static void ap_sync_levels(AudioPopup *ap) {
    SystrayWidget *t = ap->tray;
    gboolean dragging = (g_get_monotonic_time() < t->pa_ui_change_until);
    ap->updating = TRUE;
    gdouble mx = pa_max_volume(t);
    GtkAdjustment *adj = gtk_range_get_adjustment(GTK_RANGE(ap->out_scale));
    if (gtk_adjustment_get_upper(adj) != mx) {
        gtk_range_set_range(GTK_RANGE(ap->out_scale), 0, mx);
        gtk_range_set_range(GTK_RANGE(ap->in_scale), 0, mx);
    }
    if (!dragging) {
        gtk_range_set_value(GTK_RANGE(ap->out_scale), t->pa_vol);
        gtk_range_set_value(GTK_RANGE(ap->in_scale), t->pa_mic);
    }
    gchar *p1 = g_strdup_printf("%d%%", t->pa_vol), *p2 = g_strdup_printf("%d%%", t->pa_mic);
    gtk_label_set_text(GTK_LABEL(ap->out_pct), p1);
    gtk_label_set_text(GTK_LABEL(ap->in_pct), p2);
    g_free(p1); g_free(p2);
    ap_set_btn_icon(ap->out_btn, ap_out_icon(t->pa_vol, t->pa_muted));
    ap_set_btn_icon(ap->in_btn, ap_in_icon(t->pa_mic, t->pa_mic_muted));
    gtk_widget_set_opacity(ap->out_scale, t->pa_muted ? 0.45 : 1.0);
    gtk_widget_set_opacity(ap->in_scale, t->pa_mic_muted ? 0.45 : 1.0);
    gtk_widget_set_tooltip_text(ap->out_btn, t->pa_muted ? "Unmute" : "Mute");
    gtk_widget_set_tooltip_text(ap->in_btn, t->pa_mic_muted ? "Unmute microphone" : "Mute microphone");
    gtk_widget_set_visible(ap->rec_row, t->pa_recording);
    ap->updating = FALSE;
}

static void ap_sync_device_names(AudioPopup *ap) {
    gchar *out_lbl = NULL, *in_lbl = NULL;
    GList *o = parse_pulse_port_choices("sh -c \"LC_ALL=C pactl list sinks\"", "Sink #",
                                        "sh -c \"LC_ALL=C pactl get-default-sink\"", FALSE, &out_lbl);
    GList *i = parse_pulse_port_choices("sh -c \"LC_ALL=C pactl list sources\"", "Source #",
                                        "sh -c \"LC_ALL=C pactl get-default-source\"", TRUE, &in_lbl);
    gtk_label_set_text(GTK_LABEL(ap->out_sub), out_lbl ? out_lbl : "Output");
    gtk_label_set_text(GTK_LABEL(ap->in_sub), in_lbl ? in_lbl : (i ? "Input" : "No microphone"));
    g_free(out_lbl); g_free(in_lbl);
    g_list_free_full(o, free_port_choice);
    g_list_free_full(i, free_port_choice);
}

/* While dragging only the label and icon change here; the panel icon is refreshed a moment
   later (it reads /proc and re-renders, which made every slider step stutter). */
static gboolean ap_panel_icon_later(gpointer d) {
    AudioPopup *ap = d;
    ap->panel_icon_id = 0;
    update_network_and_audio(ap->tray);
    return G_SOURCE_REMOVE;
}
static void ap_set_pct(GtkWidget *lbl, gint v) {
    gchar t[16];
    g_snprintf(t, sizeof t, "%d%%", v);
    gtk_label_set_text(GTK_LABEL(lbl), t);
}
static void on_ap_out_scale(GtkRange *r, AudioPopup *ap) {
    if (ap->updating) return;
    gint v = (gint)gtk_range_get_value(r);
    pa_queue_sink_volume(ap->tray, v);
    if (ap->tray->pa_muted && v > 0) { ap->tray->pa_muted = FALSE; pa_set_sink_mute(FALSE); gtk_widget_set_opacity(ap->out_scale, 1.0); }
    ap_set_pct(ap->out_pct, v);
    ap_set_btn_icon(ap->out_btn, ap_out_icon(v, ap->tray->pa_muted));
    if (!ap->panel_icon_id) ap->panel_icon_id = g_timeout_add(150, ap_panel_icon_later, ap);
}
static void on_ap_in_scale(GtkRange *r, AudioPopup *ap) {
    if (ap->updating) return;
    gint v = (gint)gtk_range_get_value(r);
    pa_queue_source_volume(ap->tray, v);
    if (ap->tray->pa_mic_muted && v > 0) { ap->tray->pa_mic_muted = FALSE; pa_set_source_mute(FALSE); gtk_widget_set_opacity(ap->in_scale, 1.0); }
    ap_set_pct(ap->in_pct, v);
    ap_set_btn_icon(ap->in_btn, ap_in_icon(v, ap->tray->pa_mic_muted));
}
static void on_ap_out_mute(GtkButton *b, AudioPopup *ap) {
    ap->tray->pa_muted = !ap->tray->pa_muted;
    pa_set_sink_mute(ap->tray->pa_muted);
    ap_sync_levels(ap);
    update_network_and_audio(ap->tray);
}
static void on_ap_in_mute(GtkButton *b, AudioPopup *ap) {
    ap->tray->pa_mic_muted = !ap->tray->pa_mic_muted;
    pa_set_source_mute(ap->tray->pa_mic_muted);
    ap_sync_levels(ap);
}
static gboolean on_ap_out_scroll(GtkWidget *w, GdkEventScroll *ev, AudioPopup *ap) {
    on_vol_scroll(w, ev, ap->tray);
    ap_sync_levels(ap);
    return TRUE;
}

/* ---------- device chooser page ---------- */
static void on_ap_back(GtkButton *b, AudioPopup *ap) {
    ap_stack_show(ap, "main");
}

static gboolean ap_names_later(gpointer d) {
    if (g_ap) ap_sync_device_names(g_ap);
    return G_SOURCE_REMOVE;
}

static void on_ap_device_clicked(GtkButton *b, AudioPopup *ap) {
    const gchar *dev  = g_object_get_data(G_OBJECT(b), "dev");
    const gchar *port = g_object_get_data(G_OBJECT(b), "port");
    gboolean input = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "is-input"));
    if (dev && dev[0]) {
        const gchar *script = input
            ? "pactl set-default-source \"$1\"; [ -n \"$2\" ] && pactl set-source-port \"$1\" \"$2\""
            : "pactl set-default-sink \"$1\"; [ -n \"$2\" ] && pactl set-sink-port \"$1\" \"$2\"";
        gchar *argv[] = { "sh", "-c", (gchar *)script, "sh", (gchar *)dev, (gchar *)(port ? port : ""), NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL);
    }
    on_ap_back(NULL, ap);
    pa_schedule_refresh(ap->tray);
    g_timeout_add(600, ap_names_later, NULL);
}

static void ap_show_devices(AudioPopup *ap, gboolean input) {
    GList *old = gtk_container_get_children(GTK_CONTAINER(ap->dev_list));
    for (GList *l = old; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);
    gtk_label_set_text(GTK_LABEL(ap->dev_title), input ? "Input Device" : "Output Device");

    GList *ch = input
        ? parse_pulse_port_choices("sh -c \"LC_ALL=C pactl list sources\"", "Source #",
                                   "sh -c \"LC_ALL=C pactl get-default-source\"", TRUE, NULL)
        : parse_pulse_port_choices("sh -c \"LC_ALL=C pactl list sinks\"", "Sink #",
                                   "sh -c \"LC_ALL=C pactl get-default-sink\"", FALSE, NULL);
    if (!ch) {
        GtkWidget *none = gtk_label_new(input ? "No input devices" : "No output devices");
        gtk_style_context_add_class(gtk_widget_get_style_context(none), "dim-label");
        gtk_box_pack_start(GTK_BOX(ap->dev_list), none, FALSE, FALSE, 12);
    }
    for (GList *l = ch; l; l = l->next) {
        AudioPortChoice *c = l->data;
        GtkWidget *b = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(b), "ap-row-btn");
        GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        GtkWidget *ic = gtk_image_new_from_icon_name(ap_device_icon(c->label, input), GTK_ICON_SIZE_BUTTON);
        GtkWidget *lb = gtk_label_new(c->label);
        gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lb), 1); /* fill allocation, never widen popup */
        gtk_box_pack_start(GTK_BOX(h), ic, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(h), lb, TRUE, TRUE, 0);
        if (c->active) gtk_box_pack_end(GTK_BOX(h), gtk_image_new_from_icon_name("object-select-symbolic", GTK_ICON_SIZE_BUTTON), FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b), h);
        g_object_set_data_full(G_OBJECT(b), "dev", g_strdup(c->sink_or_source_name), g_free);
        g_object_set_data_full(G_OBJECT(b), "port", g_strdup(c->port_name ? c->port_name : ""), g_free);
        g_object_set_data(G_OBJECT(b), "is-input", GINT_TO_POINTER(input));
        g_signal_connect(b, "clicked", G_CALLBACK(on_ap_device_clicked), ap);
        gtk_box_pack_start(GTK_BOX(ap->dev_list), b, FALSE, FALSE, 0);
    }
    g_list_free_full(ch, free_port_choice);
    gtk_widget_show_all(ap->dev_list);
    ap_stack_show(ap, "devices");
}
static void on_ap_out_more(GtkButton *b, AudioPopup *ap) { ap_show_devices(ap, FALSE); }
static void on_ap_in_more(GtkButton *b, AudioPopup *ap)  { ap_show_devices(ap, TRUE); }

static GtkWidget *ap_level_row(AudioPopup *ap, gboolean input) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *btn = ap_flat_icon_btn(input ? "microphone-sensitivity-high-symbolic" : "audio-volume-high-symbolic", 20);
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 150, 1);
    gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(sc), "ap-scale");
    gtk_scale_add_mark(GTK_SCALE(sc), 100, GTK_POS_BOTTOM, NULL);
    gtk_widget_set_size_request(sc, 250, -1);          /* fixed, wide track: smooth sliding */
    gtk_range_set_round_digits(GTK_RANGE(sc), 0);
    GtkWidget *sub = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(sub), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 1); /* fill allocation, never widen popup */
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "ap-sub");
    gtk_box_pack_start(GTK_BOX(mid), sc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), sub, FALSE, FALSE, 0);
    GtkWidget *pct = gtk_label_new("0%");
    gtk_label_set_width_chars(GTK_LABEL(pct), 4);
    gtk_label_set_xalign(GTK_LABEL(pct), 1.0);
    GtkWidget *more = ap_flat_icon_btn("go-next-symbolic", 16);
    gtk_widget_set_tooltip_text(more, input ? "Choose input device" : "Choose output device");

    gtk_box_pack_start(GTK_BOX(row), btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), mid, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), pct, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), more, FALSE, FALSE, 0);

    if (input) {
        ap->in_btn = btn; ap->in_scale = sc; ap->in_pct = pct; ap->in_sub = sub;
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_ap_in_scale), ap);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_ap_in_mute), ap);
        g_signal_connect(more, "clicked", G_CALLBACK(on_ap_in_more), ap);
    } else {
        ap->out_btn = btn; ap->out_scale = sc; ap->out_pct = pct; ap->out_sub = sub;
        g_signal_connect(sc, "value-changed", G_CALLBACK(on_ap_out_scale), ap);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_ap_out_mute), ap);
        g_signal_connect(more, "clicked", G_CALLBACK(on_ap_out_more), ap);
        gtk_widget_add_events(row, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
        g_signal_connect(btn, "scroll-event", G_CALLBACK(on_ap_out_scroll), ap);
    }
    return row;
}

/* ---------- per-app mixer ---------- */
static void on_ap_app_scale(GtkRange *r, AudioPopup *ap) {
    if (ap->updating) return;
    gchar *v = g_strdup_printf("%d", (gint)gtk_range_get_value(r));
    pa_app_apply(g_object_get_data(G_OBJECT(r), "pa-app-name"), v, g_object_get_data(G_OBJECT(r), "pa-input-id"), FALSE);
    g_free(v);
    ap->tray->pa_ui_change_until = g_get_monotonic_time() + 1500000;
}
static void on_ap_app_mute(GtkButton *b, AudioPopup *ap) {
    gboolean m = !GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "muted"));
    g_object_set_data(G_OBJECT(b), "muted", GINT_TO_POINTER(m));
    ap_set_btn_icon(GTK_WIDGET(b), m ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic");
    GtkWidget *sc = g_object_get_data(G_OBJECT(b), "scale");
    if (sc) gtk_widget_set_opacity(sc, m ? 0.45 : 1.0);
    pa_app_apply(g_object_get_data(G_OBJECT(b), "pa-app-name"), m ? "1" : "0", g_object_get_data(G_OBJECT(b), "pa-input-id"), TRUE);
}

static void ap_sync_apps(AudioPopup *ap) {
    GList *apps = pa_parse_apps(ap->tray->pa_txt_inputs);
    GString *sig = g_string_new("");
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = apps; l; l = l->next) {
        PaAppInfo *a = l->data;
        if (g_hash_table_contains(seen, a->name)) continue;
        g_hash_table_add(seen, a->name);
        g_string_append_printf(sig, "%s;", a->name);
    }
    g_hash_table_remove_all(seen);
    gboolean dragging = (g_get_monotonic_time() < ap->tray->pa_ui_change_until);

    if (g_strcmp0(sig->str, ap->apps_sig) != 0) {
        g_free(ap->apps_sig);
        ap->apps_sig = g_strdup(sig->str);
        GList *old = gtk_container_get_children(GTK_CONTAINER(ap->apps_box));
        for (GList *l = old; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
        g_list_free(old);
        g_hash_table_remove_all(ap->app_scales);
        dragging = FALSE;
        for (GList *l = apps; l; l = l->next) {
            PaAppInfo *a = l->data;
            if (g_hash_table_contains(seen, a->name)) continue;
            g_hash_table_add(seen, a->name);
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *ic = gtk_image_new_from_icon_name(a->icon, GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(ic), 18);
            GtkWidget *lb = gtk_label_new(a->name);
            gtk_label_set_xalign(GTK_LABEL(lb), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(lb), PANGO_ELLIPSIZE_END);
            gtk_label_set_width_chars(GTK_LABEL(lb), 10);
            gtk_label_set_max_width_chars(GTK_LABEL(lb), 10);
            GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, pa_max_volume(ap->tray), 1);
            gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
            gtk_style_context_add_class(gtk_widget_get_style_context(sc), "ap-scale");
            g_object_set_data_full(G_OBJECT(sc), "pa-app-name", g_strdup(a->name), g_free);
            g_object_set_data_full(G_OBJECT(sc), "pa-input-id", g_strdup(a->id), g_free);
            GtkWidget *mb = ap_flat_icon_btn(a->muted ? "audio-volume-muted-symbolic" : "audio-volume-high-symbolic", 16);
            g_object_set_data(G_OBJECT(mb), "muted", GINT_TO_POINTER(a->muted));
            g_object_set_data(G_OBJECT(mb), "scale", sc);
            g_object_set_data_full(G_OBJECT(mb), "pa-app-name", g_strdup(a->name), g_free);
            g_object_set_data_full(G_OBJECT(mb), "pa-input-id", g_strdup(a->id), g_free);
            g_signal_connect(mb, "clicked", G_CALLBACK(on_ap_app_mute), ap);
            g_signal_connect(sc, "value-changed", G_CALLBACK(on_ap_app_scale), ap);
            gtk_box_pack_start(GTK_BOX(row), ic, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), lb, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(row), sc, TRUE, TRUE, 0);
            gtk_box_pack_start(GTK_BOX(row), mb, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(ap->apps_box), row, FALSE, FALSE, 0);
            g_hash_table_insert(ap->app_scales, g_strdup(a->name), sc);
        }
        gtk_widget_show_all(ap->apps_box);
        g_hash_table_remove_all(seen);
    }
    if (!dragging) {
        ap->updating = TRUE;
        for (GList *l = apps; l; l = l->next) {
            PaAppInfo *a = l->data;
            if (g_hash_table_contains(seen, a->name)) continue;
            g_hash_table_add(seen, a->name);
            GtkWidget *sc = g_hash_table_lookup(ap->app_scales, a->name);
            if (sc) {
                gtk_range_set_value(GTK_RANGE(sc), a->vol);
                gtk_widget_set_opacity(sc, a->muted ? 0.45 : 1.0);
            }
        }
        ap->updating = FALSE;
    }
    gboolean any = (sig->len > 0);
    gtk_widget_set_visible(ap->apps_exp, any);
    gchar *t = any ? g_strdup_printf("Apps (%u)", g_hash_table_size(ap->app_scales)) : g_strdup("Apps");
    gtk_expander_set_label(GTK_EXPANDER(ap->apps_exp), t);
    g_free(t);
    g_hash_table_destroy(seen);
    g_string_free(sig, TRUE);
    g_list_free_full(apps, pa_app_free);

    /* player cards: refresh their app-volume sliders */
    for (GList *l = ap->players; l; l = l->next) {
        ApPlayer *p = l->data;
        if (!p->vol || !p->pa_name) continue;
        GtkWidget *sc = g_hash_table_lookup(ap->app_scales, p->pa_name);
        gtk_widget_set_visible(p->vol, sc != NULL);
        if (sc && !dragging) {
            ap->updating = TRUE;
            gtk_range_set_value(GTK_RANGE(p->vol), gtk_range_get_value(GTK_RANGE(sc)));
            ap->updating = FALSE;
        }
    }
}

/* ---------- now-playing cards ---------- */
static void ap_player_free(ApPlayer *p) {
    if (!p) return;
    if (p->cancel) { g_cancellable_cancel(p->cancel); g_object_unref(p->cancel); }
    if (p->player) { g_signal_handlers_disconnect_by_data(p->player, p); g_object_unref(p->player); }
    g_free(p->bus); g_free(p->id); g_free(p->identity); g_free(p->desktop);
    g_free(p->trackid); g_free(p->art_url); g_free(p->pa_name);
    g_free(p);
}

static void ap_player_call(ApPlayer *p, const gchar *method, GVariant *args) {
    if (p->player) g_dbus_proxy_call(p->player, method, args, G_DBUS_CALL_FLAGS_NONE, 1500, NULL, NULL, NULL);
}
static void on_ap_prev(GtkButton *b, ApPlayer *p) { ap_player_call(p, "Previous", NULL); }
static void on_ap_next(GtkButton *b, ApPlayer *p) { ap_player_call(p, "Next", NULL); }
static void on_ap_play(GtkButton *b, ApPlayer *p) { ap_player_call(p, "PlayPause", NULL); }

static void ap_set_art_pixbuf(ApPlayer *p, GdkPixbuf *pb) {
    if (!pb) return;
    gint w = gdk_pixbuf_get_width(pb), h = gdk_pixbuf_get_height(pb);
    gint s = MIN(w, h);
    GdkPixbuf *sq = gdk_pixbuf_new_subpixbuf(pb, (w - s) / 2, (h - s) / 2, s, s);
    GdkPixbuf *sc = gdk_pixbuf_scale_simple(sq, 64, 64, GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(p->art), sc);
    g_object_unref(sc); g_object_unref(sq);
}

static void ap_set_art_fallback(ApPlayer *p) {
    /* the player's own app icon (via its .desktop file), else a music note */
    GIcon *gi = NULL;
    const gchar *cands[] = { p->desktop, p->id, NULL };
    for (gint i = 0; cands[i] && !gi; i++) {
        if (!cands[i][0]) continue;
        gchar *df = g_strdup_printf("%s.desktop", cands[i]);
        GDesktopAppInfo *di = g_desktop_app_info_new(df);
        g_free(df);
        if (di) {
            GIcon *ic = g_app_info_get_icon(G_APP_INFO(di));
            if (ic) gi = g_object_ref(ic);
            g_object_unref(di);
        }
    }
    if (gi) { gtk_image_set_from_gicon(GTK_IMAGE(p->art), gi, GTK_ICON_SIZE_DIALOG); g_object_unref(gi); }
    else gtk_image_set_from_icon_name(GTK_IMAGE(p->art), "audio-x-generic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(p->art), 56);
}

static void on_ap_art_loaded(GObject *src, GAsyncResult *res, gpointer data) {
    ApPlayer *p = data;
    gchar *buf = NULL; gsize len = 0;
    if (!g_file_load_contents_finish(G_FILE(src), res, &buf, &len, NULL, NULL)) return;   /* cancelled or failed */
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
    if (gdk_pixbuf_loader_write(ld, (const guchar *)buf, len, NULL) && gdk_pixbuf_loader_close(ld, NULL)) {
        GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(ld);
        if (pb) ap_set_art_pixbuf(p, pb);
    } else {
        gdk_pixbuf_loader_close(ld, NULL);
    }
    g_object_unref(ld);
    g_free(buf);
}

static void ap_player_update(ApPlayer *p) {
    if (!p->player) return;
    p->updating = TRUE;
    GVariant *md = g_dbus_proxy_get_cached_property(p->player, "Metadata");
    gchar *title = NULL, *artist = NULL, *art = NULL;
    p->length_us = 0;
    if (md && g_variant_is_of_type(md, G_VARIANT_TYPE_VARDICT)) {
        GVariant *v;
        if ((v = g_variant_lookup_value(md, "xesam:title", G_VARIANT_TYPE_STRING))) { title = g_variant_dup_string(v, NULL); g_variant_unref(v); }
        if ((v = g_variant_lookup_value(md, "xesam:artist", G_VARIANT_TYPE_STRING_ARRAY))) {
            const gchar **as = g_variant_get_strv(v, NULL);
            if (as) { artist = g_strjoinv(", ", (gchar **)as); g_free(as); }
            g_variant_unref(v);
        }
        if ((v = g_variant_lookup_value(md, "mpris:artUrl", G_VARIANT_TYPE_STRING))) { art = g_variant_dup_string(v, NULL); g_variant_unref(v); }
        if ((v = g_variant_lookup_value(md, "mpris:length", NULL))) {
            if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) p->length_us = g_variant_get_int64(v);
            else if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64)) p->length_us = (gint64)g_variant_get_uint64(v);
            g_variant_unref(v);
        }
        if ((v = g_variant_lookup_value(md, "mpris:trackid", NULL))) {
            g_free(p->trackid);
            p->trackid = (g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH) || g_variant_is_of_type(v, G_VARIANT_TYPE_STRING))
                         ? g_variant_dup_string(v, NULL) : NULL;
            g_variant_unref(v);
        }
    }
    if (md) g_variant_unref(md);

    gtk_label_set_text(GTK_LABEL(p->title), (title && title[0]) ? title : (p->identity ? p->identity : p->id));
    gchar *sub = (artist && artist[0]) ? g_strdup_printf("%s · %s", artist, p->identity ? p->identity : p->id)
                                       : g_strdup(p->identity ? p->identity : p->id);
    gtk_label_set_text(GTK_LABEL(p->artist), sub);
    g_free(sub);

    if (g_strcmp0(art, p->art_url) != 0) {
        g_free(p->art_url);
        p->art_url = g_strdup(art);
        if (p->cancel) { g_cancellable_cancel(p->cancel); g_object_unref(p->cancel); }
        p->cancel = g_cancellable_new();
        ap_set_art_fallback(p);
        if (art && art[0]) {
            GFile *f = g_file_new_for_uri(art);
            g_file_load_contents_async(f, p->cancel, on_ap_art_loaded, p);
            g_object_unref(f);
        }
    }
    g_free(title); g_free(artist); g_free(art);

    GVariant *st = g_dbus_proxy_get_cached_property(p->player, "PlaybackStatus");
    p->playing = st && g_strcmp0(g_variant_get_string(st, NULL), "Playing") == 0;
    if (st) g_variant_unref(st);
    ap_set_btn_icon(p->b_play, p->playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");

    GVariant *cs = g_dbus_proxy_get_cached_property(p->player, "CanSeek");
    gboolean can_seek = cs ? g_variant_get_boolean(cs) : FALSE;
    if (cs) g_variant_unref(cs);
    gboolean show_seek = can_seek && p->length_us > 0;
    gtk_widget_set_visible(p->seek_box, show_seek);
    if (show_seek) {
        gtk_range_set_range(GTK_RANGE(p->seek), 0, MAX(1.0, p->length_us / 1e6));
        gchar *l = ap_fmt_time(p->length_us);
        gtk_label_set_text(GTK_LABEL(p->t_len), l);
        g_free(l);
    }
    GVariant *cn = g_dbus_proxy_get_cached_property(p->player, "CanGoNext");
    GVariant *cp = g_dbus_proxy_get_cached_property(p->player, "CanGoPrevious");
    gtk_widget_set_sensitive(p->b_next, cn ? g_variant_get_boolean(cn) : TRUE);
    gtk_widget_set_sensitive(p->b_prev, cp ? g_variant_get_boolean(cp) : TRUE);
    if (cn) g_variant_unref(cn);
    if (cp) g_variant_unref(cp);
    p->updating = FALSE;
}

static void on_ap_player_props(GDBusProxy *px, GVariant *changed, GStrv inval, ApPlayer *p) {
    ap_player_update(p);
}

static void on_ap_position(GObject *src, GAsyncResult *res, gpointer data) {
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, NULL);
    if (!r) return;
    ApPlayer *p = data;
    GVariant *v = NULL;
    g_variant_get(r, "(v)", &v);
    gint64 pos = (v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT64)) ? g_variant_get_int64(v) : -1;
    if (v) g_variant_unref(v);
    g_variant_unref(r);
    if (!g_ap || pos < 0 || !g_list_find(g_ap->players, p)) return;
    if (g_get_monotonic_time() < p->user_seek_until) return;
    p->updating = TRUE;
    gtk_range_set_value(GTK_RANGE(p->seek), pos / 1e6);
    gchar *t = ap_fmt_time(pos);
    gtk_label_set_text(GTK_LABEL(p->t_pos), t);
    g_free(t);
    p->updating = FALSE;
}

static gboolean ap_pos_tick(gpointer data) {
    AudioPopup *ap = data;
    if (!ap->win || !gtk_widget_get_visible(ap->win)) { ap->pos_timer = 0; return G_SOURCE_REMOVE; }
    for (GList *l = ap->players; l; l = l->next) {
        ApPlayer *p = l->data;
        if (!p->player || !gtk_widget_get_visible(p->seek_box)) continue;
        g_dbus_connection_call(ap->tray->session_bus, p->bus, "/org/mpris/MediaPlayer2",
            "org.freedesktop.DBus.Properties", "Get",
            g_variant_new("(ss)", "org.mpris.MediaPlayer2.Player", "Position"),
            G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 800, NULL, on_ap_position, p);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_ap_seek_change(GtkRange *r, GtkScrollType sc, gdouble val, ApPlayer *p) {
    if (p->updating || !p->player) return FALSE;
    p->user_seek_until = g_get_monotonic_time() + 1500000;
    gint64 us = (gint64)(CLAMP(val, 0, p->length_us / 1e6) * 1e6);
    if (p->trackid && g_variant_is_object_path(p->trackid))
        ap_player_call(p, "SetPosition", g_variant_new("(ox)", p->trackid, us));
    gchar *t = ap_fmt_time(us);
    gtk_label_set_text(GTK_LABEL(p->t_pos), t);
    g_free(t);
    return FALSE;
}

static void on_ap_player_vol(GtkRange *r, ApPlayer *p) {
    if (p->ap->updating || !p->pa_name) return;
    GtkWidget *sc = g_hash_table_lookup(p->ap->app_scales, p->pa_name);
    if (sc) gtk_range_set_value(GTK_RANGE(sc), gtk_range_get_value(r));   /* same path as the Apps list */
}

static gboolean ap_icontains(const gchar *hay, const gchar *needle) {
    gchar *h = g_ascii_strdown(hay, -1), *n = g_ascii_strdown(needle, -1);
    gboolean r = strstr(h, n) != NULL;
    g_free(h); g_free(n);
    return r;
}

/* Which PulseAudio app name belongs to this player? */
static gchar *ap_match_pa_name(AudioPopup *ap, ApPlayer *p) {
    GList *apps = pa_parse_apps(ap->tray->pa_txt_inputs);
    gchar *found = NULL;
    for (GList *l = apps; l && !found; l = l->next) {
        PaAppInfo *a = l->data;
        if ((p->identity && g_ascii_strcasecmp(a->name, p->identity) == 0) ||
            (p->id && (g_ascii_strcasecmp(a->name, p->id) == 0 || g_ascii_strcasecmp(a->icon, p->id) == 0)) ||
            (p->desktop && g_ascii_strcasecmp(a->icon, p->desktop) == 0) ||
            (p->id && ap_icontains(a->name, p->id)))
            found = g_strdup(a->name);
    }
    g_list_free_full(apps, pa_app_free);
    return found;
}

static GtkWidget *ap_build_card(AudioPopup *ap, ApPlayer *p) {
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "ap-card");
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    p->art = gtk_image_new();
    gtk_widget_set_size_request(p->art, 64, 64);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->art), "ap-art");
    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_valign(txt, GTK_ALIGN_CENTER);
    p->title = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->title), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(p->title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->title), 1); /* fill allocation, never widen popup */
    gtk_style_context_add_class(gtk_widget_get_style_context(p->title), "ap-title");
    p->artist = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(p->artist), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(p->artist), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(p->artist), 1); /* fill allocation, never widen popup */
    gtk_style_context_add_class(gtk_widget_get_style_context(p->artist), "dim-label");
    gtk_box_pack_start(GTK_BOX(txt), p->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(txt), p->artist, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), p->art, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), txt, TRUE, TRUE, 0);

    p->seek_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    p->t_pos = gtk_label_new("0:00");
    p->t_len = gtk_label_new("0:00");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->t_pos), "ap-time");
    gtk_style_context_add_class(gtk_widget_get_style_context(p->t_len), "ap-time");
    p->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
    gtk_scale_set_draw_value(GTK_SCALE(p->seek), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->seek), "ap-scale");
    g_signal_connect(p->seek, "change-value", G_CALLBACK(on_ap_seek_change), p);
    gtk_box_pack_start(GTK_BOX(p->seek_box), p->t_pos, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(p->seek_box), p->seek, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(p->seek_box), p->t_len, FALSE, FALSE, 0);

    GtkWidget *ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    p->b_prev = ap_flat_icon_btn("media-skip-backward-symbolic", 18);
    p->b_play = ap_flat_icon_btn("media-playback-start-symbolic", 24);
    p->b_next = ap_flat_icon_btn("media-skip-forward-symbolic", 18);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->b_play), "ap-play");
    g_signal_connect(p->b_prev, "clicked", G_CALLBACK(on_ap_prev), p);
    g_signal_connect(p->b_play, "clicked", G_CALLBACK(on_ap_play), p);
    g_signal_connect(p->b_next, "clicked", G_CALLBACK(on_ap_next), p);
    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(btns), p->b_prev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), p->b_play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), p->b_next, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ctl), btns, TRUE, TRUE, 0);

    p->vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, pa_max_volume(ap->tray), 1);
    gtk_scale_set_draw_value(GTK_SCALE(p->vol), FALSE);
    gtk_widget_set_size_request(p->vol, 110, -1);
    gtk_style_context_add_class(gtk_widget_get_style_context(p->vol), "ap-scale");
    gtk_widget_set_tooltip_text(p->vol, "This app's volume");
    gtk_widget_set_no_show_all(p->vol, TRUE);
    g_signal_connect(p->vol, "value-changed", G_CALLBACK(on_ap_player_vol), p);
    gtk_box_pack_end(GTK_BOX(ctl), p->vol, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(card), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), p->seek_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), ctl, FALSE, FALSE, 0);
    return card;
}

static void on_ap_open_offline(GtkButton *b, gpointer d) {
    const gchar *id = g_object_get_data(G_OBJECT(b), "player-id");
    gchar *desk = g_strdup_printf("%s.desktop", id);
    GDesktopAppInfo *info = g_desktop_app_info_new(desk);
    if (!info) { gchar *low = g_ascii_strdown(desk, -1); info = g_desktop_app_info_new(low); g_free(low); }
    if (info) { g_app_info_launch(G_APP_INFO(info), NULL, NULL, NULL); g_object_unref(info); }
    else { gchar *argv[] = { (gchar *)id, NULL }; g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL); }
    g_free(desk);
    if (g_ap && g_ap->tray->vol_item) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_ap->tray->vol_item->button), FALSE);
}

static void on_ap_dot(GtkButton *b, AudioPopup *ap) {
    gtk_stack_set_visible_child_name(GTK_STACK(ap->players_stack), g_object_get_data(G_OBJECT(b), "page"));
    GList *dots = gtk_container_get_children(GTK_CONTAINER(ap->dots_box));
    for (GList *l = dots; l; l = l->next) {
        GtkStyleContext *c = gtk_widget_get_style_context(GTK_WIDGET(l->data));
        if (l->data == (gpointer)b) gtk_style_context_add_class(c, "ap-dot-on");
        else gtk_style_context_remove_class(c, "ap-dot-on");
    }
    g_list_free(dots);
}

static gboolean on_ap_players_scroll(GtkWidget *w, GdkEventScroll *ev, AudioPopup *ap) {
    GList *dots = gtk_container_get_children(GTK_CONTAINER(ap->dots_box));
    gint n = g_list_length(dots), cur = 0, i = 0;
    for (GList *l = dots; l; l = l->next, i++)
        if (gtk_style_context_has_class(gtk_widget_get_style_context(GTK_WIDGET(l->data)), "ap-dot-on")) cur = i;
    gint dir = (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT) ? 1 :
               (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT) ? -1 : 0;
    if (ev->direction == GDK_SCROLL_SMOOTH) dir = (ev->delta_y + ev->delta_x > 0.5) ? 1 : (ev->delta_y + ev->delta_x < -0.5) ? -1 : 0;
    gint nx = CLAMP(cur + dir, 0, n - 1);
    if (n > 1 && nx != cur) on_ap_dot(GTK_BUTTON(g_list_nth_data(dots, nx)), ap);
    g_list_free(dots);
    return dir != 0;
}

static gint ap_player_sort(gconstpointer a, gconstpointer b) {
    const ApPlayer *x = a, *y = b;
    return (y->playing ? 1 : 0) - (x->playing ? 1 : 0);
}

static void ap_rebuild_players(AudioPopup *ap) {
    SystrayWidget *t = ap->tray;
    GList *old = gtk_container_get_children(GTK_CONTAINER(ap->players_stack));
    for (GList *l = old; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);
    old = gtk_container_get_children(GTK_CONTAINER(ap->dots_box));
    for (GList *l = old; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);
    g_list_free_full(ap->players, (GDestroyNotify)ap_player_free);
    ap->players = NULL;

    UltimateShell *sh = t->shell;
    if (!t->session_bus || (sh && !sh->audio_mpris_enabled)) { gtk_widget_hide(ap->players_frame); return; }

    GList *running_ids = NULL;
    GVariant *names = g_dbus_connection_call_sync(t->session_bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "ListNames", NULL, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 500, NULL, NULL);
    if (names) {
        GVariantIter *it = NULL; const gchar *nm;
        g_variant_get(names, "(as)", &it);
        while (g_variant_iter_next(it, "&s", &nm)) {
            if (!g_str_has_prefix(nm, "org.mpris.MediaPlayer2.")) continue;
            gchar *id = mpris_player_id(nm);
            if (mpris_is_helper(id) || (sh && is_token_in_semicolon_list(sh->audio_ignored_players, id))) { g_free(id); continue; }
            ApPlayer *p = g_new0(ApPlayer, 1);
            p->ap = ap; p->bus = g_strdup(nm); p->id = id;
            p->player = g_dbus_proxy_new_sync(t->session_bus, G_DBUS_PROXY_FLAGS_GET_INVALIDATED_PROPERTIES, NULL,
                                              nm, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player", NULL, NULL);
            GDBusProxy *root = g_dbus_proxy_new_sync(t->session_bus, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL,
                                              nm, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2", NULL, NULL);
            if (root) {
                GVariant *v = g_dbus_proxy_get_cached_property(root, "Identity");
                if (v) { p->identity = g_variant_dup_string(v, NULL); g_variant_unref(v); }
                v = g_dbus_proxy_get_cached_property(root, "DesktopEntry");
                if (v) { p->desktop = g_variant_dup_string(v, NULL); g_variant_unref(v); }
                g_object_unref(root);
            }
            GVariant *st = p->player ? g_dbus_proxy_get_cached_property(p->player, "PlaybackStatus") : NULL;
            p->playing = st && g_strcmp0(g_variant_get_string(st, NULL), "Playing") == 0;
            if (st) g_variant_unref(st);
            ap->players = g_list_append(ap->players, p);
            running_ids = g_list_append(running_ids, g_strdup(id));
        }
        g_variant_iter_free(it);
        g_variant_unref(names);
    }
    ap->players = g_list_sort(ap->players, ap_player_sort);

    gint page = 0;
    for (GList *l = ap->players; l; l = l->next, page++) {
        ApPlayer *p = l->data;
        p->card = ap_build_card(ap, p);
        p->pa_name = ap_match_pa_name(ap, p);
        if (p->player) g_signal_connect(p->player, "g-properties-changed", G_CALLBACK(on_ap_player_props), p);
        gchar *pn = g_strdup_printf("p%d", page);
        gtk_stack_add_named(GTK_STACK(ap->players_stack), p->card, pn);
        g_free(pn);
        ap_player_update(p);
    }

    /* persistent players that are not running: small "Open" cards */
    if (sh && sh->audio_persistent_players) {
        gchar **toks = g_strsplit(sh->audio_persistent_players, ";", -1);
        for (gint i = 0; toks && toks[i]; i++) {
            gchar *id = g_strstrip(toks[i]);
            if (!id[0] || is_token_in_semicolon_list(sh->audio_ignored_players, id)) continue;
            gboolean run = FALSE;
            for (GList *r = running_ids; r; r = r->next) if (g_ascii_strcasecmp(r->data, id) == 0) run = TRUE;
            if (run) continue;
            GtkWidget *card = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_style_context_add_class(gtk_widget_get_style_context(card), "ap-card");
            GtkWidget *ic = gtk_image_new_from_icon_name(id, GTK_ICON_SIZE_DIALOG);
            gtk_image_set_pixel_size(GTK_IMAGE(ic), 40);
            const gchar *last = strrchr(id, '.');
            gchar *nm = g_strdup(last ? last + 1 : id);
            if (nm[0]) nm[0] = g_ascii_toupper(nm[0]);
            gchar *lbl = g_strdup_printf("Open %s", nm);
            GtkWidget *b = gtk_button_new_with_label(lbl);
            g_free(lbl); g_free(nm);
            g_object_set_data_full(G_OBJECT(b), "player-id", g_strdup(id), g_free);
            g_signal_connect(b, "clicked", G_CALLBACK(on_ap_open_offline), NULL);
            gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
            gtk_box_pack_start(GTK_BOX(card), ic, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(card), b, TRUE, TRUE, 0);
            gchar *pn = g_strdup_printf("p%d", page++);
            gtk_stack_add_named(GTK_STACK(ap->players_stack), card, pn);
            g_free(pn);
        }
        g_strfreev(toks);
    }
    g_list_free_full(running_ids, g_free);

    for (gint i = 0; i < page && page > 1; i++) {
        GtkWidget *d = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(d), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(d), "ap-dot");
        if (i == 0) gtk_style_context_add_class(gtk_widget_get_style_context(d), "ap-dot-on");
        g_object_set_data_full(G_OBJECT(d), "page", g_strdup_printf("p%d", i), g_free);
        g_signal_connect(d, "clicked", G_CALLBACK(on_ap_dot), ap);
        gtk_box_pack_start(GTK_BOX(ap->dots_box), d, FALSE, FALSE, 0);
    }
    gtk_widget_set_visible(ap->players_frame, page > 0);
    if (page > 0) {
        gtk_widget_show_all(ap->players_frame);   /* frame has no no-show-all, so this works */
        for (GList *l = ap->players; l; l = l->next) ap_player_update(l->data);   /* re-hide seek bars */
        gtk_widget_set_visible(ap->dots_box, page > 1);
        gtk_stack_set_visible_child_name(GTK_STACK(ap->players_stack), "p0");
    }
}

static gboolean ap_rebuild_idle(gpointer d) {
    AudioPopup *ap = d;
    ap->rebuild_id = 0;
    if (ap->win && gtk_widget_get_visible(ap->win)) { ap_rebuild_players(ap); ap_sync_apps(ap); }
    return G_SOURCE_REMOVE;
}

static void on_ap_mpris_owner(GDBusConnection *c, const gchar *snd, const gchar *path, const gchar *iface,
                              const gchar *sig, GVariant *params, gpointer d) {
    AudioPopup *ap = d;
    if (!ap->rebuild_id) ap->rebuild_id = g_timeout_add(350, ap_rebuild_idle, ap);
}

/* ---------- window ---------- */
static void on_ap_settings(GtkButton *b, AudioPopup *ap) {
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ap->tray->vol_item->button), FALSE);
    GtkWidget *top = gtk_widget_get_toplevel(GTK_WIDGET(b));
    if (GTK_IS_WINDOW(top) && top != ap->win) shell_popup_hide(top);       /* opened from Quick Settings */
    if (ap->tray->shell && ap->tray->shell->plugin) g_signal_emit_by_name(ap->tray->shell->plugin, "configure-plugin");
}
static void on_ap_mixer(GtkButton *b, AudioPopup *ap) { on_open_pavucontrol(NULL, ap->tray); }

static void on_ap_unmap(GtkWidget *w, AudioPopup *ap) {
    if (ap->pos_timer) { g_source_remove(ap->pos_timer); ap->pos_timer = 0; }
    if (!g_ap_in_qs) gtk_stack_set_visible_child_name(GTK_STACK(ap->stack), "main");
}

static void ap_add_css(GtkWidget *win) {
    static GtkCssProvider *css = NULL;
    if (!css) {
        css = gtk_css_provider_new();
        gtk_css_provider_load_from_data(css,
            ".ap-card { background-color: alpha(@theme_fg_color, 0.06); border-radius: 12px; padding: 10px; }"
            ".ap-title { font-weight: bold; }"
            ".ap-sub { font-size: smaller; }"
            ".ap-time { font-size: smaller; font-feature-settings: \"tnum\"; opacity: 0.7; }"
            ".ap-icon-btn { padding: 4px; min-width: 0; min-height: 0; border-radius: 99px; }"
            ".ap-play { background-color: alpha(@theme_fg_color, 0.10); padding: 8px; }"
            ".ap-scale trough { min-height: 6px; border-radius: 99px; }"
            ".ap-scale highlight { border-radius: 99px; }"
            ".ap-scale slider { min-width: 14px; min-height: 14px; margin: -5px; border-radius: 99px; }"
            ".ap-row-btn { padding: 6px 8px; border-radius: 8px; }"
            ".ap-dot { min-width: 7px; min-height: 7px; padding: 0; margin: 0 3px; border-radius: 99px;"
            "  background-color: alpha(@theme_fg_color, 0.35); background-image: none; border: none; box-shadow: none; }"
            ".ap-dot-on { min-width: 22px; background-color: @theme_selected_bg_color; }"
            ".ap-rec { color: #e5484d; font-weight: bold; }", -1, NULL);
    }
    gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(win), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

static AudioPopup *ap_create(SystrayWidget *tray) {
    AudioPopup *ap = g_new0(AudioPopup, 1);
    ap->tray = tray;
    ap->app_scales = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ap->win = shell_create_popup_window(tray->vol_item->button, 360, -1, FALSE);
    ap_add_css(ap->win);
    g_signal_connect(ap->win, "unmap", G_CALLBACK(on_ap_unmap), ap);

    ap->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ap->stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(ap->stack), 180);
    /* Both pages keep the main page's size: switching to the device list must not
       shrink / grow the popup (it jumped around); a long device list scrolls instead. */
    gtk_stack_set_hhomogeneous(GTK_STACK(ap->stack), TRUE);
    gtk_stack_set_vhomogeneous(GTK_STACK(ap->stack), TRUE);

    /* main page */
    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(main), 12);
    gtk_widget_set_size_request(main, 380, -1);
    gtk_box_pack_start(GTK_BOX(main), ap_level_row(ap, FALSE), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main), ap_level_row(ap, TRUE), FALSE, FALSE, 0);

    ap->rec_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ri = gtk_image_new_from_icon_name("media-record-symbolic", GTK_ICON_SIZE_MENU);
    ap->rec_lbl = gtk_label_new("The microphone is being used");
    gtk_style_context_add_class(gtk_widget_get_style_context(ap->rec_lbl), "ap-rec");
    gtk_box_pack_start(GTK_BOX(ap->rec_row), ri, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ap->rec_row), ap->rec_lbl, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(ap->rec_row, TRUE);
    gtk_widget_show(ri); gtk_widget_show(ap->rec_lbl);
    gtk_box_pack_start(GTK_BOX(main), ap->rec_row, FALSE, FALSE, 0);

    ap->players_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *pev = gtk_event_box_new();
    gtk_widget_add_events(pev, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(pev, "scroll-event", G_CALLBACK(on_ap_players_scroll), ap);
    ap->players_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ap->players_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_container_add(GTK_CONTAINER(pev), ap->players_stack);
    ap->dots_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_halign(ap->dots_box, GTK_ALIGN_CENTER);
    {
        GtkWidget *psep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
        g_object_set_data(G_OBJECT(ap->players_frame), "players-sep", psep);   /* hidden in Quick Settings */
        gtk_box_pack_start(GTK_BOX(ap->players_frame), psep, FALSE, FALSE, 2);
    }
    gtk_box_pack_start(GTK_BOX(ap->players_frame), pev, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ap->players_frame), ap->dots_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main), ap->players_frame, FALSE, FALSE, 0);

    ap->apps_exp = gtk_expander_new("Apps");
    ap->apps_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(ap->apps_box, 6);
    gtk_container_add(GTK_CONTAINER(ap->apps_exp), ap->apps_box);
    gtk_widget_show(ap->apps_box);
    gtk_box_pack_start(GTK_BOX(main), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main), ap->apps_exp, FALSE, FALSE, 0);

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *bset = gtk_button_new_with_label("Shell Sound Settings…");
    GtkWidget *bmix = gtk_button_new_with_label("Mixer…");
    gtk_button_set_relief(GTK_BUTTON(bset), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(bmix), GTK_RELIEF_NONE);
    g_signal_connect(bset, "clicked", G_CALLBACK(on_ap_settings), ap);
    g_signal_connect(bmix, "clicked", G_CALLBACK(on_ap_mixer), ap);
    gtk_box_pack_start(GTK_BOX(foot), bset, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), bmix, FALSE, FALSE, 0);
    ap->foot_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(main), ap->foot_sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(main), foot, FALSE, FALSE, 0);
    ap->foot = foot;

    /* device page */
    GtkWidget *dev = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(dev), 12);
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *back = ap_flat_icon_btn("go-previous-symbolic", 16);
    g_signal_connect(back, "clicked", G_CALLBACK(on_ap_back), ap);
    ap->dev_title = gtk_label_new("Output Device");
    gtk_style_context_add_class(gtk_widget_get_style_context(ap->dev_title), "ap-title");
    gtk_box_pack_start(GTK_BOX(hdr), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), ap->dev_title, FALSE, FALSE, 0);
    ap->dev_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *dev_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(dev_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(dev_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(dev_scroll), ap->dev_list);
    gtk_box_pack_start(GTK_BOX(dev), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dev), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(dev), dev_scroll, TRUE, TRUE, 0);

    gtk_stack_add_named(GTK_STACK(ap->stack), main, "main");
    gtk_stack_add_named(GTK_STACK(ap->stack), dev, "devices");
    ap->main_page = main; ap->dev_page = dev;
    gtk_container_add(GTK_CONTAINER(ap->win), ap->stack);

    if (tray->session_bus)
        ap->name_sub = g_dbus_connection_signal_subscribe(tray->session_bus, "org.freedesktop.DBus",
            "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", "org.mpris.MediaPlayer2",
            G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE, on_ap_mpris_owner, ap, NULL);
    return ap;
}

/* called after every PulseAudio refresh */
static void qs_on_pa_state(SystrayWidget *t);
static void ap_on_pa_state(SystrayWidget *tray) {
    qs_on_pa_state(tray);
    if (!g_ap || !g_ap->stack || !gtk_widget_get_mapped(g_ap->stack)) return;   /* own popup or Quick Settings */
    ap_sync_levels(g_ap);
    ap_sync_apps(g_ap);
}

static void ap_toggle(SystrayWidget *tray, GtkToggleButton *btn) {
    if (!g_ap) g_ap = ap_create(tray);
    if (gtk_toggle_button_get_active(btn)) {
        gtk_stack_set_visible_child_name(GTK_STACK(g_ap->stack), "main");
        gtk_widget_show_all(g_ap->stack);
        g_free(g_ap->apps_sig); g_ap->apps_sig = NULL;   /* rebuild the app list */
        ap_sync_device_names(g_ap);
        ap_rebuild_players(g_ap);
        ap_sync_levels(g_ap);
        ap_sync_apps(g_ap);
        pa_refresh_async(tray);
        if (!g_ap->pos_timer) g_ap->pos_timer = g_timeout_add(1000, ap_pos_tick, g_ap);
        gtk_window_resize(GTK_WINDOW(g_ap->win), 1, 1);
    }
    shell_toggle_popup_window(btn, g_ap->win);
    if (gtk_toggle_button_get_active(btn)) {
        /* show_all above re-showed hidden parts: put them back */
        ap_sync_levels(g_ap);
        ap_sync_apps(g_ap);
        for (GList *l = g_ap->players; l; l = l->next) ap_player_update(l->data);
        GList *kids = gtk_container_get_children(GTK_CONTAINER(g_ap->players_stack));
        gtk_widget_set_visible(g_ap->players_frame, kids != NULL);
        gtk_widget_set_visible(g_ap->dots_box, g_list_length(kids) > 1);
        g_list_free(kids);
    }
}

/* =========================================================================
 * Quick Settings (Windows 11 style)
 *   [ media card - only while something plays ]
 *   [ tiles, 3 columns: Network › · Bluetooth › · Airplane · Do Not Disturb · Dark · Night · Caffeine ]
 *   [ brightness / volume › / microphone › ]
 *   [ 🔋 94%                             ✎  ⚙ ]
 * The detail pages behind › are the old popups' own content (network list, Bluetooth, sound devices &
 * apps, power), moved in while the option is on and moved back when it is switched off.
 * ========================================================================= */
typedef struct {
    SystrayWidget *tray;
    GtkWidget *win, *stack, *main;
    GtkWidget *media_card, *media_slot;
    GtkWidget *net_tile, *net_icon, *net_lbl, *bt_cell, *bt_tile, *bt_lbl;
    GtkWidget *dnd_tile, *dark_tile, *night_tile, *caf_tile, *air_tile;
    GtkWidget *bright_row, *vol_btn, *mic_btn, *audio_slot, *grid, *cells[8];
    gint       ncells;
    GtkWidget *bat_btn, *bat_icon, *bat_lbl;
    GtkWidget *sound_slot, *net_slot, *bt_slot, *bat_slot;
    GtkWidget *net_content, *bt_content, *bat_content;
    GtkWidget *players_home; gint players_pos;      /* where the players card lives in the sound popup */
    gboolean   night_on, caf_on, updating;
    gchar     *net_kind;                            /* "wifi", "ethernet" or NULL (disconnected) */
} QuickSettings;
static QuickSettings *g_qs = NULL;

static void qs_sync_toggles(QuickSettings *q);
static gboolean qs_enabled(SystrayWidget *t) { return t && t->shell && t->shell->tray_quick_settings; }

static void qs_css(void) {
    static GtkCssProvider *css = NULL;
    if (css) return;
    css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        ".qs-card { background-color: alpha(@theme_fg_color, 0.05); border-radius: 8px; padding: 14px; }"
        ".qs-tile { border-radius: 6px; background-color: alpha(@theme_fg_color, 0.09); border: 1px solid alpha(@theme_fg_color, 0.06); }"
        ".qs-tile button { background: transparent; border: none; box-shadow: none; border-radius: 6px; min-height: 44px; padding: 0 6px; }"
        ".qs-tile button:hover { background-color: alpha(@theme_fg_color, 0.07); }"
        ".qs-tile .qs-split { border-left: 1px solid alpha(@theme_fg_color, 0.14); border-radius: 0 6px 6px 0; }"
        ".qs-tile.qs-on { background-color: @theme_selected_bg_color; border-color: transparent; }"
        ".qs-tile.qs-on image { color: @theme_selected_fg_color; }"
        ".qs-tile.qs-on .qs-split { border-left-color: alpha(@theme_selected_fg_color, 0.30); }"
        ".qs-name { font-size: smaller; }"
        ".qs-foot button { min-width: 0; padding: 6px; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_USER + 120);
}

/* a tile with its name underneath; with more_cb it is split: [icon | ›] */
static GtkWidget *qs_tile(QuickSettings *q, const gchar *icon, const gchar *name, GCallback cb, GCallback more_cb,
                          GtkWidget **tile_out, GtkWidget **icon_out, GtkWidget **name_out) {
    GtkWidget *cell = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(tile), "qs-tile");
    gtk_widget_set_size_request(tile, 96, -1);
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    gtk_container_add(GTK_CONTAINER(b), img);
    g_signal_connect(b, "clicked", cb, q);
    gtk_box_pack_start(GTK_BOX(tile), b, TRUE, TRUE, 0);
    if (more_cb) {
        GtkWidget *m = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(m), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(m), "qs-split");
        g_signal_connect(m, "clicked", more_cb, q);
        gtk_box_pack_start(GTK_BOX(tile), m, TRUE, TRUE, 0);
    }
    GtkWidget *lbl = gtk_label_new(name);
    gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "qs-name");
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 13);
    gtk_box_pack_start(GTK_BOX(cell), tile, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cell), lbl, FALSE, FALSE, 0);
    if (tile_out) *tile_out = tile;
    if (icon_out) *icon_out = img;
    if (name_out) *name_out = lbl;
    return cell;
}
static void qs_tile_set(GtkWidget *tile, gboolean on) {
    if (!tile) return;
    GtkStyleContext *c = gtk_widget_get_style_context(tile);
    if (on) gtk_style_context_add_class(c, "qs-on"); else gtk_style_context_remove_class(c, "qs-on");
}

/* ---- network state: Wi-Fi, Ethernet or disconnected ---- */
static gboolean qs_has_wifi_device(void) {
    gchar *out = NULL; gboolean has = FALSE;
    if (g_spawn_command_line_sync("nmcli -t -f TYPE dev", &out, NULL, NULL, NULL) && out) has = (strstr(out, "wifi") != NULL);
    g_free(out);
    return has;
}
static gboolean qs_wifi_enabled(void) {
    gchar *out = NULL; gboolean on = FALSE;
    if (g_spawn_command_line_sync("nmcli radio wifi", &out, NULL, NULL, NULL) && out) on = (strstr(out, "enabled") != NULL);
    g_free(out);
    return on;
}
/* the connection in use: name + kind */
static gchar *qs_net_active(gchar **kind) {
    gchar *out = NULL, *name = NULL;
    *kind = NULL;
    if (g_spawn_command_line_sync("nmcli -t -f TYPE,STATE,CONNECTION dev", &out, NULL, NULL, NULL) && out) {
        gchar **ln = g_strsplit(out, "\n", -1);
        for (gint pass = 0; pass < 2 && !name; pass++)          /* Wi-Fi first, then Ethernet */
            for (gint i = 0; ln[i] && !name; i++) {
                gchar **f = g_strsplit(ln[i], ":", 3);
                if (f[0] && f[1] && f[2] && g_str_has_prefix(f[1], "connected") &&
                    g_strcmp0(f[0], pass == 0 ? "wifi" : "ethernet") == 0) {
                    name = g_strdup(f[2]); *kind = g_strdup(f[0]);
                }
                g_strfreev(f);
            }
        g_strfreev(ln);
    }
    g_free(out);
    return name;
}
static gboolean qs_airplane_on(void) {
    gchar *out = NULL; gboolean off = FALSE;
    if (g_spawn_command_line_sync("nmcli radio all", &out, NULL, NULL, NULL) && out) off = (strstr(out, "enabled") == NULL);
    g_free(out);
    return off;
}

static void qs_set_night_light(gboolean on) {
    const gchar *g = on ? "1:0.86:0.72" : "1:1:1";
    /* every connected output; run through sh with argv, so no extra quoting is needed */
    gchar *script = g_strdup_printf("for o in $(xrandr | awk '/ connected/{print $1}'); do xrandr --output \"$o\" --gamma %s; done", g);
    gchar *argv[] = { "sh", "-c", script, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(script);
}
static void qs_set_caffeine(gboolean on) {
    XfconfChannel *pm = xfconf_channel_get("xfce4-power-manager");
    if (pm) xfconf_channel_set_bool(pm, "/xfce4-power-manager/presentation-mode", on);
    g_spawn_command_line_async(on ? "xset s off -dpms" : "xset s on +dpms", NULL);
}


static void qs_sync_toggles(QuickSettings *q) {
    ShellPalette p; shell_palette_get(q->tray->shell, &p);
    qs_tile_set(q->dnd_tile, notifications_get_dnd());
    qs_tile_set(q->dark_tile, p.dark);
    qs_tile_set(q->night_tile, q->night_on);
    XfconfChannel *pm = xfconf_channel_get("xfce4-power-manager");
    if (pm) q->caf_on = xfconf_channel_get_bool(pm, "/xfce4-power-manager/presentation-mode", q->caf_on);
    qs_tile_set(q->caf_tile, q->caf_on);
    qs_tile_set(q->air_tile, qs_airplane_on());
}

static void qs_sync(QuickSettings *q) {
    SystrayWidget *t = q->tray;
    q->updating = TRUE;
    /* network tile */
    gchar *kind = NULL, *name = qs_net_active(&kind);
    g_free(q->net_kind); q->net_kind = kind;
    gboolean has_wifi = qs_has_wifi_device();
    gboolean on = kind != NULL || (has_wifi && qs_wifi_enabled());
    qs_tile_set(q->net_tile, on);
    const gchar *icon = !kind ? (has_wifi ? "network-wireless-offline-symbolic" : "network-offline-symbolic")
                      : g_strcmp0(kind, "wifi") == 0 ? "network-wireless-symbolic" : "network-wired-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(q->net_icon), icon, GTK_ICON_SIZE_MENU);
    gtk_label_set_text(GTK_LABEL(q->net_lbl), name ? name : (has_wifi && qs_wifi_enabled() ? "Available" : "Disconnected"));
    gtk_widget_set_tooltip_text(q->net_lbl, name);
    g_free(name);
    /* Bluetooth tile only when there is an adapter */
    gboolean has_bt = GTK_IS_SWITCH(t->bt_power_sw) && t->bt_adapter_path;
    gtk_widget_set_visible(q->bt_cell, has_bt);
    /* tiles flow on: a missing one (Bluetooth) leaves no gap */
    {
        for (gint i = 0; i < q->ncells; i++)
            if (gtk_widget_get_parent(q->cells[i]) == q->grid) {
                g_object_ref(q->cells[i]);
                gtk_container_remove(GTK_CONTAINER(q->grid), q->cells[i]);
            } else g_object_ref(q->cells[i]);
        gint k = 0;
        for (gint i = 0; i < q->ncells; i++) {
            if (q->cells[i] != q->bt_cell || has_bt) { gtk_grid_attach(GTK_GRID(q->grid), q->cells[i], k % 3, k / 3, 1, 1); k++; }
            else gtk_grid_attach(GTK_GRID(q->grid), q->cells[i], 0, 99, 1, 1);   /* parked, hidden */
            g_object_unref(q->cells[i]);
        }
    }
    if (has_bt) {
        gboolean bon = gtk_switch_get_active(GTK_SWITCH(t->bt_power_sw));
        qs_tile_set(q->bt_tile, bon);
        gtk_label_set_text(GTK_LABEL(q->bt_lbl), bon ? "Bluetooth" : "Bluetooth off");
    }
    qs_sync_toggles(q);
    /* sliders, battery */
    if (q->bright_row) gtk_widget_set_visible(q->bright_row, t->pwr_backlight != NULL);
    gtk_widget_set_visible(q->bat_btn, g_pwr.has_battery);
    if (g_pwr.has_battery) {
        gchar *tx = g_strdup_printf("%.0f%%", g_pwr.percent);
        gtk_label_set_text(GTK_LABEL(q->bat_lbl), tx);
        g_free(tx);
        const gchar *bi = g_pwr.state == PWR_STATE_CHARGING || g_pwr.state == PWR_STATE_FULL ? "battery-full-charging-symbolic"
                        : g_pwr.percent < 15 ? "battery-caution-symbolic" : g_pwr.percent < 50 ? "battery-low-symbolic" : "battery-good-symbolic";
        gtk_image_set_from_icon_name(GTK_IMAGE(q->bat_icon), bi, GTK_ICON_SIZE_MENU);
        gint64 secs = g_pwr.state == PWR_STATE_CHARGING ? g_pwr.secs_to_full : g_pwr.state == PWR_STATE_DISCHARGING ? g_pwr.secs_to_empty : 0;
        gchar *tip = secs > 60 ? g_strdup_printf(g_pwr.state == PWR_STATE_CHARGING ? "%d:%02d until full" : "%d:%02d left",
                                                 (gint)(secs / 3600), (gint)(secs / 60 % 60))
                               : g_strdup(g_pwr.state == PWR_STATE_FULL ? "Fully charged" : "Battery & power modes");
        gtk_widget_set_tooltip_text(q->bat_btn, tip);
        g_free(tip);
    }
    /* the media card only while there is a player */
    if (g_ap && g_ap->players_stack) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(g_ap->players_stack));
        gtk_widget_set_visible(q->media_card, kids != NULL);
        g_list_free(kids);
    } else gtk_widget_set_visible(q->media_card, FALSE);
    q->updating = FALSE;
}

/* ---- actions ---- */
static gboolean qs_sync_later(gpointer d) { if (g_qs) qs_sync(g_qs); return G_SOURCE_REMOVE; }
static void on_qs_net(GtkButton *b, QuickSettings *q) {
    if (qs_has_wifi_device()) {
        gboolean on = !qs_wifi_enabled();
        g_spawn_command_line_async(on ? "nmcli radio wifi on" : "nmcli radio wifi off", NULL);
        qs_tile_set(q->net_tile, on);
    } else {                                   /* wired only: networking on / off */
        gboolean on = q->net_kind == NULL;
        g_spawn_command_line_async(on ? "nmcli networking on" : "nmcli networking off", NULL);
        qs_tile_set(q->net_tile, on);
    }
    g_timeout_add(1500, qs_sync_later, NULL);
}
static void on_qs_bt(GtkButton *b, QuickSettings *q) {
    SystrayWidget *t = q->tray;
    if (!GTK_IS_SWITCH(t->bt_power_sw)) return;
    gboolean on = !gtk_switch_get_active(GTK_SWITCH(t->bt_power_sw));
    gtk_switch_set_active(GTK_SWITCH(t->bt_power_sw), on);
    qs_tile_set(q->bt_tile, on);
    g_timeout_add(1200, qs_sync_later, NULL);
}
static void on_qs_dnd(GtkButton *b, QuickSettings *q) { notifications_set_dnd(!notifications_get_dnd()); qs_sync_toggles(q); }
static void on_qs_dark(GtkButton *b, QuickSettings *q) {
    ShellPalette p; shell_palette_get(q->tray->shell, &p);
    shell_set_dark_mode(q->tray->shell, !p.dark);
    qs_sync_toggles(q);
}
static void on_qs_night(GtkButton *b, QuickSettings *q) { q->night_on = !q->night_on; qs_set_night_light(q->night_on); qs_sync_toggles(q); }
static void on_qs_caffeine(GtkButton *b, QuickSettings *q) { q->caf_on = !q->caf_on; qs_set_caffeine(q->caf_on); qs_tile_set(q->caf_tile, q->caf_on); }
static void on_qs_airplane(GtkButton *b, QuickSettings *q) {
    gboolean on = !qs_airplane_on();
    g_spawn_command_line_async(on ? "nmcli radio all off" : "nmcli radio all on", NULL);
    SystrayWidget *t = q->tray;
    if (GTK_IS_SWITCH(t->bt_power_sw) && t->bt_adapter_path) gtk_switch_set_active(GTK_SWITCH(t->bt_power_sw), !on);
    qs_tile_set(q->air_tile, on);
    g_timeout_add(1500, qs_sync_later, NULL);
}

static void qs_stop_bt_scan(SystrayWidget *t) {
    if (t->bt_scanning && t->sys_bus && t->bt_adapter_path) {
        g_dbus_connection_call(t->sys_bus, "org.bluez", t->bt_adapter_path, "org.bluez.Adapter1", "StopDiscovery",
                               NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
        t->bt_scanning = FALSE;
    }
}
/* a detail page takes exactly the main page's size (no shrinking / growing; long lists scroll) */
static void qs_show_page(QuickSettings *q, const gchar *name) {
    GtkWidget *pg = gtk_stack_get_child_by_name(GTK_STACK(q->stack), name);
    if (!pg) return;
    /* lock the whole panel at the main page's size while a detail page shows */
    if (!g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(q->stack)), "main")) {
        gint w = gtk_widget_get_allocated_width(q->stack), h = gtk_widget_get_allocated_height(q->stack);
        if (w > 1 && h > 1) gtk_widget_set_size_request(q->stack, w, h);
    }
    gtk_widget_set_vexpand(pg, TRUE);
    gtk_stack_set_visible_child(GTK_STACK(q->stack), pg);
}
static void qs_show_sound_devices(void) { if (g_qs) qs_show_page(g_qs, "sound"); }
static void qs_back_to_main(void) { if (g_qs) { gtk_stack_set_visible_child_name(GTK_STACK(g_qs->stack), "main"); gtk_widget_set_size_request(g_qs->stack, -1, -1); } }
static void on_qs_net_more(GtkButton *b, QuickSettings *q) { refresh_network_popup_list(q->tray); qs_show_page(q, "net"); }
static void on_qs_bt_more(GtkButton *b, QuickSettings *q) { qs_show_page(q, "bt"); refresh_bluetooth_ui(q->tray); }
static void on_qs_power_more(GtkButton *b, QuickSettings *q) {
    power_query_profile(q->tray);
    pwr_sync_brightness_slider(q->tray);
    qs_show_page(q, "power");
}
static void on_qs_back(GtkButton *b, QuickSettings *q) {
    qs_stop_bt_scan(q->tray);
    gtk_stack_set_visible_child_name(GTK_STACK(q->stack), "main");
    gtk_widget_set_size_request(q->stack, -1, -1);
    qs_sync(q);
}
static void on_qs_system_settings(GtkButton *b, QuickSettings *q) { shell_popup_hide(q->win); g_spawn_command_line_async("xfce4-settings-manager", NULL); }
static void on_qs_shell_settings(GtkButton *b, QuickSettings *q) {
    shell_popup_hide(q->win);
    if (q->tray->shell && q->tray->shell->plugin) g_signal_emit_by_name(q->tray->shell->plugin, "configure-plugin");
}
static void on_qs_unmap(GtkWidget *w, QuickSettings *q) {
    qs_stop_bt_scan(q->tray);
    gtk_stack_set_visible_child_name(GTK_STACK(q->stack), "main");
    gtk_widget_set_size_request(q->stack, -1, -1);
    if (g_ap && g_ap->pos_timer) { g_source_remove(g_ap->pos_timer); g_ap->pos_timer = 0; }
}

static void qs_move(GtkWidget *w, GtkWidget *to) {
    if (!GTK_IS_WIDGET(w) || !GTK_IS_CONTAINER(to)) return;
    GtkWidget *p = gtk_widget_get_parent(w);
    if (p == to) return;
    g_object_ref(w);
    if (p) gtk_container_remove(GTK_CONTAINER(p), w);
    gtk_container_add(GTK_CONTAINER(to), w);
    if (GTK_IS_BOX(to))   /* fill the page, so its list uses all the height instead of a small scroll */
        gtk_box_set_child_packing(GTK_BOX(to), w, TRUE, TRUE, 0, GTK_PACK_START);
    g_object_unref(w);
}

static GtkWidget *qs_page(QuickSettings *q, const gchar *title, GtkWidget **slot) {
    GtkWidget *pg = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(pg), "qs-card");
    g_object_set(pg, "margin", 10, NULL);
    GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(hdr), 8);
    GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(back), GTK_RELIEF_NONE);
    g_signal_connect(back, "clicked", G_CALLBACK(on_qs_back), q);
    GtkWidget *tl = gtk_label_new(NULL);
    gchar *mk = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(tl), mk);
    g_free(mk);
    gtk_box_pack_start(GTK_BOX(hdr), back, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hdr), tl, FALSE, FALSE, 0);
    *slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(pg), hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pg), *slot, TRUE, TRUE, 0);
    return pg;
}

/* one slider row: [icon button] ━━━○━━ [›] - the scale shares the sound popup's / power popup's adjustment */
static GtkWidget *qs_slider_row(GtkAdjustment *adj, const gchar *icon, GtkWidget **icon_btn, GCallback icon_cb,
                                GCallback more_cb, QuickSettings *q) {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *ib = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(ib), GTK_RELIEF_NONE);
    if (icon_cb) g_signal_connect(ib, "clicked", icon_cb, q); else gtk_widget_set_sensitive(ib, FALSE);
    GtkWidget *sc = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adj);
    gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
    gtk_box_pack_start(GTK_BOX(row), ib, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), sc, TRUE, TRUE, 0);
    if (more_cb) {
        GtkWidget *m = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(m), GTK_RELIEF_NONE);
        gtk_widget_set_tooltip_text(m, "Devices and apps");
        g_signal_connect(m, "clicked", more_cb, q);
        gtk_box_pack_start(GTK_BOX(row), m, FALSE, FALSE, 0);
    }
    if (icon_btn) *icon_btn = ib;
    return row;
}

static QuickSettings *qs_create(SystrayWidget *t, GtkWidget *anchor) {
    qs_css();
    QuickSettings *q = g_new0(QuickSettings, 1);
    q->tray = t;
    if (!g_ap && t->vol_item && t->vol_item->button) g_ap = ap_create(t);
    q->win = shell_create_popup_window(anchor, 360, -1, FALSE);
    g_object_set_data(G_OBJECT(q->win), "center-on-button", GINT_TO_POINTER(1));
    g_signal_connect(q->win, "unmap", G_CALLBACK(on_qs_unmap), q);
    q->stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(q->stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(GTK_STACK(q->stack), (guint)shell_anim_ms(180));
    gtk_stack_set_vhomogeneous(GTK_STACK(q->stack), FALSE);
    gtk_stack_set_interpolate_size(GTK_STACK(q->stack), TRUE);

    q->main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(q->main), 10);

    /* 1. media card */
    q->media_card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    q->media_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(q->media_card), q->media_slot);
    gtk_widget_set_no_show_all(q->media_card, TRUE);
    gtk_widget_show(q->media_slot);
    gtk_box_pack_start(GTK_BOX(q->main), q->media_card, FALSE, FALSE, 0);

    /* 2. tiles + sliders */
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 22);
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "qs-card");
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE);
    GtkWidget *cells[7];
    gint n = 0;
    cells[n++] = qs_tile(q, "network-wireless-symbolic", "Network", G_CALLBACK(on_qs_net), G_CALLBACK(on_qs_net_more), &q->net_tile, &q->net_icon, &q->net_lbl);
    q->bt_cell = qs_tile(q, "bluetooth-active-symbolic", "Bluetooth", G_CALLBACK(on_qs_bt), G_CALLBACK(on_qs_bt_more), &q->bt_tile, NULL, &q->bt_lbl);
    cells[n++] = q->bt_cell;
    cells[n++] = qs_tile(q, "airplane-mode-symbolic", "Airplane mode", G_CALLBACK(on_qs_airplane), NULL, &q->air_tile, NULL, NULL);
    cells[n++] = qs_tile(q, "notifications-disabled-symbolic", "Do not disturb", G_CALLBACK(on_qs_dnd), NULL, &q->dnd_tile, NULL, NULL);
    cells[n++] = qs_tile(q, "weather-clear-night-symbolic", "Dark mode", G_CALLBACK(on_qs_dark), NULL, &q->dark_tile, NULL, NULL);
    cells[n++] = qs_tile(q, "night-light-symbolic", "Night light", G_CALLBACK(on_qs_night), NULL, &q->night_tile, NULL, NULL);
    cells[n++] = qs_tile(q, "preferences-desktop-screensaver-symbolic", "Caffeine", G_CALLBACK(on_qs_caffeine), NULL, &q->caf_tile, NULL, NULL);
    for (gint i = 0; i < n; i++) { q->cells[i] = cells[i]; gtk_widget_show_all(cells[i]); }
    q->ncells = n;
    q->grid = grid;
    gtk_widget_set_no_show_all(q->bt_cell, TRUE);
    gtk_box_pack_start(GTK_BOX(card), grid, FALSE, FALSE, 0);

    GtkWidget *sliders = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    if (GTK_IS_RANGE(t->pwr_bright_scale)) {
        q->bright_row = qs_slider_row(gtk_range_get_adjustment(GTK_RANGE(t->pwr_bright_scale)),
                                      "display-brightness-symbolic", NULL, NULL, NULL, q);
        gtk_box_pack_start(GTK_BOX(sliders), q->bright_row, FALSE, FALSE, 0);
    }
    /* volume + microphone: the sound popup's own rows (names, %, › device lists) */
    q->audio_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(sliders), q->audio_slot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(card), sliders, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(q->main), card, FALSE, FALSE, 0);

    /* 3. footer: battery (→ Power) · Ultimate Shell settings · system settings */
    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(foot), "qs-foot");
    q->bat_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(q->bat_btn), GTK_RELIEF_NONE);
    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    q->bat_icon = gtk_image_new_from_icon_name("battery-good-symbolic", GTK_ICON_SIZE_MENU);
    q->bat_lbl = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(bb), q->bat_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bb), q->bat_lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(q->bat_btn), bb);
    g_signal_connect(q->bat_btn, "clicked", G_CALLBACK(on_qs_power_more), q);
    gtk_widget_set_no_show_all(q->bat_btn, TRUE);
    gtk_widget_show_all(bb);
    GtkWidget *bsh = gtk_button_new_from_icon_name("document-edit-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(bsh, "Ultimate Shell Settings");
    GtkWidget *bsys = gtk_button_new_from_icon_name("emblem-system-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_tooltip_text(bsys, "System Settings");
    gtk_button_set_relief(GTK_BUTTON(bsh), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(bsys), GTK_RELIEF_NONE);
    g_signal_connect(bsh, "clicked", G_CALLBACK(on_qs_shell_settings), q);
    g_signal_connect(bsys, "clicked", G_CALLBACK(on_qs_system_settings), q);
    gtk_box_pack_start(GTK_BOX(foot), q->bat_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), bsys, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), bsh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(q->main), foot, FALSE, FALSE, 0);

    gtk_stack_add_named(GTK_STACK(q->stack), q->main, "main");
    gtk_stack_add_named(GTK_STACK(q->stack), qs_page(q, "Network", &q->net_slot), "net");
    gtk_stack_add_named(GTK_STACK(q->stack), qs_page(q, "Bluetooth", &q->bt_slot), "bt");
    q->sound_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(q->sound_slot), "qs-card");
    g_object_set(q->sound_slot, "margin", 10, NULL);
    gtk_stack_add_named(GTK_STACK(q->stack), q->sound_slot, "sound");
    gtk_stack_add_named(GTK_STACK(q->stack), qs_page(q, "Power", &q->bat_slot), "power");
    gtk_container_add(GTK_CONTAINER(q->win), q->stack);
    return q;
}

/* borrow the detail content (and the players card) from the old popups */
static void qs_adopt(QuickSettings *q) {
    SystrayWidget *t = q->tray;
    if (g_ap) {
        g_ap_in_qs = TRUE;
        qs_move(g_ap->main_page, q->audio_slot);
        qs_move(g_ap->dev_page, q->sound_slot);
        gtk_container_set_border_width(GTK_CONTAINER(g_ap->main_page), 0);
        gtk_widget_set_size_request(g_ap->main_page, -1, -1);
        if (gtk_widget_get_parent(g_ap->players_frame) != q->media_slot) {
            q->players_home = gtk_widget_get_parent(g_ap->players_frame);
            GValue v = G_VALUE_INIT; g_value_init(&v, G_TYPE_INT);
            gtk_container_child_get_property(GTK_CONTAINER(q->players_home), g_ap->players_frame, "position", &v);
            q->players_pos = g_value_get_int(&v);
            qs_move(g_ap->players_frame, q->media_slot);
        }
        GtkWidget *psep = g_object_get_data(G_OBJECT(g_ap->players_frame), "players-sep");
        if (psep) { gtk_widget_set_no_show_all(psep, TRUE); gtk_widget_hide(psep); }
    }
    if (!q->net_content && t->net_popup) q->net_content = gtk_bin_get_child(GTK_BIN(t->net_popup));
    if (q->net_content) qs_move(q->net_content, q->net_slot);
    if (!q->bt_content && t->bt_popup) q->bt_content = gtk_bin_get_child(GTK_BIN(t->bt_popup));
    if (q->bt_content) qs_move(q->bt_content, q->bt_slot);
    if (!q->bat_content && t->bat_popup) q->bat_content = gtk_bin_get_child(GTK_BIN(t->bat_popup));
    if (q->bat_content) qs_move(q->bat_content, q->bat_slot);
}
static void qs_return(QuickSettings *q) {
    SystrayWidget *t = q->tray;
    if (g_ap) {
        if (q->players_home && gtk_widget_get_parent(g_ap->players_frame) != q->players_home) {
            qs_move(g_ap->players_frame, q->players_home);
            gtk_box_reorder_child(GTK_BOX(q->players_home), g_ap->players_frame, q->players_pos);
        }
        GtkWidget *psep = g_object_get_data(G_OBJECT(g_ap->players_frame), "players-sep");
        if (psep) { gtk_widget_set_no_show_all(psep, FALSE); gtk_widget_show(psep); }
        GtkWidget *pages[2] = { g_ap->main_page, g_ap->dev_page };
        const gchar *names[2] = { "main", "devices" };
        for (gint i = 0; i < 2; i++) {
            g_object_ref(pages[i]);
            if (gtk_widget_get_parent(pages[i])) gtk_container_remove(GTK_CONTAINER(gtk_widget_get_parent(pages[i])), pages[i]);
            gtk_stack_add_named(GTK_STACK(g_ap->stack), pages[i], names[i]);
            g_object_unref(pages[i]);
        }
        gtk_container_set_border_width(GTK_CONTAINER(g_ap->main_page), 12);
        gtk_widget_set_size_request(g_ap->main_page, 380, -1);
        gtk_widget_set_no_show_all(g_ap->foot, FALSE); gtk_widget_show(g_ap->foot);
        gtk_widget_set_no_show_all(g_ap->foot_sep, FALSE); gtk_widget_show(g_ap->foot_sep);
        g_ap_in_qs = FALSE;
    }
    if (q->net_content && t->net_popup) qs_move(q->net_content, t->net_popup);
    if (q->bt_content && t->bt_popup) qs_move(q->bt_content, t->bt_popup);
    if (q->bat_content && t->bat_popup) qs_move(q->bat_content, t->bat_popup);
}

static void qs_open(SystrayWidget *t, GtkToggleButton *anchor) {
    if (!g_qs) g_qs = qs_create(t, GTK_WIDGET(anchor));
    QuickSettings *q = g_qs;
    qs_adopt(q);
    if (g_pwr_upower) power_query_upower(t); else { power_read_sysfs(&g_pwr); power_render(t); }
    pwr_sync_brightness_slider(t);
    if (GTK_IS_SWITCH(t->bt_power_sw)) refresh_bluetooth_ui(t);
    gtk_stack_set_visible_child_name(GTK_STACK(q->stack), "main");
    gtk_widget_show_all(q->stack);
    if (g_ap) {
        gtk_widget_hide(g_ap->foot); gtk_widget_set_no_show_all(g_ap->foot, TRUE);       /* no "Sound Settings" here */
        gtk_widget_hide(g_ap->foot_sep); gtk_widget_set_no_show_all(g_ap->foot_sep, TRUE);
        g_free(g_ap->apps_sig); g_ap->apps_sig = NULL;
        ap_sync_device_names(g_ap);
        ap_rebuild_players(g_ap);
        pa_refresh_async(t);
        if (!g_ap->pos_timer) g_ap->pos_timer = g_timeout_add(1000, ap_pos_tick, g_ap);
        ap_sync_levels(g_ap);
        ap_sync_apps(g_ap);
        for (GList *l = g_ap->players; l; l = l->next) ap_player_update(l->data);
        GList *kids = gtk_container_get_children(GTK_CONTAINER(g_ap->players_stack));
        gtk_widget_set_visible(g_ap->players_frame, kids != NULL);
        gtk_widget_set_visible(g_ap->dots_box, g_list_length(kids) > 1);
        g_list_free(kids);
    }
    qs_sync(q);
    shell_popup_cancel_close(q->win);
    shell_reposition_popup_window(anchor, q->win);
    gtk_window_resize(GTK_WINDOW(q->win), 1, 1);
    gtk_widget_show(q->win);
    shell_reposition_popup_window(anchor, q->win);
    gtk_window_present(GTK_WINDOW(q->win));
}

/* sound changes while the panel is open (keys, other apps): keep the sliders and icons current */
static void qs_on_pa_state(SystrayWidget *t) {
    if (!g_qs || !gtk_widget_get_visible(g_qs->win) || !g_ap) return;
    ap_sync_levels(g_ap);
    ap_sync_apps(g_ap);
}

static gboolean g_qs_resetting = FALSE;
static void qs_on_tray_click(SystrayWidget *t, GtkToggleButton *btn) {
    if (g_qs_resetting) return;
    g_qs_resetting = TRUE;
    gtk_toggle_button_set_active(btn, FALSE);
    g_qs_resetting = FALSE;
    if (g_qs && gtk_widget_get_visible(g_qs->win)) { shell_popup_hide(g_qs->win); return; }
    if (g_qs) {
        gint64 *last = g_object_get_data(G_OBJECT(g_qs->win), "last-focus-hide-us");
        if (last && g_get_monotonic_time() - *last < 300000) return;
    }
    qs_open(t, btn);
}

void systray_quick_settings_changed(UltimateShell *shell) {
    SystrayWidget *t = g_systray;
    if (!t) return;
    if (!shell->tray_quick_settings && g_qs) {
        if (gtk_widget_get_visible(g_qs->win)) gtk_widget_hide(g_qs->win);
        qs_return(g_qs);
    }
    relayout_tray_containers(t);
    rebuild_settings_list_ui(t);
}

/* ---- the group button: the members' icons side by side, one button ---- */
static gboolean qs_is_member(SystrayWidget *t, TrayItem *it) {
    if (!qs_enabled(t) || !it || !it->id) return FALSE;
    if (!g_strcmp0(it->id, "builtin:audio") || !g_strcmp0(it->id, "builtin:network") ||
        !g_strcmp0(it->id, "builtin:battery") || !g_strcmp0(it->id, "builtin:mic")) return TRUE;
    return !g_strcmp0(it->id, "builtin:bluetooth");
}

/* each icon in the group follows its tray item's own icon (it keeps updating that one) */
static void qs_mirror_copy(GtkImage *src, GParamSpec *ps, GtkImage *dst) {
    gint px = gtk_image_get_pixel_size(src);
    switch (gtk_image_get_storage_type(src)) {
        case GTK_IMAGE_ICON_NAME: {
            const gchar *n = NULL; GtkIconSize sz;
            gtk_image_get_icon_name(src, &n, &sz);
            if (n) gtk_image_set_from_icon_name(dst, n, GTK_ICON_SIZE_MENU);
            break; }
        case GTK_IMAGE_GICON: {
            GIcon *gi = NULL; GtkIconSize sz;
            gtk_image_get_gicon(src, &gi, &sz);
            if (gi) gtk_image_set_from_gicon(dst, gi, GTK_ICON_SIZE_MENU);
            break; }
        case GTK_IMAGE_PIXBUF: gtk_image_set_from_pixbuf(dst, gtk_image_get_pixbuf(src)); break;
        default: break;
    }
    if (px > 0) gtk_image_set_pixel_size(dst, px);
}
static GtkWidget *qs_mirror_icon(TrayItem *it) {
    GtkWidget *img = gtk_image_new();
    if (it && GTK_IS_IMAGE(it->image)) {
        qs_mirror_copy(GTK_IMAGE(it->image), NULL, GTK_IMAGE(img));
        g_signal_connect_object(it->image, "notify", G_CALLBACK(qs_mirror_copy), img, 0);
        g_object_set_data(G_OBJECT(img), "qs-item", it);
        if (it->button) gtk_widget_set_tooltip_text(img, gtk_widget_get_tooltip_text(it->button));
    }
    gtk_widget_set_no_show_all(img, TRUE);
    return img;
}

static void on_qs_btn_toggled(GtkToggleButton *b, SystrayWidget *t) {
    if (gtk_toggle_button_get_active(b)) qs_on_tray_click(t, b);
}

/* show the group (and which of its icons) as the option and the hardware say */
static void qs_group_sync(SystrayWidget *t) {
    if (!t || !t->qs_btn) return;
    gboolean on = qs_enabled(t);
    gtk_widget_set_visible(t->qs_btn, on);
    if (!on) return;
    gint icon_px = t->shell ? CLAMP(t->shell->tray_icon_size, 14, 28) : 16;
    gint spacing = t->shell ? CLAMP(t->shell->tray_icon_spacing, 0, 16) : 2;
    gtk_box_set_spacing(GTK_BOX(t->qs_icons), spacing + 10);   /* = separate buttons: 2 x (4px padding + 1px border) + spacing */
    GList *kids = gtk_container_get_children(GTK_CONTAINER(t->qs_icons));
    for (GList *l = kids; l; l = l->next) {
        TrayItem *it = g_object_get_data(G_OBJECT(l->data), "qs-item");
        if (!it) continue;
        gboolean show = TRUE;
        if (it == t->mic_item) show = t->mic_shown;                  /* joins only while the mic is used */
        else if (it == t->bat_item) show = t->bat_present && (!t->shell || t->shell->tray_enable_battery);
        else if (it == t->bt_item) show = t->bt_adapter_path != NULL;
        else if (it == t->net_item) show = !t->shell || t->shell->tray_enable_network;
        else if (it == t->vol_item) show = !t->shell || t->shell->tray_enable_audio;
        gtk_widget_set_visible(l->data, show);
        if (gtk_image_get_storage_type(GTK_IMAGE(l->data)) == GTK_IMAGE_ICON_NAME)
            gtk_image_set_pixel_size(GTK_IMAGE(l->data), icon_px);
    }
    g_list_free(kids);
}

static void qs_group_create(SystrayWidget *t) {
    t->qs_btn = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(t->qs_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(t->qs_btn), "ultimate-module-btn");
    gtk_widget_set_tooltip_text(t->qs_btn, "Quick Settings");
    t->qs_icons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    TrayItem *order[] = { t->mic_item, t->bt_item, t->net_item, t->vol_item, t->bat_item };   /* like Windows */
    for (guint i = 0; i < G_N_ELEMENTS(order); i++)
        if (order[i]) gtk_box_pack_start(GTK_BOX(t->qs_icons), qs_mirror_icon(order[i]), FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(t->qs_btn), t->qs_icons);
    gtk_widget_show(t->qs_icons);
    gtk_widget_set_no_show_all(t->qs_btn, TRUE);
    gtk_widget_add_events(t->qs_btn, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_signal_connect(t->qs_btn, "toggled", G_CALLBACK(on_qs_btn_toggled), t);
    g_signal_connect(t->qs_btn, "scroll-event", G_CALLBACK(on_vol_scroll), t);   /* scroll = volume */
    gtk_box_pack_start(GTK_BOX(t->root_box), t->qs_btn, FALSE, FALSE, 0);   /* last: next to the clock */
}

static gboolean is_tray_item_on_main_panel(TrayItem *item) {
    if (!item || !item->tray || !item->button) return !item->is_hidden;
    if (item->tray->shell && item->tray->shell->tray_show_all) return TRUE;
    if (item->is_xembed) return !item->is_hidden;          /* hidden = in the ▲ popup */
    GtkWidget *parent = gtk_widget_get_parent(item->button);
    if (parent == item->tray->visible_box) return TRUE;
    if (parent == item->tray->overflow_grid) return FALSE;
    return !item->is_hidden;
}

static void on_tray_context_menu_deactivate(GtkMenuShell *ms, SystrayWidget *tray) {
    if (tray && GTK_IS_WIDGET(tray->overflow_popup)) {
        g_object_set_data(G_OBJECT(tray->overflow_popup), "context-menu-open", GINT_TO_POINTER(0));
    }
}

static void lock_overflow_popup_for_menu(SystrayWidget *tray, GtkWidget *menu) {
    if (!tray || !GTK_IS_WIDGET(tray->overflow_popup) || !GTK_IS_MENU(menu)) return;
    g_object_set_data(G_OBJECT(tray->overflow_popup), "context-menu-open", GINT_TO_POINTER(1));
    if (!g_object_get_data(G_OBJECT(menu), "ult-ov-lock-connected")) {
        g_signal_connect(menu, "deactivate", G_CALLBACK(on_tray_context_menu_deactivate), tray);
        g_object_set_data(G_OBJECT(menu), "ult-ov-lock-connected", GINT_TO_POINTER(1));
    }
}

static void toggle_tray_item_panel_location(TrayItem *item) {
    if (!item || !item->tray) return;
    SystrayWidget *tray = item->tray;
    gboolean target_was_on_panel = is_tray_item_on_main_panel(item);

    /* First, lock in every other item's current visual location so turning off tray_show_all
       or toggling one icon NEVER drags other icons along with it! */
    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *other = (TrayItem *)l->data;
        if (!other || other == item) continue;
        other->is_hidden = !is_tray_item_on_main_panel(other);
    }

    if (target_was_on_panel) {
        /* Move ONLY this clicked icon into the Overflow Menu */
        item->is_hidden = TRUE;
        if (tray->shell && tray->shell->tray_show_all) {
            tray->shell->tray_show_all = FALSE;
            if (tray->shell->channel) {
                xfconf_channel_set_bool(tray->shell->channel, "/plugins/ultimateshell/tray_show_all", FALSE);
            }
        }
    } else {
        /* Move ONLY this clicked icon back to the Main Panel */
        item->is_hidden = FALSE;
    }

    save_tray_layout_to_xfconf(tray);
    relayout_tray_containers(tray);
    rebuild_settings_list_ui(tray);
}

static gboolean on_deferred_toggle_location(gpointer data) {
    TrayItem *item = (TrayItem *)data;
    if (item) toggle_tray_item_panel_location(item);
    return G_SOURCE_REMOVE;
}

static void on_toggle_hide_menuitem(GtkMenuItem *mi, TrayItem *item) {
    if (!item) return;
    if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "ult-handled-press")) == 1) {
        g_object_set_data(G_OBJECT(mi), "ult-handled-press", GINT_TO_POINTER(0));
        return;
    }
    g_idle_add(on_deferred_toggle_location, item);
}

static gboolean on_dbus_hide_mi_button_press(GtkWidget *mi, GdkEventButton *event, TrayItem *item) {
    if (event->type == GDK_BUTTON_PRESS && (event->button == 1 || event->button == 3)) {
        g_object_set_data(G_OBJECT(mi), "ult-handled-press", GINT_TO_POINTER(1));
        GtkWidget *parent_menu = gtk_widget_get_parent(mi);
        if (GTK_IS_MENU_SHELL(parent_menu)) {
            gtk_menu_shell_deactivate(GTK_MENU_SHELL(parent_menu));
        }
        g_idle_add(on_deferred_toggle_location, item);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_builtin_btn_press(GtkWidget *widget, GdkEventButton *event, TrayItem *item) {
    SystrayWidget *tray = item->tray;
    if (event->type == GDK_BUTTON_PRESS && event->button == 2) {
        toggle_tray_item_panel_location(item);
        return TRUE;
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        GtkWidget *menu = gtk_menu_new();
        ensure_menu_rgba_visual(menu);
        gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
        if (g_strcmp0(item->id, "builtin:audio") == 0) {
            GtkWidget *pavu_item = gtk_menu_item_new_with_label("Audio mixer...");
            g_signal_connect(pavu_item, "activate", G_CALLBACK(on_open_pavucontrol), tray);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), pavu_item);

            GtkWidget *props_item = gtk_menu_item_new_with_label("Properties / More Options...");
            g_signal_connect(props_item, "activate", G_CALLBACK(on_open_pa_props_menuitem), tray);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), props_item);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        }
        gboolean on_panel = is_tray_item_on_main_panel(item);
        GtkWidget *hide_item = gtk_menu_item_new_with_label(
            on_panel ? "📥 Hide in Overflow Menu" : "📌 Move to Main Panel"
        );
        gtk_widget_add_events(hide_item, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(hide_item, "button-press-event", G_CALLBACK(on_dbus_hide_mi_button_press), item);
        g_signal_connect(hide_item, "activate", G_CALLBACK(on_toggle_hide_menuitem), item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), hide_item);
        lock_overflow_popup_for_menu(item->tray, menu);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_hw_poll_tick(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    /* Only poll audio when the `pactl subscribe` event stream isn't running (~every 4 s) */
    if (!tray->pa_sub_alive && (++tray->pa_poll_tick % 2) == 0) pa_refresh_async(tray);
    update_network_and_audio(tray);
    return G_SOURCE_CONTINUE;
}

static void on_chevron_toggled(GtkToggleButton *btn, SystrayWidget *tray) {
    gboolean on = gtk_toggle_button_get_active(btn);
    if (!on) {
        shell_popup_hide(tray->overflow_popup);
        return;
    }
    GList *children = gtk_container_get_children(GTK_CONTAINER(tray->overflow_grid));
    guint count = g_list_length(children);
    g_list_free(children);
    if (count == 0) {
        gtk_toggle_button_set_active(btn, FALSE);
        gtk_widget_hide(tray->overflow_popup);
        return;
    }
    shell_toggle_popup_window(btn, tray->overflow_popup);
}

static void free_tray_item(gpointer data) {
    TrayItem *item = (TrayItem *)data;
    if (!item) return;
    if (item->cancel) { g_cancellable_cancel(item->cancel); g_object_unref(item->cancel); }
    if (item->watch_id) g_bus_unwatch_name(item->watch_id);
    if (item->item_proxy) {
        g_signal_handlers_disconnect_by_data(item->item_proxy, item);
        g_object_unref(item->item_proxy);
    }
    if (item->dbus_menu) gtk_widget_destroy(item->dbus_menu);
    if (item->button) gtk_widget_destroy(item->button);
    g_free(item->id);
    g_free(item->display_name);
    g_free(item->key);
    g_free(item->bus_name);
    g_free(item->object_path);
    g_free(item->menu_path);
    g_free(item);
}

static void refresh_tray_item_properties(TrayItem *item) {
    if (!item->item_proxy) return;

    GVariant *v_id = g_dbus_proxy_get_cached_property(item->item_proxy, "Id");
    if (v_id && g_variant_is_of_type(v_id, G_VARIANT_TYPE_STRING)) {
        const gchar *id_str = g_variant_get_string(v_id, NULL);
        if (id_str && id_str[0] != '\0' && g_strcmp0(item->id, id_str) != 0) {
            g_free(item->id);
            item->id = g_strdup(id_str);
            item->is_hidden = load_hidden_state_for_id(item->tray, item->id, FALSE);
            if (is_network_applet_name(id_str)) {
                item->is_network_applet = TRUE;
                relayout_tray_containers(item->tray);
            }
            item->tray->all_items = g_list_sort(item->tray->all_items, compare_tray_items_by_saved_order);
            save_tray_layout_to_xfconf(item->tray);
        }
    }
    if (v_id) g_variant_unref(v_id);

    GVariant *v_title = g_dbus_proxy_get_cached_property(item->item_proxy, "Title");
    if (v_title && g_variant_is_of_type(v_title, G_VARIANT_TYPE_STRING)) {
        const gchar *t_str = g_variant_get_string(v_title, NULL);
        if (t_str && t_str[0] != '\0') {
            g_free(item->display_name);
            item->display_name = g_strdup(t_str);
            gtk_widget_set_tooltip_text(item->button, t_str);
        }
    }
    if (v_title) g_variant_unref(v_title);

    gint icon_px = item->tray->shell ? CLAMP(item->tray->shell->tray_icon_size, 14, 28) : 16;
    GtkIconTheme *theme = gtk_icon_theme_get_default();

    /* Register custom IconThemePath if the SNI item exports one */
    GVariant *v_theme_path = g_dbus_proxy_get_cached_property(item->item_proxy, "IconThemePath");
    if (v_theme_path && g_variant_is_of_type(v_theme_path, G_VARIANT_TYPE_STRING)) {
        const gchar *tpath = g_variant_get_string(v_theme_path, NULL);
        if (tpath && tpath[0] != '\0') {
            gtk_icon_theme_append_search_path(theme, tpath);
        }
    }
    if (v_theme_path) g_variant_unref(v_theme_path);

    gboolean icon_applied = FALSE;

    gboolean req_sym = (item->tray->shell && item->tray->shell->tray_request_symbolic);

    /* 0. If Request Symbolic Icons is enabled, try -symbolic variants first */
    if (req_sym) {
        GVariant *v_sym = g_dbus_proxy_get_cached_property(item->item_proxy, "IconName");
        if (v_sym && g_variant_is_of_type(v_sym, G_VARIANT_TYPE_STRING)) {
            const gchar *s = g_variant_get_string(v_sym, NULL);
            if (s && s[0] != '\0' && !g_file_test(s, G_FILE_TEST_EXISTS)) {
                gchar *sym_name = g_str_has_suffix(s, "-symbolic") ? g_strdup(s) : g_strdup_printf("%s-symbolic", s);
                if (gtk_icon_theme_has_icon(theme, sym_name)) {
                    gtk_image_set_from_icon_name(GTK_IMAGE(item->image), sym_name, GTK_ICON_SIZE_MENU);
                    gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
                    icon_applied = TRUE;
                }
                g_free(sym_name);
            }
        }
        if (v_sym) g_variant_unref(v_sym);

        if (!icon_applied && item->id) {
            gchar *lid = g_ascii_strdown(item->id, -1);
            gchar *sym_id = g_str_has_suffix(lid, "-symbolic") ? g_strdup(lid) : g_strdup_printf("%s-symbolic", lid);
            if (gtk_icon_theme_has_icon(theme, sym_id)) {
                gtk_image_set_from_icon_name(GTK_IMAGE(item->image), sym_id, GTK_ICON_SIZE_MENU);
                gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
                icon_applied = TRUE;
            }
            g_free(sym_id);
            g_free(lid);
        }
        if (!icon_applied && item->display_name) {
            gchar *lt = g_ascii_strdown(item->display_name, -1);
            gchar *sym_t = g_str_has_suffix(lt, "-symbolic") ? g_strdup(lt) : g_strdup_printf("%s-symbolic", lt);
            if (gtk_icon_theme_has_icon(theme, sym_t)) {
                gtk_image_set_from_icon_name(GTK_IMAGE(item->image), sym_t, GTK_ICON_SIZE_MENU);
                gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
                icon_applied = TRUE;
            }
            g_free(sym_t);
            g_free(lt);
        }
    }

    /* 1. Check IconName (file path or valid themed icon name) */
    GVariant *v_icon = g_dbus_proxy_get_cached_property(item->item_proxy, "IconName");
    if (!icon_applied && v_icon && g_variant_is_of_type(v_icon, G_VARIANT_TYPE_STRING)) {
        const gchar *str = g_variant_get_string(v_icon, NULL);
        if (str && str[0] != '\0') {
            if (g_file_test(str, G_FILE_TEST_EXISTS)) {
                GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(str, icon_px, icon_px, TRUE, NULL);
                if (pb) {
                    gtk_image_set_from_pixbuf(GTK_IMAGE(item->image), pb);
                    g_object_unref(pb);
                    icon_applied = TRUE;
                }
            } else if (gtk_icon_theme_has_icon(theme, str)) {
                gtk_image_set_from_icon_name(GTK_IMAGE(item->image), str, GTK_ICON_SIZE_MENU);
                gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
                icon_applied = TRUE;
            }
        }
    }
    if (v_icon) g_variant_unref(v_icon);

    /* 2. Check IconPixmap (used by Qt apps like Strawberry for dynamic tray icons) */
    if (!icon_applied) {
        GVariant *v_pix = g_dbus_proxy_get_cached_property(item->item_proxy, "IconPixmap");
        if (!v_pix) {
            /* Fetch uncached IconPixmap directly via D-Bus Get if needed */
            GVariant *res = g_dbus_connection_call_sync(
                item->tray->session_bus, item->bus_name, item->object_path,
                "org.freedesktop.DBus.Properties", "Get",
                g_variant_new("(ss)", "org.kde.StatusNotifierItem", "IconPixmap"),
                G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 300, NULL, NULL);
            if (res) {
                g_variant_get(res, "(v)", &v_pix);
                g_variant_unref(res);
            }
        }
        if (v_pix) {
            GdkPixbuf *pb = extract_pixbuf_from_sni_variant(v_pix, icon_px);
            if (pb) {
                gtk_image_set_from_pixbuf(GTK_IMAGE(item->image), pb);
                g_object_unref(pb);
                icon_applied = TRUE;
            }
            g_variant_unref(v_pix);
        }
    }

    /* 3. Fallback to lowercase Id or Title if matching icon exists in theme (e.g. "strawberry") */
    if (!icon_applied && item->display_name) {
        gchar *lower_title = g_ascii_strdown(item->display_name, -1);
        if (gtk_icon_theme_has_icon(theme, lower_title)) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->image), lower_title, GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
            icon_applied = TRUE;
        }
        g_free(lower_title);
    }
    if (!icon_applied && item->id) {
        gchar *lower_id = g_ascii_strdown(item->id, -1);
        if (gtk_icon_theme_has_icon(theme, lower_id)) {
            gtk_image_set_from_icon_name(GTK_IMAGE(item->image), lower_id, GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
            icon_applied = TRUE;
        }
        g_free(lower_id);
    }

    /* 4. Final fallback */
    if (!icon_applied) {
        gtk_image_set_from_icon_name(GTK_IMAGE(item->image), "application-x-executable-symbolic", GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(item->image), icon_px);
    }

    GVariant *v_status = g_dbus_proxy_get_cached_property(item->item_proxy, "Status");
    if (v_status && g_variant_is_of_type(v_status, G_VARIANT_TYPE_STRING)) {
        const gchar *st = g_variant_get_string(v_status, NULL);
        shell_set_badge_count(item->badge_lbl, (g_strcmp0(st, "NeedsAttention") == 0) ? 1 : 0);
    }
    if (v_status) g_variant_unref(v_status);

    GVariant *v_menu = g_dbus_proxy_get_cached_property(item->item_proxy, "Menu");
    if (v_menu && (g_variant_is_of_type(v_menu, G_VARIANT_TYPE_OBJECT_PATH) ||
                   g_variant_is_of_type(v_menu, G_VARIANT_TYPE_STRING))) {
        const gchar *mp = g_variant_get_string(v_menu, NULL);
        if (mp && mp[0] == '/' && g_strcmp0(mp, item->menu_path) != 0) {
            g_free(item->menu_path);
            item->menu_path = g_strdup(mp);
            if (item->dbus_menu) gtk_widget_destroy(item->dbus_menu);
            item->dbus_menu = GTK_WIDGET(dbusmenu_gtkmenu_new(item->bus_name, item->menu_path));
        }
    }
    if (v_menu) g_variant_unref(v_menu);

    relayout_tray_containers(item->tray);
    rebuild_settings_list_ui(item->tray);
}

static void on_sni_activate_menuitem(GtkMenuItem *mi, TrayItem *item) {
    if (!item || !item->item_proxy) return;
    gint bx = 0, by = 0;
    if (item->button && gtk_widget_get_window(item->button)) {
        gint wx = 0, wy = 0;
        gdk_window_get_origin(gtk_widget_get_window(item->button), &wx, &wy);
        GtkAllocation alloc;
        gtk_widget_get_allocation(item->button, &alloc);
        bx = wx + alloc.x + alloc.width / 2;
        by = wy + alloc.y;
    }
    g_dbus_proxy_call(item->item_proxy, "Activate",
                      g_variant_new("(ii)", bx, by),
                      G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void ensure_sni_dbus_menu_hide_item(TrayItem *item) {
    if (!item || !item->dbus_menu || !GTK_IS_MENU(item->dbus_menu)) return;

    gboolean on_panel = is_tray_item_on_main_panel(item);
    const gchar *lbl = on_panel ? "📥 Hide in Overflow Menu" : "📌 Move to Main Panel";

    GtkWidget *sep = g_object_get_data(G_OBJECT(item->dbus_menu), "ult-hide-sep");
    GtkWidget *hide_mi = g_object_get_data(G_OBJECT(item->dbus_menu), "ult-hide-mi");

    if (sep && GTK_IS_WIDGET(sep) && gtk_widget_get_parent(sep) == item->dbus_menu) {
        gtk_container_remove(GTK_CONTAINER(item->dbus_menu), sep);
    }
    if (hide_mi && GTK_IS_WIDGET(hide_mi) && gtk_widget_get_parent(hide_mi) == item->dbus_menu) {
        gtk_container_remove(GTK_CONTAINER(item->dbus_menu), hide_mi);
    }

    sep = gtk_separator_menu_item_new();
    hide_mi = gtk_menu_item_new_with_label(lbl);
    gtk_widget_add_events(hide_mi, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);
    g_signal_connect(hide_mi, "button-press-event", G_CALLBACK(on_dbus_hide_mi_button_press), item);
    g_signal_connect(hide_mi, "activate", G_CALLBACK(on_toggle_hide_menuitem), item);

    gtk_menu_shell_append(GTK_MENU_SHELL(item->dbus_menu), sep);
    gtk_menu_shell_append(GTK_MENU_SHELL(item->dbus_menu), hide_mi);
    gtk_widget_show(sep);
    gtk_widget_show_all(hide_mi);

    g_object_set_data(G_OBJECT(item->dbus_menu), "ult-hide-sep", sep);
    g_object_set_data(G_OBJECT(item->dbus_menu), "ult-hide-mi", hide_mi);
}

static gboolean refresh_sni_dbus_menu_hide_item_delayed(gpointer data) {
    TrayItem *item = (TrayItem *)data;
    if (item && item->dbus_menu && GTK_IS_MENU(item->dbus_menu) && gtk_widget_get_visible(item->dbus_menu)) {
        ensure_sni_dbus_menu_hide_item(item);
    }
    return G_SOURCE_REMOVE;
}

/* Older (XEmbed) icons: open the app's own menu - a real right-click on its icon window. The pointer is
   moved onto the icon first (the app opens its menu there), our click shield steps aside for it. */
static gboolean xembed_open_app_menu_later(gpointer data) {
    TrayItem *item = data;
    GtkWidget *wrap = (item->ov_wrap && gtk_widget_get_mapped(item->ov_wrap)) ? item->ov_wrap : item->button;
    GdkDisplay *gd = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gd) || !GTK_IS_WIDGET(wrap) || !gtk_widget_get_window(wrap)) return G_SOURCE_REMOVE;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);
    int eb, er, mj, mn;
    if (!XTestQueryExtension(dpy, &eb, &er, &mj, &mn)) return G_SOURCE_REMOVE;
    gint ox = 0, oy = 0;
    gdk_window_get_origin(gtk_widget_get_window(wrap), &ox, &oy);
    GtkAllocation a; gtk_widget_get_allocation(wrap, &a);
    XWarpPointer(dpy, None, DefaultRootWindow(dpy), 0, 0, 0, 0, ox + a.x + a.width / 2, oy + a.y + a.height / 2);
    GdkWindow *sh = xembed_shield_of(wrap);
    if (sh) gdk_window_hide(sh);
    gdk_display_sync(gd);
    XTestFakeButtonEvent(dpy, 3, True, CurrentTime);
    XTestFakeButtonEvent(dpy, 3, False, CurrentTime);
    XFlush(dpy);
    g_timeout_add(400, xembed_restore_shield, g_object_ref(wrap));
    return G_SOURCE_REMOVE;
}
static void on_xembed_app_menu_activate(GtkMenuItem *mi, TrayItem *item) {
    g_timeout_add(120, xembed_open_app_menu_later, item);    /* after our menu has let go of the pointer */
}

static void popup_sni_shell_action_menu(TrayItem *item, GdkEventButton *event) {
    GtkWidget *menu = gtk_menu_new();
    ensure_menu_rgba_visual(menu);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");

    if (item->is_xembed) {
        gchar *lbl = g_strdup_printf("☰ %s menu", item->display_name ? item->display_name : "App");
        GtkWidget *am = gtk_menu_item_new_with_label(lbl);
        g_free(lbl);
        g_signal_connect(am, "activate", G_CALLBACK(on_xembed_app_menu_activate), item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), am);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }

    if (item->item_proxy) {
        GtkWidget *open_item = gtk_menu_item_new_with_label("🗔 Open / Toggle App Window");
        g_signal_connect(open_item, "activate", G_CALLBACK(on_sni_activate_menuitem), item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    }

    gboolean on_panel = is_tray_item_on_main_panel(item);
    GtkWidget *hide_item = gtk_menu_item_new_with_label(
        on_panel ? "📥 Hide in Overflow Menu" : "📌 Move to Main Panel"
    );
    gtk_widget_add_events(hide_item, GDK_BUTTON_PRESS_MASK);
    g_signal_connect(hide_item, "button-press-event", G_CALLBACK(on_dbus_hide_mi_button_press), item);
    g_signal_connect(hide_item, "activate", G_CALLBACK(on_toggle_hide_menuitem), item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), hide_item);

    lock_overflow_popup_for_menu(item->tray, menu);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}

static gboolean on_sni_btn_press(GtkWidget *btn, GdkEventButton *event, TrayItem *item) {
    if (event->type != GDK_BUTTON_PRESS) return FALSE;

    /* Middle-click always toggles Hide in Overflow Menu / Move to Main Panel */
    if (event->button == 2) {
        toggle_tray_item_panel_location(item);
        return TRUE;
    }

    /* Calculate exact screen root coordinates of the tray button so external SNI popups never open at (0,0) */
    gint bx = (gint)event->x_root;
    gint by = (gint)event->y_root;
    if (gtk_widget_get_window(btn)) {
        gint wx = 0, wy = 0;
        gdk_window_get_origin(gtk_widget_get_window(btn), &wx, &wy);
        GtkAllocation alloc;
        gtk_widget_get_allocation(btn, &alloc);
        bx = wx + alloc.x + alloc.width / 2;
        by = wy + alloc.y;
    }

    gboolean menu_primary = (item->tray && item->tray->shell && item->tray->shell->tray_menu_is_primary);
    gboolean item_is_menu = menu_primary;
    if (!item_is_menu && item->item_proxy) {
        GVariant *v_im = g_dbus_proxy_get_cached_property(item->item_proxy, "ItemIsMenu");
        if (v_im) {
            item_is_menu = g_variant_get_boolean(v_im);
            g_variant_unref(v_im);
        }
    }

    /* When "Menu is primary action" is enabled, right-click (button 3) opens the Shell Hide/Move menu */
    if (event->button == 3 && menu_primary) {
        popup_sni_shell_action_menu(item, event);
        return TRUE;
    }

    if (event->button == 1 && !item_is_menu) {
        if (item->item_proxy) {
            g_dbus_proxy_call(item->item_proxy, "Activate",
                              g_variant_new("(ii)", bx, by),
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        }
        return TRUE;
    }

    if (event->button == 3 || (event->button == 1 && item_is_menu)) {
        if (item->dbus_menu && GTK_IS_MENU(item->dbus_menu)) {
            ensure_menu_rgba_visual(item->dbus_menu);
            lock_overflow_popup_for_menu(item->tray, item->dbus_menu);
            ensure_sni_dbus_menu_hide_item(item);
            gtk_style_context_add_class(gtk_widget_get_style_context(item->dbus_menu), "ultimate-themed-menu");
            gtk_widget_show_all(item->dbus_menu);
            gboolean top_bar = (item->tray && item->tray->shell && item->tray->shell->mode == LAYOUT_MACOS);
            gtk_menu_popup_at_widget(GTK_MENU(item->dbus_menu), btn,
                                     top_bar ? GDK_GRAVITY_SOUTH : GDK_GRAVITY_NORTH,
                                     top_bar ? GDK_GRAVITY_NORTH : GDK_GRAVITY_SOUTH,
                                     (GdkEvent *)event);
            g_timeout_add(45, refresh_sni_dbus_menu_hide_item_delayed, item);
            return TRUE;
        }
        if (item->item_proxy) {
            if (item->tray && GTK_IS_WIDGET(item->tray->overflow_popup)) {
                g_object_set_data(G_OBJECT(item->tray->overflow_popup), "context-menu-open", GINT_TO_POINTER(1));
            }
            g_dbus_proxy_call(item->item_proxy, "ContextMenu",
                              g_variant_new("(ii)", bx, by),
                              G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_sni_proxy_properties_changed(GDBusProxy *proxy, GVariant *changed,
                                            const gchar *const *invalidated, gpointer user_data)
{
    TrayItem *item = (TrayItem *)user_data;
    refresh_tray_item_properties(item);
}

static void on_sni_proxy_signal_Callback(GObject * object, gchar *sender_name, gchar *signal_name,
                                         GVariant *parameters, gpointer user_data)
{
    TrayItem *item = (TrayItem *)user_data;
    if (g_strcmp0(signal_name, "NewIcon") == 0 ||
        g_strcmp0(signal_name, "NewTitle") == 0 ||
        g_strcmp0(signal_name, "NewStatus") == 0 ||
        g_strcmp0(signal_name, "NewToolTip") == 0) {
        /* Re-query properties asynchronously so animated/blinking icons don't stall the panel */
        g_dbus_connection_call(
            item->tray->session_bus, item->bus_name, item->object_path,
            "org.freedesktop.DBus.Properties", "GetAll",
            g_variant_new("(s)", "org.kde.StatusNotifierItem"),
            G_VARIANT_TYPE("(a{sv})"), G_DBUS_CALL_FLAGS_NONE, 1000, item->cancel,
            on_sni_getall_done, item);
    }
}

static void on_sni_getall_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    GError *err = NULL;
    GVariant *all_res = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (!all_res) { g_clear_error(&err); return; }   /* cancelled => item freed */
    TrayItem *item = (TrayItem *)user_data;
    {
        if (all_res) {
            GVariant *dict = g_variant_get_child_value(all_res, 0);
            GVariantIter iter;
            gchar *prop_key = NULL;
            GVariant *prop_val = NULL;
            g_variant_iter_init(&iter, dict);
            while (g_variant_iter_loop(&iter, "{sv}", &prop_key, &prop_val)) {
                g_dbus_proxy_set_cached_property(item->item_proxy, prop_key, prop_val);
            }
            g_variant_unref(dict);
            g_variant_unref(all_res);
        }
        refresh_tray_item_properties(item);
    }
}

static void on_item_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
    GError *err = NULL;
    GDBusProxy *proxy = g_dbus_proxy_new_finish(res, &err);
    if (!proxy) {
        /* Cancelled = the item was already freed; don't touch user_data. */
        g_clear_error(&err);
        return;
    }
    TrayItem *item = (TrayItem *)user_data;
    item->item_proxy = proxy;
    g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(on_sni_proxy_properties_changed), item);
    g_signal_connect(proxy, "g-signal", G_CALLBACK(on_sni_proxy_signal_Callback), item);
    refresh_tray_item_properties(item);
}

static void on_tray_bus_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    TrayItem *item = (TrayItem *)user_data;
    SystrayWidget *tray = item->tray;
    tray->all_items = g_list_remove(tray->all_items, item);
    g_hash_table_remove(tray->sni_by_key, item->key);
    relayout_tray_containers(tray);
    rebuild_settings_list_ui(tray);
}

static gint compare_tray_items_by_saved_order(gconstpointer a, gconstpointer b) {
    const TrayItem *ia = (const TrayItem *)a;
    const TrayItem *ib = (const TrayItem *)b;
    return get_id_order_index(ia->tray, ia->id) - get_id_order_index(ib->tray, ib->id);
}

static void register_sni_item(SystrayWidget *tray, const gchar *sender, const gchar *service_or_path) {
    gchar *bus_name = (service_or_path[0] == '/') ? g_strdup(sender) : g_strdup(service_or_path);
    gchar *obj_path = (service_or_path[0] == '/') ? g_strdup(service_or_path) : g_strdup("/StatusNotifierItem");
    gchar *unique = get_unique_bus_name(tray->session_bus, bus_name);
    gchar *key = g_strdup_printf("%s%s", unique ? unique : bus_name, obj_path);
    g_free(unique);

    if (g_hash_table_contains(tray->sni_by_key, key)) {
        g_free(key); g_free(bus_name); g_free(obj_path);
        return;
    }

    TrayItem *item = g_new0(TrayItem, 1);
    item->tray = tray;
    item->key = key;
    item->id = g_strdup(bus_name);
    item->display_name = g_strdup(bus_name);
    item->bus_name = bus_name;
    item->object_path = obj_path;
    item->is_builtin = FALSE;
    item->is_hidden = load_hidden_state_for_id(tray, item->id, FALSE);
    item->pid = get_pid_for_bus_name(tray->session_bus, bus_name);
    {
        gchar *comm = get_comm_for_pid(item->pid);
        item->is_network_applet = is_network_applet_name(comm);
        g_free(comm);
    }

    item->button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(item->button), "ultimate-module-btn");

    item->image = gtk_image_new_from_icon_name("application-x-executable-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(item->image, GTK_ALIGN_CENTER);
    GtkWidget *overlay = shell_create_badged_icon(item->image, &item->badge_lbl);
    gtk_widget_set_margin_top(item->image, 0);
    gtk_widget_set_margin_bottom(item->image, 0);
    gtk_widget_set_margin_start(item->image, 0);
    gtk_widget_set_margin_end(item->image, 0);
    gtk_widget_set_margin_top(overlay, 0);
    gtk_widget_set_margin_bottom(overlay, 0);
    gtk_widget_set_margin_start(overlay, 0);
    gtk_widget_set_margin_end(overlay, 0);
    gtk_widget_set_halign(overlay, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(overlay, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(item->button), overlay);

    setup_tray_item_dnd(item);
    g_signal_connect(item->button, "button-press-event", G_CALLBACK(on_sni_btn_press), item);
    shell_set_badge_count(item->badge_lbl, 0);

    item->watch_id = g_bus_watch_name_on_connection(tray->session_bus, item->bus_name,
        G_BUS_NAME_WATCHER_FLAGS_NONE, NULL, on_tray_bus_vanished, item, NULL);

    g_hash_table_insert(tray->sni_by_key, item->key, item);
    tray->all_items = g_list_append(tray->all_items, item);
    tray->all_items = g_list_sort(tray->all_items, compare_tray_items_by_saved_order);

    relayout_tray_containers(tray);
    rebuild_settings_list_ui(tray);

    item->cancel = g_cancellable_new();
    g_dbus_proxy_new(tray->session_bus, G_DBUS_PROXY_FLAGS_NONE, NULL,
                     item->bus_name, item->object_path, "org.kde.StatusNotifierItem",
                     item->cancel, on_item_proxy_ready, item);
}

static void handle_watcher_method_call(GDBusConnection *conn, const gchar *sender, const gchar *obj_path,
    const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer user_data)
{
    SystrayWidget *tray = (SystrayWidget *)user_data;
    if (g_strcmp0(method, "RegisterStatusNotifierItem") == 0) {
        const gchar *service = NULL;
        g_variant_get(params, "(&s)", &service);
        if (service) {
            register_sni_item(tray, sender, service);
            g_dbus_connection_emit_signal(conn, NULL, "/StatusNotifierWatcher",
                iface, "StatusNotifierItemRegistered", g_variant_new("(s)", service), NULL);
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "RegisterStatusNotifierHost") == 0) {
        g_dbus_method_invocation_return_value(inv, NULL);
    }
}

static GVariant *handle_watcher_get_property(GDBusConnection *conn, const gchar *sender, const gchar *obj_path,
    const gchar *iface, const gchar *prop, GError **error, gpointer user_data)
{
    SystrayWidget *tray = (SystrayWidget *)user_data;
    if (g_strcmp0(prop, "IsStatusNotifierHostRegistered") == 0) return g_variant_new_boolean(TRUE);
    if (g_strcmp0(prop, "ProtocolVersion") == 0) return g_variant_new_int32(0);
    if (g_strcmp0(prop, "RegisteredStatusNotifierItems") == 0) {
        GVariantBuilder b;
        g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
        if (tray && tray->sni_by_key) {
            GHashTableIter iter;
            gpointer k, v;
            g_hash_table_iter_init(&iter, tray->sni_by_key);
            while (g_hash_table_iter_next(&iter, &k, &v)) {
                g_variant_builder_add(&b, "s", (const gchar *)k);
            }
        }
        return g_variant_builder_end(&b);
    }
    return NULL;
}

static const GDBusInterfaceVTable watcher_vtable = {
    handle_watcher_method_call, handle_watcher_get_property, NULL, { 0 }
};

static gboolean g_watcher_objects_registered = FALSE;

static void on_watcher_bus_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    if (!tray->session_bus) {
        tray->session_bus = g_object_ref(conn);
    }
    if (!g_watcher_objects_registered && tray->watcher_node) {
        g_watcher_objects_registered = TRUE;
        if (tray->watcher_node->interfaces[0]) {
            g_dbus_connection_register_object(conn, "/StatusNotifierWatcher",
                tray->watcher_node->interfaces[0], &watcher_vtable, tray, NULL, NULL);
        }
        if (tray->watcher_node->interfaces[1]) {
            g_dbus_connection_register_object(conn, "/StatusNotifierWatcher",
                tray->watcher_node->interfaces[1], &watcher_vtable, tray, NULL, NULL);
        }
        if (tray->watcher_node->interfaces[0]) {
            g_dbus_connection_register_object(conn, "/org/freedesktop/StatusNotifierWatcher",
                tray->watcher_node->interfaces[0], &watcher_vtable, tray, NULL, NULL);
        }
    }
}

static void on_watcher_name_acquired(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    g_dbus_connection_emit_signal(conn, NULL, "/StatusNotifierWatcher",
        "org.kde.StatusNotifierWatcher", "StatusNotifierHostRegistered", NULL, NULL);
    g_dbus_connection_emit_signal(conn, NULL, "/StatusNotifierWatcher",
        "org.freedesktop.StatusNotifierWatcher", "StatusNotifierHostRegistered", NULL, NULL);
}

/* =========================================================================
 * X11 XEmbed (_NET_SYSTEM_TRAY_S0) Manager for Legacy Status Icons (IBus, etc.)
 * ========================================================================= */
#define SYSTEM_TRAY_REQUEST_DOCK    0
#define SYSTEM_TRAY_BEGIN_MESSAGE   1
#define SYSTEM_TRAY_CANCEL_MESSAGE  2

/* Keep an XEmbed icon embedded (so it never pops out as a stray window) but
 * hide it when the same app is already shown through SNI, or when it is a
 * network applet and the built-in Network item is enabled. */
static void xembed_refresh_visibility(SystrayWidget *tray) {
    if (!tray) return;
    for (GList *l = tray->all_items; l; l = l->next) {
        TrayItem *it = (TrayItem *)l->data;
        if (!it || !it->is_xembed || !GTK_IS_WIDGET(it->button)) continue;
        gboolean show_all = tray->shell ? tray->shell->tray_show_all : FALSE;
        (void)show_all;
        gboolean hide = (it->is_network_applet && network_applet_suppressed(tray)) || sni_item_has_pid(tray, it->pid) ||
                        (it->is_hidden && !show_all);        /* hidden: its icon lives in the ▲ popup */
        gtk_widget_set_visible(it->button, !hide);
        if (it->ov_wrap) gtk_widget_set_visible(it->ov_wrap, it->is_hidden && !show_all &&
                        !((it->is_network_applet && network_applet_suppressed(tray)) || sni_item_has_pid(tray, it->pid)));
    }
}

/* The wrapper event box sits above the socket so we can drag it.  A plain
 * click (no drag) is forwarded to the embedded icon as a synthetic press +
 * release, which is what GtkStatusIcon-style icons (IBus) react to. */
static void xembed_send_button(TrayItem *item, GdkEventButton *ev, int type) {
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy) || !item->plug_xid) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);

    gint sx = (gint)ev->x, sy = (gint)ev->y;
    if (item->socket) {
        gtk_widget_translate_coordinates(item->button, item->socket, (gint)ev->x, (gint)ev->y, &sx, &sy);
    }

    XEvent xe;
    memset(&xe, 0, sizeof(xe));
    xe.xbutton.type        = type;
    xe.xbutton.display     = dpy;
    xe.xbutton.window      = (Window)item->plug_xid;
    xe.xbutton.root        = DefaultRootWindow(dpy);
    xe.xbutton.subwindow   = None;
    xe.xbutton.time        = ev->time;
    xe.xbutton.x           = MAX(sx, 0);
    xe.xbutton.y           = MAX(sy, 0);
    xe.xbutton.x_root      = (int)ev->x_root;
    xe.xbutton.y_root      = (int)ev->y_root;
    xe.xbutton.state       = ev->state & ~(Button1Mask | Button2Mask | Button3Mask);
    xe.xbutton.button      = ev->button;
    xe.xbutton.same_screen = True;
    if (type == ButtonRelease) xe.xbutton.state |= (Button1Mask << (ev->button - 1));

    gdk_x11_display_error_trap_push(gdk_dpy);
    XSendEvent(dpy, (Window)item->plug_xid, False, 0, &xe);
    XFlush(dpy);
    gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
}

static void xembed_raise_shield(GtkWidget *wrap);
static gboolean xembed_restore_shield(gpointer data) {
    GtkWidget *wrap = GTK_WIDGET(data);
    xembed_raise_shield(wrap);
    g_object_unref(wrap);
    return G_SOURCE_REMOVE;
}

static void popup_sni_shell_action_menu(TrayItem *item, GdkEventButton *event);
static void toggle_tray_item_panel_location(TrayItem *item);

static gboolean on_xembed_wrapper_release(GtkWidget *w, GdkEventButton *ev, TrayItem *item) {
    /* Runs only when the drag handler did not consume the release (i.e. it was a click). */
    if (ev->button < 1 || ev->button > 3) return FALSE;

    /* Same rules as the other tray icons: middle-click hides / brings back; with
       "Menu is primary action" right-click opens our Hide / Move menu. Everything else goes to the app. */
    if (item->xembed_dragged) { item->xembed_dragged = FALSE; return TRUE; }   /* it was a move */
    if (ev->button == 2) { toggle_tray_item_panel_location(item); return TRUE; }
    if (ev->button == 3 && item->tray && item->tray->shell && item->tray->shell->tray_menu_is_primary) {
        popup_sni_shell_action_menu(item, ev);
        return TRUE;
    }

    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return FALSE;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);

    int ev_base, err_base, maj, min;
    if (XTestQueryExtension(dpy, &ev_base, &err_base, &maj, &min)) {
        /* GTK3 apps ignore synthetic (XSendEvent) clicks, so drop our drag
         * shield for a moment and replay a real click with XTest — the pointer
         * is still over the icon, so the app receives it directly. */
        if (xembed_shield_of(w)) gdk_window_hide(xembed_shield_of(w));   /* let this one click through */
        gdk_display_sync(gdk_dpy);
        XTestFakeButtonEvent(dpy, ev->button, True, CurrentTime);
        XTestFakeButtonEvent(dpy, ev->button, False, CurrentTime);
        XFlush(dpy);
        g_timeout_add(400, xembed_restore_shield, g_object_ref(w));
    } else {
        xembed_send_button(item, ev, ButtonPress);
        xembed_send_button(item, ev, ButtonRelease);
    }
    return TRUE;
}

/* The app's icon is a real X11 window inside our GtkSocket. GTK's own event-box input window is only
   simulated by GDK, so the X server delivered every click to the app underneath. Put a real
   (native) input-only window above it: clicks, drags and hover reach us; we forward clicks. */

static GdkWindow *xembed_shield_of(GtkWidget *wrap) { return g_object_get_data(G_OBJECT(wrap), "xembed-shield"); }

static void xembed_raise_shield(GtkWidget *wrap) {
    GdkWindow *sh = xembed_shield_of(wrap);
    if (sh && gtk_widget_get_mapped(wrap)) { gdk_window_show(sh); gdk_window_raise(sh); }
}
static gboolean xembed_raise_shield_later(gpointer wrap) {
    xembed_raise_shield(GTK_WIDGET(wrap));
    g_object_unref(wrap);
    return G_SOURCE_REMOVE;
}
static void on_xembed_plug_added(GtkSocket *sock, GtkWidget *wrap) {
    xembed_raise_shield(wrap);
    g_timeout_add(300, xembed_raise_shield_later, g_object_ref(wrap));   /* once the app has mapped it */
}
static void on_xembed_wrap_realize(GtkWidget *wrap, gpointer d) {
    if (xembed_shield_of(wrap)) return;
    GtkAllocation a; gtk_widget_get_allocation(wrap, &a);
    GdkWindowAttr at = { 0 };
    at.window_type = GDK_WINDOW_CHILD;
    at.wclass = GDK_INPUT_ONLY;
    at.x = a.x; at.y = a.y; at.width = MAX(1, a.width); at.height = MAX(1, a.height);
    at.event_mask = gtk_widget_get_events(wrap) | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                    GDK_POINTER_MOTION_MASK | GDK_BUTTON1_MOTION_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK;
    GdkWindow *sh = gdk_window_new(gtk_widget_get_parent_window(wrap), &at, GDK_WA_X | GDK_WA_Y);
    gtk_widget_register_window(wrap, sh);
    gdk_window_ensure_native(sh);
    g_object_set_data(G_OBJECT(wrap), "xembed-shield", sh);
}
static void on_xembed_wrap_unrealize(GtkWidget *wrap, gpointer d) {
    GdkWindow *sh = xembed_shield_of(wrap);
    if (!sh) return;
    g_object_set_data(G_OBJECT(wrap), "xembed-shield", NULL);
    gtk_widget_unregister_window(wrap, sh);
    gdk_window_destroy(sh);
}
static void on_xembed_wrap_allocate(GtkWidget *wrap, GdkRectangle *a, gpointer d) {
    GdkWindow *sh = xembed_shield_of(wrap);
    if (sh) {
        gdk_window_move_resize(sh, a->x, a->y, MAX(1, a->width), MAX(1, a->height));
        if (gtk_widget_get_mapped(wrap)) gdk_window_raise(sh);
    }
}
static void on_xembed_wrap_map(GtkWidget *wrap, gpointer d) {
    xembed_raise_shield(wrap);
    g_timeout_add(50, xembed_raise_shield_later, g_object_ref(wrap));
}
static void on_xembed_wrap_unmap(GtkWidget *wrap, gpointer d) {
    GdkWindow *sh = xembed_shield_of(wrap);
    if (sh) gdk_window_hide(sh);
}

static gboolean on_xembed_wrapper_crossing(GtkWidget *w, GdkEventCrossing *ev, gpointer d) {
    if (ev->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    if (ev->type == GDK_ENTER_NOTIFY) gtk_widget_set_state_flags(w, GTK_STATE_FLAG_PRELIGHT, FALSE);
    else gtk_widget_unset_state_flags(w, GTK_STATE_FLAG_PRELIGHT);
    return FALSE;
}

static gboolean on_xembed_wrapper_press(GtkWidget *w, GdkEventButton *ev, TrayItem *item) {
    /* right / middle press: ours (handled on release) - must not reach the panel, which would open
       its own plugin menu (Properties / Move / Remove). Left press goes on to the drag tracking. */
    if (ev->button == 2 || ev->button == 3) return TRUE;
    return FALSE;
}

/* one slot for the app's icon window: a socket in an event box with our click shield */
static GtkWidget *xembed_make_slot(TrayItem *item, gint icon_px, GtkWidget **sock_out) {
    GtkWidget *sock = gtk_socket_new();
    /* the app draws its icon at the socket's size: exactly the tray icon size, padded like the others */
    gtk_widget_set_size_request(sock, icon_px, icon_px);
    gtk_widget_set_valign(sock, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(sock, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(sock, 5);
    gtk_widget_set_margin_end(sock, 5);
    gtk_widget_show(sock);

    GtkWidget *wrap = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(wrap), FALSE);
    gtk_event_box_set_above_child(GTK_EVENT_BOX(wrap), FALSE);    /* our native shield does this */
    g_signal_connect(wrap, "realize", G_CALLBACK(on_xembed_wrap_realize), NULL);
    g_signal_connect(wrap, "unrealize", G_CALLBACK(on_xembed_wrap_unrealize), NULL);
    g_signal_connect(wrap, "size-allocate", G_CALLBACK(on_xembed_wrap_allocate), NULL);
    g_signal_connect(wrap, "unmap", G_CALLBACK(on_xembed_wrap_unmap), NULL);
    g_signal_connect(wrap, "map", G_CALLBACK(on_xembed_wrap_map), NULL);
    gtk_widget_set_no_show_all(wrap, TRUE);
    gtk_widget_set_valign(wrap, GTK_ALIGN_FILL);
    gtk_style_context_add_class(gtk_widget_get_style_context(wrap), "ultimate-module-btn");
    gtk_container_add(GTK_CONTAINER(wrap), sock);
    gtk_widget_set_tooltip_text(wrap, item->display_name);
    g_signal_connect(wrap, "button-press-event", G_CALLBACK(on_xembed_wrapper_press), item);
    g_signal_connect(wrap, "button-release-event", G_CALLBACK(on_xembed_wrapper_release), item);
    gtk_widget_add_events(wrap, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(wrap, "enter-notify-event", G_CALLBACK(on_xembed_wrapper_crossing), NULL);
    g_signal_connect(wrap, "leave-notify-event", G_CALLBACK(on_xembed_wrapper_crossing), NULL);
    g_signal_connect(sock, "plug-removed", G_CALLBACK(on_xembed_plug_removed), item);
    g_signal_connect(sock, "plug-added", G_CALLBACK(on_xembed_plug_added), wrap);
    *sock_out = sock;
    return wrap;
}

/* Put the app's icon window into the panel slot or the ▲ popup slot. Done only when that changes
   (hiding / showing), never per opening: the window is moved, not re-created. */
static void xembed_place(TrayItem *item, gboolean in_popup, gint col, gint row, gint icon_px) {
    SystrayWidget *tray = item->tray;
    if (in_popup && !item->ov_wrap) {
        item->ov_wrap = xembed_make_slot(item, icon_px, &item->ov_socket);
        gtk_style_context_add_class(gtk_widget_get_style_context(item->ov_wrap), "overflow-tray-btn");
        gtk_widget_set_size_request(item->ov_wrap, 36, 36);
        gtk_widget_set_halign(item->ov_wrap, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(item->ov_wrap, GTK_ALIGN_CENTER);
        setup_tray_item_dnd_on(item, item->ov_wrap);
    }
    if (in_popup) {    /* its cell in the popup grid (the slot itself stays: moving it would drop the icon) */
        if (gtk_widget_get_parent(item->ov_wrap) != tray->overflow_grid)
            gtk_grid_attach(GTK_GRID(tray->overflow_grid), item->ov_wrap, col, row, 1, 1);
        else
            gtk_container_child_set(GTK_CONTAINER(tray->overflow_grid), item->ov_wrap, "left-attach", col, "top-attach", row, NULL);
    }
    GtkWidget *target = in_popup ? item->ov_socket : item->socket;
    if (item->plug_xid && item->plug_in != target) {
        GdkDisplay *gd = gdk_display_get_default();
        gtk_widget_realize(target);                     /* the popup may never have been shown yet */
        item->migrating = (item->plug_in != NULL);
        gdk_x11_display_error_trap_push(gd);
        gtk_socket_add_id(GTK_SOCKET(target), (Window)item->plug_xid);
        gdk_x11_display_error_trap_pop_ignored(gd);
        item->plug_in = target;
    }
    gtk_widget_set_visible(item->button, !in_popup);
    if (item->ov_wrap) gtk_widget_set_visible(item->ov_wrap, in_popup);
}

static gboolean on_xembed_plug_removed(GtkSocket *socket, gpointer user_data) {
    TrayItem *item = (TrayItem *)user_data;
    /* moved to the other slot: not gone */
    if (item && item->migrating) { item->migrating = FALSE; return TRUE; }
    if (item && GTK_WIDGET(socket) != item->plug_in) return TRUE;   /* a slot it already left */
    SystrayWidget *tray = item ? item->tray : NULL;
    if (tray) tray->all_items = g_list_remove(tray->all_items, item);
    if (item && GTK_IS_WIDGET(item->button)) gtk_widget_destroy(item->button);
    if (item && GTK_IS_WIDGET(item->ov_wrap)) gtk_widget_destroy(item->ov_wrap);
    if (item) { g_free(item->id); g_free(item->display_name); g_free(item); }
    if (tray) { relayout_tray_containers(tray); rebuild_settings_list_ui(tray); }
    return TRUE;
}

static GdkFilterReturn xembed_root_event_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data) {
    SystrayWidget *tray = (SystrayWidget *)data;
    XEvent *xev = (XEvent *)xevent;
    if (!tray || !tray->xembed_box || xev->type != ClientMessage) {
        return GDK_FILTER_CONTINUE;
    }

    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return GDK_FILTER_CONTINUE;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);

    Atom opcode_atom = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);
    if (xev->xclient.message_type == opcode_atom && xev->xclient.data.l[1] == SYSTEM_TRAY_REQUEST_DOCK) {
        Window plug_xid = (Window)xev->xclient.data.l[2];
        if (plug_xid != None) {
            gint icon_px = tray->shell ? CLAMP(tray->shell->tray_icon_size, 14, 28) : 16;   /* same as the others */
            gint plug_pid = 0;
            gchar *plug_name = NULL;
            {
                Atom pid_atom = XInternAtom(dpy, "_NET_WM_PID", False);
                Atom type_ret; int fmt_ret; unsigned long n_items = 0, bytes_after = 0;
                unsigned char *prop = NULL;
                gdk_x11_display_error_trap_push(gdk_dpy);
                if (XGetWindowProperty(dpy, plug_xid, pid_atom, 0, 1, False, XA_CARDINAL,
                        &type_ret, &fmt_ret, &n_items, &bytes_after, &prop) == Success && prop) {
                    if (n_items == 1 && fmt_ret == 32) plug_pid = (gint)(*(unsigned long *)prop);
                    XFree(prop);
                }
                XClassHint hint = { NULL, NULL };
                if (XGetClassHint(dpy, plug_xid, &hint)) {
                    plug_name = g_strdup(hint.res_name);
                    if (hint.res_name) XFree(hint.res_name);
                    if (hint.res_class) XFree(hint.res_class);
                }
                gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
            }
            gchar *comm = get_comm_for_pid(plug_pid);
            const gchar *app = (plug_name && *plug_name) ? plug_name : (comm && *comm ? comm : "unknown");

            /* Stable id so the user's drag order is remembered between sessions */
            gchar *base_id = g_strdup_printf("xembed:%s", app);
            gchar *item_id = g_strdup(base_id);
            for (gint n = 2; find_tray_item_by_id(tray, item_id); n++) {
                g_free(item_id);
                item_id = g_strdup_printf("%s#%d", base_id, n);
            }
            g_free(base_id);

            TrayItem *item = g_new0(TrayItem, 1);
            item->tray = tray;
            item->id = item_id;
            item->display_name = g_strdup(app);
            item->is_xembed = TRUE;
            item->is_builtin = FALSE;
            item->is_hidden = load_hidden_state_for_id(tray, item_id, FALSE);   /* remembered like any icon */
            item->pid = plug_pid;
            item->plug_xid = (gulong)plug_xid;
            item->is_network_applet = is_network_applet_name(comm) || is_network_applet_name(plug_name);
            g_free(comm);
            g_free(plug_name);

            GtkWidget *wrap = xembed_make_slot(item, icon_px, &item->socket);
            item->button = wrap;
            setup_tray_item_dnd(item);
            item->plug_in = NULL;                                  /* relayout embeds it where it belongs */

            tray->all_items = g_list_append(tray->all_items, item);
            tray->all_items = g_list_sort(tray->all_items, compare_tray_items_by_saved_order);
            relayout_tray_containers(tray);
            xembed_refresh_visibility(tray);
            tray_hide_empty_boxes(tray);
            rebuild_settings_list_ui(tray);
            return GDK_FILTER_REMOVE;
        }
    }
    return GDK_FILTER_CONTINUE;
}

static void init_x11_xembed_tray_manager(SystrayWidget *tray) {
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    int screen_num = DefaultScreen(dpy);
    Window root_win = RootWindow(dpy, screen_num);

    gchar *atom_name = g_strdup_printf("_NET_SYSTEM_TRAY_S%d", screen_num);
    Atom tray_sel_atom = XInternAtom(dpy, atom_name, False);
    g_free(atom_name);

    tray->xembed_Owner_win = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_window_move(GTK_WINDOW(tray->xembed_Owner_win), -9999, -9999);
    gtk_window_resize(GTK_WINDOW(tray->xembed_Owner_win), 1, 1);
    gtk_widget_realize(tray->xembed_Owner_win);

    GdkWindow *gdk_owner = gtk_widget_get_window(tray->xembed_Owner_win);
    if (!gdk_owner) return;
    Window xid_owner = GDK_WINDOW_XID(gdk_owner);

    Atom orient_atom = XInternAtom(dpy, "_NET_SYSTEM_TRAY_ORIENTATION", False);
    long horz = 0;
    XChangeProperty(dpy, xid_owner, orient_atom, XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&horz, 1);

    GdkScreen *gscreen = gdk_display_get_default_screen(gdk_dpy);
    GdkVisual *rgba_vis = gdk_screen_get_rgba_visual(gscreen);
    if (rgba_vis) {
        Atom visual_atom = XInternAtom(dpy, "_NET_SYSTEM_TRAY_VISUAL", False);
        VisualID vid = XVisualIDFromVisual(GDK_VISUAL_XVISUAL(rgba_vis));
        XChangeProperty(dpy, xid_owner, visual_atom, XA_VISUALID, 32, PropModeReplace, (unsigned char *)&vid, 1);
    }

    XSetSelectionOwner(dpy, tray_sel_atom, xid_owner, CurrentTime);
    if (XGetSelectionOwner(dpy, tray_sel_atom) == xid_owner) {
        gdk_window_add_filter(gdk_owner, xembed_root_event_filter, tray);
        gdk_window_add_filter(NULL, xembed_root_event_filter, tray);

        XClientMessageEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = ClientMessage;
        ev.window = root_win;
        ev.message_type = XInternAtom(dpy, "MANAGER", False);
        ev.format = 32;
        ev.data.l[0] = CurrentTime;
        ev.data.l[1] = tray_sel_atom;
        ev.data.l[2] = xid_owner;
        ev.data.l[3] = 0;
        ev.data.l[4] = 0;
        XSendEvent(dpy, root_win, False, StructureNotifyMask, (XEvent *)&ev);
        XFlush(dpy);
    }
}

static void on_item_show_toggled(GtkToggleButton *chk, TrayItem *item) {
    item->is_hidden = !gtk_toggle_button_get_active(chk);
    save_tray_layout_to_xfconf(item->tray);
    relayout_tray_containers(item->tray);
}

static void on_item_move_left_clicked(GtkButton *btn, TrayItem *item) {
    SystrayWidget *tray = item->tray;
    gint idx = g_list_index(tray->all_items, item);
    if (idx > 0) {
        tray->all_items = g_list_remove(tray->all_items, item);
        tray->all_items = g_list_insert(tray->all_items, item, idx - 1);
        save_tray_layout_to_xfconf(tray);
        relayout_tray_containers(tray);
        rebuild_settings_list_ui(tray);
    }
}

static void on_item_move_right_clicked(GtkButton *btn, TrayItem *item) {
    SystrayWidget *tray = item->tray;
    gint idx = g_list_index(tray->all_items, item);
    gint count = g_list_length(tray->all_items);
    if (idx >= 0 && idx < count - 1) {
        tray->all_items = g_list_remove(tray->all_items, item);
        tray->all_items = g_list_insert(tray->all_items, item, idx + 1);
        save_tray_layout_to_xfconf(tray);
        relayout_tray_containers(tray);
        rebuild_settings_list_ui(tray);
    }
}

static void rebuild_settings_list_ui(SystrayWidget *tray) {
    if (!tray || !tray->settings_list_box || !GTK_IS_WIDGET(tray->settings_list_box)) return;

    GList *old = gtk_container_get_children(GTK_CONTAINER(tray->settings_list_box));
    for (GList *l = old; l != NULL; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(old);

    if (qs_enabled(tray)) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *note = gtk_label_new("Sound, Network, Power, Bluetooth and the microphone are in Quick Settings.");
        gtk_label_set_xalign(GTK_LABEL(note), 0.0);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_style_context_add_class(gtk_widget_get_style_context(note), "dim-label");
        gtk_widget_set_margin_start(note, 6); gtk_widget_set_margin_top(note, 4); gtk_widget_set_margin_bottom(note, 4);
        gtk_container_add(GTK_CONTAINER(row), note);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_container_add(GTK_CONTAINER(tray->settings_list_box), row);
    }
    for (GList *l = tray->all_items; l != NULL; l = l->next) {
        TrayItem *item = (TrayItem *)l->data;
        if (qs_is_member(tray, item)) continue;            /* managed in Quick Settings */
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(hbox, 6);
        gtk_widget_set_margin_end(hbox, 6);
        gtk_widget_set_margin_top(hbox, 4);
        gtk_widget_set_margin_bottom(hbox, 4);

        /* first line only, shortened with "…" (some apps put a whole help text in their title);
           the full text is the tooltip - it used to make the Settings window very wide */
        const gchar *full = item->display_name ? item->display_name : item->id;
        gchar *first = g_strdup(full ? full : "");
        gchar *nl = strchr(first, '\n');
        if (nl) *nl = '\0';
        GtkWidget *lbl = gtk_label_new(g_strstrip(first));
        g_free(first);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_width_chars(GTK_LABEL(lbl), 8);
        if (full && (strchr(full, '\n') || g_utf8_strlen(full, -1) > 24)) gtk_widget_set_tooltip_text(lbl, full);

        GtkWidget *show_chk = gtk_check_button_new_with_label("Show on Panel");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show_chk), !item->is_hidden);
        if (item->is_xembed)
            gtk_widget_set_tooltip_text(show_chk, "Older tray icon: when hidden it waits in its place and ▲ shows it.");
        g_signal_connect(show_chk, "toggled", G_CALLBACK(on_item_show_toggled), item);

        GtkWidget *btn_left  = gtk_button_new_with_label("⬅");
        GtkWidget *btn_right = gtk_button_new_with_label("➡");
        g_signal_connect(btn_left, "clicked", G_CALLBACK(on_item_move_left_clicked), item);
        g_signal_connect(btn_right, "clicked", G_CALLBACK(on_item_move_right_clicked), item);

        gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), btn_right, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), btn_left, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), show_chk, FALSE, FALSE, 6);

        gtk_container_add(GTK_CONTAINER(row), hbox);
        gtk_container_add(GTK_CONTAINER(tray->settings_list_box), row);
    }
    gtk_widget_show_all(tray->settings_list_box);
}

void systray_populate_settings_box(UltimateShell *shell, GtkWidget *target_vbox) {
    if (!g_systray) return;

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>Arrange &amp; Show/Hide Status Icons:</b>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
    gtk_box_pack_start(GTK_BOX(target_vbox), hdr, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 380);

    g_systray->settings_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(g_systray->settings_list_box), GTK_SELECTION_NONE);
    g_object_add_weak_pointer(G_OBJECT(g_systray->settings_list_box), (gpointer *)&g_systray->settings_list_box);
    gtk_container_add(GTK_CONTAINER(scroll), g_systray->settings_list_box);
    gtk_box_pack_start(GTK_BOX(target_vbox), scroll, TRUE, TRUE, 0);

    rebuild_settings_list_ui(g_systray);
}


/* =========================================================================
 * Built-in BlueZ Bluetooth Manager Flyout (System D-Bus org.bluez)
 * ========================================================================= */
typedef struct {
    gchar    *path;
    gchar    *adapter;
    gchar    *address;
    gchar    *name;
    gchar    *icon;
    gboolean  has_real_name;
    gboolean  paired;
    gboolean  connected;
    gboolean  trusted;
    gint      rssi;
    gint      battery_pct;
} BtDevInfo;

typedef struct {
    gchar    *path;
    gchar    *alias;
    gboolean  powered;
    gboolean  discovering;
} BtAdapterInfo;

static void free_bt_dev_info(gpointer data) {
    BtDevInfo *d = (BtDevInfo *)data;
    if (!d) return;
    g_free(d->path);
    g_free(d->adapter);
    g_free(d->address);
    g_free(d->name);
    g_free(d->icon);
    g_free(d);
}

static void free_bt_adapter_info(gpointer data) {
    BtAdapterInfo *a = (BtAdapterInfo *)data;
    if (!a) return;
    g_free(a->path);
    g_free(a->alias);
    g_free(a);
}

static void refresh_bluetooth_ui(SystrayWidget *tray);

/* -------------------------------------------------------------------------
 * Live updates: BlueZ signals -> one debounced refresh (replaces polling)
 * ------------------------------------------------------------------------- */
static gboolean on_bt_refresh_debounce(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    tray->bt_refresh_id = 0;
    refresh_bluetooth_ui(tray);
    return G_SOURCE_REMOVE;
}

static void schedule_bt_refresh(SystrayWidget *tray) {
    if (!tray) return;
    if (tray->bt_refresh_id == 0) {
        tray->bt_refresh_id = g_timeout_add(350, on_bt_refresh_debounce, tray);
    }
}

static void on_bluez_signal(GDBusConnection *conn, const gchar *sender, const gchar *obj_path,
                            const gchar *iface, const gchar *signal, GVariant *params, gpointer user_data) {
    schedule_bt_refresh((SystrayWidget *)user_data);
}

/* -------------------------------------------------------------------------
 * Friendly error messages
 * ------------------------------------------------------------------------- */
static gboolean on_bt_error_expired(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    tray->bt_error_timer_id = 0;
    g_clear_pointer(&tray->bt_error_msg, g_free);
    schedule_bt_refresh(tray);
    return G_SOURCE_REMOVE;
}

static void bt_set_error(SystrayWidget *tray, const gchar *msg) {
    if (!tray || !msg) return;
    g_free(tray->bt_error_msg);
    tray->bt_error_msg = g_strdup(msg);
    if (tray->bt_error_timer_id) g_source_remove(tray->bt_error_timer_id);
    tray->bt_error_timer_id = g_timeout_add_seconds(12, on_bt_error_expired, tray);
    schedule_bt_refresh(tray);
}

/* Returns a newly allocated user-facing message, or NULL when the "error" is harmless. */
static gchar *bt_friendly_error(const GError *err) {
    if (!err) return NULL;
    gchar *remote = g_dbus_error_get_remote_error(err);
    const gchar *r = remote ? remote : "";
    const gchar *m = err->message ? err->message : "";
    gchar *out = NULL;

    if (g_str_has_suffix(r, "AlreadyConnected") || g_str_has_suffix(r, "AlreadyExists")) {
        out = NULL;
    } else if (g_strstr_len(m, -1, "profile-unavailable")) {
        out = g_strdup("No Bluetooth audio service is running. Install the PipeWire Bluetooth plugin "
                       "(libspa-0.2-bluetooth) or pulseaudio-module-bluetooth, then reconnect.");
    } else if (g_str_has_suffix(r, "AuthenticationCanceled") || g_str_has_suffix(r, "Canceled")) {
        out = g_strdup("Pairing was cancelled.");
    } else if (g_str_has_suffix(r, "AuthenticationRejected") || g_str_has_suffix(r, "Rejected")) {
        out = g_strdup("Pairing was rejected.");
    } else if (g_str_has_suffix(r, "AuthenticationFailed")) {
        out = g_strdup("Pairing failed. The code didn't match or the device refused.");
    } else if (g_str_has_suffix(r, "AuthenticationTimeout") ||
               g_error_matches(err, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ||
               g_str_has_suffix(r, "NoReply")) {
        out = g_strdup("The device didn't respond in time. Make sure it is in pairing mode.");
    } else if (g_str_has_suffix(r, "ConnectionAttemptFailed") ||
               g_strstr_len(m, -1, "page-timeout") || g_strstr_len(m, -1, "Host is down")) {
        out = g_strdup("Couldn't reach the device. Is it switched on and in range?");
    } else if (g_str_has_suffix(r, "InProgress")) {
        out = g_strdup("Already working on it. Please wait a moment.");
    } else if (g_str_has_suffix(r, "NotReady")) {
        out = g_strdup("Bluetooth is still starting up. Try again in a few seconds.");
    } else {
        GError *copy = g_error_copy(err);
        g_dbus_error_strip_remote_error(copy);
        out = g_strdup_printf("Bluetooth error: %s", copy->message ? copy->message : "unknown");
        g_error_free(copy);
    }
    g_free(remote);
    return out;
}

/* -------------------------------------------------------------------------
 * Small sync helpers (system bus, bounded timeout)
 * ------------------------------------------------------------------------- */
static GVariant *bt_get_device_prop(SystrayWidget *tray, const gchar *dev_path, const gchar *prop) {
    if (!tray->sys_bus || !dev_path) return NULL;
    GVariant *r = g_dbus_connection_call_sync(tray->sys_bus, "org.bluez", dev_path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.bluez.Device1", prop),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    if (!r) return NULL;
    GVariant *v = NULL;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    return v;
}

static gchar *bt_device_display_name(SystrayWidget *tray, const gchar *dev_path) {
    GVariant *v = bt_get_device_prop(tray, dev_path, "Alias");
    gchar *name = NULL;
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) name = g_strdup(g_variant_get_string(v, NULL));
    if (v) g_variant_unref(v);
    return name ? name : g_strdup("A Bluetooth device");
}

static gboolean bt_device_is_trusted(SystrayWidget *tray, const gchar *dev_path) {
    GVariant *v = bt_get_device_prop(tray, dev_path, "Trusted");
    gboolean t = FALSE;
    if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN)) t = g_variant_get_boolean(v);
    if (v) g_variant_unref(v);
    return t;
}

/* -------------------------------------------------------------------------
 * Pairing agent (org.bluez.Agent1) so phones/keyboards can show codes and PINs
 * ------------------------------------------------------------------------- */
#define BT_AGENT_PATH "/org/ultimateshell/BluetoothAgent"

static const gchar bt_agent_xml[] =
    "<node>"
    "  <interface name='org.bluez.Agent1'>"
    "    <method name='Release'/>"
    "    <method name='RequestPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPinCode'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='pincode' direction='in'/>"
    "    </method>"
    "    <method name='RequestPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='out'/>"
    "    </method>"
    "    <method name='DisplayPasskey'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "      <arg type='q' name='entered' direction='in'/>"
    "    </method>"
    "    <method name='RequestConfirmation'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='u' name='passkey' direction='in'/>"
    "    </method>"
    "    <method name='RequestAuthorization'>"
    "      <arg type='o' name='device' direction='in'/>"
    "    </method>"
    "    <method name='AuthorizeService'>"
    "      <arg type='o' name='device' direction='in'/>"
    "      <arg type='s' name='uuid' direction='in'/>"
    "    </method>"
    "    <method name='Cancel'/>"
    "  </interface>"
    "</node>";

static void bt_agent_close_display(SystrayWidget *tray) {
    if (tray->bt_agent_display && GTK_IS_WIDGET(tray->bt_agent_display)) {
        gtk_widget_destroy(tray->bt_agent_display);
    }
    tray->bt_agent_display = NULL;
}

/* Answer the pending request (if any) with a rejection and close its dialog. */
static void bt_agent_cancel_pending(SystrayWidget *tray, const gchar *dbus_err, const gchar *why) {
    if (tray->bt_agent_inv) {
        g_dbus_method_invocation_return_dbus_error(tray->bt_agent_inv, dbus_err, why);
        tray->bt_agent_inv = NULL;
    }
    g_clear_pointer(&tray->bt_agent_method, g_free);
    if (tray->bt_agent_dialog && GTK_IS_WIDGET(tray->bt_agent_dialog)) {
        GtkWidget *dlg = tray->bt_agent_dialog;
        tray->bt_agent_dialog = NULL;
        gtk_widget_destroy(dlg);
    }
    tray->bt_agent_dialog = NULL;
    tray->bt_agent_entry = NULL;
}

static void on_bt_agent_dialog_response(GtkDialog *dlg, gint response, SystrayWidget *tray) {
    if (tray->bt_agent_dialog != GTK_WIDGET(dlg)) {   /* stale dialog */
        gtk_widget_destroy(GTK_WIDGET(dlg));
        return;
    }
    GDBusMethodInvocation *inv = tray->bt_agent_inv;
    const gchar *method = tray->bt_agent_method ? tray->bt_agent_method : "";
    tray->bt_agent_inv = NULL;

    if (inv) {
        if (response != GTK_RESPONSE_ACCEPT) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Rejected by user");
        } else if (g_strcmp0(method, "RequestPinCode") == 0) {
            const gchar *txt = tray->bt_agent_entry ? gtk_entry_get_text(GTK_ENTRY(tray->bt_agent_entry)) : "";
            if (txt && txt[0]) {
                g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", txt));
            } else {
                g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Empty PIN");
            }
        } else if (g_strcmp0(method, "RequestPasskey") == 0) {
            const gchar *txt = tray->bt_agent_entry ? gtk_entry_get_text(GTK_ENTRY(tray->bt_agent_entry)) : "";
            gchar *end = NULL;
            guint64 val = g_ascii_strtoull(txt ? txt : "", &end, 10);
            if (txt && txt[0] && end && *end == '\0' && val <= 999999) {
                g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", (guint32)val));
            } else {
                g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Invalid passkey");
            }
        } else {
            /* RequestConfirmation / RequestAuthorization / AuthorizeService */
            g_dbus_method_invocation_return_value(inv, NULL);
        }
    }
    g_clear_pointer(&tray->bt_agent_method, g_free);
    tray->bt_agent_dialog = NULL;
    tray->bt_agent_entry = NULL;
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

/* Build a question dialog; with_entry adds a text field (PIN / passkey). */
static void bt_agent_ask(SystrayWidget *tray, GDBusMethodInvocation *inv, const gchar *method,
                         const gchar *title, const gchar *body, const gchar *ok_label, gboolean with_entry) {
    /* Only one request at a time: a new one supersedes the old */
    bt_agent_cancel_pending(tray, "org.bluez.Error.Canceled", "Superseded by a new request");

    GtkWidget *dlg = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", body);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg), "_Cancel", GTK_RESPONSE_REJECT, ok_label, GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_window_set_title(GTK_WINDOW(dlg), "Bluetooth");
    gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);

    tray->bt_agent_entry = NULL;
    if (with_entry) {
        GtkWidget *area = gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(dlg));
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        gtk_entry_set_max_length(GTK_ENTRY(entry), 16);
        gtk_box_pack_start(GTK_BOX(area), entry, FALSE, FALSE, 0);
        tray->bt_agent_entry = entry;
    }

    tray->bt_agent_inv = inv;
    tray->bt_agent_method = g_strdup(method);
    tray->bt_agent_dialog = dlg;
    g_signal_connect(dlg, "response", G_CALLBACK(on_bt_agent_dialog_response), tray);
    gtk_widget_show_all(dlg);
    if (tray->bt_agent_entry) gtk_widget_grab_focus(tray->bt_agent_entry);
    gtk_window_present(GTK_WINDOW(dlg));
}

static void on_bt_display_response(GtkDialog *dlg, gint response, SystrayWidget *tray) {
    if (tray->bt_agent_display == GTK_WIDGET(dlg)) tray->bt_agent_display = NULL;
    gtk_widget_destroy(GTK_WIDGET(dlg));
}

/* Informational dialog: "type this code on the device" (no answer needed). */
static void bt_agent_show_code(SystrayWidget *tray, const gchar *dev_name, const gchar *code) {
    bt_agent_close_display(tray);
    GtkWidget *dlg = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
                                            "Pairing with %s", dev_name);
    gchar *markup = g_markup_printf_escaped(
        "Type this code on the device, then press Enter:\n\n<span size='xx-large' weight='bold'>%s</span>", code);
    gtk_message_dialog_format_secondary_markup(GTK_MESSAGE_DIALOG(dlg), "%s", markup);
    g_free(markup);
    gtk_window_set_title(GTK_WINDOW(dlg), "Bluetooth");
    gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    g_signal_connect(dlg, "response", G_CALLBACK(on_bt_display_response), tray);
    tray->bt_agent_display = dlg;
    gtk_widget_show_all(dlg);
    gtk_window_present(GTK_WINDOW(dlg));
}

static void handle_bt_agent_method(GDBusConnection *conn, const gchar *sender, const gchar *obj_path,
                                   const gchar *iface, const gchar *method, GVariant *params,
                                   GDBusMethodInvocation *inv, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;

    if (g_strcmp0(method, "Release") == 0) {
        tray->bt_agent_registered = FALSE;
        bt_agent_cancel_pending(tray, "org.bluez.Error.Canceled", "Agent released");
        bt_agent_close_display(tray);
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }
    if (g_strcmp0(method, "Cancel") == 0) {
        bt_agent_cancel_pending(tray, "org.bluez.Error.Canceled", "Canceled");
        bt_agent_close_display(tray);
        g_dbus_method_invocation_return_value(inv, NULL);
        return;
    }

    const gchar *dev = NULL;
    if (g_strcmp0(method, "RequestPinCode") == 0) {
        g_variant_get(params, "(&o)", &dev);
        gchar *name = bt_device_display_name(tray, dev);
        gchar *title = g_strdup_printf("Pair with %s?", name);
        bt_agent_ask(tray, inv, method, title,
                     "Enter the PIN for this device. Many devices use 0000 or 1234, "
                     "or show a PIN on their own screen.", "_Pair", TRUE);
        g_free(title); g_free(name);
    } else if (g_strcmp0(method, "RequestPasskey") == 0) {
        g_variant_get(params, "(&o)", &dev);
        gchar *name = bt_device_display_name(tray, dev);
        gchar *title = g_strdup_printf("Pair with %s?", name);
        bt_agent_ask(tray, inv, method, title,
                     "Enter the 6-digit code shown on the device.", "_Pair", TRUE);
        g_free(title); g_free(name);
    } else if (g_strcmp0(method, "DisplayPinCode") == 0) {
        const gchar *pin = NULL;
        g_variant_get(params, "(&o&s)", &dev, &pin);
        gchar *name = bt_device_display_name(tray, dev);
        bt_agent_show_code(tray, name, pin ? pin : "");
        g_free(name);
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "DisplayPasskey") == 0) {
        guint32 passkey = 0;
        guint16 entered = 0;
        g_variant_get(params, "(&ouq)", &dev, &passkey, &entered);
        /* BlueZ calls this again as digits are typed; only open the dialog once */
        if (!tray->bt_agent_display) {
            gchar *name = bt_device_display_name(tray, dev);
            gchar *code = g_strdup_printf("%06u", passkey);
            bt_agent_show_code(tray, name, code);
            g_free(code); g_free(name);
        }
        g_dbus_method_invocation_return_value(inv, NULL);
    } else if (g_strcmp0(method, "RequestConfirmation") == 0) {
        guint32 passkey = 0;
        g_variant_get(params, "(&ou)", &dev, &passkey);
        gchar *name = bt_device_display_name(tray, dev);
        gchar *title = g_strdup_printf("Pair with %s?", name);
        gchar *body = g_strdup_printf("Make sure this code is shown on the device too:\n\n%06u", passkey);
        bt_agent_ask(tray, inv, method, title, body, "_Pair", FALSE);
        g_free(body); g_free(title); g_free(name);
    } else if (g_strcmp0(method, "RequestAuthorization") == 0) {
        g_variant_get(params, "(&o)", &dev);
        gchar *name = bt_device_display_name(tray, dev);
        gchar *title = g_strdup_printf("Pair with %s?", name);
        bt_agent_ask(tray, inv, method, title, "This device wants to pair with your computer.", "_Allow", FALSE);
        g_free(title); g_free(name);
    } else if (g_strcmp0(method, "AuthorizeService") == 0) {
        const gchar *uuid = NULL;
        g_variant_get(params, "(&o&s)", &dev, &uuid);
        if (bt_device_is_trusted(tray, dev)) {
            g_dbus_method_invocation_return_value(inv, NULL);
        } else {
            gchar *name = bt_device_display_name(tray, dev);
            gchar *title = g_strdup_printf("Allow %s to connect?", name);
            bt_agent_ask(tray, inv, method, title,
                         "This device is asking to use a Bluetooth service on your computer.", "_Allow", FALSE);
            g_free(title); g_free(name);
        }
    } else {
        g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Unknown method");
    }
}

static const GDBusInterfaceVTable bt_agent_vtable = {
    handle_bt_agent_method, NULL, NULL, { 0 }
};

static void on_bt_agent_registered(GObject *src, GAsyncResult *res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r) {
        g_variant_unref(r);
        tray->bt_agent_registered = TRUE;
        /* Become the default agent so incoming pairing requests reach us */
        g_dbus_connection_call(tray->sys_bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1",
            "RequestDefaultAgent", g_variant_new("(o)", BT_AGENT_PATH),
            NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
    } else {
        /* AlreadyExists means we are registered already (e.g. after a BlueZ hiccup) */
        gchar *remote = err ? g_dbus_error_get_remote_error(err) : NULL;
        if (remote && g_str_has_suffix(remote, "AlreadyExists")) tray->bt_agent_registered = TRUE;
        g_free(remote);
        g_clear_error(&err);
    }
}

static void bt_register_agent(SystrayWidget *tray) {
    if (!tray->sys_bus || tray->bt_agent_obj_id == 0) return;
    g_dbus_connection_call(tray->sys_bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1",
        "RegisterAgent", g_variant_new("(os)", BT_AGENT_PATH, "KeyboardDisplay"),
        NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, on_bt_agent_registered, tray);
}

static void on_bluez_appeared(GDBusConnection *conn, const gchar *name, const gchar *owner, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    tray->bt_agent_registered = FALSE;
    bt_register_agent(tray);
    schedule_bt_refresh(tray);
}

static void on_bluez_vanished(GDBusConnection *conn, const gchar *name, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    tray->bt_agent_registered = FALSE;
    bt_agent_cancel_pending(tray, "org.bluez.Error.Canceled", "BlueZ stopped");
    bt_agent_close_display(tray);
    schedule_bt_refresh(tray);
}

/* One-time setup: system bus, agent object, BlueZ watch, live signal subscriptions. */
static void bt_setup_live(SystrayWidget *tray) {
    if (!tray || tray->bt_live_ready) return;
    if (tray->shell && !tray->shell->tray_enable_bluetooth) return;
    if (!tray->sys_bus) tray->sys_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!tray->sys_bus) return;
    tray->bt_live_ready = TRUE;

    GError *err = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml(bt_agent_xml, &err);
    if (node) {
        tray->bt_agent_obj_id = g_dbus_connection_register_object(tray->sys_bus, BT_AGENT_PATH,
            node->interfaces[0], &bt_agent_vtable, tray, NULL, &err);
        g_dbus_node_info_unref(node);   /* the registration keeps its own ref on the interface */
    }
    g_clear_error(&err);

    tray->bt_sig_props_id = g_dbus_connection_signal_subscribe(tray->sys_bus, "org.bluez",
        "org.freedesktop.DBus.Properties", "PropertiesChanged", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_signal, tray, NULL);
    tray->bt_sig_added_id = g_dbus_connection_signal_subscribe(tray->sys_bus, "org.bluez",
        "org.freedesktop.DBus.ObjectManager", "InterfacesAdded", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_signal, tray, NULL);
    tray->bt_sig_removed_id = g_dbus_connection_signal_subscribe(tray->sys_bus, "org.bluez",
        "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved", NULL, NULL,
        G_DBUS_SIGNAL_FLAGS_NONE, on_bluez_signal, tray, NULL);

    /* Fires on_bluez_appeared right away if BlueZ is already running */
    tray->bt_bluez_watch_id = g_bus_watch_name_on_connection(tray->sys_bus, "org.bluez",
        G_BUS_NAME_WATCHER_FLAGS_NONE, on_bluez_appeared, on_bluez_vanished, tray, NULL);
}

/* -------------------------------------------------------------------------
 * Pair -> Trust -> Connect, in order, with feedback
 * ------------------------------------------------------------------------- */
typedef struct {
    SystrayWidget *tray;
    gchar         *path;
} BtOp;

static BtOp *bt_op_new(SystrayWidget *tray, const gchar *path) {
    BtOp *op = g_new0(BtOp, 1);
    op->tray = tray;
    op->path = g_strdup(path);
    return op;
}

static void bt_op_free(BtOp *op) {
    if (!op) return;
    g_free(op->path);
    g_free(op);
}

static void bt_set_busy(SystrayWidget *tray, const gchar *path, const gchar *label) {
    if (!tray->bt_busy) tray->bt_busy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (label) g_hash_table_replace(tray->bt_busy, g_strdup(path), g_strdup(label));
    else g_hash_table_remove(tray->bt_busy, path);
    schedule_bt_refresh(tray);
}

/* Shared finisher: report error (if any), clear the busy state */
static void bt_op_finish(BtOp *op, GError *err) {
    if (err) {
        gchar *msg = bt_friendly_error(err);
        if (msg) bt_set_error(op->tray, msg);
        g_free(msg);
    }
    bt_set_busy(op->tray, op->path, NULL);
    bt_agent_close_display(op->tray);
    bt_op_free(op);
}

static void on_bt_connect_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    BtOp *op = (BtOp *)user_data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r) g_variant_unref(r);
    bt_op_finish(op, err);
    g_clear_error(&err);
}

static void bt_start_connect(BtOp *op) {
    bt_set_busy(op->tray, op->path, "Connecting…");
    g_dbus_connection_call(op->tray->sys_bus, "org.bluez", op->path, "org.bluez.Device1", "Connect",
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 25000, NULL, on_bt_connect_done, op);
}

static void on_bt_pair_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    BtOp *op = (BtOp *)user_data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r) g_variant_unref(r);

    gboolean ok = (err == NULL);
    if (!ok) {
        gchar *remote = g_dbus_error_get_remote_error(err);
        if (remote && g_str_has_suffix(remote, "AlreadyExists")) ok = TRUE;   /* already paired */
        g_free(remote);
    }
    if (!ok) {
        bt_op_finish(op, err);
        g_clear_error(&err);
        return;
    }
    g_clear_error(&err);
    bt_agent_close_display(op->tray);

    /* Trust it so it can reconnect by itself later, then connect */
    g_dbus_connection_call(op->tray->sys_bus, "org.bluez", op->path, "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", "org.bluez.Device1", "Trusted", g_variant_new_boolean(TRUE)),
        NULL, G_DBUS_CALL_FLAGS_NONE, 4000, NULL, NULL, NULL);
    bt_start_connect(op);
}

static void on_bt_disconnect_done(GObject *src, GAsyncResult *res, gpointer user_data) {
    BtOp *op = (BtOp *)user_data;
    GError *err = NULL;
    GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
    if (r) g_variant_unref(r);
    bt_op_finish(op, err);
    g_clear_error(&err);
}

static void on_bt_dev_connect_clicked(GtkButton *btn, SystrayWidget *tray) {
    if (!tray || !tray->sys_bus) return;
    const gchar *path = g_object_get_data(G_OBJECT(btn), "bt-path");
    gboolean is_conn  = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bt-conn"));
    gboolean is_pair  = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "bt-pair"));
    if (!path) return;
    if (tray->bt_busy && g_hash_table_contains(tray->bt_busy, path)) return;

    g_clear_pointer(&tray->bt_error_msg, g_free);
    BtOp *op = bt_op_new(tray, path);

    if (is_conn) {
        bt_set_busy(tray, path, "Disconnecting…");
        g_dbus_connection_call(tray->sys_bus, "org.bluez", path, "org.bluez.Device1", "Disconnect",
            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 10000, NULL, on_bt_disconnect_done, op);
    } else if (is_pair) {
        bt_start_connect(op);
    } else {
        /* Discovery must stop before pairing on many adapters */
        if (tray->bt_scanning && tray->bt_adapter_path) {
            g_dbus_connection_call(tray->sys_bus, "org.bluez", tray->bt_adapter_path,
                "org.bluez.Adapter1", "StopDiscovery", NULL, NULL,
                G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL);
        }
        bt_set_busy(tray, path, "Pairing…");
        g_dbus_connection_call(tray->sys_bus, "org.bluez", path, "org.bluez.Device1", "Pair",
            NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 60000, NULL, on_bt_pair_done, op);
    }
}

/* Generic completion for power / scan / remove: report errors, refresh */
static void on_bt_dbus_call_done(GObject *source, GAsyncResult *res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (ret) g_variant_unref(ret);
    if (err) {
        gchar *msg = bt_friendly_error(err);
        if (msg && tray) bt_set_error(tray, msg);
        g_free(msg);
        g_error_free(err);
    }
    if (tray) schedule_bt_refresh(tray);
}

static void on_bt_dev_remove_clicked(GtkButton *btn, SystrayWidget *tray) {
    if (!tray || !tray->sys_bus) return;
    const gchar *path = g_object_get_data(G_OBJECT(btn), "bt-path");
    const gchar *adapter = g_object_get_data(G_OBJECT(btn), "bt-adapter");
    if (!path || !adapter) return;

    g_dbus_connection_call(
        tray->sys_bus, "org.bluez", adapter, "org.bluez.Adapter1", "RemoveDevice",
        g_variant_new("(o)", path),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_bt_dbus_call_done, tray
    );
}

/* -------------------------------------------------------------------------
 * Headset audio mode (Music = high quality, Calls = headset + microphone)
 * ------------------------------------------------------------------------- */
static void bt_set_audio_profile(const gchar *address, gboolean calls) {
    if (!address || !address[0]) return;
    gchar *card = g_strdup_printf("bluez_card.%s", address);
    g_strdelimit(card, ":", '_');
    const gchar *script = calls
        ? "pactl set-card-profile \"$1\" headset-head-unit 2>/dev/null || "
          "pactl set-card-profile \"$1\" handsfree_head_unit 2>/dev/null || "
          "pactl set-card-profile \"$1\" headset_head_unit"
        : "pactl set-card-profile \"$1\" a2dp-sink 2>/dev/null || "
          "pactl set-card-profile \"$1\" a2dp_sink";
    gchar *argv[] = { "sh", "-c", (gchar *)script, "sh", card, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(card);
}

static void on_bt_audio_music_clicked(GtkButton *btn, SystrayWidget *tray) {
    bt_set_audio_profile(g_object_get_data(G_OBJECT(btn), "bt-addr"), FALSE);
}

static void on_bt_audio_calls_clicked(GtkButton *btn, SystrayWidget *tray) {
    bt_set_audio_profile(g_object_get_data(G_OBJECT(btn), "bt-addr"), TRUE);
}

/* -------------------------------------------------------------------------
 * Adapter power / scan / selection
 * ------------------------------------------------------------------------- */
static gboolean on_bt_power_state_set(GtkSwitch *sw, gboolean state, SystrayWidget *tray) {
    if (!tray || tray->updating_bt_ui) return FALSE;
    if (state) {
        gchar *argv[] = { "rfkill", "unblock", "bluetooth", NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, NULL);
    }
    if (tray->sys_bus && tray->bt_adapter_path) {
        g_dbus_connection_call(
            tray->sys_bus, "org.bluez", tray->bt_adapter_path,
            "org.freedesktop.DBus.Properties", "Set",
            g_variant_new("(ssv)", "org.bluez.Adapter1", "Powered", g_variant_new_boolean(state)),
            NULL, G_DBUS_CALL_FLAGS_NONE, 4000, NULL, on_bt_dbus_call_done, tray
        );
    }
    return FALSE;
}

static void on_bt_scan_clicked(GtkButton *btn, SystrayWidget *tray) {
    if (!tray || !tray->sys_bus || !tray->bt_adapter_path) return;
    const gchar *method = tray->bt_scanning ? "StopDiscovery" : "StartDiscovery";
    g_dbus_connection_call(
        tray->sys_bus, "org.bluez", tray->bt_adapter_path,
        "org.bluez.Adapter1", method,
        NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, on_bt_dbus_call_done, tray
    );
}

static void on_bt_adapter_combo_changed(GtkComboBox *combo, SystrayWidget *tray) {
    if (!tray || tray->updating_bt_ui) return;
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id) return;
    g_free(tray->bt_selected_adapter);
    tray->bt_selected_adapter = g_strdup(id);
    schedule_bt_refresh(tray);
}

/* Connected first, then paired, then strongest signal */
static gint compare_bt_devices(gconstpointer a, gconstpointer b) {
    const BtDevInfo *da = (const BtDevInfo *)a;
    const BtDevInfo *db = (const BtDevInfo *)b;
    if (da->connected != db->connected) return da->connected ? -1 : 1;
    if (da->paired != db->paired) return da->paired ? -1 : 1;
    gint ra = da->rssi ? da->rssi : -999;
    gint rb = db->rssi ? db->rssi : -999;
    if (ra != rb) return rb - ra;
    return g_utf8_collate(da->name ? da->name : "", db->name ? db->name : "");
}

/* -------------------------------------------------------------------------
 * Main refresh: read everything from BlueZ, update icon/tooltip and flyout
 * ------------------------------------------------------------------------- */
static void on_bt_managed_objects_ready(GObject *source, GAsyncResult *asyn_res, gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    GError *err = NULL;
    GVariant *res = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), asyn_res, &err);

    if (!tray || !tray->bt_item) {
        if (res) g_variant_unref(res);
        if (err) g_error_free(err);
        return;
    }

    GList *adapters = NULL;
    GList *devices = NULL;

    if (res) {
        GVariantIter *obj_iter = NULL;
        g_variant_get(res, "(a{oa{sa{sv}}})", &obj_iter);
        const gchar *obj_path = NULL;
        GVariant *ifaces_dict = NULL;

        while (g_variant_iter_next(obj_iter, "{&o@a{sa{sv}}}", &obj_path, &ifaces_dict)) {
            GVariant *adapter_props = g_variant_lookup_value(ifaces_dict, "org.bluez.Adapter1", G_VARIANT_TYPE_VARDICT);
            if (adapter_props) {
                BtAdapterInfo *a = g_new0(BtAdapterInfo, 1);
                const gchar *alias = NULL;
                a->path = g_strdup(obj_path);
                g_variant_lookup(adapter_props, "Powered", "b", &a->powered);
                g_variant_lookup(adapter_props, "Discovering", "b", &a->discovering);
                g_variant_lookup(adapter_props, "Alias", "&s", &alias);
                a->alias = g_strdup((alias && alias[0]) ? alias : obj_path);
                adapters = g_list_append(adapters, a);
                g_variant_unref(adapter_props);
            }

            GVariant *dev_props = g_variant_lookup_value(ifaces_dict, "org.bluez.Device1", G_VARIANT_TYPE_VARDICT);
            if (dev_props) {
                BtDevInfo *d = g_new0(BtDevInfo, 1);
                d->path = g_strdup(obj_path);
                d->battery_pct = -1;
                const gchar *addr = NULL, *name = NULL, *alias = NULL, *icon = NULL, *adapter = NULL;
                gint16 rssi_val = 0;
                g_variant_lookup(dev_props, "Address", "&s", &addr);
                g_variant_lookup(dev_props, "Name", "&s", &name);
                g_variant_lookup(dev_props, "Alias", "&s", &alias);
                g_variant_lookup(dev_props, "Icon", "&s", &icon);
                g_variant_lookup(dev_props, "Adapter", "&o", &adapter);
                g_variant_lookup(dev_props, "Paired", "b", &d->paired);
                g_variant_lookup(dev_props, "Connected", "b", &d->connected);
                g_variant_lookup(dev_props, "Trusted", "b", &d->trusted);
                if (g_variant_lookup(dev_props, "RSSI", "n", &rssi_val)) d->rssi = rssi_val;

                GVariant *bat_props = g_variant_lookup_value(ifaces_dict, "org.bluez.Battery1", G_VARIANT_TYPE_VARDICT);
                if (bat_props) {
                    guchar pct = 0;
                    if (g_variant_lookup(bat_props, "Percentage", "y", &pct)) d->battery_pct = (gint)pct;
                    g_variant_unref(bat_props);
                }

                d->address = g_strdup(addr ? addr : "");
                d->adapter = g_strdup(adapter ? adapter : "");
                d->has_real_name = (name && name[0]);
                const gchar *best_name = (alias && alias[0]) ? alias : ((name && name[0]) ? name : d->address);
                d->name = g_strdup(best_name);
                d->icon = g_strdup((icon && icon[0]) ? icon : "bluetooth-symbolic");
                devices = g_list_append(devices, d);
                g_variant_unref(dev_props);
            }
            g_variant_unref(ifaces_dict);
        }
        g_variant_iter_free(obj_iter);
        g_variant_unref(res);
    } else if (err) {
        g_error_free(err);
    }

    /* Pick the adapter: the user's choice if still present, else a powered one, else the first */
    BtAdapterInfo *chosen = NULL;
    for (GList *l = adapters; l && !chosen; l = l->next) {
        BtAdapterInfo *a = (BtAdapterInfo *)l->data;
        if (tray->bt_selected_adapter && g_strcmp0(a->path, tray->bt_selected_adapter) == 0) chosen = a;
    }
    for (GList *l = adapters; l && !chosen; l = l->next) {
        BtAdapterInfo *a = (BtAdapterInfo *)l->data;
        if (a->powered) chosen = a;
    }
    if (!chosen && adapters) chosen = (BtAdapterInfo *)adapters->data;

    gboolean adapter_found = (chosen != NULL);
    gboolean powered       = chosen ? chosen->powered : FALSE;
    gboolean discovering   = chosen ? chosen->discovering : FALSE;
    g_free(tray->bt_adapter_path);
    tray->bt_adapter_path = chosen ? g_strdup(chosen->path) : NULL;
    tray->bt_scanning = discovering;

    /* Tray icon + tooltip (always, even with the flyout closed) */
    gint connected_count = 0;
    GString *conn_names = g_string_new("");
    for (GList *l = devices; l; l = l->next) {
        BtDevInfo *d = (BtDevInfo *)l->data;
        if (!d->connected) continue;
        connected_count++;
        if (conn_names->len > 0) g_string_append(conn_names, ", ");
        g_string_append(conn_names, d->name);
    }

    gint icon_sz = CLAMP(tray->shell->tray_icon_size, 14, 32);
    const gchar *tray_ico = !(adapter_found && powered) ? "bluetooth-disabled-symbolic"
                          : "bluetooth-active-symbolic";
    gtk_image_set_from_icon_name(GTK_IMAGE(tray->bt_item->image), tray_ico, GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(tray->bt_item->image), icon_sz);

    gchar *tip = NULL;
    if (!adapter_found) tip = g_strdup("Bluetooth: Off / No Adapter");
    else if (!powered)  tip = g_strdup("Bluetooth: Powered Off");
    else if (connected_count > 0) tip = g_strdup_printf("Bluetooth: Connected to %s", conn_names->str);
    else tip = g_strdup("Bluetooth: On (Not Connected)");
    gtk_widget_set_tooltip_text(tray->bt_item->button, tip);
    g_free(tip);
    g_string_free(conn_names, TRUE);

    if (!tray->bt_popup || !gtk_widget_get_visible(tray->bt_popup)) {
        g_list_free_full(devices, free_bt_dev_info);
        g_list_free_full(adapters, free_bt_adapter_info);
        return;
    }

    tray->updating_bt_ui = TRUE;

    /* Adapter chooser: only visible with 2+ adapters */
    if (GTK_IS_COMBO_BOX_TEXT(tray->bt_adapter_combo)) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(tray->bt_adapter_combo));
        for (GList *l = adapters; l; l = l->next) {
            BtAdapterInfo *a = (BtAdapterInfo *)l->data;
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(tray->bt_adapter_combo), a->path, a->alias);
        }
        if (chosen) gtk_combo_box_set_active_id(GTK_COMBO_BOX(tray->bt_adapter_combo), chosen->path);
        gtk_widget_set_visible(tray->bt_adapter_combo, g_list_length(adapters) > 1);
    }

    if (GTK_IS_SWITCH(tray->bt_power_sw)) {
        gtk_widget_set_sensitive(tray->bt_power_sw, adapter_found);
        gtk_switch_set_active(GTK_SWITCH(tray->bt_power_sw), adapter_found && powered);
    }
    if (GTK_IS_BUTTON(tray->bt_scan_btn)) {
        gtk_widget_set_sensitive(tray->bt_scan_btn, adapter_found && powered);
        gtk_button_set_label(GTK_BUTTON(tray->bt_scan_btn), discovering ? "Stop Scan" : "Scan Nearby");
    }
    if (GTK_IS_LABEL(tray->bt_status_lbl)) {
        const gchar *state = !adapter_found ? "Service / Adapter Inactive"
                           : !powered       ? "Powered Off"
                           : discovering    ? "Scanning for Devices..."
                           :                  "Ready";
        gchar *markup = NULL;
        if (tray->bt_error_msg) {
            gchar *esc = g_markup_escape_text(tray->bt_error_msg, -1);
            markup = g_strdup_printf("<b>Bluetooth</b> — %s\n<span foreground='#e5534b'>%s</span>", state, esc);
            g_free(esc);
        } else {
            markup = g_strdup_printf("<b>Bluetooth</b> — %s", state);
        }
        gtk_label_set_markup(GTK_LABEL(tray->bt_status_lbl), markup);
        g_free(markup);
    }
    tray->updating_bt_ui = FALSE;

    if (GTK_IS_CONTAINER(tray->bt_list_box)) {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(tray->bt_list_box));
        for (GList *c = ch; c != NULL; c = c->next) gtk_widget_destroy(GTK_WIDGET(c->data));
        g_list_free(ch);

        if (!adapter_found || !powered) {
            GtkWidget *empty_lbl = gtk_label_new(adapter_found
                ? "Turn on the switch above to enable Bluetooth."
                : "No active Bluetooth hardware adapter found.");
            gtk_widget_set_margin_top(empty_lbl, 24);
            gtk_widget_set_margin_bottom(empty_lbl, 24);
            gtk_box_pack_start(GTK_BOX(tray->bt_list_box), empty_lbl, FALSE, FALSE, 0);
        } else {
            devices = g_list_sort(devices, compare_bt_devices);
            gint shown = 0;
            for (GList *l = devices; l != NULL; l = l->next) {
                BtDevInfo *d = (BtDevInfo *)l->data;
                if (g_strcmp0(d->adapter, chosen->path) != 0) continue;
                if (!d->paired && !d->connected && !discovering) continue;
                /* Hide nameless scan results (bare addresses) */
                if (!d->paired && !d->connected && !d->has_real_name) continue;

                const gchar *busy = tray->bt_busy ? g_hash_table_lookup(tray->bt_busy, d->path) : NULL;

                GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
                gtk_widget_set_margin_start(row, 4);
                gtk_widget_set_margin_end(row, 4);
                gtk_widget_set_margin_top(row, 4);
                gtk_widget_set_margin_bottom(row, 4);

                GtkWidget *ico = gtk_image_new_from_icon_name(d->icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
                gtk_widget_set_valign(ico, GTK_ALIGN_START);
                gtk_box_pack_start(GTK_BOX(row), ico, FALSE, FALSE, 0);

                GtkWidget *text_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
                GtkWidget *name_lbl = gtk_label_new(NULL);
                gchar *esc_name = g_markup_escape_text(d->name, -1);
                gchar *name_markup = d->connected
                    ? g_strdup_printf("<b>%s</b>", esc_name)
                    : g_strdup(esc_name);
                gtk_label_set_markup(GTK_LABEL(name_lbl), name_markup);
                gtk_label_set_ellipsize(GTK_LABEL(name_lbl), PANGO_ELLIPSIZE_END);
                gtk_widget_set_halign(name_lbl, GTK_ALIGN_START);
                gtk_box_pack_start(GTK_BOX(text_vbox), name_lbl, FALSE, FALSE, 0);
                g_free(name_markup);
                g_free(esc_name);

                gchar *sub_str = NULL;
                if (busy)
                    sub_str = g_strdup(busy);
                else if (d->connected && d->battery_pct >= 0)
                    sub_str = g_strdup_printf("Connected • Battery %d%%", d->battery_pct);
                else if (d->connected)
                    sub_str = g_strdup("Connected");
                else if (d->paired)
                    sub_str = g_strdup("Paired");
                else
                    sub_str = g_strdup("Available");

                GtkWidget *sub_lbl = gtk_label_new(sub_str);
                gtk_widget_set_halign(sub_lbl, GTK_ALIGN_START);
                gtk_widget_set_opacity(sub_lbl, 0.72);
                gtk_box_pack_start(GTK_BOX(text_vbox), sub_lbl, FALSE, FALSE, 0);
                g_free(sub_str);

                /* Music / Calls mode for connected audio devices */
                if (d->connected && !busy && g_str_has_prefix(d->icon, "audio-")) {
                    GtkWidget *mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
                    GtkWidget *music_btn = gtk_button_new_with_label("Music");
                    GtkWidget *calls_btn = gtk_button_new_with_label("Calls");
                    gtk_widget_set_tooltip_text(music_btn, "High-quality audio (no microphone)");
                    gtk_widget_set_tooltip_text(calls_btn, "Headset mode with microphone (lower audio quality)");
                    g_object_set_data_full(G_OBJECT(music_btn), "bt-addr", g_strdup(d->address), g_free);
                    g_object_set_data_full(G_OBJECT(calls_btn), "bt-addr", g_strdup(d->address), g_free);
                    g_signal_connect(music_btn, "clicked", G_CALLBACK(on_bt_audio_music_clicked), tray);
                    g_signal_connect(calls_btn, "clicked", G_CALLBACK(on_bt_audio_calls_clicked), tray);
                    gtk_button_set_relief(GTK_BUTTON(music_btn), GTK_RELIEF_NONE);
                    gtk_button_set_relief(GTK_BUTTON(calls_btn), GTK_RELIEF_NONE);
                    gtk_box_pack_start(GTK_BOX(mode_box), music_btn, FALSE, FALSE, 0);
                    gtk_box_pack_start(GTK_BOX(mode_box), calls_btn, FALSE, FALSE, 0);
                    gtk_box_pack_start(GTK_BOX(text_vbox), mode_box, FALSE, FALSE, 0);
                }

                gtk_box_pack_start(GTK_BOX(row), text_vbox, TRUE, TRUE, 0);

                GtkWidget *act_btn = gtk_button_new_with_label(
                    busy ? busy : (d->connected ? "Disconnect" : (d->paired ? "Connect" : "Pair & Connect"))
                );
                gtk_widget_set_valign(act_btn, GTK_ALIGN_CENTER);
                gtk_widget_set_sensitive(act_btn, busy == NULL);
                g_object_set_data_full(G_OBJECT(act_btn), "bt-path", g_strdup(d->path), g_free);
                g_object_set_data(G_OBJECT(act_btn), "bt-conn", GINT_TO_POINTER(d->connected ? 1 : 0));
                g_object_set_data(G_OBJECT(act_btn), "bt-pair", GINT_TO_POINTER(d->paired ? 1 : 0));
                g_signal_connect(act_btn, "clicked", G_CALLBACK(on_bt_dev_connect_clicked), tray);

                if (d->paired && !busy) {
                    GtkWidget *rem_btn = gtk_button_new_with_label("×");
                    gtk_widget_set_valign(rem_btn, GTK_ALIGN_CENTER);
                    gtk_widget_set_tooltip_text(rem_btn, "Forget / Remove Device");
                    g_object_set_data_full(G_OBJECT(rem_btn), "bt-path", g_strdup(d->path), g_free);
                    g_object_set_data_full(G_OBJECT(rem_btn), "bt-adapter", g_strdup(d->adapter), g_free);
                    g_signal_connect(rem_btn, "clicked", G_CALLBACK(on_bt_dev_remove_clicked), tray);
                    gtk_box_pack_end(GTK_BOX(row), rem_btn, FALSE, FALSE, 0);
                }
                gtk_box_pack_end(GTK_BOX(row), act_btn, FALSE, FALSE, 0);

                gtk_box_pack_start(GTK_BOX(tray->bt_list_box), row, FALSE, FALSE, 0);
                shown++;
            }
            if (shown == 0) {
                GtkWidget *none_lbl = gtk_label_new(discovering
                    ? "Searching… Put your device in pairing mode."
                    : "No paired devices yet.\nClick 'Scan Nearby' to discover Bluetooth devices.");
                gtk_label_set_justify(GTK_LABEL(none_lbl), GTK_JUSTIFY_CENTER);
                gtk_widget_set_margin_top(none_lbl, 20);
                gtk_widget_set_margin_bottom(none_lbl, 20);
                gtk_box_pack_start(GTK_BOX(tray->bt_list_box), none_lbl, FALSE, FALSE, 0);
            }
        }
        gtk_widget_show_all(tray->bt_list_box);
    }

    g_list_free_full(devices, free_bt_dev_info);
    g_list_free_full(adapters, free_bt_adapter_info);
}

static void refresh_bluetooth_ui(SystrayWidget *tray) {
    if (!tray || !tray->bt_item) return;
    if (tray->shell && !tray->shell->tray_enable_bluetooth) return;
    bt_setup_live(tray);
    if (!tray->sys_bus) return;

    gboolean popup_open = (tray->bt_popup && gtk_widget_get_visible(tray->bt_popup));
    GDBusCallFlags flags = popup_open ? G_DBUS_CALL_FLAGS_NONE : G_DBUS_CALL_FLAGS_NO_AUTO_START;

    g_dbus_connection_call(
        tray->sys_bus, "org.bluez", "/",
        "org.freedesktop.DBus.ObjectManager", "GetManagedObjects",
        NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
        flags, 1500, NULL, on_bt_managed_objects_ready, tray
    );
}

static void on_bt_toggled(GtkToggleButton *btn, SystrayWidget *tray) {
    if (!tray || !tray->bt_popup) return;
    if (qs_enabled(tray)) {
        if (gtk_toggle_button_get_active(btn)) qs_on_tray_click(tray, btn);
        return;
    }
    if (gtk_toggle_button_get_active(btn)) {
        shell_toggle_popup_window(btn, tray->bt_popup);
        refresh_bluetooth_ui(tray);   /* live signals keep it current from here on */
    } else {
        if (tray->bt_scanning && tray->sys_bus && tray->bt_adapter_path) {
            g_dbus_connection_call(
                tray->sys_bus, "org.bluez", tray->bt_adapter_path,
                "org.bluez.Adapter1", "StopDiscovery",
                NULL, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, NULL, NULL
            );
            tray->bt_scanning = FALSE;
        }
        shell_toggle_popup_window(btn, tray->bt_popup);
    }
}


/* =========================================================================
 * Built-in Removable Drives & USB Eject Manager (Event-Driven GVolumeMonitor)
 * ========================================================================= */
static void refresh_removable_ui(SystrayWidget *tray);

static gboolean is_removable_volume_or_mount(GVolume *vol, GMount *mnt) {
    if (vol) {
        if (g_volume_can_eject(vol)) return TRUE;
        GDrive *drv = g_volume_get_drive(vol);
        if (drv) {
            gboolean rem = g_drive_is_removable(drv) || g_drive_is_media_removable(drv) || g_drive_can_eject(drv);
            g_object_unref(drv);
            if (rem) return TRUE;
        }
    }
    if (mnt) {
        if (g_mount_can_eject(mnt)) return TRUE;
        GDrive *drv = g_mount_get_drive(mnt);
        if (drv) {
            gboolean rem = g_drive_is_removable(drv) || g_drive_is_media_removable(drv) || g_drive_can_eject(drv);
            g_object_unref(drv);
            if (rem) return TRUE;
        }
        /* Phones / cameras / iPhones (gvfs MTP, PTP, AFC). Shared folders (vboxsf) and network
           shares live under /media too, but have no removable drive, so they are NOT listed. */
        GFile *root = g_mount_get_root(mnt);
        if (root) {
            gboolean is_device = g_file_has_uri_scheme(root, "mtp") ||
                                 g_file_has_uri_scheme(root, "gphoto2") ||
                                 g_file_has_uri_scheme(root, "afc");
            g_object_unref(root);
            if (is_device) return TRUE;
        }
    }
    return FALSE;
}

/* ---- messages under the header (errors in red, success in green) ---- */
static gboolean on_rem_msg_expired(gpointer user_data) {
    SystrayWidget *tray = (SystrayWidget *)user_data;
    tray->rem_msg_timer = 0;
    g_clear_pointer(&tray->rem_msg, g_free);
    refresh_removable_ui(tray);
    return G_SOURCE_REMOVE;
}

static void rem_set_msg(SystrayWidget *tray, const gchar *msg, gboolean is_error) {
    if (!tray || !msg) return;
    g_free(tray->rem_msg);
    tray->rem_msg = g_strdup(msg);
    tray->rem_msg_is_error = is_error;
    if (tray->rem_msg_timer) g_source_remove(tray->rem_msg_timer);
    tray->rem_msg_timer = g_timeout_add_seconds(10, on_rem_msg_expired, tray);
    refresh_removable_ui(tray);
}

/* Report a failed mount/unmount/eject in plain words. Cancelled password prompts are silent. */
static void rem_report_error(SystrayWidget *tray, const gchar *name, const gchar *action, GError *err) {
    if (!err) return;
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED) ||
        g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return;
    gchar *msg = NULL;
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_BUSY)) {
        msg = g_strdup_printf("%s is busy. Close any open files or windows using it, then try again.", name);
    } else {
        msg = g_strdup_printf("Couldn't %s %s: %s", action, name, err->message ? err->message : "unknown error");
    }
    rem_set_msg(tray, msg, TRUE);
    g_free(msg);
}

typedef struct {
    SystrayWidget *tray;
    gchar         *name;
} RemOp;

static RemOp *rem_op_new(SystrayWidget *tray, const gchar *name) {
    RemOp *op = g_new0(RemOp, 1);
    op->tray = tray;
    op->name = g_strdup(name && name[0] ? name : "The drive");
    return op;
}

static void rem_op_free(RemOp *op) {
    if (!op) return;
    g_free(op->name);
    g_free(op);
}

/* GTK mount operation: password prompts for encrypted drives, "files in use" dialogs */
static GMountOperation *rem_new_mount_op(void) {
    return gtk_mount_operation_new(NULL);
}

static void on_rem_open_clicked(GtkButton *btn, SystrayWidget *tray) {
    const gchar *uri = g_object_get_data(G_OBJECT(btn), "rem-uri");
    if (!uri || !uri[0]) return;
    gchar *q = g_shell_quote(uri);
    gchar *cmd = g_strdup_printf("exo-open --launch FileManager %s", q);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    g_free(q);
    if (tray && tray->rem_item && GTK_IS_TOGGLE_BUTTON(tray->rem_item->button)) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tray->rem_item->button), FALSE);
    }
}

static void on_rem_mount_finish(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    RemOp *op = (RemOp *)user_data;
    SystrayWidget *tray = op->tray;
    GVolume *vol = G_VOLUME(source_object);
    GError *err = NULL;
    if (g_volume_mount_finish(vol, res, &err)) {
        GMount *mnt = g_volume_get_mount(vol);
        if (mnt) {
            GFile *root = g_mount_get_root(mnt);
            if (root) {
                gchar *uri = g_file_get_uri(root);
                if (uri) {
                    gchar *q = g_shell_quote(uri);
                    gchar *cmd = g_strdup_printf("exo-open --launch FileManager %s", q);
                    g_spawn_command_line_async(cmd, NULL);
                    g_free(cmd);
                    g_free(q);
                    g_free(uri);
                }
                g_object_unref(root);
            }
            g_object_unref(mnt);
        }
    } else if (err) {
        rem_report_error(tray, op->name, "open", err);
        g_error_free(err);
    }
    rem_op_free(op);
    if (tray) refresh_removable_ui(tray);
}

static void on_rem_mount_clicked(GtkButton *btn, SystrayWidget *tray) {
    GVolume *vol = G_VOLUME(g_object_get_data(G_OBJECT(btn), "rem-vol"));
    if (!vol) return;
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    gtk_button_set_label(btn, "Mounting…");
    GMountOperation *mop = rem_new_mount_op();
    g_volume_mount(vol, G_MOUNT_MOUNT_NONE, mop, NULL, on_rem_mount_finish,
                   rem_op_new(tray, g_object_get_data(G_OBJECT(btn), "rem-name")));
    g_object_unref(mop);
}

static void on_rem_unmount_finish(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    RemOp *op = (RemOp *)user_data;
    GError *err = NULL;
    if (g_mount_unmount_with_operation_finish(G_MOUNT(source_object), res, &err)) {
        gchar *msg = g_strdup_printf("You can now safely remove %s.", op->name);
        rem_set_msg(op->tray, msg, FALSE);
        g_free(msg);
    } else {
        rem_report_error(op->tray, op->name, "eject", err);
        g_clear_error(&err);
        refresh_removable_ui(op->tray);
    }
    rem_op_free(op);
}

static void on_rem_eject_finish(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    RemOp *op = (RemOp *)user_data;
    GMount *mnt = G_MOUNT(source_object);
    GError *err = NULL;
    if (g_mount_eject_with_operation_finish(mnt, res, &err)) {
        gchar *msg = g_strdup_printf("You can now safely remove %s.", op->name);
        rem_set_msg(op->tray, msg, FALSE);
        g_free(msg);
        rem_op_free(op);
        return;
    }
    if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
        /* Device can only be unmounted, not ejected */
        g_clear_error(&err);
        GMountOperation *mop = rem_new_mount_op();
        g_mount_unmount_with_operation(mnt, G_MOUNT_UNMOUNT_NONE, mop, NULL, on_rem_unmount_finish, op);
        g_object_unref(mop);
        return;
    }
    rem_report_error(op->tray, op->name, "eject", err);
    g_clear_error(&err);
    refresh_removable_ui(op->tray);
    rem_op_free(op);
}

static void on_rem_eject_clicked(GtkButton *btn, SystrayWidget *tray) {
    GMount *mnt = G_MOUNT(g_object_get_data(G_OBJECT(btn), "rem-mnt"));
    if (!mnt) return;
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    gtk_button_set_label(btn, "Ejecting…");   /* eject/unmount flushes all pending writes itself */
    RemOp *op = rem_op_new(tray, g_object_get_data(G_OBJECT(btn), "rem-name"));
    GMountOperation *mop = rem_new_mount_op();
    if (g_mount_can_eject(mnt)) {
        g_mount_eject_with_operation(mnt, G_MOUNT_UNMOUNT_NONE, mop, NULL, on_rem_eject_finish, op);
    } else {
        g_mount_unmount_with_operation(mnt, G_MOUNT_UNMOUNT_NONE, mop, NULL, on_rem_unmount_finish, op);
    }
    g_object_unref(mop);
}

/* ---- #6: eject / power off a drive that has nothing mounted ---- */
static void on_rem_drive_done(GObject *source_object, GAsyncResult *res, gpointer user_data) {
    RemOp *op = (RemOp *)user_data;
    GDrive *drv = G_DRIVE(source_object);
    GError *err = NULL;
    gboolean ok = g_drive_can_eject(drv)
        ? g_drive_eject_with_operation_finish(drv, res, &err)
        : g_drive_stop_finish(drv, res, &err);
    if (ok) {
        gchar *msg = g_strdup_printf("You can now safely remove %s.", op->name);
        rem_set_msg(op->tray, msg, FALSE);
        g_free(msg);
    } else {
        rem_report_error(op->tray, op->name, "eject", err);
        g_clear_error(&err);
        refresh_removable_ui(op->tray);
    }
    rem_op_free(op);
}

static void on_rem_drive_eject_clicked(GtkButton *btn, SystrayWidget *tray) {
    GDrive *drv = G_DRIVE(g_object_get_data(G_OBJECT(btn), "rem-drive"));
    if (!drv) return;
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
    gtk_button_set_label(btn, "Ejecting…");
    RemOp *op = rem_op_new(tray, g_object_get_data(G_OBJECT(btn), "rem-name"));
    GMountOperation *mop = rem_new_mount_op();
    if (g_drive_can_eject(drv)) {
        g_drive_eject_with_operation(drv, G_MOUNT_UNMOUNT_NONE, mop, NULL, on_rem_drive_done, op);
    } else {
        g_drive_stop(drv, G_MOUNT_UNMOUNT_NONE, mop, NULL, on_rem_drive_done, op);
    }
    g_object_unref(mop);
}

/* ---- #7: "12.3 GB free of 32.0 GB" for local mounts ---- */
static gchar *rem_free_space_text(GFile *root) {
    if (!root || !g_file_is_native(root)) return NULL;
    GFileInfo *fi = g_file_query_filesystem_info(root,
        G_FILE_ATTRIBUTE_FILESYSTEM_FREE "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, NULL, NULL);
    if (!fi) return NULL;
    gchar *out = NULL;
    if (g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
        guint64 size = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        guint64 fre  = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        if (size > 0) {
            gchar *fs = g_format_size(fre);
            gchar *ts = g_format_size(size);
            out = g_strdup_printf("%s free of %s", fs, ts);
            g_free(fs);
            g_free(ts);
        }
    }
    g_object_unref(fi);
    return out;
}

static void refresh_removable_ui(SystrayWidget *tray) {
    if (!tray || !tray->rem_item || !tray->vol_mon) return;

    GList *volumes = g_volume_monitor_get_volumes(tray->vol_mon);
    GList *mounts  = g_volume_monitor_get_mounts(tray->vol_mon);

    gint rem_count = 0;
    GString *names_str = g_string_new("");

    if (GTK_IS_CONTAINER(tray->rem_list_box)) {
        GList *ch = gtk_container_get_children(GTK_CONTAINER(tray->rem_list_box));
        for (GList *c = ch; c != NULL; c = c->next) gtk_widget_destroy(GTK_WIDGET(c->data));
        g_list_free(ch);
    }

    /* 1. Inspect all GVolumes (mounted or unmounted removable partitions) */
    for (GList *l = volumes; l != NULL; l = l->next) {
        GVolume *vol = G_VOLUME(l->data);
        GMount  *mnt = g_volume_get_mount(vol);
        if (!is_removable_volume_or_mount(vol, mnt)) {
            if (mnt) g_object_unref(mnt);
            continue;
        }

        rem_count++;
        gchar *vname = g_volume_get_name(vol);
        if (names_str->len > 0) g_string_append(names_str, ", ");
        g_string_append(names_str, vname ? vname : "USB Drive");

        if (GTK_IS_CONTAINER(tray->rem_list_box)) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_start(row, 4);
            gtk_widget_set_margin_end(row, 4);
            gtk_widget_set_margin_top(row, 4);
            gtk_widget_set_margin_bottom(row, 4);

            GIcon *gico = g_volume_get_icon(vol);
            GtkWidget *ico = gico
                ? gtk_image_new_from_gicon(gico, GTK_ICON_SIZE_LARGE_TOOLBAR)
                : gtk_image_new_from_icon_name("drive-removable-media-usb", GTK_ICON_SIZE_LARGE_TOOLBAR);
            if (gico) g_object_unref(gico);
            gtk_box_pack_start(GTK_BOX(row), ico, FALSE, FALSE, 0);

            GtkWidget *tvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
            GtkWidget *nlbl = gtk_label_new(NULL);
            gchar *esc = g_markup_escape_text(vname ? vname : "Removable Drive", -1);
            gchar *mk  = g_strdup_printf("<b>%s</b>", esc);
            gtk_label_set_markup(GTK_LABEL(nlbl), mk);
            gtk_label_set_ellipsize(GTK_LABEL(nlbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_halign(nlbl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(tvbox), nlbl, FALSE, FALSE, 0);
            g_free(mk);
            g_free(esc);

            gchar *uri = NULL;
            gchar *sub_txt = NULL;
            if (mnt) {
                GFile *root = g_mount_get_root(mnt);
                if (root) {
                    uri = g_file_get_uri(root);
                    gchar *space = rem_free_space_text(root);
                    gchar *path = g_file_get_path(root);
                    sub_txt = space ? g_strdup(space) : g_strdup(path ? path : "Mounted");
                    g_free(space);
                    g_free(path);
                    g_object_unref(root);
                } else {
                    sub_txt = g_strdup("Mounted");
                }
            } else {
                sub_txt = g_strdup("Unmounted • Click Mount to open");
            }

            GtkWidget *slbl = gtk_label_new(sub_txt);
            gtk_label_set_ellipsize(GTK_LABEL(slbl), PANGO_ELLIPSIZE_MIDDLE);
            gtk_widget_set_halign(slbl, GTK_ALIGN_START);
            gtk_widget_set_opacity(slbl, 0.72);
            gtk_box_pack_start(GTK_BOX(tvbox), slbl, FALSE, FALSE, 0);
            g_free(sub_txt);

            gtk_box_pack_start(GTK_BOX(row), tvbox, TRUE, TRUE, 0);

            if (mnt) {
                GtkWidget *ej_btn = gtk_button_new_with_label("⏏ Eject");
                gtk_widget_set_tooltip_text(ej_btn, "Safely Sync & Eject Drive");
                g_object_set_data_full(G_OBJECT(ej_btn), "rem-mnt", g_object_ref(mnt), g_object_unref);
                g_object_set_data_full(G_OBJECT(ej_btn), "rem-name", g_strdup(vname ? vname : "The drive"), g_free);
                g_signal_connect(ej_btn, "clicked", G_CALLBACK(on_rem_eject_clicked), tray);
                gtk_box_pack_end(GTK_BOX(row), ej_btn, FALSE, FALSE, 0);

                if (uri) {
                    GtkWidget *op_btn = gtk_button_new_with_label("Open");
                    g_object_set_data_full(G_OBJECT(op_btn), "rem-uri", uri, g_free);
                    g_signal_connect(op_btn, "clicked", G_CALLBACK(on_rem_open_clicked), tray);
                    gtk_box_pack_end(GTK_BOX(row), op_btn, FALSE, FALSE, 0);
                }
            } else {
                g_free(uri);
                GDrive *drv = g_volume_get_drive(vol);
                if (drv && (g_drive_can_eject(drv) || g_drive_can_stop(drv))) {
                    GtkWidget *dej_btn = gtk_button_new_with_label("⏏");
                    gtk_widget_set_tooltip_text(dej_btn, "Safely eject / power off this drive");
                    g_object_set_data_full(G_OBJECT(dej_btn), "rem-drive", g_object_ref(drv), g_object_unref);
                    g_object_set_data_full(G_OBJECT(dej_btn), "rem-name", g_strdup(vname ? vname : "The drive"), g_free);
                    g_signal_connect(dej_btn, "clicked", G_CALLBACK(on_rem_drive_eject_clicked), tray);
                    gtk_box_pack_end(GTK_BOX(row), dej_btn, FALSE, FALSE, 0);
                }
                if (drv) g_object_unref(drv);

                GtkWidget *mt_btn = gtk_button_new_with_label("Mount & Open");
                g_object_set_data_full(G_OBJECT(mt_btn), "rem-vol", g_object_ref(vol), g_object_unref);
                g_object_set_data_full(G_OBJECT(mt_btn), "rem-name", g_strdup(vname ? vname : "The drive"), g_free);
                g_signal_connect(mt_btn, "clicked", G_CALLBACK(on_rem_mount_clicked), tray);
                gtk_box_pack_end(GTK_BOX(row), mt_btn, FALSE, FALSE, 0);
            }

            gtk_box_pack_start(GTK_BOX(tray->rem_list_box), row, FALSE, FALSE, 0);
        }

        g_free(vname);
        if (mnt) g_object_unref(mnt);
    }

    /* 2. Also include any standalone GMounts without a parent GVolume (e.g. MTP phones / camera mounts) */
    for (GList *l = mounts; l != NULL; l = l->next) {
        GMount *mnt = G_MOUNT(l->data);
        GVolume *vol = g_mount_get_volume(mnt);
        if (vol) {
            g_object_unref(vol);
            continue; /* Already handled in volumes loop above */
        }
        if (!is_removable_volume_or_mount(NULL, mnt)) continue;

        rem_count++;
        gchar *mname = g_mount_get_name(mnt);
        if (names_str->len > 0) g_string_append(names_str, ", ");
        g_string_append(names_str, mname ? mname : "External Media");

        if (GTK_IS_CONTAINER(tray->rem_list_box)) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_start(row, 4);
            gtk_widget_set_margin_end(row, 4);
            gtk_widget_set_margin_top(row, 4);
            gtk_widget_set_margin_bottom(row, 4);

            GIcon *gico = g_mount_get_icon(mnt);
            GtkWidget *ico = gico
                ? gtk_image_new_from_gicon(gico, GTK_ICON_SIZE_LARGE_TOOLBAR)
                : gtk_image_new_from_icon_name("media-removable", GTK_ICON_SIZE_LARGE_TOOLBAR);
            if (gico) g_object_unref(gico);
            gtk_box_pack_start(GTK_BOX(row), ico, FALSE, FALSE, 0);

            GtkWidget *tvbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
            GtkWidget *nlbl = gtk_label_new(NULL);
            gchar *esc = g_markup_escape_text(mname ? mname : "External Media", -1);
            gchar *mk  = g_strdup_printf("<b>%s</b>", esc);
            gtk_label_set_markup(GTK_LABEL(nlbl), mk);
            gtk_label_set_ellipsize(GTK_LABEL(nlbl), PANGO_ELLIPSIZE_END);
            gtk_widget_set_halign(nlbl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(tvbox), nlbl, FALSE, FALSE, 0);
            g_free(mk);
            g_free(esc);

            GFile *root = g_mount_get_root(mnt);
            gchar *uri  = root ? g_file_get_uri(root) : NULL;
            gchar *path = root ? g_file_get_path(root) : NULL;
            if (root) g_object_unref(root);

            GtkWidget *slbl = gtk_label_new(path ? path : "Mounted Media");
            gtk_label_set_ellipsize(GTK_LABEL(slbl), PANGO_ELLIPSIZE_MIDDLE);
            gtk_widget_set_halign(slbl, GTK_ALIGN_START);
            gtk_widget_set_opacity(slbl, 0.72);
            gtk_box_pack_start(GTK_BOX(tvbox), slbl, FALSE, FALSE, 0);
            g_free(path);

            gtk_box_pack_start(GTK_BOX(row), tvbox, TRUE, TRUE, 0);

            GtkWidget *ej_btn = gtk_button_new_with_label("⏏ Eject");
            g_object_set_data_full(G_OBJECT(ej_btn), "rem-mnt", g_object_ref(mnt), g_object_unref);
            g_object_set_data_full(G_OBJECT(ej_btn), "rem-name", g_strdup(mname ? mname : "The device"), g_free);
            g_signal_connect(ej_btn, "clicked", G_CALLBACK(on_rem_eject_clicked), tray);
            gtk_box_pack_end(GTK_BOX(row), ej_btn, FALSE, FALSE, 0);

            if (uri) {
                GtkWidget *op_btn = gtk_button_new_with_label("Open");
                g_object_set_data_full(G_OBJECT(op_btn), "rem-uri", uri, g_free);
                g_signal_connect(op_btn, "clicked", G_CALLBACK(on_rem_open_clicked), tray);
                gtk_box_pack_end(GTK_BOX(row), op_btn, FALSE, FALSE, 0);
            }

            gtk_box_pack_start(GTK_BOX(tray->rem_list_box), row, FALSE, FALSE, 0);
        }
        g_free(mname);
    }

    g_list_free_full(volumes, g_object_unref);
    g_list_free_full(mounts, g_object_unref);

    gint icon_sz = CLAMP(tray->shell->tray_icon_size, 14, 32);
    gtk_image_set_from_icon_name(GTK_IMAGE(tray->rem_item->image), "media-removable-symbolic", GTK_ICON_SIZE_MENU);
    gtk_image_set_pixel_size(GTK_IMAGE(tray->rem_item->image), icon_sz);

    gchar *tip = (rem_count > 0)
        ? g_strdup_printf("Removable Drives (%d): %s", rem_count, names_str->str)
        : g_strdup("Removable Drives: None Connected");
    gtk_widget_set_tooltip_text(tray->rem_item->button, tip);
    g_free(tip);
    g_string_free(names_str, TRUE);

    if (GTK_IS_LABEL(tray->rem_status_lbl)) {
        gchar *hdr = (rem_count > 0)
            ? g_strdup_printf("<b>Removable Drives</b> (%d connected)", rem_count)
            : g_strdup("<b>Removable Drives &amp; USB Media</b>");
        if (tray->rem_msg) {
            gchar *esc = g_markup_escape_text(tray->rem_msg, -1);
            gchar *full = g_strdup_printf("%s\n<span foreground='%s'>%s</span>", hdr,
                                          tray->rem_msg_is_error ? "#e5534b" : "#3fb950", esc);
            g_free(esc);
            g_free(hdr);
            hdr = full;
        }
        gtk_label_set_markup(GTK_LABEL(tray->rem_status_lbl), hdr);
        g_free(hdr);
    }

    if (GTK_IS_CONTAINER(tray->rem_list_box)) {
        if (rem_count == 0) {
            GtkWidget *empty = gtk_label_new("No removable USB drives or SD cards connected.");
            gtk_widget_set_margin_top(empty, 26);
            gtk_widget_set_margin_bottom(empty, 26);
            gtk_box_pack_start(GTK_BOX(tray->rem_list_box), empty, FALSE, FALSE, 0);
        }
        gtk_widget_show_all(tray->rem_list_box);
    }
}

static void on_vol_mon_changed(GVolumeMonitor *vm, gpointer item, SystrayWidget *tray) {
    if (tray) refresh_removable_ui(tray);
}

static void on_rem_toggled(GtkToggleButton *btn, SystrayWidget *tray) {
    if (!tray || !tray->rem_popup) return;
    if (gtk_toggle_button_get_active(btn)) {
        refresh_removable_ui(tray);
    }
    shell_toggle_popup_window(btn, tray->rem_popup);
}

/* -------------------------------------------------------------------------
 * IBus tray protocol. ibus-ui-gtk3 only uses a StatusNotifier icon (movable,
 * like every other app) when it believes it runs on KDE; otherwise it uses
 * an old XEmbed icon that can't leave the panel. We restart IBus once in the
 * wanted mode, keeping the user's engines (they live in dconf).
 * ------------------------------------------------------------------------- */
static gint ibus_find_pid(const gchar *comm_name) {
    GDir *d = g_dir_open("/proc", 0, NULL);
    if (!d) return 0;
    const gchar *e;
    gint found = 0;
    uid_t me = getuid();
    while (!found && (e = g_dir_read_name(d))) {
        if (!g_ascii_isdigit(e[0])) continue;
        gint pid = atoi(e);
        gchar *comm = get_comm_for_pid(pid);
        if (comm && g_strcmp0(comm, comm_name) == 0) {
            gchar *p = g_strdup_printf("/proc/%d", pid);
            struct stat st;
            if (stat(p, &st) == 0 && st.st_uid == me) found = pid;
            g_free(p);
        }
        g_free(comm);
    }
    g_dir_close(d);
    return found;
}

/* TRUE if the running ibus-ui-gtk3 was started with XDG_CURRENT_DESKTOP containing KDE */
static gboolean ibus_ui_is_modern(gint pid, gboolean *readable) {
    *readable = FALSE;
    gchar *path = g_strdup_printf("/proc/%d/environ", pid);
    gchar *env = NULL; gsize len = 0;
    gboolean modern = FALSE;
    if (g_file_get_contents(path, &env, &len, NULL)) {
        *readable = TRUE;
        for (gsize off = 0; off < len; off += strlen(env + off) + 1) {
            if (g_str_has_prefix(env + off, "XDG_CURRENT_DESKTOP=")) {
                modern = (strstr(env + off, "KDE") != NULL);
                break;
            }
        }
    }
    g_free(env); g_free(path);
    return modern;
}

static void ibus_restart(gboolean modern) {
    gchar **envp = g_get_environ();
    const gchar *cur = g_environ_getenv(envp, "XDG_CURRENT_DESKTOP");
    envp = g_environ_setenv(envp, "XDG_CURRENT_DESKTOP",
                            modern ? "KDE" : ((cur && !strstr(cur, "KDE")) ? cur : "XFCE"), TRUE);
    gchar *argv[] = { "ibus-daemon", "-drxR", NULL };
    g_spawn_async(NULL, argv, envp, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_strfreev(envp);
}

/* force = the user just changed the option; otherwise only once per panel start */
void systray_ibus_apply_mode(UltimateShell *shell, gboolean force) {
    static gboolean done_once = FALSE;
    if (!shell || (!force && done_once)) return;
    done_once = TRUE;
    gchar *ibus_bin = g_find_program_in_path("ibus-daemon");
    if (!ibus_bin) return;
    g_free(ibus_bin);
    gint ui = ibus_find_pid("ibus-ui-gtk3");
    if (ui <= 0) return;                              /* IBus not running: nothing to do */
    gboolean readable = FALSE;
    gboolean modern = ibus_ui_is_modern(ui, &readable);
    if (!readable) return;
    if (modern != shell->tray_ibus_modern) ibus_restart(shell->tray_ibus_modern);
}

static gboolean ibus_mode_check_delayed(gpointer data) {
    systray_ibus_apply_mode((UltimateShell *)data, FALSE);
    return G_SOURCE_REMOVE;
}

/* Mic indicator click: open the sound popup (anchored to the speaker icon) */
static void on_mic_item_clicked(GtkButton *b, SystrayWidget *tray) {
    if (tray->vol_item && GTK_IS_TOGGLE_BUTTON(tray->vol_item->button) && gtk_widget_get_visible(tray->vol_item->button)) {
        GtkToggleButton *vb = GTK_TOGGLE_BUTTON(tray->vol_item->button);
        gtk_toggle_button_set_active(vb, !gtk_toggle_button_get_active(vb));
    } else {
        on_open_pavucontrol(NULL, tray);
    }
}

GtkWidget *systray_widget_new(UltimateShell *shell) {
    SystrayWidget *tray = g_new0(SystrayWidget, 1);
    g_systray = tray;
    tray->shell = shell;
    tray->session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

    tray->root_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    tray->visible_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);

    tray->sni_by_key = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, free_tray_item);
    tray->watcher_node = g_dbus_node_info_new_for_xml(sni_watcher_xml, NULL);

    /* Chevron */
    tray->chevron_btn = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->chevron_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->chevron_btn), "ultimate-module-btn");
    gtk_widget_set_tooltip_text(tray->chevron_btn, "Show Hidden Icons");
    gtk_widget_set_valign(tray->chevron_btn, GTK_ALIGN_FILL);

    tray->chevron_img = gtk_image_new_from_icon_name("pan-up-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->chevron_img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->chevron_img, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->chevron_btn), tray->chevron_img);

    gtk_drag_dest_set(tray->chevron_btn, GTK_DEST_DEFAULT_ALL, tray_dnd_targets, G_N_ELEMENTS(tray_dnd_targets), GDK_ACTION_MOVE);
    g_signal_connect(tray->chevron_btn, "drag-data-received", G_CALLBACK(on_chevron_drag_received), tray);

    tray->overflow_popup = shell_create_popup_window(tray->chevron_btn, -1, -1, FALSE);
    g_object_set_data(G_OBJECT(tray->overflow_popup), "center-on-button", GINT_TO_POINTER(1));
    GtkWidget *ov_frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_set_border_width(GTK_CONTAINER(ov_frame), 10);

    tray->overflow_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(tray->overflow_grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(tray->overflow_grid), 6);
    gtk_widget_set_halign(tray->overflow_grid, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->overflow_grid, GTK_ALIGN_CENTER);

    gtk_box_pack_start(GTK_BOX(ov_frame), tray->overflow_grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(tray->overflow_popup), ov_frame);
    g_signal_connect(tray->chevron_btn, "toggled", G_CALLBACK(on_chevron_toggled), tray);

    gtk_box_pack_start(GTK_BOX(tray->root_box), tray->chevron_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tray->root_box), tray->visible_box, FALSE, FALSE, 0);

    /* Built-in Network */
    tray->net_item = g_new0(TrayItem, 1);
    tray->net_item->tray = tray;
    tray->net_item->id = g_strdup("builtin:network");
    tray->net_item->display_name = g_strdup("🌐 Network Connection");
    tray->net_item->is_builtin = TRUE;
    tray->net_item->is_hidden = load_hidden_state_for_id(tray, "builtin:network", TRUE);
    tray->net_item->button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->net_item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->net_item->button), "ultimate-module-btn");
    tray->net_item->image = gtk_image_new_from_icon_name("network-wireless-signal-excellent-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->net_item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->net_item->image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->net_item->button), tray->net_item->image);
    setup_tray_item_dnd(tray->net_item);
    g_signal_connect(tray->net_item->button, "button-press-event", G_CALLBACK(on_builtin_btn_press), tray->net_item);

    tray->net_popup = shell_create_popup_window(tray->net_item->button, 310, 320, FALSE);
    g_object_set_data(G_OBJECT(tray->net_popup), "center-on-button", GINT_TO_POINTER(1));
    GtkWidget *net_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(net_vbox), 12);
    tray->net_status_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(tray->net_status_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(net_vbox), tray->net_status_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(net_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    GtkWidget *net_scroll = gtk_scrolled_window_new(NULL, NULL);
    tray->wifi_list_box = gtk_list_box_new();
    g_signal_connect(tray->wifi_list_box, "row-activated", G_CALLBACK(on_wifi_row_activated), tray);
    gtk_container_add(GTK_CONTAINER(net_scroll), tray->wifi_list_box);
    gtk_box_pack_start(GTK_BOX(net_vbox), net_scroll, TRUE, TRUE, 0);

    GtkWidget *net_cfg_btn = gtk_button_new_with_label("Network Connections & Settings...");
    g_signal_connect(net_cfg_btn, "clicked", G_CALLBACK(on_net_settings_clicked), tray);
    gtk_box_pack_start(GTK_BOX(net_vbox), net_cfg_btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(tray->net_popup), net_vbox);
    g_signal_connect(tray->net_item->button, "toggled", G_CALLBACK(on_net_toggled), tray);

    /* Built-in PulseAudio Exact Parity Menu */
    tray->vol_item = g_new0(TrayItem, 1);
    tray->vol_item->tray = tray;
    tray->vol_item->id = g_strdup("builtin:audio");
    tray->vol_item->display_name = g_strdup("🔊 Audio Control");
    tray->vol_item->is_builtin = TRUE;
    tray->vol_item->is_hidden = load_hidden_state_for_id(tray, "builtin:audio", TRUE);
    tray->vol_item->button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->vol_item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->vol_item->button), "ultimate-module-btn");
    gtk_widget_add_events(tray->vol_item->button, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    tray->vol_item->image = gtk_image_new_from_icon_name("audio-volume-high-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->vol_item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->vol_item->image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->vol_item->button), tray->vol_item->image);

    /* Microphone-in-use indicator: a separate tray icon (own highlight, order, hiding) */
    tray->mic_item = g_new0(TrayItem, 1);
    tray->mic_item->tray = tray;
    tray->mic_item->id = g_strdup("builtin:mic");
    tray->mic_item->display_name = g_strdup("🎤 Microphone in Use");
    tray->mic_item->is_builtin = TRUE;
    tray->mic_item->is_hidden = load_hidden_state_for_id(tray, "builtin:mic", TRUE);
    tray->mic_item->button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->mic_item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->mic_item->button), "ultimate-module-btn");
    tray->mic_item->image = gtk_image_new_from_icon_name("microphone-sensitivity-high-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->mic_item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->mic_item->image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->mic_item->button), tray->mic_item->image);
    tray->pa_rec_img = tray->mic_item->image;
    setup_tray_item_dnd(tray->mic_item);
    g_signal_connect(tray->mic_item->button, "button-press-event", G_CALLBACK(on_builtin_btn_press), tray->mic_item);
    g_signal_connect(tray->mic_item->button, "clicked", G_CALLBACK(on_mic_item_clicked), tray);
    {
        {   /* orange while an app is actually recording (like macOS) */
            GtkCssProvider *rp = gtk_css_provider_new();
            gtk_css_provider_load_from_data(rp, ".ult-rec-live { color: #ff9f0a; }", -1, NULL);
            gtk_style_context_add_provider(gtk_widget_get_style_context(tray->pa_rec_img),
                                           GTK_STYLE_PROVIDER(rp), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 10);
            g_object_unref(rp);
        }
    }
    setup_tray_item_dnd(tray->vol_item);

    tray->vol_menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(tray->vol_menu), TRUE);
    g_signal_connect(tray->vol_menu, "deactivate", G_CALLBACK(on_vol_menu_deactivated), tray);

    /* Index 0: Master Output Volume Scale Row (snaps at 100%) */
    GtkWidget *master_row = create_upstream_scale_menu_item(
        tray, "audio-volume-high-symbolic", FALSE,
        &tray->vol_scale, &tray->vol_switch, &tray->vol_row_img);
    g_signal_connect(tray->vol_scale, "value-changed", G_CALLBACK(on_vol_scale_changed), tray);
    g_signal_connect(tray->vol_switch, "notify::active", G_CALLBACK(on_vol_switch_notify), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), master_row);

    /* Index 1: Output Port/Device Submenu */
    tray->output_submenu_item = gtk_menu_item_new_with_label("Analog Output / Amplifier");
    GtkWidget *output_sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(tray->output_submenu_item), output_sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), tray->output_submenu_item);

    /* Index 2: Separator */
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), gtk_separator_menu_item_new());

    /* Index 3: Microphone Volume Scale Row (snaps at 42% and 100%) */
    GtkWidget *mic_row = create_upstream_scale_menu_item(
        tray, "microphone-sensitivity-high-symbolic", TRUE,
        &tray->mic_scale, &tray->mic_switch, &tray->mic_row_img);
    g_signal_connect(tray->mic_scale, "value-changed", G_CALLBACK(on_mic_scale_changed), tray);
    g_signal_connect(tray->mic_switch, "notify::active", G_CALLBACK(on_mic_switch_notify), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), mic_row);

    /* Index 4: Input Port/Device Submenu */
    tray->input_submenu_item = gtk_menu_item_new_with_label("Microphone / Microphone 1");
    GtkWidget *input_sub = gtk_menu_new();
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(tray->input_submenu_item), input_sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), tray->input_submenu_item);

    /* Index 5: Separator */
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), gtk_separator_menu_item_new());

    /* Index 6+: Dynamic MPRIS items inserted before mpris_separator */
    tray->mpris_separator = gtk_separator_menu_item_new();
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), tray->mpris_separator);

    gtk_style_context_add_class(gtk_widget_get_style_context(tray->vol_menu), "ultimate-themed-menu");
    gtk_style_context_add_class(gtk_widget_get_style_context(output_sub), "ultimate-themed-menu");
    gtk_style_context_add_class(gtk_widget_get_style_context(input_sub), "ultimate-themed-menu");

    /* Footer: Audio mixer... & More Options... */
    GtkWidget *mixer_item = gtk_menu_item_new_with_mnemonic("_Audio mixer...");
    g_signal_connect(mixer_item, "activate", G_CALLBACK(on_open_pavucontrol), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), mixer_item);

    GtkWidget *more_item = gtk_menu_item_new_with_label("More Options / Audio Settings...");
    g_signal_connect(more_item, "activate", G_CALLBACK(on_open_pa_props_menuitem), tray);
    gtk_menu_shell_append(GTK_MENU_SHELL(tray->vol_menu), more_item);

    gtk_widget_show_all(tray->vol_menu);
    gtk_widget_hide(tray->mpris_separator);

    g_signal_connect(tray->vol_item->button, "toggled", G_CALLBACK(on_vol_toggled), tray);
    g_signal_connect(tray->vol_item->button, "scroll-event", G_CALLBACK(on_vol_scroll), tray);
    pa_audio_setup(tray);
    g_signal_connect(tray->vol_item->button, "button-press-event", G_CALLBACK(on_builtin_btn_press), tray->vol_item);

    /* Built-in Battery / Power Status */
    tray->bat_item = g_new0(TrayItem, 1);
    tray->bat_item->tray = tray;
    tray->bat_item->id = g_strdup("builtin:battery");
    tray->bat_item->display_name = g_strdup("🔋 Battery & Power");
    tray->bat_item->is_builtin = TRUE;
    tray->bat_item->is_hidden = load_hidden_state_for_id(tray, "builtin:battery", TRUE);
    tray->bat_item->button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->bat_item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->bat_item->button), "ultimate-module-btn");
    tray->bat_item->image = gtk_image_new_from_icon_name("battery-full-charging-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->bat_item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->bat_item->image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->bat_item->button), tray->bat_item->image);
    setup_tray_item_dnd(tray->bat_item);
    g_signal_connect(tray->bat_item->button, "button-press-event", G_CALLBACK(on_builtin_btn_press), tray->bat_item);

    tray->bat_popup = shell_create_popup_window(tray->bat_item->button, 300, -1, FALSE);
    g_object_set_data(G_OBJECT(tray->bat_popup), "center-on-button", GINT_TO_POINTER(1));
    GtkWidget *bat_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(bat_vbox), 12);
    tray->bat_status_lbl = gtk_label_new("<b>Battery &amp; Power</b>");
    gtk_label_set_use_markup(GTK_LABEL(tray->bat_status_lbl), TRUE);
    gtk_label_set_xalign(GTK_LABEL(tray->bat_status_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(bat_vbox), tray->bat_status_lbl, FALSE, FALSE, 0);

    tray->bat_detail_lbl = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(tray->bat_detail_lbl), 0.0);
    gtk_widget_set_opacity(tray->bat_detail_lbl, 0.75);
    gtk_widget_set_no_show_all(tray->bat_detail_lbl, TRUE);
    gtk_box_pack_start(GTK_BOX(bat_vbox), tray->bat_detail_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bat_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    /* Power mode (shown only when power-profiles-daemon is installed) */
    tray->pwr_profile_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_no_show_all(tray->pwr_profile_box, TRUE);
    GtkWidget *prof_lbl = gtk_label_new("Power mode");
    gtk_label_set_xalign(GTK_LABEL(prof_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(tray->pwr_profile_box), prof_lbl, FALSE, FALSE, 0);
    GtkWidget *prof_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(prof_row), "linked");
    static const gchar *const prof_ids[3]    = { "power-saver", "balanced", "performance" };
    static const gchar *const prof_labels[3] = { "Saver", "Balanced", "Performance" };
    for (gint i = 0; i < 3; i++) {
        GtkWidget *pb = gtk_toggle_button_new_with_label(prof_labels[i]);
        g_object_set_data(G_OBJECT(pb), "pwr-profile", (gpointer)prof_ids[i]);
        g_signal_connect(pb, "toggled", G_CALLBACK(on_pwr_profile_clicked), tray);
        gtk_box_pack_start(GTK_BOX(prof_row), pb, TRUE, TRUE, 0);
        tray->pwr_profile_btns[i] = pb;
    }
    gtk_box_pack_start(GTK_BOX(tray->pwr_profile_box), prof_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bat_vbox), tray->pwr_profile_box, FALSE, FALSE, 0);

    /* Screen brightness (only when the machine has a backlight) */
    tray->pwr_backlight = pwr_find_backlight();
    if (tray->pwr_backlight) {
        GtkWidget *br_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_box_pack_start(GTK_BOX(br_row),
            gtk_image_new_from_icon_name("display-brightness-symbolic", GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
        tray->pwr_bright_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 1, 100, 1);
        gtk_scale_set_draw_value(GTK_SCALE(tray->pwr_bright_scale), FALSE);
        g_signal_connect(tray->pwr_bright_scale, "value-changed", G_CALLBACK(on_pwr_brightness_changed), tray);
        gtk_box_pack_start(GTK_BOX(br_row), tray->pwr_bright_scale, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(bat_vbox), br_row, FALSE, FALSE, 0);
    }

    GtkWidget *pwr_cfg_btn = gtk_button_new_with_label("Power Manager Settings...");
    g_signal_connect(pwr_cfg_btn, "clicked", G_CALLBACK(on_pwr_settings_clicked), tray);
    gtk_box_pack_start(GTK_BOX(bat_vbox), pwr_cfg_btn, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(tray->bat_popup), bat_vbox);
    g_signal_connect(tray->bat_item->button, "toggled", G_CALLBACK(on_bat_toggled), tray);
    power_setup(tray);

    /* Built-in Clipboard Manager (Native Themed Popup + Clipman Integration) */
    tray->clip_item = g_new0(TrayItem, 1);
    tray->clip_item->tray = tray;
    tray->clip_item->id = g_strdup("builtin:clipboard");
    tray->clip_item->display_name = g_strdup("📋 Clipboard Manager (Clipman)");
    tray->clip_item->is_builtin = TRUE;
    tray->clip_item->is_hidden = load_hidden_state_for_id(tray, "builtin:clipboard", TRUE);
    tray->clip_item->button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->clip_item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->clip_item->button), "ultimate-module-btn");
    gtk_widget_set_tooltip_text(tray->clip_item->button, "Clipboard History");
    tray->clip_item->image = gtk_image_new_from_icon_name("edit-paste-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->clip_item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->clip_item->image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->clip_item->button), tray->clip_item->image);
    setup_tray_item_dnd(tray->clip_item);
    g_signal_connect(tray->clip_item->button, "button-press-event", G_CALLBACK(on_builtin_btn_press), tray->clip_item);

    tray->clip_popup = shell_create_popup_window(tray->clip_item->button, 310, 340, FALSE);
    g_object_set_data(G_OBJECT(tray->clip_popup), "center-on-button", GINT_TO_POINTER(1));
    GtkWidget *clip_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(clip_vbox), 12);

    GtkWidget *clip_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *clip_title = gtk_label_new("<b>📋 Clipboard History</b>");
    gtk_label_set_use_markup(GTK_LABEL(clip_title), TRUE);
    gtk_label_set_xalign(GTK_LABEL(clip_title), 0.0);
    GtkWidget *clip_clr_btn = gtk_button_new_with_label("Clear");
    gtk_button_set_relief(GTK_BUTTON(clip_clr_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(clip_clr_btn), "win11-pill-btn");
    g_signal_connect(clip_clr_btn, "clicked", G_CALLBACK(on_clip_clear_clicked), tray);
    gtk_box_pack_start(GTK_BOX(clip_hdr), clip_title, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(clip_hdr), clip_clr_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(clip_vbox), clip_hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(clip_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    GtkWidget *clip_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(clip_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(clip_scroll, TRUE);
    tray->clip_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(tray->clip_list_box), GTK_SELECTION_NONE);
    g_signal_connect(tray->clip_list_box, "row-activated", G_CALLBACK(on_clip_row_activated), tray);
    gtk_container_add(GTK_CONTAINER(clip_scroll), tray->clip_list_box);
    gtk_box_pack_start(GTK_BOX(clip_vbox), clip_scroll, TRUE, TRUE, 0);

    gtk_box_pack_start(GTK_BOX(clip_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    GtkWidget *clip_cfg_btn = gtk_button_new_with_label("Clipman Settings...");
    g_signal_connect(clip_cfg_btn, "clicked", G_CALLBACK(on_clip_settings_clicked), tray);
    gtk_box_pack_start(GTK_BOX(clip_vbox), clip_cfg_btn, FALSE, FALSE, 0);

    gtk_container_add(GTK_CONTAINER(tray->clip_popup), clip_vbox);
        g_signal_connect(tray->clip_item->button, "toggled", G_CALLBACK(on_clip_toggled), tray);

    /* Built-in Removable Drives & USB Eject Indicator (Event-Driven GVolumeMonitor) */
    tray->vol_mon = g_volume_monitor_get();
    g_signal_connect(tray->vol_mon, "volume-added", G_CALLBACK(on_vol_mon_changed), tray);
    g_signal_connect(tray->vol_mon, "volume-removed", G_CALLBACK(on_vol_mon_changed), tray);
    g_signal_connect(tray->vol_mon, "volume-changed", G_CALLBACK(on_vol_mon_changed), tray);
    g_signal_connect(tray->vol_mon, "mount-added", G_CALLBACK(on_vol_mon_changed), tray);
    g_signal_connect(tray->vol_mon, "mount-removed", G_CALLBACK(on_vol_mon_changed), tray);
    g_signal_connect(tray->vol_mon, "mount-changed", G_CALLBACK(on_vol_mon_changed), tray);

    tray->rem_item = g_new0(TrayItem, 1);
    tray->rem_item->tray = tray;
    tray->rem_item->id = g_strdup("builtin:removable");
    tray->rem_item->display_name = g_strdup("Removable Drives & USB Eject");
    tray->rem_item->is_builtin = TRUE;
    tray->rem_item->is_hidden = load_hidden_state_for_id(tray, "builtin:removable", FALSE);
    tray->rem_item->button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->rem_item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->rem_item->button), "ultimate-module-btn");
    gtk_widget_set_tooltip_text(tray->rem_item->button, "Removable Drives");
    tray->rem_item->image = gtk_image_new_from_icon_name("media-removable-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->rem_item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->rem_item->image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->rem_item->button), tray->rem_item->image);
    setup_tray_item_dnd(tray->rem_item);
    g_signal_connect(tray->rem_item->button, "button-press-event", G_CALLBACK(on_builtin_btn_press), tray->rem_item);

    tray->rem_popup = shell_create_popup_window(tray->rem_item->button, 340, 280, FALSE);
    g_object_set_data(G_OBJECT(tray->rem_popup), "center-on-button", GINT_TO_POINTER(1));

    GtkWidget *rem_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(rem_vbox, 14);
    gtk_widget_set_margin_end(rem_vbox, 14);
    gtk_widget_set_margin_top(rem_vbox, 12);
    gtk_widget_set_margin_bottom(rem_vbox, 12);

    GtkWidget *rem_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    tray->rem_status_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(tray->rem_status_lbl), "<b>Removable Drives &amp; USB Media</b>");
    gtk_widget_set_halign(tray->rem_status_lbl, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(tray->rem_status_lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(tray->rem_status_lbl), 34);
    gtk_label_set_xalign(GTK_LABEL(tray->rem_status_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(rem_hdr), tray->rem_status_lbl, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(rem_vbox), rem_hdr, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(rem_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    GtkWidget *rem_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(rem_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(rem_scroll, TRUE);
    tray->rem_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(rem_scroll), tray->rem_list_box);
    gtk_box_pack_start(GTK_BOX(rem_vbox), rem_scroll, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(tray->rem_popup), rem_vbox);
    g_signal_connect(tray->rem_item->button, "toggled", G_CALLBACK(on_rem_toggled), tray);
    refresh_removable_ui(tray);

    tray->all_items = g_list_append(tray->all_items, tray->rem_item);

    /* Built-in Bluetooth Manager Item & Flyout */
    /* Built-in Bluetooth Manager (BlueZ System D-Bus) */
    tray->bt_item = g_new0(TrayItem, 1);
    tray->bt_item->tray = tray;
    tray->bt_item->id = g_strdup("builtin:bluetooth");
    tray->bt_item->display_name = g_strdup("Bluetooth Manager");
    tray->bt_item->is_builtin = TRUE;
    tray->bt_item->is_hidden = load_hidden_state_for_id(tray, "builtin:bluetooth", TRUE);
    tray->bt_item->button = gtk_toggle_button_new();
    gtk_button_set_relief(GTK_BUTTON(tray->bt_item->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(tray->bt_item->button), "ultimate-module-btn");
    gtk_widget_set_tooltip_text(tray->bt_item->button, "Bluetooth Manager");
    tray->bt_item->image = gtk_image_new_from_icon_name("bluetooth-active-symbolic", GTK_ICON_SIZE_MENU);
    gtk_widget_set_halign(tray->bt_item->image, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tray->bt_item->image, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(tray->bt_item->button), tray->bt_item->image);
    setup_tray_item_dnd(tray->bt_item);
    g_signal_connect(tray->bt_item->button, "button-press-event", G_CALLBACK(on_builtin_btn_press), tray->bt_item);
    tray->bt_popup = shell_create_popup_window(tray->bt_item->button, 340, 360, FALSE);
    g_object_set_data(G_OBJECT(tray->bt_popup), "center-on-button", GINT_TO_POINTER(1));

    GtkWidget *bt_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(bt_vbox, 14);
    gtk_widget_set_margin_end(bt_vbox, 14);
    gtk_widget_set_margin_top(bt_vbox, 12);
    gtk_widget_set_margin_bottom(bt_vbox, 12);

    GtkWidget *bt_hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    tray->bt_status_lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(tray->bt_status_lbl), "<b>Bluetooth</b>");
    gtk_widget_set_halign(tray->bt_status_lbl, GTK_ALIGN_START);
    gtk_label_set_line_wrap(GTK_LABEL(tray->bt_status_lbl), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(tray->bt_status_lbl), 30);
    gtk_label_set_xalign(GTK_LABEL(tray->bt_status_lbl), 0.0);
    gtk_box_pack_start(GTK_BOX(bt_hdr), tray->bt_status_lbl, TRUE, TRUE, 0);

    tray->bt_scan_btn = gtk_button_new_with_label("Scan Nearby");
    g_signal_connect(tray->bt_scan_btn, "clicked", G_CALLBACK(on_bt_scan_clicked), tray);
    gtk_box_pack_end(GTK_BOX(bt_hdr), tray->bt_scan_btn, FALSE, FALSE, 0);

    tray->bt_power_sw = gtk_switch_new();
    gtk_widget_set_valign(tray->bt_power_sw, GTK_ALIGN_CENTER);
    g_signal_connect(tray->bt_power_sw, "state-set", G_CALLBACK(on_bt_power_state_set), tray);
    gtk_box_pack_end(GTK_BOX(bt_hdr), tray->bt_power_sw, FALSE, FALSE, 4);

    gtk_box_pack_start(GTK_BOX(bt_vbox), bt_hdr, FALSE, FALSE, 0);

    /* Adapter chooser (shown only when there are 2+ adapters) */
    tray->bt_adapter_combo = gtk_combo_box_text_new();
    gtk_widget_set_no_show_all(tray->bt_adapter_combo, TRUE);
    gtk_widget_set_tooltip_text(tray->bt_adapter_combo, "Bluetooth adapter");
    g_signal_connect(tray->bt_adapter_combo, "changed", G_CALLBACK(on_bt_adapter_combo_changed), tray);
    gtk_box_pack_start(GTK_BOX(bt_vbox), tray->bt_adapter_combo, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(bt_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);

    GtkWidget *bt_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(bt_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(bt_scroll, TRUE);
    tray->bt_list_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_add(GTK_CONTAINER(bt_scroll), tray->bt_list_box);
    gtk_box_pack_start(GTK_BOX(bt_vbox), bt_scroll, TRUE, TRUE, 0);

    gtk_container_add(GTK_CONTAINER(tray->bt_popup), bt_vbox);
    g_signal_connect(tray->bt_item->button, "toggled", G_CALLBACK(on_bt_toggled), tray);
    refresh_bluetooth_ui(tray);   /* sets up the pairing agent + live updates, and the initial icon */

    tray->all_items = g_list_append(tray->all_items, tray->bt_item);
    tray->all_items = g_list_append(tray->all_items, tray->clip_item);
    tray->all_items = g_list_append(tray->all_items, tray->net_item);
    tray->all_items = g_list_append(tray->all_items, tray->vol_item);
    tray->all_items = g_list_append(tray->all_items, tray->mic_item);
    tray->all_items = g_list_append(tray->all_items, tray->bat_item);
    tray->all_items = g_list_sort(tray->all_items, compare_tray_items_by_saved_order);

    update_network_and_audio(tray);
    relayout_tray_containers(tray);
    tray->hw_timer_id = g_timeout_add_seconds(2, on_hw_poll_tick, tray);
    g_signal_connect(tray->root_box, "destroy", G_CALLBACK(on_systray_root_destroy), tray);

    tray->xembed_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_valign(tray->xembed_box, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(tray->root_box), tray->xembed_box, FALSE, FALSE, 0);
    qs_group_create(tray);
    relayout_tray_containers(tray);

    init_x11_xembed_tray_manager(tray);

    tray->kde_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, "org.kde.StatusNotifierWatcher",
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_watcher_bus_acquired, on_watcher_name_acquired, NULL, tray, NULL);

    tray->fdo_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.StatusNotifierWatcher",
        G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
        on_watcher_bus_acquired, on_watcher_name_acquired, NULL, tray, NULL);

    /* after login IBus may start a little after the panel */
    g_timeout_add_seconds(6, ibus_mode_check_delayed, shell);
    return tray->root_box;
}
