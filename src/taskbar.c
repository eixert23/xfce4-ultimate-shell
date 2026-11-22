#include "taskbar.h"
#include "start-menu.h"
#include "launcher-entry.h"
#include <gdk/gdkx.h>
#include <X11/Xatom.h>
#include <cairo-xlib.h>
#include <signal.h>
#include <unistd.h>
#include <gio/gdesktopappinfo.h>
#include <xfconf/xfconf.h>
#include <cairo.h>
#include <math.h>
#include <string.h>

typedef struct _TaskbarWidget TaskbarWidget;

typedef struct {
    TaskbarWidget  *tb;
    gchar          *group_id;
    gchar          *desktop_id;
    gchar          *display_title;
    GAppInfo       *pinned_app;
    gboolean        is_pinned;
    gboolean        is_hovered;
    gboolean        is_pressed;
    gboolean        preview_hovered;
    gboolean        needs_attention;
    GList          *windows;
    GtkWidget      *button;
    GtkWidget      *image;
    GdkPixbuf      *pb_master;
    GtkWidget      *badge_lbl;
    GtkWidget      *preview_popover;
    guint           preview_timeout_id;
    guint           preview_show_id;      /* hover delay before the preview opens */

    /* Click & Urgent Animation State */
    gint64          click_anim_start_us;
    gint            click_anim_type; /* 0 = none, 1 = quick hop, 2 = launch bounce */
    gdouble         press_scale;
    gdouble         bounce_offset;

    /* Badge / progress / urgent sent by the app itself (LauncherEntry) */
    LauncherEntryState le;
    gboolean        has_le;
    gboolean        drop_hover;      /* a file is being dragged over this icon */
    gint            xapp_progress;   /* Firefox & co: _NET_WM_XAPP_PROGRESS 0-100, -1 = none */
} AppGroupItem;

struct _TaskbarWidget {
    UltimateShell         *shell;
    UltimateWindowTracker *tracker;
    XfconfChannel         *channel;
    GtkWidget             *box;
    GHashTable            *groups;
    GHashTable            *win_to_grp;
    guint                  anim_timer_id;
};

static TaskbarWidget *g_taskbar = NULL;

static void refresh_group_visuals(AppGroupItem *grp);
static void ensure_taskbar_anim_timer(TaskbarWidget *tb);

static gint get_current_target_icon_size(TaskbarWidget *tb) {
    if (tb->shell && tb->shell->mode == LAYOUT_MACOS) {
        return CLAMP(tb->shell->dock_icon_size, 20, 64);
    }
    return CLAMP(tb->shell->tb_icon_size, 16, 48);
}

static gboolean is_trackable_window(WnckWindow *window) {
    if (!window) return FALSE;
    if (wnck_window_is_skip_tasklist(window)) return FALSE;
    WnckWindowType type = wnck_window_get_window_type(window);
    return (type == WNCK_WINDOW_NORMAL || type == WNCK_WINDOW_DIALOG);
}

static gchar *normalize_group_key(const gchar *raw) {
    if (!raw || raw[0] == '\0') return g_strdup("unknown-app");
    gchar *lower = g_utf8_strdown(raw, -1);
    if (g_str_has_suffix(lower, ".desktop")) {
        lower[strlen(lower) - 8] = '\0';
    }
    const gchar *last_dot = strrchr(lower, '.');
    if (last_dot && *(last_dot + 1) != '\0') {
        gchar *short_key = g_strdup(last_dot + 1);
        g_free(lower);
        return short_key;
    }
    return lower;
}

static gchar *get_window_normalized_key(WnckWindow *window) {
    WnckClassGroup *cg = wnck_window_get_class_group(window);
    if (cg) {
        const gchar *id = wnck_class_group_get_id(cg);
        if (id && id[0] != '\0') return normalize_group_key(id);
        const gchar *name = wnck_class_group_get_name(cg);
        if (name && name[0] != '\0') return normalize_group_key(name);
    }
    WnckApplication *app = wnck_window_get_application(window);
    if (app) {
        const gchar *aname = wnck_application_get_name(app);
        if (aname && aname[0] != '\0') return normalize_group_key(aname);
    }
    return g_strdup("default-app");
}

static gchar *strip_xfce_prefix(const gchar *s) {
    if (!s) return g_strdup("");
    if (g_str_has_prefix(s, "xfce4-")) return g_strdup(s + 6);
    if (g_str_has_prefix(s, "xfce-"))  return g_strdup(s + 5);
    return g_strdup(s);
}

static gboolean keys_match_loosely(const gchar *a, const gchar *b) {
    if (!a || !b) return FALSE;
    if (g_strcmp0(a, b) == 0) return TRUE;
    gchar *sa = strip_xfce_prefix(a);
    gchar *sb = strip_xfce_prefix(b);
    gboolean eq = (sa[0] != '\0' && g_strcmp0(sa, sb) == 0);
    g_free(sa);
    g_free(sb);
    return eq;
}

static GAppInfo *find_app_info_for_key(const gchar *key_or_desktop) {
    if (!key_or_desktop) return NULL;
    if (g_str_has_suffix(key_or_desktop, ".desktop")) {
        GDesktopAppInfo *dapp = g_desktop_app_info_new(key_or_desktop);
        if (dapp) return G_APP_INFO(dapp);
    } else {
        gchar *try_id = g_strdup_printf("%s.desktop", key_or_desktop);
        GDesktopAppInfo *dapp = g_desktop_app_info_new(try_id);
        g_free(try_id);
        if (dapp) return G_APP_INFO(dapp);
    }

    gchar *norm = normalize_group_key(key_or_desktop);
    GList *all = g_app_info_get_all();
    GAppInfo *found = NULL;
    for (GList *l = all; l != NULL; l = l->next) {
        GAppInfo *app = G_APP_INFO(l->data);
        const gchar *id = g_app_info_get_id(app);
        const gchar *name = g_app_info_get_display_name(app);
        const gchar *exe = g_app_info_get_executable(app);
        gchar *base_exe = exe ? g_path_get_basename(exe) : NULL;

        gchar *n_id  = id ? normalize_group_key(id) : NULL;
        gchar *n_nm  = name ? normalize_group_key(name) : NULL;
        gchar *n_exe = base_exe ? normalize_group_key(base_exe) : NULL;

        const gchar *wm_cls = G_IS_DESKTOP_APP_INFO(app)
            ? g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(app))
            : NULL;
        gchar *n_wm = wm_cls ? normalize_group_key(wm_cls) : NULL;

        if (keys_match_loosely(n_id, norm) ||
            keys_match_loosely(n_exe, norm) ||
            keys_match_loosely(n_wm, norm) ||
            (n_nm && g_strcmp0(n_nm, norm) == 0)) {
            found = g_object_ref(app);
            g_free(base_exe); g_free(n_id); g_free(n_nm); g_free(n_exe); g_free(n_wm);
            break;
        }
        g_free(base_exe); g_free(n_id); g_free(n_nm); g_free(n_exe); g_free(n_wm);
    }
    g_list_free_full(all, g_object_unref);
    g_free(norm);
    return found;
}

GAppInfo *taskbar_find_app_info(const gchar *key) { return find_app_info_for_key(key); }

static AppGroupItem *find_matching_group_for_window(TaskbarWidget *tb, const gchar *win_norm_key) {
    if (!tb || !tb->groups || !win_norm_key) return NULL;

    /* 1. Direct hash table lookup */
    AppGroupItem *direct = g_hash_table_lookup(tb->groups, win_norm_key);
    if (direct) return direct;

    /* 2. Check existing groups by executable name, StartupWMClass, or xfce4/xfce alias */
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, tb->groups);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        AppGroupItem *grp = (AppGroupItem *)v;
        if (keys_match_loosely(grp->group_id, win_norm_key)) {
            return grp;
        }
        if (grp->pinned_app) {
            const gchar *exe = g_app_info_get_executable(grp->pinned_app);
            gchar *base_exe = exe ? g_path_get_basename(exe) : NULL;
            gchar *n_exe = base_exe ? normalize_group_key(base_exe) : NULL;

            const gchar *id = g_app_info_get_id(grp->pinned_app);
            gchar *n_id = id ? normalize_group_key(id) : NULL;

            const gchar *wm_cls = G_IS_DESKTOP_APP_INFO(grp->pinned_app)
                ? g_desktop_app_info_get_startup_wm_class(G_DESKTOP_APP_INFO(grp->pinned_app))
                : NULL;
            gchar *n_wm = wm_cls ? normalize_group_key(wm_cls) : NULL;

            gboolean matched = keys_match_loosely(n_exe, win_norm_key) ||
                               keys_match_loosely(n_id, win_norm_key) ||
                               keys_match_loosely(n_wm, win_norm_key);

            g_free(base_exe); g_free(n_exe); g_free(n_id); g_free(n_wm);
            if (matched) return grp;
        }
    }
    return NULL;
}

static void update_group_display_title(AppGroupItem *grp) {
    if (!grp) return;
    const gchar *best = NULL;
    if (grp->pinned_app) {
        best = g_app_info_get_display_name(grp->pinned_app);
    }
    if ((!best || !best[0]) && grp->windows != NULL) {
        WnckWindow *w = WNCK_WINDOW(grp->windows->data);
        WnckClassGroup *cg = wnck_window_get_class_group(w);
        if (cg && wnck_class_group_get_name(cg) && wnck_class_group_get_name(cg)[0]) {
            best = wnck_class_group_get_name(cg);
        } else {
            best = wnck_window_get_name(w);
        }
    }
    if (!best || !best[0]) best = grp->group_id ? grp->group_id : "Application";
    g_free(grp->display_title);
    grp->display_title = g_strdup(best);
    if (grp->display_title && grp->display_title[0]) {
        grp->display_title[0] = g_ascii_toupper(grp->display_title[0]);
    }
}

static gboolean compute_group_needs_attention(AppGroupItem *grp) {
    if (!grp) return FALSE;
    for (GList *l = grp->windows; l != NULL; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        if (w && wnck_window_needs_attention(w)) return TRUE;
    }
    return FALSE;
}

static void save_pinned_apps_to_xfconf(TaskbarWidget *tb) {
    if (!tb->channel) return;
    GString *csv = g_string_new("");
    GList *children = gtk_container_get_children(GTK_CONTAINER(tb->box));
    for (GList *l = children; l != NULL; l = l->next) {
        GtkWidget *btn = GTK_WIDGET(l->data);
        AppGroupItem *grp = g_object_get_data(G_OBJECT(btn), "app-group-item");
        if (grp && grp->is_pinned) {
            const gchar *save_id = grp->desktop_id ? grp->desktop_id : grp->group_id;
            if (csv->len > 0) g_string_append_c(csv, ';');
            g_string_append(csv, save_id);
        }
    }
    g_list_free(children);
    xfconf_channel_set_string(tb->channel, "/plugins/ultimateshell/pinned_apps", csv->str);
    g_string_free(csv, TRUE);
}

static void ensure_group_master_pixbuf(AppGroupItem *grp) {
    if (!grp || grp->pb_master) return;
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    if (grp->pinned_app) {
        GIcon *gicon = g_app_info_get_icon(grp->pinned_app);
        if (gicon) {
            GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(theme, gicon, 96, GTK_ICON_LOOKUP_FORCE_SIZE);
            if (info) {
                grp->pb_master = gtk_icon_info_load_icon(info, NULL);
                g_object_unref(info);
            }
        }
    }
    if (!grp->pb_master && grp->windows != NULL) {
        WnckWindow *first_win = WNCK_WINDOW(grp->windows->data);
        GdkPixbuf *pix = wnck_window_get_icon(first_win);
        if (pix) grp->pb_master = gdk_pixbuf_scale_simple(pix, 96, 96, GDK_INTERP_BILINEAR);
    }
    if (!grp->pb_master) {
        grp->pb_master = gtk_icon_theme_load_icon(theme, "application-x-executable", 96, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
}

static void set_group_icon_size(AppGroupItem *grp, gint px_size) {
    if (!grp || !GTK_IS_IMAGE(grp->image)) return;
    px_size = CLAMP(px_size, 12, 128);
    ensure_group_master_pixbuf(grp);
    if (grp->pb_master) {
        GdkPixbuf *scaled = gdk_pixbuf_scale_simple(grp->pb_master, px_size, px_size, GDK_INTERP_BILINEAR);
        if (scaled) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(grp->image), scaled);
            g_object_unref(scaled);
            return;
        }
    }
}

/* =========================================================================
 * 60 FPS Animation Ticker (Click Bounce, Tactile Press & Urgent Attention)
 * ========================================================================= */
static gboolean taskbar_anim_tick(gpointer user_data) {
    TaskbarWidget *tb = (TaskbarWidget *)user_data;
    if (!tb || !tb->groups) return G_SOURCE_REMOVE;

    gint64 now_us = g_get_monotonic_time();
    gboolean any_active = FALSE;
    gint base_px = get_current_target_icon_size(tb);

    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, tb->groups);

    while (g_hash_table_iter_next(&iter, &k, &v)) {
        AppGroupItem *grp = (AppGroupItem *)v;
        gboolean redraw_btn = FALSE;

        /* 1. Tactile Press Scale (Windows Taskbar) */
        gdouble target_scale = grp->is_pressed ? 0.85 : 1.0;
        if (fabs(grp->press_scale - target_scale) > 0.01) {
            grp->press_scale += (target_scale - grp->press_scale) * 0.38;
            if (tb->shell && tb->shell->mode != LAYOUT_MACOS) {
                gint scaled_px = MAX(12, (gint)(base_px * grp->press_scale + 0.5));
                set_group_icon_size(grp, scaled_px);
            }
            redraw_btn = TRUE;
            any_active = TRUE;
        } else if (grp->press_scale != target_scale) {
            grp->press_scale = target_scale;
            if (tb->shell && tb->shell->mode != LAYOUT_MACOS) {
                set_group_icon_size(grp, (gint)(base_px * grp->press_scale + 0.5));
            }
            redraw_btn = TRUE;
        }

        /* 2. Parabolic Click Bounce (macOS Dock) */
        gdouble b_off = 0.0;
        if (grp->click_anim_type > 0 && grp->click_anim_start_us > 0) {
            gdouble elapsed = (now_us - grp->click_anim_start_us) / 1000000.0;
            gdouble dur     = (grp->click_anim_type == 2) ? 1.55 : 0.42;
            gdouble hops    = (grp->click_anim_type == 2) ? 3.0  : 1.0;
            gdouble amp     = (grp->click_anim_type == 2) ? 22.0 : 12.0;

            if (elapsed >= dur) {
                grp->click_anim_type = 0;
                grp->click_anim_start_us = 0;
                b_off = 0.0;
            } else {
                gdouble phase = (elapsed / dur) * hops * G_PI;
                gdouble decay = 1.0 - (elapsed / dur) * 0.58;
                b_off = fabs(sin(phase)) * amp * decay;
                any_active = TRUE;
            }
            redraw_btn = TRUE;
        }

        /* 3. Urgent / Needs-Attention Periodic Hop & Glow Pulse */
        grp->needs_attention = compute_group_needs_attention(grp) || (grp->has_le && grp->le.urgent);
        if (grp->needs_attention) {
            any_active = TRUE;
            redraw_btn = TRUE;
            if (b_off == 0.0) {
                /* Periodic double-hop every 2.4 seconds until user focuses window */
                gdouble cyc = fmod(now_us / 1000000.0, 2.4);
                if (cyc < 0.72) {
                    b_off = fabs(sin((cyc / 0.72) * 2.0 * G_PI)) * 14.0 * (1.0 - (cyc / 0.72) * 0.4);
                }
            }
        }

        grp->bounce_offset = b_off;
        if (redraw_btn && GTK_IS_WIDGET(grp->button)) {
            gtk_widget_queue_draw(grp->button);
        }
    }

    if (tb->shell && tb->shell->mode == LAYOUT_MACOS && tb->shell->dock_inner_box) {
        gtk_widget_queue_draw(tb->shell->dock_inner_box);
    }

    if (!any_active) {
        tb->anim_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void ensure_taskbar_anim_timer(TaskbarWidget *tb) {
    if (tb && tb->anim_timer_id == 0) {
        tb->anim_timer_id = g_timeout_add(16, taskbar_anim_tick, tb);
    }
}

/* =========================================================================
 * Cairo Custom Indicator, Active Window Card & Urgent Attention Renderer
 * ========================================================================= */
static void draw_rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r) {
    r = MIN(r, MIN(w / 2.0, h / 2.0));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
    cairo_close_path(cr);
}

static gboolean on_draw_taskbar_item_indicator(GtkWidget *btn, cairo_t *cr, AppGroupItem *grp) {
    if (!grp || !grp->tb || !grp->tb->shell) return FALSE;

    guint count = g_list_length(grp->windows);
    if (count == 0) return FALSE;

    UltimateShell *sh = grp->tb->shell;
    if (sh->mode == LAYOUT_MACOS) return FALSE;

    WnckWindow *active = wnck_screen_get_active_window(grp->tb->tracker->screen);
    gboolean has_active = (g_list_find(grp->windows, active) != NULL);
    gboolean urgent     = grp->needs_attention && sh->ind_urgent_pulse;

    if (!has_active && !urgent && !sh->tb_show_inactive_ind) return FALSE;
    if (sh->ind_style == IND_STYLE_NONE && !urgent) return FALSE;     /* "None" still flags urgent apps */

    gint w = gtk_widget_get_allocated_width(btn);
    gint h = gtk_widget_get_allocated_height(btn);
    if (w <= 4 || h <= 4) return FALSE;

    /* Resolve Accent Color (or pulsing Amber/Orange #FF9500 when window needs attention) */
    GdkRGBA ac = { 0.22, 0.58, 1.00, 1.0 };
    gdouble pulse_alpha = 1.0;

    GdkRGBA c_act, c_ina, c_urg;
    shell_indicator_colors(sh, &c_act, &c_ina, &c_urg);
    if (urgent) {
        ac = c_urg;
        gdouble t_sec = g_get_monotonic_time() / 1000000.0;
        pulse_alpha = 0.65 + 0.35 * (0.5 + 0.5 * sin(t_sec * 6.5));
    } else {
        ac = has_active ? c_act : c_ina;
    }

    TaskbarIndicatorStyle style = sh->ind_style;

    if (style == IND_STYLE_WIN10) {
        if (has_active || urgent) {
            cairo_rectangle(cr, 1.0, 1.0, w - 2.0, h - 2.0);
            if (urgent) cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, 0.22 * pulse_alpha);
            else        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
            cairo_fill(cr);
        }

        gdouble bar_h = 3.0;
        gdouble bar_w = (has_active || urgent) ? w : (grp->is_hovered ? (w * 0.90) : (w * 0.62));
        gdouble bar_x = (w - bar_w) / 2.0;
        gdouble bar_y = h - bar_h;

        cairo_rectangle(cr, bar_x, bar_y, bar_w, bar_h);
        cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, (has_active || urgent) ? pulse_alpha : 0.90);
        cairo_fill(cr);

    } else if (style == IND_STYLE_PLASMA) {
        gdouble bar_h = 3.0;
        gdouble bar_w = (has_active || urgent) ? w : (grp->is_hovered ? (w * 0.85) : (w * 0.52));
        gdouble bar_x = (w - bar_w) / 2.0;
        gdouble bar_y = 0.0;

        if (has_active || urgent) {
            cairo_pattern_t *glow = cairo_pattern_create_linear(0, 0, 0, h * 0.75);
            cairo_pattern_add_color_stop_rgba(glow, 0.0, ac.red, ac.green, ac.blue, 0.28 * pulse_alpha);
            cairo_pattern_add_color_stop_rgba(glow, 1.0, ac.red, ac.green, ac.blue, 0.0);
            cairo_rectangle(cr, 1.0, 0.0, w - 2.0, h);
            cairo_set_source(cr, glow);
            cairo_fill(cr);
            cairo_pattern_destroy(glow);
        }

        draw_rounded_rect(cr, bar_x, bar_y, bar_w, bar_h, 1.5);
        cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, (has_active || urgent) ? pulse_alpha : 0.90);
        cairo_fill(cr);

    } else {
        /* Windows 11 Style: Symmetric rounded active/urgent card + bottom pill */
        if (has_active || urgent) {
            draw_rounded_rect(cr, 2.0, 3.0, w - 4.0, h - 6.0, 5.0);
            if (urgent) {
                cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, 0.22 * pulse_alpha);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, 0.55 * pulse_alpha);
            } else {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
                cairo_fill_preserve(cr);
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.18);
            }
            cairo_set_line_width(cr, 0.75);
            cairo_stroke(cr);
        }

        gdouble pill_h = 3.0;
        gdouble pill_w = (has_active || urgent) ? MIN(16.0, w - 8.0) : (grp->is_hovered ? 10.0 : 6.0);
        gdouble pill_x = (w - pill_w) / 2.0;
        gdouble pill_y = h - pill_h - 1.0;

        draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, pill_h / 2.0);
        cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, (has_active || urgent) ? pulse_alpha : 0.92);
        cairo_fill(cr);
    }

    return FALSE;
}


/* ---- LauncherEntry: badge counts and progress sent by apps ---- */
static void group_update_launcher_entry(AppGroupItem *grp) {
    grp->has_le = FALSE;
    memset(&grp->le, 0, sizeof(grp->le));
    const gchar *cands[2] = { grp->desktop_id, grp->group_id };
    for (gint i = 0; i < 2 && !grp->has_le; i++) {
        gchar *k = launcher_entry_normalize_key(cands[i]);
        if (k) grp->has_le = launcher_entry_lookup(k, &grp->le);
        g_free(k);
    }
}

static gboolean group_le_count_shown(AppGroupItem *grp) {
    return grp->has_le && grp->le.count_visible && grp->le.count > 0;
}

/* Progress 0..1 from LauncherEntry, else from the XApp window hint; < 0 = none */
static gdouble group_progress(AppGroupItem *grp) {
    if (grp->has_le && grp->le.progress_visible) return grp->le.progress;
    if (grp->xapp_progress >= 0) return CLAMP(grp->xapp_progress, 0, 100) / 100.0;
    return -1.0;
}

/* Number for the corner badge: the app's own count, else open windows when more than one */
static gint64 group_badge_count(AppGroupItem *grp, gboolean *alert) {
    if (group_le_count_shown(grp)) { if (alert) *alert = TRUE; return grp->le.count; }
    if (alert) *alert = FALSE;
    guint n = g_list_length(grp->windows);
    return n > 1 ? n : 0;
}

void taskbar_draw_icon_badge(cairo_t *cr, gdouble icon_x, gdouble icon_y, gdouble icon_sz,
                             gint64 count, gboolean alert) {
    if (count <= 0 || icon_sz < 8) return;
    gchar *txt = (count > 99) ? g_strdup("99+") : g_strdup_printf("%" G_GINT64_FORMAT, count);

    gdouble bh = CLAMP(icon_sz * 0.44, 13.0, 22.0);      /* pill height */
    gdouble fs = bh * 0.66;
    cairo_save(cr);
    cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
    cairo_set_font_size(cr, fs);
    cairo_text_extents_t te;
    cairo_text_extents(cr, txt, &te);
    gdouble bw = MAX(bh, te.x_advance + bh * 0.62);

    /* Centre the pill on the icon's top-right corner, pulled in a little so it just overlaps */
    gdouble bx = icon_x + icon_sz - bw * 0.62;
    gdouble by = icon_y - bh * 0.32;
    gdouble r = bh / 2.0;

    cairo_new_path(cr);
    cairo_arc(cr, bx + r, by + r, r, G_PI / 2.0, 3.0 * G_PI / 2.0);
    cairo_arc(cr, bx + bw - r, by + r, r, -G_PI / 2.0, G_PI / 2.0);
    cairo_close_path(cr);
    if (alert) cairo_set_source_rgba(cr, 1.0, 0.23, 0.19, 1.0);          /* notification red */
    else       cairo_set_source_rgba(cr, 0.17, 0.17, 0.19, 0.94);        /* neutral dark pill */
    cairo_fill_preserve(cr);
    cairo_set_line_width(cr, 1.2);                                      /* thin ring keeps it readable on any icon */
    if (alert) cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.28);
    else       cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.30);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 1.0);
    cairo_move_to(cr, bx + (bw - te.x_advance) / 2.0,
                  by + bh / 2.0 - (te.y_bearing + te.height / 2.0));
    cairo_show_text(cr, txt);
    cairo_restore(cr);
    g_free(txt);
}

static void refresh_group_visuals(AppGroupItem *grp) {
    if (!grp) return;

    update_group_display_title(grp);
    group_update_launcher_entry(grp);
    grp->needs_attention = compute_group_needs_attention(grp) || (grp->has_le && grp->le.urgent);
    if (grp->needs_attention) ensure_taskbar_anim_timer(grp->tb);

    guint count = g_list_length(grp->windows);
    (void)count;
    shell_set_badge_count(grp->badge_lbl, 0);     /* badges are drawn in cairo now, see taskbar_draw_icon_badge */

    if (GTK_IS_WIDGET(grp->image)) {
        gtk_widget_set_margin_top(grp->image, 0);
        gtk_widget_set_margin_bottom(grp->image, 0);
        gtk_widget_set_margin_start(grp->image, 0);
        gtk_widget_set_margin_end(grp->image, 0);
        gtk_widget_set_halign(grp->image, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(grp->image, GTK_ALIGN_CENTER);
        GtkWidget *ov = gtk_widget_get_parent(grp->image);
        if (ov) {
            gtk_widget_set_margin_top(ov, 0);
            gtk_widget_set_margin_bottom(ov, 0);
            gtk_widget_set_margin_start(ov, 0);
            gtk_widget_set_margin_end(ov, 0);
            gtk_widget_set_halign(ov, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(ov, GTK_ALIGN_CENTER);
        }
    }

    gint base_px = get_current_target_icon_size(grp->tb);
    if (GTK_IS_WIDGET(grp->image)) {
        gtk_widget_set_size_request(grp->image, base_px, base_px);
    }
    gdouble sc = (grp->press_scale > 0.1) ? grp->press_scale : 1.0;
    set_group_icon_size(grp, (gint)(base_px * sc + 0.5));

    if (grp->tb->shell && grp->tb->shell->mode != LAYOUT_MACOS) {
        gint sp = CLAMP(grp->tb->shell->tb_icon_spacing, 0, 24);
        gint pad = CLAMP(grp->tb->shell->tb_icon_padding, 0, 16);

        gtk_widget_set_margin_top(grp->tb->box, 0);
        gtk_widget_set_margin_bottom(grp->tb->box, 0);
        gtk_widget_set_margin_start(grp->tb->box, 0);
        gtk_widget_set_margin_end(grp->tb->box, 0);
        gtk_widget_set_valign(grp->tb->box, GTK_ALIGN_FILL);
        gtk_widget_set_vexpand(grp->tb->box, TRUE);

        gtk_widget_set_valign(grp->button, GTK_ALIGN_FILL);
        gtk_widget_set_vexpand(grp->button, TRUE);
        gtk_widget_set_halign(grp->button, GTK_ALIGN_CENTER);
        gtk_box_set_spacing(GTK_BOX(grp->tb->box), sp);
        gtk_widget_set_margin_start(grp->button, 0);
        gtk_widget_set_margin_end(grp->button, 0);
        gtk_widget_set_margin_top(grp->button, 0);
        gtk_widget_set_margin_bottom(grp->button, 0);

        GtkWidget *child = gtk_bin_get_child(GTK_BIN(grp->button));
        if (child) {
            gtk_widget_set_margin_start(child, pad);
            gtk_widget_set_margin_end(child, pad);
            gtk_widget_set_margin_top(child, 0);
            gtk_widget_set_margin_bottom(child, 0);
            gtk_widget_set_halign(child, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(child, GTK_ALIGN_CENTER);
            gtk_widget_set_vexpand(child, TRUE);
        }
    } else if (grp->tb->shell && grp->tb->shell->mode == LAYOUT_MACOS && grp->tb->shell->dock_style == 1 &&
               grp->tb->shell->dock_pos != DOCK_POS_BOTTOM) {
        /* Ubuntu dock: each button is a full-width row of the strip, so the whole row clicks */
        gtk_widget_set_halign(grp->button, GTK_ALIGN_FILL);
        gtk_widget_set_valign(grp->button, GTK_ALIGN_CENTER);
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(grp->button));
        if (child) {
            gtk_widget_set_margin_start(child, 0);
            gtk_widget_set_margin_end(child, 0);
            gtk_widget_set_margin_top(child, 4);
            gtk_widget_set_margin_bottom(child, 4);
        }
    } else if (grp->tb->shell && grp->tb->shell->mode == LAYOUT_MACOS) {
        if (grp->tb->shell->dock_pos == DOCK_POS_LEFT) {
            gtk_widget_set_halign(grp->button, GTK_ALIGN_START);
            gtk_widget_set_valign(grp->button, GTK_ALIGN_CENTER);
        } else if (grp->tb->shell->dock_pos == DOCK_POS_RIGHT) {
            gtk_widget_set_halign(grp->button, GTK_ALIGN_END);
            gtk_widget_set_valign(grp->button, GTK_ALIGN_CENTER);
        } else {
            gtk_widget_set_valign(grp->button, GTK_ALIGN_END);
            gtk_widget_set_halign(grp->button, GTK_ALIGN_CENTER);
        }
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(grp->button));
        if (child) {
            gtk_widget_set_margin_start(child, 2);
            gtk_widget_set_margin_end(child, 2);
            gtk_widget_set_margin_top(child, 0);
            gtk_widget_set_margin_bottom(child, 7);
        }
    }

    gtk_widget_queue_draw(grp->button);
}

/* =========================================================================
 * Interactive Window Hover Thumbnail Previews (Click to Focus, '×' to Close)
 * ========================================================================= */
static gboolean hide_preview_popover(gpointer user_data) {
    AppGroupItem *grp = (AppGroupItem *)user_data;
    if (grp->preview_hovered || grp->is_hovered) {
        return G_SOURCE_CONTINUE;
    }
    if (grp->preview_popover) {
        gtk_widget_destroy(grp->preview_popover);
        grp->preview_popover = NULL;
    }
    grp->preview_timeout_id = 0;
    return G_SOURCE_REMOVE;
}

static gboolean on_preview_popover_enter(GtkWidget *pop, GdkEventCrossing *ev, AppGroupItem *grp) {
    grp->preview_hovered = TRUE;
    if (grp->preview_timeout_id > 0) {
        g_source_remove(grp->preview_timeout_id);
        grp->preview_timeout_id = 0;
    }
    return FALSE;
}

static gboolean on_preview_popover_leave(GtkWidget *pop, GdkEventCrossing *ev, AppGroupItem *grp) {
    if (ev->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    grp->preview_hovered = FALSE;
    if (grp->preview_timeout_id == 0) {
        grp->preview_timeout_id = g_timeout_add(240, hide_preview_popover, grp);
    }
    return FALSE;
}

static void on_thumb_card_clicked(GtkButton *card_btn, WnckWindow *win) {
    AppGroupItem *grp = g_object_get_data(G_OBJECT(card_btn), "app-group-item");
    if (WNCK_IS_WINDOW(win)) {
        guint32 ts = gtk_get_current_event_time();
        WnckWorkspace *ws = wnck_window_get_workspace(win);
        WnckScreen *sc = wnck_window_get_screen(win);
        if (ws && ws != wnck_screen_get_active_workspace(sc)) {
            wnck_workspace_activate(ws, ts);
        }
        wnck_window_activate(win, ts);
    }
    if (grp && grp->preview_popover) {
        grp->preview_hovered = FALSE;
        gtk_widget_destroy(grp->preview_popover);
        grp->preview_popover = NULL;
    }
}

static void on_thumb_close_clicked(GtkButton *close_btn, WnckWindow *win) {
    AppGroupItem *grp = g_object_get_data(G_OBJECT(close_btn), "app-group-item");
    if (WNCK_IS_WINDOW(win)) {
        wnck_window_close(win, gtk_get_current_event_time());
    }
    if (grp && grp->preview_popover) {
        grp->preview_hovered = FALSE;
        gtk_widget_destroy(grp->preview_popover);
        grp->preview_popover = NULL;
    }
}

/* A real picture of the window, scaled to fit max_w × max_h.
   Minimized windows have no picture on X11 → NULL (the card shows the icon instead). */
static GdkPixbuf *capture_window_thumbnail(WnckWindow *w, gint max_w, gint max_h) {
    if (!w || wnck_window_is_minimized(w)) return NULL;
    GdkDisplay *disp = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(disp)) return NULL;
    WnckWorkspace *ws = wnck_window_get_workspace(w);
    WnckScreen *sc = wnck_window_get_screen(w);
    if (ws && sc && ws != wnck_screen_get_active_workspace(sc) && !wnck_window_is_pinned(w)) return NULL;

    Display *dpy = GDK_DISPLAY_XDISPLAY(disp);
    Window xid = (Window)wnck_window_get_xid(w);
    GdkPixbuf *thumb = NULL;

    gdk_x11_display_error_trap_push(disp);
    XWindowAttributes attr;
    if (XGetWindowAttributes(dpy, xid, &attr) && attr.map_state == IsViewable &&
        attr.width > 8 && attr.height > 8) {
        gdouble scale = MIN((gdouble)max_w / attr.width, (gdouble)max_h / attr.height);
        if (scale > 1.0) scale = 1.0;
        gint tw = MAX(1, (gint)(attr.width * scale)), th = MAX(1, (gint)(attr.height * scale));

        cairo_surface_t *src = cairo_xlib_surface_create(dpy, xid, attr.visual, attr.width, attr.height);
        /* scale on the X server into a small pixmap, then read back only that */
        cairo_surface_t *small = cairo_surface_create_similar(src, CAIRO_CONTENT_COLOR, tw, th);
        cairo_t *cr = cairo_create(small);
        cairo_scale(cr, scale, scale);
        cairo_set_source_surface(cr, src, 0, 0);
        cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        cairo_paint(cr);
        cairo_destroy(cr);
        cairo_surface_flush(small);
        if (cairo_surface_status(small) == CAIRO_STATUS_SUCCESS)
            thumb = gdk_pixbuf_get_from_surface(small, 0, 0, tw, th);
        cairo_surface_destroy(small);
        cairo_surface_destroy(src);
    }
    XSync(dpy, False);
    gdk_x11_display_error_trap_pop_ignored(disp);
    return thumb;
}

static gint64 g_preview_last_visible_us = 0;   /* to switch instantly between icons */

static void show_preview_now(AppGroupItem *grp) {
    GtkWidget *btn = grp->button;
    if (!grp->tb->shell || !grp->tb->shell->show_thumbnails || grp->windows == NULL || !btn) return;
    if (grp->tb->shell->mode == LAYOUT_MACOS && grp->tb->shell->dock_style != 1) return;   /* a macOS Dock shows only the name; the Ubuntu dock previews */

    if (grp->preview_timeout_id > 0) {
        g_source_remove(grp->preview_timeout_id);
        grp->preview_timeout_id = 0;
    }
    if (grp->preview_popover) {
        gtk_widget_destroy(grp->preview_popover);
        grp->preview_popover = NULL;
    }

    grp->preview_popover = shell_create_popup_window(btn, -1, -1, FALSE);
    g_object_set_data(G_OBJECT(grp->preview_popover), "center-on-button", GINT_TO_POINTER(1));
    gtk_widget_add_events(grp->preview_popover, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(grp->preview_popover, "enter-notify-event", G_CALLBACK(on_preview_popover_enter), grp);
    g_signal_connect(grp->preview_popover, "leave-notify-event", G_CALLBACK(on_preview_popover_leave), grp);

    /* a bar on the left / right edge (Ubuntu dock) stacks the cards top to bottom */
    UltimateShell *psh = grp->tb->shell;
    gboolean side_bar = (psh->mode == LAYOUT_MACOS && psh->dock_style == 1 && psh->dock_pos != DOCK_POS_BOTTOM);
    GtkWidget *hbox = gtk_box_new(side_bar ? GTK_ORIENTATION_VERTICAL : GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 8);

    gint shown = 0;
    for (GList *l = grp->windows; l != NULL && shown < 8; l = l->next, shown++) {
        WnckWindow *w = WNCK_WINDOW(l->data);

        GtkWidget *card_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_size_request(card_vbox, 200, -1);

        /* Header: app icon + window title + × */
        GtkWidget *hdr = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GdkPixbuf *mini = wnck_window_get_mini_icon(w);
        if (mini) gtk_box_pack_start(GTK_BOX(hdr), gtk_image_new_from_pixbuf(mini), FALSE, FALSE, 0);
        const gchar *wtitle = wnck_window_get_name(w);
        GtkWidget *lbl = gtk_label_new(wtitle ? wtitle : "Window");
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 20);

        GtkWidget *close_btn = gtk_button_new_with_label("×");
        gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(close_btn), "winck-btn");
        gtk_widget_set_tooltip_text(close_btn, "Close Window");
        g_object_set_data(G_OBJECT(close_btn), "app-group-item", grp);
        g_signal_connect(close_btn, "clicked", G_CALLBACK(on_thumb_close_clicked), w);

        gtk_box_pack_start(GTK_BOX(hdr), lbl, TRUE, TRUE, 2);
        gtk_box_pack_end(GTK_BOX(hdr), close_btn, FALSE, FALSE, 0);

        /* Body: live picture of the window (or its icon if minimized / elsewhere) */
        GtkWidget *act_btn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(act_btn), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(act_btn), "app-grid-tile");
        g_object_set_data(G_OBJECT(act_btn), "app-group-item", grp);
        g_signal_connect(act_btn, "clicked", G_CALLBACK(on_thumb_card_clicked), w);
        gtk_widget_set_size_request(act_btn, 200, 124);

        GtkWidget *thumb_img = NULL;
        GdkPixbuf *shot = capture_window_thumbnail(w, 192, 116);
        if (shot) {
            thumb_img = gtk_image_new_from_pixbuf(shot);
            g_object_unref(shot);
        } else {
            GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            GdkPixbuf *icon_pix = wnck_window_get_icon(w);
            GtkWidget *ic = NULL;
            if (icon_pix) {
                GdkPixbuf *scaled = gdk_pixbuf_scale_simple(icon_pix, 48, 48, GDK_INTERP_BILINEAR);
                ic = gtk_image_new_from_pixbuf(scaled);
                g_object_unref(scaled);
            } else {
                ic = gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
            }
            gtk_box_pack_start(GTK_BOX(vb), ic, TRUE, TRUE, 0);
            if (wnck_window_is_minimized(w)) {
                GtkWidget *m = gtk_label_new("Minimized");
                gtk_style_context_add_class(gtk_widget_get_style_context(m), "dim-label");
                gtk_box_pack_start(GTK_BOX(vb), m, FALSE, FALSE, 0);
            }
            thumb_img = vb;
        }
        gtk_container_add(GTK_CONTAINER(act_btn), thumb_img);

        gtk_box_pack_start(GTK_BOX(card_vbox), hdr, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(card_vbox), act_btn, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), card_vbox, FALSE, FALSE, 4);
    }

    if (side_bar) {
        /* many windows: the stack scrolls instead of running off the screen */
        GdkRectangle wa = { 0, 0, 1920, 1080 };
        GdkWindow *bw = gtk_widget_get_window(btn);
        GdkMonitor *mon = bw ? gdk_display_get_monitor_at_window(gdk_display_get_default(), bw) : NULL;
        if (!mon) mon = gdk_display_get_primary_monitor(gdk_display_get_default());
        if (mon) gdk_monitor_get_workarea(mon, &wa);
        GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sc), TRUE);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sc), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sc), MAX(200, wa.height - 24));
        gtk_container_add(GTK_CONTAINER(sc), hbox);
        gtk_container_add(GTK_CONTAINER(grp->preview_popover), sc);
    } else {
        gtk_container_add(GTK_CONTAINER(grp->preview_popover), hbox);
    }
    gtk_widget_show_all(grp->preview_popover);
    shell_reposition_popup_window((GtkToggleButton *)btn, grp->preview_popover);
    g_preview_last_visible_us = g_get_monotonic_time();

}

static gboolean on_preview_show_delay(gpointer user_data) {
    AppGroupItem *grp = (AppGroupItem *)user_data;
    grp->preview_show_id = 0;
    if (grp->is_hovered) show_preview_now(grp);
    return G_SOURCE_REMOVE;
}

static gboolean on_icon_enter_notify(GtkWidget *btn, GdkEventCrossing *ev, AppGroupItem *grp) {
    grp->is_hovered = TRUE;
    gtk_widget_queue_draw(btn);

    /* Not running: just the app's name (a normal tooltip); running: the preview instead */
    if (grp->windows == NULL && grp->pinned_app) {
        gboolean mac = (grp->tb->shell && grp->tb->shell->mode == LAYOUT_MACOS);
        gboolean dock_pill = mac && grp->tb->shell->dock_show_tooltips;   /* the dock draws its own */
        gtk_widget_set_tooltip_text(btn, dock_pill ? NULL : g_app_info_get_name(grp->pinned_app));
    } else {
        gtk_widget_set_tooltip_text(btn, NULL);
    }

    if (!grp->tb->shell || !grp->tb->shell->show_thumbnails || grp->windows == NULL) return FALSE;
    if (grp->tb->shell->mode == LAYOUT_MACOS && grp->tb->shell->dock_style != 1) return FALSE;   /* a macOS Dock shows only the name */
    if (grp->preview_show_id) { g_source_remove(grp->preview_show_id); grp->preview_show_id = 0; }

    /* A preview was open a moment ago (moving along the taskbar): switch instantly */
    if (g_get_monotonic_time() - g_preview_last_visible_us < 350000) {
        show_preview_now(grp);
    } else {
        grp->preview_show_id = g_timeout_add(450, on_preview_show_delay, grp);
    }
    return FALSE;
}

static gboolean on_icon_leave_notify(GtkWidget *btn, GdkEventCrossing *ev, AppGroupItem *grp) {
    grp->is_hovered = FALSE;
    gtk_widget_queue_draw(btn);
    if (grp->preview_show_id) { g_source_remove(grp->preview_show_id); grp->preview_show_id = 0; }

    if (grp->preview_popover) {
        g_preview_last_visible_us = g_get_monotonic_time();
        if (grp->preview_timeout_id == 0)
            grp->preview_timeout_id = g_timeout_add(240, hide_preview_popover, grp);
    }
    return FALSE;
}

/* =========================================================================
 * Scroll-Wheel Window Cycling & Click / Middle-Click Actions
 * ========================================================================= */
static gboolean on_group_btn_scroll(GtkWidget *btn, GdkEventScroll *ev, AppGroupItem *grp) {
    if (!grp || !grp->windows) return FALSE;
    if (ev->direction != GDK_SCROLL_UP && ev->direction != GDK_SCROLL_DOWN) return FALSE;

    guint count = g_list_length(grp->windows);
    if (count == 0) return FALSE;

    WnckScreen *sc = grp->tb->tracker->screen;
    WnckWindow *active = wnck_screen_get_active_window(sc);
    gint idx = g_list_index(grp->windows, active);

    gint next_idx = 0;
    if (idx < 0) {
        next_idx = 0;
    } else if (ev->direction == GDK_SCROLL_UP) {
        next_idx = (idx + 1) % (gint)count;
    } else {
        next_idx = (idx - 1 + (gint)count) % (gint)count;
    }

    WnckWindow *target = WNCK_WINDOW(g_list_nth_data(grp->windows, next_idx));
    if (target) {
        guint32 ts = gtk_get_current_event_time();
        WnckWorkspace *ws = wnck_window_get_workspace(target);
        if (ws && ws != wnck_screen_get_active_workspace(sc)) {
            wnck_workspace_activate(ws, ts);
        }
        wnck_window_activate(target, ts);
    }
    return TRUE;
}

static void on_win_selector_activate(GtkMenuItem *item, WnckWindow *win) {
    guint32 ts = gtk_get_current_event_time();
    WnckWorkspace *ws = wnck_window_get_workspace(win);
    WnckScreen *sc = wnck_window_get_screen(win);
    if (ws && ws != wnck_screen_get_active_workspace(sc)) wnck_workspace_activate(ws, ts);
    wnck_window_activate(win, ts);
}

static void on_launch_pinned_clicked(GtkMenuItem *item, AppGroupItem *grp) {
    if (grp->pinned_app) shell_launch_app_info(grp->pinned_app);
}

static void on_close_all_group_windows(GtkMenuItem *item, AppGroupItem *grp) {
    guint32 ts = gtk_get_current_event_time();
    GList *copy = g_list_copy(grp->windows);
    for (GList *l = copy; l != NULL; l = l->next) {
        wnck_window_close(WNCK_WINDOW(l->data), ts);
    }
    g_list_free(copy);
}

static AppGroupItem *g_tb_drag_grp = NULL;
static gdouble       g_tb_drag_start_root_x = 0.0;
static gdouble       g_tb_drag_start_root_y = 0.0;
static gboolean      g_tb_drag_moved = FALSE;

static void on_group_btn_clicked(GtkButton *btn, AppGroupItem *grp) {
    if (g_tb_drag_moved) { g_tb_drag_moved = FALSE; return; }
    if (grp->preview_popover) {
        grp->preview_hovered = FALSE;
        gtk_widget_destroy(grp->preview_popover);
        grp->preview_popover = NULL;
    }

    guint count = g_list_length(grp->windows);

    /* Trigger macOS Dock Parabolic Bounce (type 2 = multi-hop launch, type 1 = single activation hop).
       The Ubuntu dock uses the taskbar's press animation instead. */
    if (!(grp->tb->shell && grp->tb->shell->mode == LAYOUT_MACOS && grp->tb->shell->dock_style == 1)) {
        grp->click_anim_start_us = g_get_monotonic_time();
        grp->click_anim_type = (count == 0) ? 2 : 1;
        ensure_taskbar_anim_timer(grp->tb);
    }

    if (count == 0) {
        if (grp->pinned_app) shell_launch_app_info(grp->pinned_app);
        return;
    }
    if (count == 1) {
        WnckWindow *window = WNCK_WINDOW(grp->windows->data);
        WnckScreen *screen = wnck_window_get_screen(window);
        WnckWindow *active = wnck_screen_get_active_window(screen);
        guint32 timestamp = gtk_get_current_event_time();
        if (active == window && !wnck_window_is_minimized(window)) wnck_window_minimize(window);
        else wnck_window_activate(window, timestamp);
        return;
    }

    GtkWidget *menu = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
    for (GList *l = grp->windows; l != NULL; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        GtkWidget *item = gtk_menu_item_new_with_label(wnck_window_get_name(w));
        g_signal_connect(item, "activate", G_CALLBACK(on_win_selector_activate), w);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    shell_popup_menu_at_button(GTK_MENU(menu), GTK_WIDGET(btn), NULL);
}

static void destroy_group_item(AppGroupItem *grp) {
    TaskbarWidget *tb = grp->tb;
    if (grp->preview_timeout_id > 0) g_source_remove(grp->preview_timeout_id);
    if (grp->preview_show_id > 0) g_source_remove(grp->preview_show_id);
    if (grp->preview_popover) gtk_widget_destroy(grp->preview_popover);
    gtk_widget_destroy(grp->button);
    g_hash_table_remove(tb->groups, grp->group_id);
    if (grp->pb_master) g_object_unref(grp->pb_master);
    if (grp->pinned_app) g_object_unref(grp->pinned_app);
    g_free(grp->display_title);
    g_free(grp->desktop_id);
    g_free(grp->group_id);
    g_free(grp);
}

static void on_toggle_pin_activate(GtkMenuItem *item, AppGroupItem *grp) {
    grp->is_pinned = !grp->is_pinned;
    if (grp->is_pinned && !grp->pinned_app) {
        grp->pinned_app = find_app_info_for_key(grp->desktop_id ? grp->desktop_id : grp->group_id);
    }
    save_pinned_apps_to_xfconf(grp->tb);
    if (!grp->is_pinned && grp->windows == NULL) destroy_group_item(grp);
    else refresh_group_visuals(grp);
}

/* ---- right-click menu helpers ---- */
static GAppInfo *group_app_info(AppGroupItem *grp) {
    if (grp->pinned_app) return g_object_ref(grp->pinned_app);
    if (grp->desktop_id && grp->desktop_id[0]) {
        GDesktopAppInfo *di = g_desktop_app_info_new(grp->desktop_id);
        if (di) return G_APP_INFO(di);
    }
    return NULL;
}

static void on_ctx_activate_window(GtkMenuItem *mi, gpointer data) {
    WnckWindow *w = g_object_get_data(G_OBJECT(mi), "ctx-window");
    if (!WNCK_IS_WINDOW(w)) return;
    guint32 ts = gtk_get_current_event_time();
    WnckWorkspace *ws = wnck_window_get_workspace(w);
    WnckScreen *sc = wnck_window_get_screen(w);
    if (ws && ws != wnck_screen_get_active_workspace(sc)) wnck_workspace_activate(ws, ts);
    wnck_window_activate(w, ts);
}

static void on_ctx_minimize_all(GtkMenuItem *mi, AppGroupItem *grp) {
    for (GList *l = grp->windows; l; l = l->next) wnck_window_minimize(WNCK_WINDOW(l->data));
}

static void on_ctx_restore_all(GtkMenuItem *mi, AppGroupItem *grp) {
    guint32 ts = gtk_get_current_event_time();
    for (GList *l = grp->windows; l; l = l->next)
        if (wnck_window_is_minimized(WNCK_WINDOW(l->data))) wnck_window_unminimize(WNCK_WINDOW(l->data), ts);
}

static void on_ctx_move_to_ws(GtkMenuItem *mi, AppGroupItem *grp) {
    WnckWorkspace *ws = g_object_get_data(G_OBJECT(mi), "ctx-ws");
    if (!WNCK_IS_WORKSPACE(ws)) return;
    for (GList *l = grp->windows; l; l = l->next) wnck_window_move_to_workspace(WNCK_WINDOW(l->data), ws);
}

static void on_ctx_run_admin(GtkMenuItem *mi, AppGroupItem *grp) {
    GAppInfo *app = group_app_info(grp);
    if (app) { app_run_as_admin(app); g_object_unref(app); }
}

static void on_ctx_edit_launcher(GtkMenuItem *mi, AppGroupItem *grp) {
    GAppInfo *app = group_app_info(grp);
    if (app) { app_edit_launcher(app); g_object_unref(app); }
}

static void on_ctx_force_quit(GtkMenuItem *mi, AppGroupItem *grp) {
    if (!grp->windows) return;
    gint pid = wnck_window_get_pid(WNCK_WINDOW(grp->windows->data));
    if (pid <= 1 || pid == getpid()) return;
    const gchar *name = grp->display_title ? grp->display_title : "this app";
    GtkWidget *d = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE,
                                          "Force “%s” to quit?", name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "You will lose any unsaved changes.");
    gtk_dialog_add_buttons(GTK_DIALOG(d), "_Cancel", GTK_RESPONSE_CANCEL, "_Force Quit", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_keep_above(GTK_WINDOW(d), TRUE);
    gint r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == GTK_RESPONSE_ACCEPT) kill((pid_t)pid, SIGKILL);
}

static gboolean on_group_btn_press(GtkWidget *btn, GdkEventButton *event, AppGroupItem *grp) {
    if (event->type == GDK_BUTTON_PRESS && event->button == 1) {
        g_tb_drag_grp = grp;
        g_tb_drag_start_root_x = event->x_root;
        g_tb_drag_start_root_y = event->y_root;
        g_tb_drag_moved = FALSE;
        grp->is_pressed = TRUE;
        ensure_taskbar_anim_timer(grp->tb);
        return FALSE;
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 2) {
        /* Middle-Click Action: Close Window or Launch New Instance */
        if (grp->tb->shell && grp->tb->shell->tb_middle_click_close) {
            if (grp->windows != NULL) {
                WnckWindow *active = wnck_screen_get_active_window(grp->tb->tracker->screen);
                WnckWindow *target = (g_list_find(grp->windows, active) != NULL)
                    ? active : WNCK_WINDOW(grp->windows->data);
                wnck_window_close(target, gtk_get_current_event_time());
            }
        } else {
            if (grp->pinned_app) {
                grp->click_anim_start_us = g_get_monotonic_time();
                grp->click_anim_type = 2;
                ensure_taskbar_anim_timer(grp->tb);
                shell_launch_app_info(grp->pinned_app);
            }
        }
        return TRUE;
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 3) {
        if (grp->preview_popover && gtk_widget_get_visible(grp->preview_popover)) {
            gtk_widget_hide(grp->preview_popover);
        }
        gboolean is_mac = (grp->tb && grp->tb->shell && grp->tb->shell->mode == LAYOUT_MACOS);
        GtkWidget *menu = gtk_menu_new();
        gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
        GAppInfo *app = group_app_info(grp);

        /* 1. Open windows (click to switch) */
        if (grp->windows) {
            WnckWindow *active = wnck_screen_get_active_window(grp->tb->tracker->screen);
            for (GList *l = grp->windows; l; l = l->next) {
                WnckWindow *w = WNCK_WINDOW(l->data);
                const gchar *t = wnck_window_get_name(w);
                gchar *lbl = g_strdup_printf("%s%s", (w == active) ? "● " : "", (t && t[0]) ? t : "Window");
                GtkWidget *wi = gtk_menu_item_new_with_label(lbl);
                g_free(lbl);
                GtkWidget *wl = gtk_bin_get_child(GTK_BIN(wi));
                if (GTK_IS_LABEL(wl)) { gtk_label_set_ellipsize(GTK_LABEL(wl), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(wl), 40); }
                g_object_set_data(G_OBJECT(wi), "ctx-window", w);
                g_signal_connect(wi, "activate", G_CALLBACK(on_ctx_activate_window), NULL);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), wi);
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        }

        /* 2. The app's own quick actions (e.g. New Private Window) */
        if (app && app_append_jump_actions(menu, app) > 0)
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

        /* 3. Launching */
        if (grp->pinned_app) {
            GtkWidget *launch_item = gtk_menu_item_new_with_label(grp->windows ? "New Window" : "Open");
            g_signal_connect(launch_item, "activate", G_CALLBACK(on_launch_pinned_clicked), grp);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), launch_item);
        }
        if (app) {
            GtkWidget *adm = gtk_menu_item_new_with_label("Run as Administrator");
            g_signal_connect(adm, "activate", G_CALLBACK(on_ctx_run_admin), grp);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), adm);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

        /* 4. Pinning & launcher */
        const gchar *pin_lbl = is_mac
            ? (grp->is_pinned ? "Remove from Dock" : "Keep in Dock")
            : (grp->is_pinned ? "Unpin from Taskbar" : "Pin to Taskbar");
        GtkWidget *pin_item = gtk_menu_item_new_with_label(pin_lbl);
        g_signal_connect(pin_item, "activate", G_CALLBACK(on_toggle_pin_activate), grp);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), pin_item);
        if (app && G_IS_DESKTOP_APP_INFO(app) && g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app))) {
            GtkWidget *ed = gtk_menu_item_new_with_label("Edit Launcher…");
            g_signal_connect(ed, "activate", G_CALLBACK(on_ctx_edit_launcher), grp);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), ed);
        }

        /* 5. Window management */
        if (grp->windows != NULL) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            GtkWidget *mn = gtk_menu_item_new_with_label("Minimize All");
            g_signal_connect(mn, "activate", G_CALLBACK(on_ctx_minimize_all), grp);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), mn);
            GtkWidget *rs = gtk_menu_item_new_with_label("Restore All");
            g_signal_connect(rs, "activate", G_CALLBACK(on_ctx_restore_all), grp);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), rs);

            WnckScreen *sc = grp->tb->tracker->screen;
            if (wnck_screen_get_workspace_count(sc) > 1) {
                GtkWidget *mv = gtk_menu_item_new_with_label("Move to Workspace");
                GtkWidget *mvs = gtk_menu_new();
                gtk_style_context_add_class(gtk_widget_get_style_context(mvs), "ultimate-themed-menu");
                for (GList *l = wnck_screen_get_workspaces(sc); l; l = l->next) {
                    WnckWorkspace *ws = WNCK_WORKSPACE(l->data);
                    GtkWidget *wsi = gtk_menu_item_new_with_label(wnck_workspace_get_name(ws));
                    g_object_set_data(G_OBJECT(wsi), "ctx-ws", ws);
                    g_signal_connect(wsi, "activate", G_CALLBACK(on_ctx_move_to_ws), grp);
                    gtk_menu_shell_append(GTK_MENU_SHELL(mvs), wsi);
                }
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(mv), mvs);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), mv);
            }

            /* 6. Quitting */
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
            GtkWidget *close_item = gtk_menu_item_new_with_label(
                g_list_length(grp->windows) > 1 ? "Close All Windows" : "Close Window");
            g_signal_connect(close_item, "activate", G_CALLBACK(on_close_all_group_windows), grp);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), close_item);
            GtkWidget *fq = gtk_menu_item_new_with_label("Force Quit");
            gtk_widget_set_tooltip_text(fq, "For an app that has stopped responding");
            g_signal_connect(fq, "activate", G_CALLBACK(on_ctx_force_quit), grp);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), fq);
        }
        if (app) g_object_unref(app);
        gtk_widget_show_all(menu);
        shell_popup_menu_at_button(GTK_MENU(menu), btn, (GdkEvent *)event);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_group_btn_motion(GtkWidget *btn, GdkEventMotion *event, AppGroupItem *grp) {
    if (g_tb_drag_grp != grp || !(event->state & GDK_BUTTON1_MASK)) return FALSE;

    gdouble dx = ABS(event->x_root - g_tb_drag_start_root_x);
    gdouble dy = ABS(event->y_root - g_tb_drag_start_root_y);
    if (!g_tb_drag_moved && dx < 6.0 && dy < 6.0) return FALSE;
    g_tb_drag_moved = TRUE;
    grp->is_pressed = FALSE;

    if (grp->preview_popover) {
        gtk_widget_destroy(grp->preview_popover);
        grp->preview_popover = NULL;
    }

    GList *children = gtk_container_get_children(GTK_CONTAINER(grp->tb->box));
    gint cur_idx = g_list_index(children, btn);
    gint target_idx = cur_idx;
    gint idx = 0;

    for (GList *l = children; l != NULL; l = l->next, idx++) {
        GtkWidget *child = GTK_WIDGET(l->data);
        if (!gtk_widget_get_visible(child) || !gtk_widget_get_window(child)) continue;
        gint wx = 0, wy = 0;
        gdk_window_get_origin(gtk_widget_get_window(child), &wx, &wy);
        GtkAllocation ca;
        gtk_widget_get_allocation(child, &ca);
        gint cx = wx + ca.x + (ca.width / 2);
        if ((gint)event->x_root < cx) {
            target_idx = idx;
            break;
        }
        target_idx = idx;
    }
    g_list_free(children);

    if (target_idx != cur_idx && target_idx >= 0) {
        gtk_box_reorder_child(GTK_BOX(grp->tb->box), btn, target_idx);
        save_pinned_apps_to_xfconf(grp->tb);
        if (grp->tb->shell && grp->tb->shell->dock_window) {
            gtk_widget_queue_draw(grp->tb->shell->dock_window);
        }
    }
    return TRUE;
}

static gboolean on_group_btn_release(GtkWidget *btn, GdkEventButton *event, AppGroupItem *grp) {
    grp->is_pressed = FALSE;
    ensure_taskbar_anim_timer(grp->tb);
    if (event->button == 1 && g_tb_drag_grp == grp) {
        gboolean was_dragged = g_tb_drag_moved;
        g_tb_drag_grp = NULL;
        if (was_dragged) {
            gtk_widget_unset_state_flags(btn, GTK_STATE_FLAG_ACTIVE);
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean on_suppress_dock_child_draw(GtkWidget *widget, cairo_t *cr, AppGroupItem *grp) {
    if (grp && grp->tb && grp->tb->shell && grp->tb->shell->mode == LAYOUT_MACOS) {
        return TRUE;
    }
    return FALSE;
}

/* Windows taskbar: thin progress bar across the bottom of the icon, like Win11 */
static gboolean on_draw_taskbar_item_progress(GtkWidget *btn, cairo_t *cr, AppGroupItem *grp) {
    if (!grp || !grp->tb || !grp->tb->shell || grp->tb->shell->mode == LAYOUT_MACOS) return FALSE;
    gint w = gtk_widget_get_allocated_width(btn);
    gint h = gtk_widget_get_allocated_height(btn);
    if (w <= 8 || h <= 8) return FALSE;

    if (grp->drop_hover) {        /* file dragged over: same highlight as hover */
        draw_rounded_rect(cr, 1.0, 1.0, w - 2.0, h - 2.0, 5.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.14);
        cairo_fill(cr);
    }
    /* Icon rectangle inside the button */
    gint ix = 0, iy = 0, isz = get_current_target_icon_size(grp->tb);
    if (GTK_IS_WIDGET(grp->image) && gtk_widget_get_realized(grp->image)) {
        GtkAllocation ia;
        gtk_widget_get_allocation(grp->image, &ia);
        gtk_widget_translate_coordinates(grp->image, btn, 0, 0, &ix, &iy);
        ix += (ia.width - isz) / 2;
        iy += (ia.height - isz) / 2;
    } else {
        ix = (w - isz) / 2; iy = (h - isz) / 2;
    }

    gdouble prog = group_progress(grp);
    if (prog >= 0.0) {          /* Win11-style: thin green bar just under the icon */
        gdouble bar_w = MAX(12.0, isz * 1.05), bar_h = 3.0;
        gdouble bx = ix + (isz - bar_w) / 2.0, by = MIN(h - 7.0, iy + isz + 2.0);
        draw_rounded_rect(cr, bx, by, bar_w, bar_h, bar_h / 2.0);
        cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.22);
        cairo_fill(cr);
        if (prog > 0.0) {
            draw_rounded_rect(cr, bx, by, MAX(bar_h, bar_w * prog), bar_h, bar_h / 2.0);
            cairo_set_source_rgba(cr, 0.30, 0.82, 0.39, 1.0);
            cairo_fill(cr);
        }
    }

    gboolean alert = FALSE;
    gint64 n = group_badge_count(grp, &alert);
    if (n > 0) taskbar_draw_icon_badge(cr, ix, iy, isz, n, alert);
    return FALSE;
}

/* ---- Drop files on an icon: open them with that app ---- */
static const GtkTargetEntry g_tb_uri_target[] = { { "text/uri-list", 0, 0 } };

static void group_set_drop_hover(AppGroupItem *grp, gboolean on) {
    if (grp->drop_hover == on) return;
    grp->drop_hover = on;
    grp->is_hovered = on;
    if (GTK_IS_WIDGET(grp->button)) gtk_widget_queue_draw(grp->button);
    if (grp->tb->shell && GTK_IS_WIDGET(grp->tb->shell->dock_window)) gtk_widget_queue_draw(grp->tb->shell->dock_window);
}

static gboolean on_group_drag_motion(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, guint t, AppGroupItem *grp) {
    if (!grp->pinned_app) { gdk_drag_status(ctx, 0, t); return FALSE; }   /* nothing to open it with */
    group_set_drop_hover(grp, TRUE);
    gdk_drag_status(ctx, GDK_ACTION_COPY, t);
    return TRUE;
}

static void on_group_drag_leave(GtkWidget *w, GdkDragContext *ctx, guint t, AppGroupItem *grp) {
    group_set_drop_hover(grp, FALSE);
}

static void on_group_drag_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y,
                                   GtkSelectionData *sd, guint info, guint t, AppGroupItem *grp) {
    group_set_drop_hover(grp, FALSE);
    gboolean ok = FALSE;
    gchar **uris = gtk_selection_data_get_uris(sd);
    if (grp->pinned_app && uris && uris[0]) {
        GList *list = NULL;
        for (gint i = 0; uris[i]; i++) list = g_list_append(list, uris[i]);
        GdkAppLaunchContext *lc = gdk_display_get_app_launch_context(gtk_widget_get_display(w));
        gdk_app_launch_context_set_timestamp(lc, t);
        GError *err = NULL;
        if (g_app_info_supports_uris(grp->pinned_app) || g_app_info_supports_files(grp->pinned_app)) {
            ok = g_app_info_launch_uris(grp->pinned_app, list, G_APP_LAUNCH_CONTEXT(lc), &err);
        } else {
            ok = g_app_info_launch(grp->pinned_app, NULL, G_APP_LAUNCH_CONTEXT(lc), &err);  /* app takes no files */
        }
        if (err) { g_warning("Open with %s failed: %s", g_app_info_get_name(grp->pinned_app), err->message); g_error_free(err); }
        g_object_unref(lc);
        g_list_free(list);
        if (ok) {
            grp->click_anim_start_us = g_get_monotonic_time();
            grp->click_anim_type = 2;
            ensure_taskbar_anim_timer(grp->tb);
        }
    }
    g_strfreev(uris);
    gtk_drag_finish(ctx, ok, FALSE, t);
}

static void group_enable_file_drop(AppGroupItem *grp) {
    gtk_drag_dest_set(grp->button, GTK_DEST_DEFAULT_DROP, g_tb_uri_target, 1, GDK_ACTION_COPY);
    g_signal_connect(grp->button, "drag-motion", G_CALLBACK(on_group_drag_motion), grp);
    g_signal_connect(grp->button, "drag-leave", G_CALLBACK(on_group_drag_leave), grp);
    g_signal_connect(grp->button, "drag-data-received", G_CALLBACK(on_group_drag_received), grp);
}

/* Redraw every icon (LauncherEntry update or icon theme change) */
static void taskbar_refresh_all_groups(TaskbarWidget *tb, gboolean reload_icons) {
    if (!tb || !tb->groups) return;
    GHashTableIter iter; gpointer k, v;
    g_hash_table_iter_init(&iter, tb->groups);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        AppGroupItem *grp = v;
        if (reload_icons && grp->pb_master) { g_object_unref(grp->pb_master); grp->pb_master = NULL; }
        refresh_group_visuals(grp);
        if (GTK_IS_WIDGET(grp->button)) gtk_widget_queue_draw(grp->button);
    }
    if (tb->shell && GTK_IS_WIDGET(tb->shell->dock_window)) gtk_widget_queue_draw(tb->shell->dock_window);
}

/* ---- XApp progress hint (Firefox, Cinnamon/MATE apps): _NET_WM_XAPP_PROGRESS on the window ---- */
static gint read_xapp_progress(WnckWindow *win) {
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    static Atom a_prog = None;
    if (a_prog == None) a_prog = XInternAtom(dpy, "_NET_WM_XAPP_PROGRESS", False);
    Atom type; int fmt; unsigned long n = 0, left = 0; unsigned char *data = NULL;
    gint val = -1;
    gdk_x11_display_error_trap_push(gdk_display_get_default());
    if (XGetWindowProperty(dpy, wnck_window_get_xid(win), a_prog, 0, 1, False, XA_CARDINAL,
                           &type, &fmt, &n, &left, &data) == Success && data && n == 1 && fmt == 32) {
        val = (gint)*(unsigned long *)data;
    }
    if (data) XFree(data);
    gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
    return (val >= 0 && val <= 100) ? val : -1;   /* 0 = cleared */
}

static gboolean on_xapp_progress_poll(gpointer data) {
    if (!g_taskbar || !g_taskbar->groups) return G_SOURCE_CONTINUE;
    gboolean dock_dirty = FALSE;
    GHashTableIter iter; gpointer k, v;
    g_hash_table_iter_init(&iter, g_taskbar->groups);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        AppGroupItem *grp = v;
        gint best = -1;
        for (GList *l = grp->windows; l; l = l->next) {
            gint p = read_xapp_progress(WNCK_WINDOW(l->data));
            if (p > 0 && p > best) best = p;
        }
        if (best == 100) best = -1;          /* finished */
        if (best != grp->xapp_progress) {
            grp->xapp_progress = best;
            if (GTK_IS_WIDGET(grp->button)) gtk_widget_queue_draw(grp->button);
            dock_dirty = TRUE;
        }
    }
    if (dock_dirty && g_taskbar->shell && GTK_IS_WIDGET(g_taskbar->shell->dock_window))
        gtk_widget_queue_draw(g_taskbar->shell->dock_window);
    return G_SOURCE_CONTINUE;
}

/* Both use g_taskbar, not a captured pointer: the taskbar is rebuilt on layout switches */
static void on_launcher_entry_changed(gpointer data) {
    taskbar_refresh_all_groups(g_taskbar, FALSE);
}

static void on_tb_icon_theme_changed(GtkIconTheme *theme, gpointer data) {
    taskbar_refresh_all_groups(g_taskbar, TRUE);
}

static AppGroupItem *ensure_group_item(TaskbarWidget *tb, const gchar *norm_key, GAppInfo *optional_app, gboolean pinned) {
    AppGroupItem *grp = g_hash_table_lookup(tb->groups, norm_key);
    if (grp) {
        if (pinned) grp->is_pinned = TRUE;
        if (optional_app && !grp->pinned_app) grp->pinned_app = g_object_ref(optional_app);
        refresh_group_visuals(grp);
        return grp;
    }

    grp = g_new0(AppGroupItem, 1);
    grp->tb = tb;
    grp->group_id = g_strdup(norm_key);
    grp->is_pinned = pinned;
    grp->press_scale = 1.0;
    grp->xapp_progress = -1;
    grp->pinned_app = optional_app ? g_object_ref(optional_app) : find_app_info_for_key(norm_key);
    if (grp->pinned_app && g_app_info_get_id(grp->pinned_app)) {
        grp->desktop_id = g_strdup(g_app_info_get_id(grp->pinned_app));
    }
    update_group_display_title(grp);

    grp->button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(grp->button), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(grp->button), "taskbar-item-btn");
    g_object_set_data(G_OBJECT(grp->button), "app-group-item", grp);

    grp->image = gtk_image_new();
    GtkWidget *overlay = shell_create_badged_icon(grp->image, &grp->badge_lbl);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(vbox, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(vbox), overlay, TRUE, TRUE, 0);

    g_signal_connect(vbox, "draw", G_CALLBACK(on_suppress_dock_child_draw), grp);
    gtk_container_add(GTK_CONTAINER(grp->button), vbox);

    g_signal_connect_after(grp->button, "draw", G_CALLBACK(on_draw_taskbar_item_indicator), grp);
    g_signal_connect_after(grp->button, "draw", G_CALLBACK(on_draw_taskbar_item_progress), grp);
    group_enable_file_drop(grp);

    gtk_widget_add_events(grp->button, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
                          GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_BUTTON1_MOTION_MASK | GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK);
    g_signal_connect(grp->button, "enter-notify-event", G_CALLBACK(on_icon_enter_notify), grp);
    g_signal_connect(grp->button, "leave-notify-event", G_CALLBACK(on_icon_leave_notify), grp);
    g_signal_connect(grp->button, "scroll-event", G_CALLBACK(on_group_btn_scroll), grp);

    g_signal_connect(grp->button, "clicked", G_CALLBACK(on_group_btn_clicked), grp);
    g_signal_connect(grp->button, "button-press-event", G_CALLBACK(on_group_btn_press), grp);
    g_signal_connect(grp->button, "motion-notify-event", G_CALLBACK(on_group_btn_motion), grp);
    g_signal_connect(grp->button, "button-release-event", G_CALLBACK(on_group_btn_release), grp);

    gtk_box_pack_start(GTK_BOX(tb->box), grp->button, FALSE, FALSE, 0);
    gtk_widget_show_all(grp->button);
    shell_set_badge_count(grp->badge_lbl, 0);

    g_hash_table_insert(tb->groups, grp->group_id, grp);
    refresh_group_visuals(grp);
    return grp;
}

void taskbar_pin_app_info(GAppInfo *app) {
    if (!g_taskbar || !app) return;
    gchar *norm = normalize_group_key(g_app_info_get_id(app) ? g_app_info_get_id(app) : g_app_info_get_display_name(app));
    AppGroupItem *existing = find_matching_group_for_window(g_taskbar, norm);
    if (existing) {
        existing->is_pinned = TRUE;
        if (!existing->pinned_app) existing->pinned_app = g_object_ref(app);
        if (!existing->desktop_id && g_app_info_get_id(app)) {
            existing->desktop_id = g_strdup(g_app_info_get_id(app));
        }
        refresh_group_visuals(existing);
    } else {
        ensure_group_item(g_taskbar, norm, app, TRUE);
    }
    save_pinned_apps_to_xfconf(g_taskbar);
    g_free(norm);
}

gint taskbar_collect_dock_render_items(DockRenderItem *out_items, gint max_items) {
    if (!g_taskbar || !GTK_IS_CONTAINER(g_taskbar->box) || !g_taskbar->tracker || !g_taskbar->tracker->screen || !out_items || max_items <= 0) return 0;
    WnckWindow *active = wnck_screen_get_active_window(g_taskbar->tracker->screen);
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_taskbar->box));
    gint count = 0;

    for (GList *l = children; l != NULL && count < max_items; l = l->next) {
        GtkWidget *btn = GTK_WIDGET(l->data);
        if (!gtk_widget_get_visible(btn)) continue;
        AppGroupItem *grp = g_object_get_data(G_OBJECT(btn), "app-group-item");
        if (!grp) continue;
        ensure_group_master_pixbuf(grp);
        update_group_display_title(grp);
        guint w_cnt = g_list_length(grp->windows);
        out_items[count].btn             = btn;
        out_items[count].pb_master       = grp->pb_master;
        out_items[count].has_windows     = (w_cnt > 0);
        out_items[count].has_active      = (w_cnt > 0 && g_list_find(grp->windows, active) != NULL);
        out_items[count].is_hovered      = grp->is_hovered;
        out_items[count].is_pinned       = grp->is_pinned;
        out_items[count].needs_attention = grp->needs_attention;
        out_items[count].win_count       = w_cnt;
        /* the Ubuntu dock has no room for hops: urgent apps get the glow / pulse indicator only */
        out_items[count].bounce_offset   = (g_taskbar->shell && g_taskbar->shell->dock_style == 1) ? 0.0 : grp->bounce_offset;
        out_items[count].press_scale     = (grp->press_scale > 0.1) ? grp->press_scale : 1.0;
        out_items[count].title           = grp->display_title;
        out_items[count].le_count        = group_le_count_shown(grp) ? grp->le.count : 0;
        out_items[count].le_progress     = group_progress(grp);
        count++;
    }
    g_list_free(children);
    return count;
}

GtkAlign taskbar_dock_halign(UltimateShell *shell) {
    if (shell->dock_pos == DOCK_POS_BOTTOM)
        return (shell->dock_full_width && shell->dock_full_width_layout == 3) ? GTK_ALIGN_START : GTK_ALIGN_CENTER;
    if (shell->dock_style == 1) return GTK_ALIGN_FILL;          /* Ubuntu dock: buttons span the strip */
    return shell->dock_pos == DOCK_POS_LEFT ? GTK_ALIGN_START : GTK_ALIGN_END;   /* along the screen edge, like the icons */
}

void taskbar_update_icon_sizes(UltimateShell *shell) {
    if (!g_taskbar) return;
    if (shell->mode == LAYOUT_MACOS) {
        gtk_widget_set_halign(g_taskbar->box, taskbar_dock_halign(shell));
    } else if (shell->tb_align == TASKBAR_ALIGN_LEFT) {
        gtk_widget_set_halign(g_taskbar->box, GTK_ALIGN_START);
    } else {
        gtk_widget_set_halign(g_taskbar->box, GTK_ALIGN_CENTER);
    }
    gtk_box_set_spacing(GTK_BOX(g_taskbar->box), CLAMP(shell->tb_icon_spacing, 0, 24));
    if (shell->left_box && shell->mode != LAYOUT_MACOS)
        gtk_box_set_spacing(GTK_BOX(shell->left_box), CLAMP(shell->tb_icon_spacing, 0, 24));

    GHashTableIter iter; gpointer k, v;
    g_hash_table_iter_init(&iter, g_taskbar->groups);
    while (g_hash_table_iter_next(&iter, &k, &v)) refresh_group_visuals((AppGroupItem *)v);
}

static void on_tracked_window_changed(WnckWindow *window, TaskbarWidget *tb) {
    AppGroupItem *grp = g_hash_table_lookup(tb->win_to_grp, window);
    if (grp) {
        if (!grp->pinned_app && grp->pb_master) { g_object_unref(grp->pb_master); grp->pb_master = NULL; }
        refresh_group_visuals(grp);
    }
}

static void on_tracked_window_state_changed(WnckWindow *window, WnckWindowState changed_mask,
                                            WnckWindowState new_state, TaskbarWidget *tb) {
    AppGroupItem *grp = g_hash_table_lookup(tb->win_to_grp, window);
    if (grp) {
        refresh_group_visuals(grp);
    }
}

static void add_window_to_taskbar(TaskbarWidget *tb, WnckWindow *window) {
    if (!is_trackable_window(window)) return;
    if (g_hash_table_contains(tb->win_to_grp, window)) return;
    gchar *norm_key = get_window_normalized_key(window);
    AppGroupItem *grp = find_matching_group_for_window(tb, norm_key);
    if (!grp) {
        grp = ensure_group_item(tb, norm_key, NULL, FALSE);
    }
    g_free(norm_key);
    grp->windows = g_list_append(grp->windows, window);

    /* Stop launch bounce as soon as the app's window opens on screen */
    if (grp->click_anim_type == 2) {
        grp->click_anim_type = 0;
        grp->click_anim_start_us = 0;
        grp->bounce_offset = 0.0;
    }

    g_hash_table_insert(tb->win_to_grp, window, grp);
    g_signal_connect(window, "name-changed", G_CALLBACK(on_tracked_window_changed), tb);
    g_signal_connect(window, "icon-changed", G_CALLBACK(on_tracked_window_changed), tb);
    g_signal_connect(window, "state-changed", G_CALLBACK(on_tracked_window_state_changed), tb);
    refresh_group_visuals(grp);
}

static void on_window_opened(WnckScreen *screen, WnckWindow *window, TaskbarWidget *tb) {
    add_window_to_taskbar(tb, window);
}

static void on_window_closed(WnckScreen *screen, WnckWindow *window, TaskbarWidget *tb) {
    if (window && tb) g_signal_handlers_disconnect_by_data(window, tb);
    AppGroupItem *grp = g_hash_table_lookup(tb->win_to_grp, window);
    if (!grp) return;
    g_hash_table_remove(tb->win_to_grp, window);
    grp->windows = g_list_remove(grp->windows, window);
    if (grp->windows == NULL && !grp->is_pinned) destroy_group_item(grp);
    else refresh_group_visuals(grp);
}

static void on_active_window_changed(WnckScreen *screen, WnckWindow *prev, TaskbarWidget *tb) {
    GHashTableIter iter; gpointer k, v;
    g_hash_table_iter_init(&iter, tb->groups);
    while (g_hash_table_iter_next(&iter, &k, &v)) refresh_group_visuals((AppGroupItem *)v);
}

static void load_pinned_apps_from_xfconf(TaskbarWidget *tb) {
    gchar *saved = tb->channel ? xfconf_channel_get_string(tb->channel, "/plugins/ultimateshell/pinned_apps", "xfce4-terminal.desktop;thunar.desktop") : g_strdup("xfce4-terminal.desktop;thunar.desktop");
    if (saved && saved[0] != '\0') {
        gchar **tokens = g_strsplit(saved, ";", -1);
        for (int i = 0; tokens && tokens[i]; i++) {
            if (tokens[i][0] == '\0') continue;
            GAppInfo *app = find_app_info_for_key(tokens[i]);
            if (app) {
                gchar *norm = normalize_group_key(tokens[i]);
                ensure_group_item(tb, norm, app, TRUE);
                g_free(norm); g_object_unref(app);
            }
        }
        g_strfreev(tokens);
    }
    g_free(saved);
}

GtkWidget *taskbar_widget_new(UltimateShell *shell) {
    TaskbarWidget *tb = g_new0(TaskbarWidget, 1);
    g_taskbar = tb;
    tb->shell = shell;
    tb->tracker = shell->tracker;
    if (xfconf_init(NULL)) tb->channel = xfconf_channel_get("xfce4-panel");

    tb->box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, CLAMP(shell->tb_icon_spacing, 0, 24));
    if (shell->tb_align == TASKBAR_ALIGN_LEFT) gtk_widget_set_halign(tb->box, GTK_ALIGN_START);
    else gtk_widget_set_halign(tb->box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(tb->box, GTK_ALIGN_FILL);

    tb->groups = g_hash_table_new(g_str_hash, g_str_equal);
    tb->win_to_grp = g_hash_table_new(g_direct_hash, g_direct_equal);

    load_pinned_apps_from_xfconf(tb);

    GList *windows = wnck_screen_get_windows(tb->tracker->screen);
    for (GList *l = windows; l != NULL; l = l->next) add_window_to_taskbar(tb, WNCK_WINDOW(l->data));

    g_signal_connect(tb->tracker->screen, "window-opened", G_CALLBACK(on_window_opened), tb);
    g_signal_connect(tb->tracker->screen, "window-closed", G_CALLBACK(on_window_closed), tb);
    g_signal_connect(tb->tracker->screen, "active-window-changed", G_CALLBACK(on_active_window_changed), tb);

    launcher_entry_init(on_launcher_entry_changed, NULL);
    static gboolean theme_hooked = FALSE;
    if (!theme_hooked) {
        g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_tb_icon_theme_changed), NULL);
        g_timeout_add(1000, on_xapp_progress_poll, NULL);
        theme_hooked = TRUE;
    }

    return tb->box;
}
