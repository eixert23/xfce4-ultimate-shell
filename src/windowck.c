#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "shell-plugin.h"
#include "windowck.h"
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    UltimateWindowTracker *tracker;
    UltimateShell         *shell;
    GtkWidget             *root_box;
    GtkWidget             *controls_box;
    GtkWidget             *btn_close;
    GtkWidget             *btn_min;
    GtkWidget             *btn_max;
    GtkWidget             *img_close;
    GtkWidget             *img_min;
    GtkWidget             *img_max;

    GdkPixbuf             *pb_close[3]; /* 0=normal, 1=hover, 2=pressed */
    GdkPixbuf             *pb_min[3];
    GdkPixbuf             *pb_max[3];
    gboolean               has_xfwm_pixbufs;
    gchar                 *loaded_xfwm_theme;

    WnckWindow            *tracked_active;
    gulong                 state_sig_id;
    gulong                 actions_sig_id;
    XfconfChannel         *xfwm_chan;
    XfconfChannel         *xset_chan;
} WindowckWidget;

static WindowckWidget *g_wck_instance = NULL;

static gboolean is_top_bar_mode(UltimateShell *shell) {
    if (!shell) return FALSE;
    return (shell->mode == LAYOUT_MACOS);
}

typedef struct {
    unsigned long flags;
    unsigned long functions;
    unsigned long decorations;
    long          input_mode;
    unsigned long status;
} MotifWmHints;

#define MWM_HINTS_DECORATIONS (1L << 1)

static gboolean is_shell_or_dock_window(WnckWindow *win) {
    if (!win) return TRUE;
    WnckWindowType wtype = wnck_window_get_window_type(win);
    /* Real app windows: normal windows, dialogs (VLC/Thunar/GTK preferences) and
       Qt "utility" tool windows. Docks, panels, desktop, menus and popups are rejected. */
    if (wtype != WNCK_WINDOW_NORMAL && wtype != WNCK_WINDOW_DIALOG &&
        wtype != WNCK_WINDOW_UTILITY) return TRUE;

    /* NOTE: skip_tasklist / skip_pager are NOT used here — xfwm4 sets them on
       transient dialogs (app preferences), which made maximized dialogs lose
       their top-bar controls while their titlebar was hidden. */

    const gchar *name = wnck_window_get_name(win);
    /* Our own settings window is a normal app window */
    if (name && g_str_has_prefix(name, "Ultimate Shell")) return FALSE;

    if (name && (strcmp(name, "ultimate-macos-dock") == 0 || strcmp(name, "xfce4-panel") == 0)) {
        return TRUE;
    }

    const gchar *cls = wnck_window_get_class_group_name(win);
    if (cls && (g_ascii_strcasecmp(cls, "Xfdesktop") == 0 ||
                g_ascii_strcasecmp(cls, "Wrapper-2.0") == 0 ||
                g_ascii_strcasecmp(cls, "xfce4-panel") == 0)) {
        return TRUE;
    }

    /* Any other surface created by the shell's own process (dock, popups) */
    if (wnck_window_get_pid(win) == (int)getpid()) return TRUE;
    return FALSE;
}

/* Terminals ask the WM to resize in whole character steps (PResizeInc), which leaves a
   small gap above the panel when maximized. While a window is maximized we drop the step
   rule (and re-maximize once so xfwm4 fills the work area); when it is restored we put
   the app's original step values back, so manual resizing snaps to rows/columns again. */
static void sync_terminal_smooth_maximize_hints(Display *dpy, Window xid, WnckWindow *win, gboolean is_max) {
    XSizeHints hints;
    long supplied = 0;
    if (!XGetWMNormalHints(dpy, xid, &hints, &supplied)) return;

    gboolean stripped = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "wck-inc-stripped"));
    gboolean pending  = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "wck-remax-pending"));

    if (is_max) {
        if (pending) {
            /* This is the maximize we triggered ourselves: done, never loop */
            g_object_set_data(G_OBJECT(win), "wck-remax-pending", GINT_TO_POINTER(0));
            return;
        }
        if (!stripped && (hints.flags & PResizeInc) && (hints.width_inc > 1 || hints.height_inc > 1)) {
            g_object_set_data(G_OBJECT(win), "wck-inc-w", GINT_TO_POINTER(hints.width_inc));
            g_object_set_data(G_OBJECT(win), "wck-inc-h", GINT_TO_POINTER(hints.height_inc));
            g_object_set_data(G_OBJECT(win), "wck-inc-stripped", GINT_TO_POINTER(1));
            hints.flags &= ~PResizeInc;
            hints.width_inc = 1;
            hints.height_inc = 1;
            XSetWMNormalHints(dpy, xid, &hints);

            /* xfwm4 already rounded the maximized size down; if that left a small gap,
               re-maximize exactly once so it fills the work area */
            gint wx = 0, wy = 0, ww = 0, wh = 0;
            wnck_window_get_geometry(win, &wx, &wy, &ww, &wh);
            GdkMonitor *mon = gdk_display_get_monitor_at_point(gdk_display_get_default(), wx + ww / 2, wy + wh / 2);
            if (mon) {
                GdkRectangle wa;
                gdk_monitor_get_workarea(mon, &wa);
                gint bottom_gap = (wa.y + wa.height) - (wy + wh);
                if (bottom_gap > 1 && bottom_gap < 48) {
                    g_object_set_data(G_OBJECT(win), "wck-remax-pending", GINT_TO_POINTER(1));
                    wnck_window_unmaximize(win);
                    wnck_window_maximize(win);
                }
            }
        }
    } else {
        if (pending) return;   /* the brief unmaximize of our own re-maximize: keep the stripped hints */
        if (stripped) {
            gint w_inc = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "wck-inc-w"));
            gint h_inc = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(win), "wck-inc-h"));
            g_object_set_data(G_OBJECT(win), "wck-inc-stripped", GINT_TO_POINTER(0));
            if (w_inc > 0 && h_inc > 0) {
                hints.flags |= PResizeInc;
                hints.width_inc = w_inc;
                hints.height_inc = h_inc;
                XSetWMNormalHints(dpy, xid, &hints);
            }
        }
    }
}

/* Title bars of maximized windows are removed by xfwm4's own borderless_maximize option.
   We no longer write _MOTIF_WM_HINTS: overwriting them gave apps that draw their own
   title bar (GNOME/GTK4 apps, Chrome, Firefox) a second one, and wiped apps' own rules. */
static void set_x11_hide_titlebar_when_maximized(WnckWindow *win, gboolean enable) {
    if (is_shell_or_dock_window(win)) return;

    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    Window xid = wnck_window_get_xid(win);
    if (!xid) return;

    gdk_x11_display_error_trap_push(gdk_dpy);
    Atom gtk_hide_atom = XInternAtom(dpy, "_GTK_HIDE_TITLEBAR_WHEN_MAXIMIZED", False);

    gboolean is_max = (wnck_window_is_maximized(win) && !wnck_window_is_minimized(win));
    sync_terminal_smooth_maximize_hints(dpy, xid, win, enable && is_max);

    if (enable) {
        unsigned long val = 1UL;
        XChangeProperty(dpy, xid, gtk_hide_atom, XA_CARDINAL, 32, PropModeReplace,
                        (unsigned char *)&val, 1);
    } else {
        XDeleteProperty(dpy, xid, gtk_hide_atom);
    }
    XFlush(dpy);
    gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
}

static void sync_all_windows_titlebar_policy(WindowckWidget *wck) {
    if (!wck || !wck->tracker || !wck->tracker->screen) return;
    gboolean active_feature = (wck->shell && is_top_bar_mode(wck->shell) && wck->shell->winck_enabled);

    /* Xfwm4 native borderless_maximize handles stripping/restoring titlebars cleanly */
    if (!wck->xfwm_chan && xfconf_init(NULL)) {
        wck->xfwm_chan = xfconf_channel_get("xfwm4");
    }
    if (wck->xfwm_chan) {
        XfconfChannel *own = (wck->shell && wck->shell->channel) ? wck->shell->channel : NULL;
        const gchar *saved_key = "/plugins/ultimateshell/winck_saved_borderless_maximize";
        gboolean cur = xfconf_channel_get_bool(wck->xfwm_chan, "/general/borderless_maximize", FALSE);
        if (active_feature) {
            /* Remember the user's own value the first time we take over the option */
            if (own && !xfconf_channel_has_property(own, saved_key)) {
                xfconf_channel_set_bool(own, saved_key, cur);
            }
            if (!cur) xfconf_channel_set_bool(wck->xfwm_chan, "/general/borderless_maximize", TRUE);
        } else if (own && xfconf_channel_has_property(own, saved_key)) {
            /* Feature off: give the user back exactly what they had */
            gboolean orig = xfconf_channel_get_bool(own, saved_key, FALSE);
            if (cur != orig) xfconf_channel_set_bool(wck->xfwm_chan, "/general/borderless_maximize", orig);
            xfconf_channel_reset_property(own, saved_key, FALSE);
        }
    }

    GList *wins = wnck_screen_get_windows(wck->tracker->screen);
    for (GList *l = wins; l != NULL; l = l->next) {
        set_x11_hide_titlebar_when_maximized(WNCK_WINDOW(l->data), active_feature);
    }
}

/* Auto-trim uniform outer background / transparent border pixels so the actual
   button circle/glyph fills the requested target_sz without a square tile frame */
/* Key out the solid Xfwm4 titlebar background color from the outer tile so only
   the bright crisp button glyph / hover circle is drawn at native 1:1 sharpness */
/* Load Xfwm4 .xpm files by substituting Xfwm4 symbolic color variables
   (active_text_color, active_color_1, etc.) so symbolic XPM themes like Default
   render with bright white glyphs and a transparent background instead of invisible black */
/* Check if a hex string #RRGGBB in an XPM line is a saturated color (e.g. #E81123 red hover box) */
static gboolean xpm_line_has_saturated_hex(const gchar *line, gchar *out_hex) {
    const gchar *hash = strchr(line, '#');
    if (!hash || strlen(hash) < 7) return FALSE;
    for (int i = 1; i <= 6; i++) {
        if (!g_ascii_isxdigit(hash[i])) return FALSE;
    }
    gchar r_str[3] = { hash[1], hash[2], 0 };
    gchar g_str[3] = { hash[3], hash[4], 0 };
    gchar b_str[3] = { hash[5], hash[6], 0 };
    gint r = (gint)g_ascii_strtoull(r_str, NULL, 16);
    gint g = (gint)g_ascii_strtoull(g_str, NULL, 16);
    gint b = (gint)g_ascii_strtoull(b_str, NULL, 16);
    gint cmax = MAX(r, MAX(g, b));
    gint cmin = MIN(r, MIN(g, b));
    if ((cmax - cmin) > 40) {
        if (out_hex) {
            memcpy(out_hex, hash, 7);
            out_hex[7] = '\0';
        }
        return TRUE;
    }
    return FALSE;
}

static GdkPixbuf *load_xfwm_symbolic_xpm(const gchar *full_path, gboolean is_prelight, gboolean is_pressed) {
    gchar *contents = NULL;
    if (!g_file_get_contents(full_path, &contents, NULL, NULL) || !contents) return NULL;

    /* Only use symbolic remapping if the XPM actually uses Xfwm4 symbolic color tokens */
    if (!strstr(contents, "active_text_color") && !strstr(contents, "active_color") &&
        !strstr(contents, "active_hilight")    && !strstr(contents, "active_mid") &&
        !strstr(contents, "active_shadow")) {
        g_free(contents);
        return NULL;
    }

    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    const gchar *p = contents;
    gint line_idx = 0;
    gint num_colors = 0;
    gint cpp = 1;

    while ((p = strchr(p, '"')) != NULL) {
        p++;
        const gchar *end = strchr(p, '"');
        if (!end) break;
        gchar *line = g_strndup(p, end - p);

        if (line_idx == 0) {
            gint w = 0, h = 0;
            if (sscanf(line, "%d %d %d %d", &w, &h, &num_colors, &cpp) < 4) {
                num_colors = 0;
                cpp = 1;
            }
        } else if (line_idx <= num_colors && (gint)strlen(line) > cpp) {
            gchar *sym_prefix = g_strndup(line, cpp);
            gchar hex_buf[16] = { 0 };

            if (strstr(line, "s active_text_color") || strstr(line, "s inactive_text_color")) {
                g_free(line);
                line = g_strdup_printf("%s\tc #FFFFFF", sym_prefix);
            } else if ((is_prelight || is_pressed) && xpm_line_has_saturated_hex(line, hex_buf)) {
                /* Preserve explicit saturated hover colors like #E81123 red close button */
                g_free(line);
                line = g_strdup_printf("%s\tc %s", sym_prefix, hex_buf);
            } else if (strstr(line, "s active_hilight") || strstr(line, "s inactive_hilight")) {
                g_free(line);
                line = g_strdup_printf("%s\tc %s", sym_prefix, is_prelight ? "#454545" : (is_pressed ? "#2A2A2A" : "None"));
            } else if (strstr(line, "s active_mid") || strstr(line, "s inactive_mid")) {
                g_free(line);
                line = g_strdup_printf("%s\tc %s", sym_prefix, is_prelight ? "#3B3B3B" : (is_pressed ? "#252525" : "None"));
            } else if (strstr(line, "s active_shadow") || strstr(line, "s inactive_shadow") ||
                       strstr(line, "s active_border") || strstr(line, "s inactive_border")) {
                g_free(line);
                line = g_strdup_printf("%s\tc %s", sym_prefix, (is_prelight || is_pressed) ? "#525252" : "None");
            } else if (strstr(line, "s active_color") || strstr(line, "s inactive_color")) {
                g_free(line);
                line = g_strdup_printf("%s\tc None", sym_prefix);
            }
            g_free(sym_prefix);
        }
        g_ptr_array_add(lines, line);
        line_idx++;
        p = end + 1;
    }
    g_free(contents);

    GdkPixbuf *pb = NULL;
    if (lines->len >= 4) {
        g_ptr_array_add(lines, NULL);
        pb = gdk_pixbuf_new_from_xpm_data((const char **)lines->pdata);
    }
    g_ptr_array_free(lines, TRUE);
    return pb;
}

static GdkPixbuf *make_xfwm_bg_transparent(GdkPixbuf *src, gboolean dark_panel,
                                           gboolean has_ref_bg, guchar ref_r, guchar ref_g, guchar ref_b,
                                           gboolean *out_sampled_bg, guchar *out_r, guchar *out_g, guchar *out_b) {
    if (!src) return NULL;
    GdkPixbuf *pb = gdk_pixbuf_add_alpha(src, FALSE, 0, 0, 0);
    if (!pb) return g_object_ref(src);

    gint w = gdk_pixbuf_get_width(pb);
    gint h = gdk_pixbuf_get_height(pb);
    gint stride = gdk_pixbuf_get_rowstride(pb);
    guchar *pixels = gdk_pixbuf_get_pixels(pb);
    if (!pixels || w < 4 || h < 4) return pb;

    /* 1. If corners are already transparent, only invert if monochrome AND dark (< 110) */
    if (pixels[3] < 30 && pixels[(w - 1) * 4 + 3] < 30) {
        if (out_sampled_bg) *out_sampled_bg = FALSE;
        gint vis_count = 0;
        gint max_channel = 0;
        gint max_chroma  = 0;
        for (gint y = 0; y < h; y++) {
            guchar *row = pixels + y * stride;
            for (gint x = 0; x < w; x++) {
                guchar *px = row + x * 4;
                if (px[3] > 50) {
                    vis_count++;
                    gint cmax = MAX(px[0], MAX(px[1], px[2]));
                    gint cmin = MIN(px[0], MIN(px[1], px[2]));
                    if (cmax > max_channel) max_channel = cmax;
                    if ((cmax - cmin) > max_chroma) max_chroma = (cmax - cmin);
                }
            }
        }
        /* NEVER invert colored icons (like yellow/green/red macOS circles where max_chroma > 30)!
           Only invert monochrome dark-grey/black icons on a dark panel. */
        if (dark_panel && vis_count > 0 && max_chroma <= 30 && max_channel < 120) {
            for (gint y = 0; y < h; y++) {
                guchar *row = pixels + y * stride;
                for (gint x = 0; x < w; x++) {
                    guchar *px = row + x * 4;
                    if (px[3] > 20) {
                        px[0] = 255 - px[0];
                        px[1] = 255 - px[1];
                        px[2] = 255 - px[2];
                    }
                }
            }
        }
        return pb;
    }

    /* 2. Solid background tile:
          - For normal state (!has_ref_bg), sample corner (0,0) and export it as reference titlebar color.
          - For prelight/pressed state (has_ref_bg), compare against the normal state's reference color!
            That way, if prelight fills the whole tile with red (#E81123) or a hover shade different
            from the normal titlebar color, the full-bleed hover box stays 100% intact! */
    guchar bg_r = has_ref_bg ? ref_r : pixels[0];
    guchar bg_g = has_ref_bg ? ref_g : pixels[1];
    guchar bg_b = has_ref_bg ? ref_b : pixels[2];

    if (!has_ref_bg) {
        if (out_sampled_bg) *out_sampled_bg = TRUE;
        if (out_r) *out_r = bg_r;
        if (out_g) *out_g = bg_g;
        if (out_b) *out_b = bg_b;
    }

    gint visible_fg = 0;
    gint max_fg_channel = 0;
    gint max_fg_chroma  = 0;

    for (gint y = 0; y < h; y++) {
        guchar *row = pixels + y * stride;
        for (gint x = 0; x < w; x++) {
            guchar *p = row + x * 4;
            gint diff_corner = ABS((gint)p[0] - (gint)bg_r) +
                               ABS((gint)p[1] - (gint)bg_g) +
                               ABS((gint)p[2] - (gint)bg_b);
            gint diff = diff_corner;
            if (!has_ref_bg) {
                const guchar *edge_ref = row;
                gint diff_row = ABS((gint)p[0] - (gint)edge_ref[0]) +
                                ABS((gint)p[1] - (gint)edge_ref[1]) +
                                ABS((gint)p[2] - (gint)edge_ref[2]);
                diff = MIN(diff_corner, diff_row);
            }

            if (diff <= 14) {
                p[3] = 0;
            } else if (p[3] > 50) {
                visible_fg++;
                gint cmax = MAX(p[0], MAX(p[1], p[2]));
                gint cmin = MIN(p[0], MIN(p[1], p[2]));
                if (cmax > max_fg_channel) max_fg_channel = cmax;
                if ((cmax - cmin) > max_fg_chroma) max_fg_chroma = (cmax - cmin);
            }
        }
    }

    /* Only invert if monochrome dark glyph on dark panel (never invert colored buttons!) */
    if (dark_panel && visible_fg > 0 && max_fg_chroma <= 30 && max_fg_channel < 120) {
        for (gint y = 0; y < h; y++) {
            guchar *row = pixels + y * stride;
            for (gint x = 0; x < w; x++) {
                guchar *p = row + x * 4;
                if (p[3] > 25) {
                    p[0] = MIN(255, 255 - p[0] + 40);
                    p[1] = MIN(255, 255 - p[1] + 40);
                    p[2] = MIN(255, 255 - p[2] + 40);
                }
            }
        }
    }
    return pb;
}

static gboolean pixbuf_has_visible_glyph(GdkPixbuf *pb) {
    if (!pb) return FALSE;
    gint w = gdk_pixbuf_get_width(pb);
    gint h = gdk_pixbuf_get_height(pb);
    if (gdk_pixbuf_get_n_channels(pb) < 4) return TRUE;
    gint stride = gdk_pixbuf_get_rowstride(pb);
    const guchar *pixels = gdk_pixbuf_read_pixels(pb);
    gint count = 0;
    for (gint y = 0; y < h; y++) {
        const guchar *row = pixels + y * stride;
        for (gint x = 0; x < w; x++) {
            if (row[x * 4 + 3] > 60) count++;
        }
    }
    return (count >= 6);
}

static GdkPixbuf *load_xfwm_asset_ex(const gchar *theme_name, const gchar *base_name, gint target_sz,
                                     gboolean has_ref_bg, guchar ref_r, guchar ref_g, guchar ref_b,
                                     gboolean *out_sampled_bg, guchar *out_r, guchar *out_g, guchar *out_b) {
    if (!theme_name || !base_name) return NULL;
    const gchar *exts[] = { "png", "svg", "xpm", NULL };
    gchar *dirs[4] = {
        g_build_filename(g_get_home_dir(), ".themes", theme_name, "xfwm4", NULL),
        g_build_filename(g_get_user_data_dir(), "themes", theme_name, "xfwm4", NULL),
        g_build_filename("/usr/share/themes", theme_name, "xfwm4", NULL),
        NULL
    };

    gboolean dark_panel = (g_wck_instance && g_wck_instance->shell) ? g_wck_instance->shell->dark_mode : TRUE;
    gboolean is_prelight = (strstr(base_name, "prelight") != NULL);
    gboolean is_pressed  = (strstr(base_name, "pressed") != NULL);

    GdkPixbuf *pb = NULL;
    for (int d = 0; dirs[d] != NULL && !pb; d++) {
        for (int e = 0; exts[e] != NULL && !pb; e++) {
            gchar *fname = g_strdup_printf("%s.%s", base_name, exts[e]);
            gchar *full  = g_build_filename(dirs[d], fname, NULL);
            if (g_file_test(full, G_FILE_TEST_IS_REGULAR)) {
                GdkPixbuf *raw = NULL;
                /* 1. For .xpm files that use Xfwm4 symbolic tokens (like Default), remap symbols cleanly first */
                if (strcmp(exts[e], "xpm") == 0) {
                    raw = load_xfwm_symbolic_xpm(full, is_prelight, is_pressed);
                    if (raw) {
                        pb = make_xfwm_bg_transparent(raw, dark_panel, FALSE, 0, 0, 0, NULL, NULL, NULL, NULL);
                        g_object_unref(raw);
                        if (pb && !pixbuf_has_visible_glyph(pb)) {
                            g_object_unref(pb);
                            pb = NULL;
                        }
                    }
                }
                /* 2. For PNG/SVG or non-symbolic XPMs, load directly and key out the reference background */
                if (!pb) {
                    raw = gdk_pixbuf_new_from_file(full, NULL);
                    if (raw) {
                        pb = make_xfwm_bg_transparent(raw, dark_panel, has_ref_bg, ref_r, ref_g, ref_b,
                                                      out_sampled_bg, out_r, out_g, out_b);
                        g_object_unref(raw);
                        if (pb && !pixbuf_has_visible_glyph(pb)) {
                            g_object_unref(pb);
                            pb = NULL;
                        }
                    }
                }
                if (pb) {
                    gint w = gdk_pixbuf_get_width(pb);
                    gint h = gdk_pixbuf_get_height(pb);
                    if (h > w + 2 && w >= 14) {
                        gint crop_y = (h - w) / 2;
                        GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(pb, 0, crop_y, w, w);
                        if (sub) {
                            GdkPixbuf *cp = gdk_pixbuf_copy(sub);
                            g_object_unref(sub);
                            g_object_unref(pb);
                            pb = cp;
                        }
                    }
                }
            }
            g_free(full);
            g_free(fname);
        }
    }
    for (int d = 0; dirs[d] != NULL; d++) g_free(dirs[d]);
    return pb;
}

static void clear_xfwm_pixbufs(WindowckWidget *wck) {
    for (int i = 0; i < 3; i++) {
        if (wck->pb_close[i]) { g_object_unref(wck->pb_close[i]); wck->pb_close[i] = NULL; }
        if (wck->pb_min[i])   { g_object_unref(wck->pb_min[i]);   wck->pb_min[i]   = NULL; }
        if (wck->pb_max[i])   { g_object_unref(wck->pb_max[i]);   wck->pb_max[i]   = NULL; }
    }
    wck->has_xfwm_pixbufs = FALSE;
}

static void reload_windowck_theme_assets(WindowckWidget *wck) {
    if (!wck) return;
    if (!wck->xfwm_chan && xfconf_init(NULL)) wck->xfwm_chan = xfconf_channel_get("xfwm4");
    if (!wck->xset_chan && xfconf_init(NULL)) wck->xset_chan = xfconf_channel_get("xsettings");

    gchar *theme = NULL;
    if (wck->xfwm_chan) {
        theme = xfconf_channel_get_string(wck->xfwm_chan, "/general/theme", NULL);
    }
    if ((!theme || !theme[0]) && wck->xset_chan) {
        g_free(theme);
        theme = xfconf_channel_get_string(wck->xset_chan, "/Net/ThemeName", "Default");
    }
    if (!theme || !theme[0]) {
        g_free(theme);
        theme = g_strdup("Default");
    }

    clear_xfwm_pixbufs(wck);
    g_free(wck->loaded_xfwm_theme);
    wck->loaded_xfwm_theme = g_strdup(theme);

    gint btn_sz = (wck->shell && wck->shell->winck_btn_size >= 12)
                ? CLAMP(wck->shell->winck_btn_size, 12, 32)
                : 22;

    gboolean has_c_bg = FALSE, has_h_bg = FALSE, has_m_bg = FALSE;
    guchar cr = 0, cg = 0, cb = 0, hr = 0, hg = 0, hb = 0, mr = 0, mg = 0, mb = 0;

    wck->pb_close[0] = load_xfwm_asset_ex(theme, "close-active", btn_sz, FALSE, 0, 0, 0, &has_c_bg, &cr, &cg, &cb);
    wck->pb_close[1] = load_xfwm_asset_ex(theme, "close-prelight", btn_sz, has_c_bg, cr, cg, cb, NULL, NULL, NULL, NULL);
    wck->pb_close[2] = load_xfwm_asset_ex(theme, "close-pressed", btn_sz, has_c_bg, cr, cg, cb, NULL, NULL, NULL, NULL);

    wck->pb_min[0]   = load_xfwm_asset_ex(theme, "hide-active", btn_sz, FALSE, 0, 0, 0, &has_h_bg, &hr, &hg, &hb);
    wck->pb_min[1]   = load_xfwm_asset_ex(theme, "hide-prelight", btn_sz, has_h_bg, hr, hg, hb, NULL, NULL, NULL, NULL);
    wck->pb_min[2]   = load_xfwm_asset_ex(theme, "hide-pressed", btn_sz, has_h_bg, hr, hg, hb, NULL, NULL, NULL, NULL);

    /* The controls only show for maximized windows, so this button means "Restore":
       prefer the theme's toggled (restore) artwork, fall back to the plain maximize one. */
    wck->pb_max[0]   = load_xfwm_asset_ex(theme, "maximize-toggled-active", btn_sz, FALSE, 0, 0, 0, &has_m_bg, &mr, &mg, &mb);
    if (!wck->pb_max[0]) wck->pb_max[0] = load_xfwm_asset_ex(theme, "maximize-active", btn_sz, FALSE, 0, 0, 0, &has_m_bg, &mr, &mg, &mb);
    wck->pb_max[1]   = load_xfwm_asset_ex(theme, "maximize-toggled-prelight", btn_sz, has_m_bg, mr, mg, mb, NULL, NULL, NULL, NULL);
    if (!wck->pb_max[1]) wck->pb_max[1] = load_xfwm_asset_ex(theme, "maximize-prelight", btn_sz, has_m_bg, mr, mg, mb, NULL, NULL, NULL, NULL);
    wck->pb_max[2]   = load_xfwm_asset_ex(theme, "maximize-toggled-pressed", btn_sz, has_m_bg, mr, mg, mb, NULL, NULL, NULL, NULL);
    if (!wck->pb_max[2]) wck->pb_max[2] = load_xfwm_asset_ex(theme, "maximize-pressed", btn_sz, has_m_bg, mr, mg, mb, NULL, NULL, NULL, NULL);

    if (wck->pb_close[0] && wck->pb_min[0] && wck->pb_max[0]) {
        wck->has_xfwm_pixbufs = TRUE;
        gtk_image_set_from_pixbuf(GTK_IMAGE(wck->img_close), wck->pb_close[0]);
        gtk_image_set_from_pixbuf(GTK_IMAGE(wck->img_min),   wck->pb_min[0]);
        gtk_image_set_from_pixbuf(GTK_IMAGE(wck->img_max),   wck->pb_max[0]);
    } else {
        clear_xfwm_pixbufs(wck);
        /* Fallback: let GtkImage render symbolic icon names directly so CSS @theme_fg_color makes them bright white */
        gtk_image_set_from_icon_name(GTK_IMAGE(wck->img_close), "window-close-symbolic",    GTK_ICON_SIZE_MENU);
        gtk_image_set_from_icon_name(GTK_IMAGE(wck->img_min),   "window-minimize-symbolic", GTK_ICON_SIZE_MENU);
        gtk_image_set_from_icon_name(GTK_IMAGE(wck->img_max),   "window-restore-symbolic",  GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(wck->img_close), btn_sz);
        gtk_image_set_pixel_size(GTK_IMAGE(wck->img_min),   btn_sz);
        gtk_image_set_pixel_size(GTK_IMAGE(wck->img_max),   btn_sz);
    }

    g_free(theme);
}

/* Does this window draw its own title bar + buttons (client-side decorations)?
   - GTK3/GTK4 CSD apps set _GTK_FRAME_EXTENTS
   - Firefox, Chrome/Electron etc. ask the WM for no decorations via _MOTIF_WM_HINTS
   Those buttons are part of the app, so we must not add a second set in the top bar. */
static gboolean window_draws_own_buttons(WnckWindow *win) {
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!win || !GDK_IS_X11_DISPLAY(gdk_dpy)) return FALSE;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    Window xid = wnck_window_get_xid(win);
    if (!xid) return FALSE;

    gboolean csd = FALSE;
    Atom type_ret; int fmt_ret; unsigned long n_items = 0, bytes_after = 0;
    unsigned char *prop = NULL;

    gdk_x11_display_error_trap_push(gdk_dpy);
    Atom frame_atom = XInternAtom(dpy, "_GTK_FRAME_EXTENTS", False);
    if (XGetWindowProperty(dpy, xid, frame_atom, 0, 4, False, XA_CARDINAL,
                           &type_ret, &fmt_ret, &n_items, &bytes_after, &prop) == Success && prop) {
        if (n_items == 4) csd = TRUE;
        XFree(prop);
        prop = NULL;
    }

    if (!csd) {
        Atom motif_atom = XInternAtom(dpy, "_MOTIF_WM_HINTS", False);
        n_items = 0;
        if (XGetWindowProperty(dpy, xid, motif_atom, 0, 5, False, AnyPropertyType,
                               &type_ret, &fmt_ret, &n_items, &bytes_after, &prop) == Success && prop) {
            if (fmt_ret == 32 && n_items >= 3) {
                unsigned long *h = (unsigned long *)prop;
                /* flags has MWM_HINTS_DECORATIONS and decorations == 0 -> app draws its own */
                if ((h[0] & MWM_HINTS_DECORATIONS) && h[2] == 0) csd = TRUE;
            }
            XFree(prop);
        }
    }
    gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
    return csd;
}

static WnckWindow *get_valid_active_window(WindowckWidget *wck) {
    if (!wck || !wck->tracker || !wck->tracker->screen) return NULL;
    WnckWindow *win = wnck_screen_get_active_window(wck->tracker->screen);
    if (!win || is_shell_or_dock_window(win)) return NULL;
    return win;
}

static void refresh_windowck_ui(WindowckWidget *wck) {
    if (!wck || !GTK_IS_WIDGET(wck->controls_box)) return;

    WnckWindow *active_any = get_valid_active_window(wck);
    gboolean enabled = (wck->shell && is_top_bar_mode(wck->shell) && wck->shell->winck_enabled);
    if (!enabled) {
        if (active_any) set_x11_hide_titlebar_when_maximized(active_any, FALSE);
        gtk_widget_hide(wck->controls_box);
        gtk_widget_hide(wck->root_box);
        return;
    }

    WnckWindow *win = get_valid_active_window(wck);

    gboolean is_max = (win != NULL && wnck_window_is_maximized(win) && !wnck_window_is_minimized(win));
    /* Apps that draw their own buttons (Firefox, Chrome, GNOME apps) keep theirs; we hide ours */
    if (is_max && window_draws_own_buttons(win)) is_max = FALSE;
    if (is_max) {
        gtk_widget_show(wck->root_box);
        gtk_widget_show(wck->controls_box);
        gtk_widget_show_all(wck->btn_min);
        gtk_widget_show_all(wck->btn_max);
        gtk_widget_show_all(wck->btn_close);

        /* Grey out what the window manager says this window can't do — e.g. a
           modal dialog (VLC Preferences) has no _NET_WM_ACTION_MINIMIZE. */
        WnckWindowActions acts = wnck_window_get_actions(win);
        gtk_widget_set_sensitive(wck->btn_min, (acts & WNCK_WINDOW_ACTION_MINIMIZE) != 0);
        gtk_widget_set_sensitive(wck->btn_max, (acts & (WNCK_WINDOW_ACTION_UNMAXIMIZE |
                                                        WNCK_WINDOW_ACTION_MAXIMIZE)) != 0);
        gtk_widget_set_sensitive(wck->btn_close, (acts & WNCK_WINDOW_ACTION_CLOSE) != 0);
    } else {
        gtk_widget_hide(wck->controls_box);
    }
}

static void on_active_win_state_changed(WnckWindow *window, WnckWindowState changed, WnckWindowState new_state, WindowckWidget *wck) {
    /* Window hints are handled once per change by on_any_win_state_changed (every window);
       here we only update the top-bar buttons. */
    refresh_windowck_ui(wck);
}

static void on_any_win_state_changed(WnckWindow *window, WnckWindowState changed,
                                     WnckWindowState new_state, WindowckWidget *wck) {
    if (!wck || !wck->shell) return;
    if (!(changed & (WNCK_WINDOW_STATE_MAXIMIZED_HORIZONTALLY | WNCK_WINDOW_STATE_MAXIMIZED_VERTICALLY |
                     WNCK_WINDOW_STATE_MINIMIZED))) return;
    gboolean active_feature = (is_top_bar_mode(wck->shell) && wck->shell->winck_enabled);
    set_x11_hide_titlebar_when_maximized(window, active_feature);
}

static void on_window_opened(WnckScreen *screen, WnckWindow *window, WindowckWidget *wck) {
    g_signal_connect(window, "state-changed", G_CALLBACK(on_any_win_state_changed), wck);
    if (wck && wck->shell) {
        gboolean active_feature = (is_top_bar_mode(wck->shell) && wck->shell->winck_enabled);
        set_x11_hide_titlebar_when_maximized(window, active_feature);
    }
    refresh_windowck_ui(wck);
}

static void on_active_window_changed(WnckScreen *screen, WnckWindow *prev, WindowckWidget *wck) {
    if (wck->tracked_active) {
        if (wck->state_sig_id) g_signal_handler_disconnect(wck->tracked_active, wck->state_sig_id);
        wck->state_sig_id = 0;
        if (wck->actions_sig_id) g_signal_handler_disconnect(wck->tracked_active, wck->actions_sig_id);
        wck->actions_sig_id = 0;
        g_object_remove_weak_pointer(G_OBJECT(wck->tracked_active), (gpointer *)&wck->tracked_active);
    }

    wck->tracked_active = get_valid_active_window(wck);
    if (wck->tracked_active) {
        g_object_add_weak_pointer(G_OBJECT(wck->tracked_active), (gpointer *)&wck->tracked_active);
        wck->state_sig_id = g_signal_connect(wck->tracked_active, "state-changed",
                                             G_CALLBACK(on_active_win_state_changed), wck);
        wck->actions_sig_id = g_signal_connect_swapped(wck->tracked_active, "actions-changed",
                                             G_CALLBACK(refresh_windowck_ui), wck);
    }

    refresh_windowck_ui(wck);
}

static void on_close_clicked(GtkButton *btn, WindowckWidget *wck) {
    WnckWindow *win = get_valid_active_window(wck);
    if (win) wnck_window_close(win, gtk_get_current_event_time());
}

static void on_min_clicked(GtkButton *btn, WindowckWidget *wck) {
    WnckWindow *win = get_valid_active_window(wck);
    if (win && (wnck_window_get_actions(win) & WNCK_WINDOW_ACTION_MINIMIZE)) wnck_window_minimize(win);
}

static void on_max_clicked(GtkButton *btn, WindowckWidget *wck) {
    WnckWindow *win = get_valid_active_window(wck);
    if (!win) return;
    if (wnck_window_is_maximized(win))
        wnck_window_unmaximize(win);
    else
        wnck_window_maximize(win);
}

static gboolean on_wck_btn_state_event(GtkWidget *btn, GdkEvent *ev, WindowckWidget *wck) {
    if (!wck || !wck->has_xfwm_pixbufs) return FALSE;
    gint state_idx = 0;
    if (!gtk_widget_is_sensitive(btn)) {
        state_idx = 0;                      /* disabled: never show hover/pressed art */
    } else if (ev->type == GDK_ENTER_NOTIFY) {
        state_idx = 1;
    } else if (ev->type == GDK_BUTTON_PRESS) {
        state_idx = 2;
    } else if (ev->type == GDK_BUTTON_RELEASE) {
        /* still under the pointer after a click -> back to hover, not normal */
        GtkAllocation al;
        gtk_widget_get_allocation(btn, &al);
        gdouble ex = ((GdkEventButton *)ev)->x, ey = ((GdkEventButton *)ev)->y;
        state_idx = (ex >= 0 && ey >= 0 && ex < al.width && ey < al.height) ? 1 : 0;
    } else {
        state_idx = 0;                      /* leave */
    }

    if (btn == wck->btn_close) {
        GdkPixbuf *pb = wck->pb_close[state_idx] ? wck->pb_close[state_idx] : wck->pb_close[0];
        if (pb) gtk_image_set_from_pixbuf(GTK_IMAGE(wck->img_close), pb);
    } else if (btn == wck->btn_min) {
        GdkPixbuf *pb = wck->pb_min[state_idx] ? wck->pb_min[state_idx] : wck->pb_min[0];
        if (pb) gtk_image_set_from_pixbuf(GTK_IMAGE(wck->img_min), pb);
    } else if (btn == wck->btn_max) {
        GdkPixbuf *pb = wck->pb_max[state_idx] ? wck->pb_max[state_idx] : wck->pb_max[0];
        if (pb) gtk_image_set_from_pixbuf(GTK_IMAGE(wck->img_max), pb);
    }
    return FALSE;
}

static GtkWidget *create_win_control_btn(WindowckWidget *wck, const gchar *role_class,
                                         const gchar *tooltip, GtkWidget **out_img) {
    GtkWidget *btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_valign(btn, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(btn, tooltip);
    gtk_widget_add_events(btn, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK |
                               GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK);

    GtkStyleContext *ctx = gtk_widget_get_style_context(btn);
    gtk_style_context_add_class(ctx, "winck-btn");
        gtk_style_context_add_class(ctx, role_class);

    GtkWidget *img = gtk_image_new();
    gtk_container_add(GTK_CONTAINER(btn), img);
    if (out_img) *out_img = img;

    g_signal_connect(btn, "enter-notify-event",   G_CALLBACK(on_wck_btn_state_event), wck);
    g_signal_connect(btn, "leave-notify-event",   G_CALLBACK(on_wck_btn_state_event), wck);
    g_signal_connect(btn, "button-press-event",   G_CALLBACK(on_wck_btn_state_event), wck);
    g_signal_connect(btn, "button-release-event", G_CALLBACK(on_wck_btn_state_event), wck);
    return btn;
}

void windowck_set_left_order(GtkWidget *wck_root, gboolean left_order_chm) {
    if (!GTK_IS_WIDGET(wck_root)) return;
    WindowckWidget *wck = g_object_get_data(G_OBJECT(wck_root), "wck-struct");
    if (!wck || !GTK_IS_WIDGET(wck->controls_box)) return;

    if (left_order_chm) {
        if (gtk_widget_get_parent(wck->btn_close) == wck->controls_box)
            gtk_box_reorder_child(GTK_BOX(wck->controls_box), wck->btn_close, 0);
        if (gtk_widget_get_parent(wck->btn_min) == wck->controls_box)
            gtk_box_reorder_child(GTK_BOX(wck->controls_box), wck->btn_min,   1);
        if (gtk_widget_get_parent(wck->btn_max) == wck->controls_box)
            gtk_box_reorder_child(GTK_BOX(wck->controls_box), wck->btn_max,   2);
    } else {
        if (gtk_widget_get_parent(wck->btn_min) == wck->controls_box)
            gtk_box_reorder_child(GTK_BOX(wck->controls_box), wck->btn_min,   0);
        if (gtk_widget_get_parent(wck->btn_max) == wck->controls_box)
            gtk_box_reorder_child(GTK_BOX(wck->controls_box), wck->btn_max,   1);
        if (gtk_widget_get_parent(wck->btn_close) == wck->controls_box)
            gtk_box_reorder_child(GTK_BOX(wck->controls_box), wck->btn_close, 2);
    }
}

void windowck_apply_settings(UltimateShell *shell) {
    if (!g_wck_instance || !shell || !GTK_IS_BOX(g_wck_instance->controls_box)) return;
    g_wck_instance->shell = shell;
    gtk_box_set_spacing(GTK_BOX(g_wck_instance->controls_box), CLAMP(shell->winck_btn_pad, 0, 20));
    windowck_set_left_order(g_wck_instance->root_box, shell_winck_on_left(shell));
    reload_windowck_theme_assets(g_wck_instance);
    sync_all_windows_titlebar_policy(g_wck_instance);
    refresh_windowck_ui(g_wck_instance);
}

static void on_theme_prop_changed(XfconfChannel *chan, const gchar *prop, const GValue *val, WindowckWidget *wck) {
    reload_windowck_theme_assets(wck);
    refresh_windowck_ui(wck);
}

GtkWidget *windowck_widget_new(UltimateWindowTracker *tracker) {
    WindowckWidget *wck = g_new0(WindowckWidget, 1);
    g_wck_instance = wck;
    wck->tracker = tracker;

    wck->root_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_valign(wck->root_box, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(wck->root_box), "wck-struct", wck);

    wck->controls_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_valign(wck->controls_box, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(wck->controls_box, TRUE);

    wck->btn_min   = create_win_control_btn(wck, "minimize", "Minimize Maximized Window",   &wck->img_min);
    wck->btn_max   = create_win_control_btn(wck, "maximize", "Unmaximize / Restore Window", &wck->img_max);
    wck->btn_close = create_win_control_btn(wck, "close",    "Close Maximized Window",      &wck->img_close);

    g_signal_connect(wck->btn_min,   "clicked", G_CALLBACK(on_min_clicked),   wck);
    g_signal_connect(wck->btn_max,   "clicked", G_CALLBACK(on_max_clicked),   wck);
    g_signal_connect(wck->btn_close, "clicked", G_CALLBACK(on_close_clicked), wck);

    gtk_box_pack_start(GTK_BOX(wck->controls_box), wck->btn_min,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wck->controls_box), wck->btn_max,   FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(wck->controls_box), wck->btn_close, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(wck->root_box), wck->controls_box, FALSE, FALSE, 0);

    reload_windowck_theme_assets(wck);

    if (wck->xfwm_chan) {
        g_signal_connect(wck->xfwm_chan, "property-changed::/general/theme",
                         G_CALLBACK(on_theme_prop_changed), wck);
    }
    if (wck->xset_chan) {
        g_signal_connect(wck->xset_chan, "property-changed::/Net/ThemeName",
                         G_CALLBACK(on_theme_prop_changed), wck);
    }

    g_signal_connect(tracker->screen, "active-window-changed",
                     G_CALLBACK(on_active_window_changed), wck);
    g_signal_connect(tracker->screen, "window-opened",
                     G_CALLBACK(on_window_opened), wck);

    for (GList *l = wnck_screen_get_windows(tracker->screen); l != NULL; l = l->next) {
        g_signal_connect(WNCK_WINDOW(l->data), "state-changed", G_CALLBACK(on_any_win_state_changed), wck);
    }

    on_active_window_changed(tracker->screen, NULL, wck);
    return wck->root_box;
}

/* Called when the plugin is freed: hand xfwm4's option back to the user. */
void windowck_shutdown(void) {
    WindowckWidget *wck = g_wck_instance;
    if (!wck || !wck->xfwm_chan || !wck->shell || !wck->shell->channel) return;
    const gchar *saved_key = "/plugins/ultimateshell/winck_saved_borderless_maximize";
    if (xfconf_channel_has_property(wck->shell->channel, saved_key)) {
        gboolean orig = xfconf_channel_get_bool(wck->shell->channel, saved_key, FALSE);
        xfconf_channel_set_bool(wck->xfwm_chan, "/general/borderless_maximize", orig);
        /* keep the saved key: the next start takes over again and still knows the original */
    }
}
