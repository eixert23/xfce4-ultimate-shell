#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "dock-window.h"
#include "taskbar.h"
#include "start-menu.h"
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <cairo.h>
#include <math.h>

#define MAX_DOCK_ITEMS 64

static guint g_dock_poll_id = 0;
static guint g_dock_zoom_anim_id = 0;
static guint g_dock_slide_anim_id = 0;
static gboolean g_dock_should_hide = FALSE;
static gint64 g_dock_hide_deadline_us = 0;
static gdouble g_dock_hide_progress = 0.0;
static gboolean g_dock_hovered = FALSE;
static gdouble g_dock_zoom_progress = 0.0;
static gdouble g_dock_cursor_x = 0.0;
static gdouble g_dock_cursor_y = 0.0;

static GtkWidget *g_dock_launchpad_btn = NULL;
static GtkWidget *g_dock_launchpad_img = NULL;
GtkWidget *dock_window_launchpad_btn(void) { return g_dock_launchpad_btn; }
static GtkWidget *g_dock_trash_img = NULL;
static GtkWidget *g_dock_spacer_left = NULL;
static GtkWidget *g_dock_spacer_right = NULL;

static gint get_plank_buffer_thickness(UltimateShell *shell) {
    gint base_icon = CLAMP(shell->dock_icon_size, 24, 64);
    gint pad_v     = CLAMP(shell->dock_pad_v, 0, 24);
    /* Ubuntu dock: no zoom and no hops, so the window is just the strip - the drawn icons
       and the real buttons share it exactly (the zoom room is what pushed them apart) */
    if (shell->dock_style == 1) return base_icon + (pad_v * 2) + 12;
    gdouble max_z  = 2.0;
    gint max_icon  = (gint)(base_icon * max_z + 0.5);
    return max_icon + (pad_v * 2) + 46;
}

static gint get_plank_shelf_thickness(UltimateShell *shell);

/* Distance from the screen edge to the visible shelf's inner edge (popups open past this line) */
gint dock_window_shelf_extent(UltimateShell *shell) {
    if (!shell) return 0;
    return get_plank_shelf_thickness(shell) + (shell->dock_full_width ? 0 : CLAMP(shell->dock_edge_dist, 0, 64));
}

static gint get_plank_shelf_thickness(UltimateShell *shell) {
    gint base_icon = CLAMP(shell->dock_icon_size, 24, 64);
    gint pad_v     = CLAMP(shell->dock_pad_v, 0, 24);
    return base_icon + (pad_v * 2) + 12;
}

static gint get_plank_side_headroom(UltimateShell *shell) {
    gint base_icon = CLAMP(shell->dock_icon_size, 24, 64);
    return (gint)(base_icon * 1.8) + 24;
}

static void draw_shelf_rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r) {
    if (w <= 0 || h <= 0) return;
    r = CLAMP(r, 0.0, MIN(w / 2.0, h / 2.0));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2.0);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
    cairo_close_path(cr);
}

static gboolean on_suppress_special_img_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    return TRUE;
}

static gboolean on_draw_dock_toplevel_clear(GtkWidget *widget, cairo_t *cr, gpointer data) {
    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);
    return FALSE;
}


/* =========================================================================
 * Hover label (macOS style): a small real window, so it can sit past the
 * shelf on any dock side — same 6 px rule as menus/popups. It is a plain
 * popup (not a "shell popup"), so it never changes the dock's hide/lock state.
 * ========================================================================= */
static GtkWidget *g_dock_label_win = NULL;
static GtkWidget *g_dock_label_lbl = NULL;
static GtkCssProvider *g_dock_label_css = NULL;
static gchar *g_dock_label_css_key = NULL;

static void dock_label_hide(void) {
    if (g_dock_label_win && gtk_widget_get_visible(g_dock_label_win)) gtk_widget_hide(g_dock_label_win);
}

static void dock_label_show(UltimateShell *shell, const gchar *text, const GdkRGBA *bg, const GdkRGBA *fg,
                            gint center_screen, gint outer_screen) {
    if (!g_dock_label_win) {
        g_dock_label_win = gtk_window_new(GTK_WINDOW_POPUP);
        gtk_window_set_type_hint(GTK_WINDOW(g_dock_label_win), GDK_WINDOW_TYPE_HINT_TOOLTIP);
        gtk_widget_set_app_paintable(g_dock_label_win, TRUE);
        GdkScreen *scr = gtk_widget_get_screen(g_dock_label_win);
        GdkVisual *v = gdk_screen_get_rgba_visual(scr);
        if (v && gdk_screen_is_composited(scr)) gtk_widget_set_visual(g_dock_label_win, v);
        gtk_widget_set_name(g_dock_label_win, "ultimate-dock-label");
        g_dock_label_lbl = gtk_label_new(NULL);
        gtk_widget_set_name(g_dock_label_lbl, "ultimate-dock-label-text");
        gtk_container_add(GTK_CONTAINER(g_dock_label_win), g_dock_label_lbl);
        gtk_widget_show(g_dock_label_lbl);
        g_dock_label_css = gtk_css_provider_new();
        gtk_style_context_add_provider(gtk_widget_get_style_context(g_dock_label_win),
                                       GTK_STYLE_PROVIDER(g_dock_label_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 5);
        gtk_style_context_add_provider(gtk_widget_get_style_context(g_dock_label_lbl),
                                       GTK_STYLE_PROVIDER(g_dock_label_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 5);
    }
    /* themed like the menus: theme background / foreground */
    gboolean rounded = gdk_screen_is_composited(gtk_widget_get_screen(g_dock_label_win));
    gchar *bgs = gdk_rgba_to_string(bg), *fgs = gdk_rgba_to_string(fg);
    gchar *css = g_strdup_printf(
        "#ultimate-dock-label { background-color: %s; border: 1px solid alpha(%s, 0.18); border-radius: %dpx; }"
        "#ultimate-dock-label-text { color: %s; font-weight: bold; padding: 4px 11px; }",
        rounded ? "transparent" : bgs, fgs, rounded ? 8 : 0, fgs);
    if (rounded) {
        g_free(css);
        css = g_strdup_printf(
            "#ultimate-dock-label { background-color: transparent; }"
            "#ultimate-dock-label-text { background-color: alpha(%s, 0.94); color: %s; font-weight: bold;"
            " padding: 4px 11px; border-radius: 8px; border: 1px solid alpha(%s, 0.18); }", bgs, fgs, fgs);
    }
    if (g_strcmp0(css, g_dock_label_css_key) != 0) {
        gtk_css_provider_load_from_data(g_dock_label_css, css, -1, NULL);
        g_free(g_dock_label_css_key);
        g_dock_label_css_key = css;
    } else {
        g_free(css);
    }
    g_free(bgs); g_free(fgs);

    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(g_dock_label_lbl)), text) != 0) {
        gtk_label_set_text(GTK_LABEL(g_dock_label_lbl), text);
        gtk_window_resize(GTK_WINDOW(g_dock_label_win), 1, 1);
    }

    GtkRequisition req;
    gtk_widget_get_preferred_size(g_dock_label_win, NULL, &req);
    gint lw = MAX(req.width, 10), lh = MAX(req.height, 10);

    GdkDisplay *d = gdk_display_get_default();
    gboolean vert = (shell->dock_pos == DOCK_POS_LEFT || shell->dock_pos == DOCK_POS_RIGHT);
    GdkMonitor *m = vert ? gdk_display_get_monitor_at_point(d, outer_screen, center_screen)
                         : gdk_display_get_monitor_at_point(d, center_screen, outer_screen);
    GdkRectangle g = { 0, 0, 1920, 1080 };
    if (m) gdk_monitor_get_geometry(m, &g);
    const gint GAP = 6;
    gint x, y;
    if (shell->dock_pos == DOCK_POS_LEFT) {
        x = outer_screen + GAP;            y = center_screen - lh / 2;
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        x = outer_screen - lw - GAP;       y = center_screen - lh / 2;
    } else {
        x = center_screen - lw / 2;        y = outer_screen - lh - GAP;
    }
    x = CLAMP(x, g.x + GAP, MAX(g.x + GAP, g.x + g.width - lw - GAP));
    y = CLAMP(y, g.y + GAP, MAX(g.y + GAP, g.y + g.height - lh - GAP));
    gtk_window_move(GTK_WINDOW(g_dock_label_win), x, y);
    if (!gtk_widget_get_visible(g_dock_label_win)) gtk_widget_show(g_dock_label_win);
}

/* ---- Dock shadow ----
   xfwm4's dock-window shadow is switched off while the panel frame is hidden
   (it is what drew the square line around the panel), so the dock draws its own:
   a click-through window around the shelf with a soft shadow and a hole where the
   shelf is. */
#define DOCK_SHADOW_M 14
static GtkWidget   *g_dshadow = NULL;
static GdkRectangle g_dshadow_shelf;      /* shelf rect inside the shadow window */
static gint         g_dshadow_rad = 0;

static void dshadow_rrect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r) {
    r = MIN(r, MIN(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static gboolean on_dshadow_draw(GtkWidget *w, cairo_t *cr, gpointer d) {
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0, 0, 0, 0);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    GdkRectangle r = g_dshadow_shelf;
    gdouble oy = 2.0;                                    /* light from above: shadow sits a bit lower */
    for (gint k = DOCK_SHADOW_M; k >= 1; k--) {          /* soft edge from rings of fading alpha */
        gdouble t = (gdouble)k / DOCK_SHADOW_M;
        gdouble a = 0.028 * pow(1.0 - t, 1.8);       /* lighter, closer to xfwm4's own */
        dshadow_rrect(cr, r.x - k, r.y - k + oy, r.width + 2 * k, r.height + 2 * k, g_dshadow_rad + k);
        cairo_set_source_rgba(cr, 0, 0, 0, a);
        cairo_fill(cr);
    }
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);       /* never darken the shelf itself */
    dshadow_rrect(cr, r.x, r.y, r.width, r.height, g_dshadow_rad);
    cairo_fill(cr);
    return TRUE;
}

static void dshadow_clickthrough(GtkWidget *w) {
    cairo_region_t *empty = cairo_region_create();
    gtk_widget_input_shape_combine_region(w, empty);
    if (gtk_widget_get_window(w)) gdk_window_input_shape_combine_region(gtk_widget_get_window(w), empty, 0, 0);
    cairo_region_destroy(empty);
}
static void on_dshadow_map(GtkWidget *w, gpointer d) { dshadow_clickthrough(w); }

static void dock_shadow_hide(void) {
    if (g_dshadow) gtk_widget_hide(g_dshadow);
}
static void on_dock_unmap(GtkWidget *w, gpointer d) { dock_shadow_hide(); }

/* sx..sh: shelf rect in dock-window coordinates */
static void dock_shadow_update(UltimateShell *shell, GtkWidget *widget, gdouble sx, gdouble sy, gdouble sw, gdouble sh, gint rad) {
    GdkScreen *scr = gdk_screen_get_default();
    if (!shell->ui_panel_frameless || !shell->dock_icon_shadow || !scr || !gdk_screen_is_composited(scr)
        || shell->mode != LAYOUT_MACOS || sw < 8 || sh < 8) { dock_shadow_hide(); return; }
    GtkWidget *top = gtk_widget_get_toplevel(widget);
    GdkWindow *gw = top ? gtk_widget_get_window(top) : NULL;
    if (!gw) return;
    gint ox = 0, oy = 0, wx = 0, wy = 0;
    gdk_window_get_origin(gw, &ox, &oy);
    gtk_widget_translate_coordinates(widget, top, 0, 0, &wx, &wy);   /* widget may sit inside the window */
    ox += wx; oy += wy;
    if (!g_dshadow) {
        g_dshadow = gtk_window_new(GTK_WINDOW_POPUP);
        GdkVisual *v = gdk_screen_get_rgba_visual(scr);
        if (v) gtk_widget_set_visual(g_dshadow, v);
        gtk_widget_set_app_paintable(g_dshadow, TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(g_dshadow), FALSE);
        g_signal_connect(g_dshadow, "realize", G_CALLBACK(on_dshadow_map), NULL);
        g_signal_connect_after(g_dshadow, "map", G_CALLBACK(on_dshadow_map), NULL);
        g_signal_connect(g_dshadow, "draw", G_CALLBACK(on_dshadow_draw), NULL);
    }
    gint M = DOCK_SHADOW_M;
    gint W = (gint)sw + 2 * M, H = (gint)sh + 2 * M;
    gint X = ox + (gint)sx - M, Y = oy + (gint)sy - M;
    GdkRectangle shelf = { M, M, (gint)sw, (gint)sh };
    gboolean changed = shelf.width != g_dshadow_shelf.width || shelf.height != g_dshadow_shelf.height || rad != g_dshadow_rad;
    g_dshadow_shelf = shelf;
    g_dshadow_rad = rad;
    gint cx, cy, cw, ch;
    gtk_window_get_position(GTK_WINDOW(g_dshadow), &cx, &cy);
    gtk_window_get_size(GTK_WINDOW(g_dshadow), &cw, &ch);
    if (cw != W || ch != H) { gtk_widget_set_size_request(g_dshadow, W, H); gtk_window_resize(GTK_WINDOW(g_dshadow), W, H); }
    if (cx != X || cy != Y) gtk_window_move(GTK_WINDOW(g_dshadow), X, Y);
    if (!gtk_widget_get_visible(g_dshadow)) gtk_widget_show(g_dshadow);
    if (changed) gtk_widget_queue_draw(g_dshadow);
}

static gboolean on_draw_dock_shelf_bg(GtkWidget *widget, cairo_t *cr, UltimateShell *shell) {
    if (!shell) return FALSE;

    cairo_save(cr);
    cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(cr);
    cairo_restore(cr);

    gint alloc_w = gtk_widget_get_allocated_width(widget);
    gint alloc_h = gtk_widget_get_allocated_height(widget);
    if (alloc_w <= 4 || alloc_h <= 4) return FALSE;

    DockRenderItem items[MAX_DOCK_ITEMS];
    memset(items, 0, sizeof(items));
    for (gint k = 0; k < MAX_DOCK_ITEMS; k++) { items[k].le_progress = -1.0; items[k].press_scale = 1.0; }   /* Launchpad / Trash: no progress, never pressed-in */
    gint total = 0;

    if (GTK_IS_WIDGET(g_dock_launchpad_btn) && gtk_widget_get_visible(g_dock_launchpad_btn)) {
        items[total].btn             = g_dock_launchpad_btn;
        items[total].pb_master       = g_dock_launchpad_img ? g_object_get_data(G_OBJECT(g_dock_launchpad_img), "pb-master") : NULL;
        items[total].is_pinned       = TRUE;
        items[total].title           = "Launchpad";
        total++;
    }

    gint tb_cnt = taskbar_collect_dock_render_items(&items[total], MAX_DOCK_ITEMS - total - 1);
    total += tb_cnt;

    gint trash_idx = -1;
    if (GTK_IS_WIDGET(shell->dock_trash_btn) && gtk_widget_get_visible(shell->dock_trash_btn) && total < MAX_DOCK_ITEMS) {
        trash_idx = total;
        items[total].btn             = shell->dock_trash_btn;
        items[total].pb_master       = g_dock_trash_img ? g_object_get_data(G_OBJECT(g_dock_trash_img), "pb-master") : NULL;
        items[total].is_pinned       = FALSE;
        items[total].title           = "Trash";
        total++;
    }

    if (total == 0 || g_dock_hide_progress >= 0.995) { dock_label_hide(); dock_shadow_hide(); return FALSE; }

    gboolean vert      = (shell->dock_pos == DOCK_POS_LEFT || shell->dock_pos == DOCK_POS_RIGHT);
    gint slide_dist    = get_plank_shelf_thickness(shell) + CLAMP(shell->dock_edge_dist, 0, 64) + 12;
    gdouble t_s        = CLAMP(g_dock_hide_progress, 0.0, 1.0);
    gdouble eased_hide = t_s * t_s * (3.0 - 2.0 * t_s);
    gdouble slide_px   = slide_dist * eased_hide;

    cairo_save(cr);
    if (shell->dock_pos == DOCK_POS_LEFT) {
        cairo_translate(cr, -slide_px, 0.0);
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        cairo_translate(cr, slide_px, 0.0);
    } else {
        cairo_translate(cr, 0.0, slide_px);
    }

    gint base_px       = CLAMP(shell->dock_icon_size, 24, 64);
    gint pad_h         = CLAMP(shell->dock_pad_h, 0, 40);
    gint pad_v         = CLAMP(shell->dock_pad_v, 0, 24);
    gint bw            = CLAMP(shell->dock_border_w, 0, 8);
    gint rad           = shell->dock_full_width ? 0 : CLAMP(shell->dock_radius_val, 0, 32);
    gdouble alpha      = CLAMP(shell->dock_opacity, 10, 100) / 100.0;
    gint fixed_sh      = get_plank_shelf_thickness(shell);
    gdouble max_ratio  = (shell->dock_magnify && shell->dock_zoom_scale >= 100) ? (shell->dock_zoom_scale / 100.0) : 1.0;
    gdouble z_prog     = shell->dock_magnify ? g_dock_zoom_progress : 0.0;
    gdouble wave_rad   = (base_px + (pad_h / 2.0)) * 1.75;

    gint win_x = 0, win_y = 0;
    if (gtk_widget_get_window(widget)) {
        gdk_window_get_origin(gtk_widget_get_window(widget), &win_x, &win_y);
    }
    gdouble local_cursor = vert ? (g_dock_cursor_y - win_y) : (g_dock_cursor_x - win_x);

    gdouble rest_c[MAX_DOCK_ITEMS];
    gdouble scale[MAX_DOCK_ITEMS];
    gdouble extra_sz[MAX_DOCK_ITEMS];
    gdouble disp_c[MAX_DOCK_ITEMS];
    gint    hovered_idx = -1;
    gdouble best_dist   = 9999.0;

    for (gint i = 0; i < total; i++) {
        GtkAllocation ba;
        gtk_widget_get_allocation(items[i].btn, &ba);
        rest_c[i] = vert ? (ba.y + ba.height / 2.0) : (ba.x + ba.width / 2.0);
        scale[i]  = 1.0;

        gdouble dist = fabs(rest_c[i] - local_cursor);
        if (g_dock_hovered && dist < (base_px + pad_h) * 0.65 && dist < best_dist) {
            best_dist = dist;
            hovered_idx = i;
        }

        if (z_prog > 0.002 && max_ratio > 1.0 && dist < wave_rad) {
            gdouble norm = (dist / wave_rad) * (G_PI / 2.0);
            gdouble c_val = cos(norm);
            scale[i] = 1.0 + (max_ratio - 1.0) * (c_val * c_val) * z_prog;
        }
        extra_sz[i] = base_px * (scale[i] - 1.0) * 0.88;
    }

    /* Compute magnification displacement per contiguous icon cluster so far-left/right
       full-width icons stay anchored to their screen edges while center icons expand symmetrically */
    gint i_start = 0;
    while (i_start < total) {
        gint i_end = i_start;
        while (i_end + 1 < total && fabs(rest_c[i_end + 1] - rest_c[i_end]) < (base_px + pad_h) * 2.2) {
            i_end++;
        }
        gdouble grp_extra = 0.0;
        for (gint k = i_start; k <= i_end; k++) grp_extra += extra_sz[k];

        gdouble accum = -grp_extra / 2.0;
        if (shell->dock_full_width && !vert) {
            if (rest_c[i_start] < 120.0) {
                accum = 0.0;
            } else if (rest_c[i_end] > alloc_w - 120.0) {
                accum = -grp_extra;
            }
        }
        for (gint k = i_start; k <= i_end; k++) {
            disp_c[k] = rest_c[k] + accum + (extra_sz[k] / 2.0);
            accum += extra_sz[k];
        }
        i_start = i_end + 1;
    }

    gdouble first_edge = disp_c[0] - (base_px * scale[0]) / 2.0 - MAX(10, pad_h);
    gdouble last_edge  = disp_c[total - 1] + (base_px * scale[total - 1]) / 2.0 + MAX(10, pad_h);

    gdouble sx = 0.0, sy = 0.0, sw = alloc_w, sh = alloc_h;
    if (shell->dock_pos == DOCK_POS_LEFT) {
        sw = MIN((gdouble)alloc_w, (gdouble)fixed_sh);
        sx = 0.0;
        sy = MAX(0.0, first_edge);
        sh = MIN((gdouble)alloc_h - sy, last_edge - first_edge);
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        sw = MIN((gdouble)alloc_w, (gdouble)fixed_sh);
        sx = alloc_w - sw;
        sy = MAX(0.0, first_edge);
        sh = MIN((gdouble)alloc_h - sy, last_edge - first_edge);
    }
    if (vert && shell->dock_full_width) { sy = 0.0; sh = alloc_h; }   /* full height: the whole edge */ else {
        sh = MIN((gdouble)alloc_h, (gdouble)fixed_sh);
        sy = alloc_h - sh;
        if (!shell->dock_full_width) {
            sx = MAX(2.0, first_edge);
            sw = MIN((gdouble)alloc_w - sx - 2.0, last_edge - first_edge);
        }
    }

    GdkRGBA bg_col = { 0.12, 0.12, 0.13, 1.0 };
    GdkRGBA fg_col = { 0.92, 0.92, 0.94, 1.0 };
    GdkRGBA bd_col = { 0.22, 0.58, 1.00, 1.0 };
    GdkRGBA ac_col = { 0.22, 0.58, 1.00, 1.0 };
    GdkRGBA ind_col, ind_ina, ind_urg;
    shell_indicator_colors(shell, &ind_col, &ind_ina, &ind_urg);

    {   /* the shell palette: same colours as the panel and popups */
        ShellPalette pl;
        shell_palette_get(shell, &pl);
        bg_col = pl.bg; fg_col = pl.fg; ac_col = pl.accent; bd_col = pl.border;
        alpha *= pl.bg.alpha;               /* a transparent palette background stays transparent */
    }

    dock_shadow_update(shell, widget, sx, sy, sw, sh, rad);
    gdouble half_bw = bw / 2.0;
    draw_shelf_rounded_rect(cr, sx + half_bw, sy + half_bw, sw - bw, sh - bw, rad);
    cairo_set_source_rgba(cr, bg_col.red, bg_col.green, bg_col.blue, alpha);
    cairo_fill_preserve(cr);

    if (bw > 0) {
        if (shell->dock_border_color && shell->dock_border_color[0] != '\0') {
            gdk_rgba_parse(&bd_col, shell->dock_border_color);
        }
        gdouble bd_alpha = CLAMP(shell->dock_border_opacity, 0, 100) / 100.0;
        cairo_set_source_rgba(cr, bd_col.red, bd_col.green, bd_col.blue, bd_alpha);
        cairo_set_line_width(cr, bw);
        cairo_stroke(cr);
    } else {
        cairo_new_path(cr);
    }

    gdouble icon_edge_gap = MAX(10.0, pad_v + 6.0);
    gdouble shelf_bottom_y = alloc_h - icon_edge_gap;

    /* Draw subtle macOS Dock Separator Lines (between Pinned & Unpinned apps, and before Trash) */
    for (gint i = 1; i < total; i++) {
        gboolean draw_sep = (i == trash_idx) || (items[i - 1].is_pinned && !items[i].is_pinned && i != trash_idx);
        if (draw_sep) {
            gdouble prev_right = disp_c[i - 1] + (base_px * scale[i - 1]) / 2.0;
            gdouble curr_left  = disp_c[i]     - (base_px * scale[i]) / 2.0;
            gdouble mid_pos    = (curr_left - prev_right > (base_px + pad_h) * 1.5)
                               ? (curr_left - MAX(8.0, pad_h * 0.75))
                               : ((prev_right + curr_left) / 2.0);
            cairo_save(cr);
            cairo_set_source_rgba(cr, shell->dark_mode ? 1.0 : 0.0,
                                      shell->dark_mode ? 1.0 : 0.0,
                                      shell->dark_mode ? 1.0 : 0.0, 0.24);
            cairo_set_line_width(cr, 1.0);
            if (vert) {
                cairo_move_to(cr, sx + sw * 0.20, mid_pos);
                cairo_line_to(cr, sx + sw * 0.80, mid_pos);
            } else {
                cairo_move_to(cr, mid_pos, sy + sh * 0.18);
                cairo_line_to(cr, mid_pos, sy + sh * 0.82);
            }
            cairo_stroke(cr);
            cairo_restore(cr);
        }
    }

    for (gint i = 0; i < total; i++) {
        GdkPixbuf *pb = items[i].pb_master;
        if (!pb || !GDK_IS_PIXBUF(pb)) continue;

        if (shell->dock_style == 1 && vert) {
            /* Ubuntu dock: a rounded tile behind the hovered icon, a softer one for the focused app */
            gboolean hov = items[i].is_hovered || i == hovered_idx;
            gdouble a = hov ? (items[i].has_active ? 0.22 : 0.14) : (items[i].has_active ? 0.10 : 0.0);
            if (a > 0.0) {
                gdouble tile = MIN(sw - 8.0, base_px + 14.0);
                gdouble tx = sx + (sw - tile) / 2.0;
                gdouble ty = disp_c[i] - tile / 2.0;
                draw_shelf_rounded_rect(cr, tx, ty, tile, tile, 8.0);
                cairo_set_source_rgba(cr, fg_col.red, fg_col.green, fg_col.blue, a);
                cairo_fill(cr);
            }
        }
        gdouble press   = (shell->dock_style == 1) ? CLAMP(items[i].press_scale, 0.5, 1.0) : 1.0;
        gdouble draw_sz = base_px * scale[i] * press;            /* Ubuntu dock: shrinks while pressed */
        gdouble bounce  = items[i].bounce_offset + base_px * scale[i] * (1.0 - press) / 2.0;   /* stays centred */
        gdouble ix = 0.0, iy = 0.0;

        if (shell->dock_style == 1 && vert) {          /* Ubuntu dock: centred in the strip */
            ix = sx + (sw - draw_sz) / 2.0;
            iy = disp_c[i] - draw_sz / 2.0;
        } else if (shell->dock_pos == DOCK_POS_LEFT) {
            ix = icon_edge_gap + bounce;
            iy = disp_c[i] - draw_sz / 2.0;
        } else if (shell->dock_pos == DOCK_POS_RIGHT) {
            ix = alloc_w - icon_edge_gap - draw_sz - bounce;
            iy = disp_c[i] - draw_sz / 2.0;
        } else {
            ix = disp_c[i] - draw_sz / 2.0;
            iy = shelf_bottom_y - draw_sz - bounce;
        }

        gint pb_w = gdk_pixbuf_get_width(pb);
        gint pb_h = gdk_pixbuf_get_height(pb);
        if (pb_w > 0 && pb_h > 0 && shell->dock_icon_shadow) {
            /* soft drop shadow: the icon's own shape, dark, nudged down and spread a little */
            static const gdouble OFF[][2] = { {0, 2.5}, {-1.0, 3.0}, {1.0, 3.0}, {0, 3.8}, {0, 1.6} };
            gdouble k = draw_sz / 48.0;
            for (guint o = 0; o < G_N_ELEMENTS(OFF); o++) {
                cairo_save(cr);
                cairo_translate(cr, ix + OFF[o][0] * k, iy + OFF[o][1] * k);
                cairo_scale(cr, draw_sz / (gdouble)pb_w, draw_sz / (gdouble)pb_h);
                gdk_cairo_set_source_pixbuf(cr, pb, 0.0, 0.0);
                cairo_pattern_t *mask = cairo_pattern_reference(cairo_get_source(cr));
                cairo_set_source_rgba(cr, 0, 0, 0, 0.09);
                cairo_mask(cr, mask);
                cairo_pattern_destroy(mask);
                cairo_restore(cr);
            }
        }
        if (pb_w > 0 && pb_h > 0) {
            cairo_save(cr);
            cairo_translate(cr, ix, iy);
            cairo_scale(cr, draw_sz / (gdouble)pb_w, draw_sz / (gdouble)pb_h);
            gdk_cairo_set_source_pixbuf(cr, pb, 0.0, 0.0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
            cairo_restore(cr);
        }

        /* Running-app indicator: style from Settings > Indicators */
        gboolean urgent_i = items[i].needs_attention && shell->ind_urgent_pulse;
        gint dstyle = shell->dock_ind_style;
        if (items[i].has_windows && (items[i].has_active || urgent_i || shell->tb_show_inactive_ind)
            && (dstyle != DOCK_IND_NONE || urgent_i)) {
            if (dstyle == DOCK_IND_NONE) dstyle = DOCK_IND_DOT;            /* "None" still flags urgent apps */
            gboolean vert = (shell->dock_pos != DOCK_POS_BOTTOM);
            gdouble cx, cy;                                                  /* centre of the indicator */
            if (shell->dock_pos == DOCK_POS_LEFT)       { cx = icon_edge_gap / 2.0;           cy = disp_c[i]; }
            else if (shell->dock_pos == DOCK_POS_RIGHT) { cx = alloc_w - icon_edge_gap / 2.0; cy = disp_c[i]; }
            else                                        { cx = disp_c[i]; cy = shelf_bottom_y + icon_edge_gap / 2.0; }

            gboolean strong = items[i].has_active || urgent_i;
            gdouble len, thick;
            gint n = 1;
            switch (dstyle) {
                case DOCK_IND_DOT:  len = 5.0;  thick = 5.0; break;
                case DOCK_IND_DOTS: len = 4.6;  thick = 4.6; n = (gint)CLAMP(items[i].win_count, 1, 3); break;
                case DOCK_IND_LINE: len = strong ? draw_sz * 0.62 : draw_sz * 0.34; thick = 2.6; break;
                default:            len = strong ? 19.0 : (items[i].is_hovered ? 12.0 : 7.5); thick = 4.6; break;
            }
            gdouble gap = 3.0, total_len = n * len + (n - 1) * gap;
            for (gint d = 0; d < n; d++) {
                gdouble off = -total_len / 2.0 + d * (len + gap);
                if (vert) draw_shelf_rounded_rect(cr, cx - thick / 2.0, cy + off, thick, len, thick / 2.0);
                else      draw_shelf_rounded_rect(cr, cx + off, cy - thick / 2.0, len, thick, thick / 2.0);
            }
            if (urgent_i) {
                gdouble t_sec = g_get_monotonic_time() / 1000000.0;
                gdouble pa = 0.65 + 0.35 * (0.5 + 0.5 * sin(t_sec * 6.5));
                cairo_set_source_rgba(cr, ind_urg.red, ind_urg.green, ind_urg.blue, ind_urg.alpha * pa);
            } else if (strong) {
                cairo_set_source_rgba(cr, ind_col.red, ind_col.green, ind_col.blue, ind_col.alpha);
            } else {
                gboolean custom = shell->ind_inactive_color && shell->ind_inactive_color[0];
                gdouble dim = custom ? 1.0 : ((dstyle == DOCK_IND_PILL) ? 0.92 : 0.55);
                cairo_set_source_rgba(cr, ind_ina.red, ind_ina.green, ind_ina.blue, ind_ina.alpha * dim);
            }
            cairo_fill(cr);
        }

        /* App-sent progress (LauncherEntry): macOS-style bar across the lower part of the icon */
        if (items[i].le_progress >= 0.0) {
            gdouble bw = draw_sz * 0.78, bh = MAX(4.0, draw_sz * 0.11);
            gdouble bx = ix + (draw_sz - bw) / 2.0, by = iy + draw_sz * 0.80 - bh / 2.0;
            draw_shelf_rounded_rect(cr, bx - 1.0, by - 1.0, bw + 2.0, bh + 2.0, (bh + 2.0) / 2.0);
            cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.55);
            cairo_fill(cr);
            if (items[i].le_progress > 0.0) {
                draw_shelf_rounded_rect(cr, bx, by, MAX(bh, bw * items[i].le_progress), bh, bh / 2.0);
                cairo_set_source_rgba(cr, ac_col.red, ac_col.green, ac_col.blue, 1.0);
                cairo_fill(cr);
            }
        }

        /* Corner badge: the app's own count (red), else open windows when more than one */
        if (items[i].le_count > 0)
            taskbar_draw_icon_badge(cr, ix, iy, draw_sz, items[i].le_count, TRUE);
        else if (items[i].win_count > 1)
            taskbar_draw_icon_badge(cr, ix, iy, draw_sz, items[i].win_count, FALSE);
    }

    /* macOS hover label: 6 px past the (magnified) icon, on any dock side.
       Hidden while a menu is open, while the dock slides away, or when turned off. */
    if (shell->dock_show_tooltips && hovered_idx >= 0 && items[hovered_idx].title &&
        items[hovered_idx].title[0] != '\0' && g_dock_hide_progress < 0.05 && gtk_grab_get_current() == NULL) {
        gchar *txt = (items[hovered_idx].win_count > 1)
            ? g_strdup_printf("%s (%u)", items[hovered_idx].title, items[hovered_idx].win_count)
            : g_strdup(items[hovered_idx].title);
        GtkWidget *top = gtk_widget_get_toplevel(widget);
        gint ox = 0, oy = 0, wx = 0, wy = 0;
        if (gtk_widget_get_window(top)) gdk_window_get_origin(gtk_widget_get_window(top), &ox, &oy);
        gtk_widget_translate_coordinates(widget, top, 0, 0, &wx, &wy);
        ox += wx; oy += wy;
        /* how far the hovered icon reaches from the dock's screen edge */
        gdouble reach = icon_edge_gap + base_px * scale[hovered_idx] + items[hovered_idx].bounce_offset;
        gint center, outer;
        if (shell->dock_pos == DOCK_POS_LEFT)       { center = oy + (gint)disp_c[hovered_idx]; outer = ox + (gint)reach; }
        else if (shell->dock_pos == DOCK_POS_RIGHT) { center = oy + (gint)disp_c[hovered_idx]; outer = ox + alloc_w - (gint)reach; }
        else                                        { center = ox + (gint)disp_c[hovered_idx]; outer = oy + alloc_h - (gint)reach; }
        dock_label_show(shell, txt, &bg_col, &fg_col, center, outer);
        g_free(txt);
    } else {
        dock_label_hide();
    }

    cairo_restore(cr);
    return FALSE;
}

static void update_dock_special_icon_masters(UltimateShell *shell, gint base_px) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    if (GTK_IS_IMAGE(g_dock_launchpad_img)) {
        const gchar *custom_lp = (shell && shell->dock_launchpad_icon && shell->dock_launchpad_icon[0] != '\0')
                               ? shell->dock_launchpad_icon : "applications-other";
        const gchar *loaded_lp = g_object_get_data(G_OBJECT(g_dock_launchpad_img), "loaded-icon-key");
        GdkPixbuf *m1 = g_object_get_data(G_OBJECT(g_dock_launchpad_img), "pb-master");
        if (!m1 || g_strcmp0(loaded_lp, custom_lp) != 0) {
            m1 = NULL;
            if (g_file_test(custom_lp, G_FILE_TEST_EXISTS)) {
                m1 = gdk_pixbuf_new_from_file_at_scale(custom_lp, 96, 96, TRUE, NULL);
            }
            if (!m1) {
                m1 = gtk_icon_theme_load_icon(theme, custom_lp, 96, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
            }
            if (!m1) {
                m1 = gtk_icon_theme_load_icon(theme, "applications-other", 96, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
            }
            if (m1) {
                g_object_set_data_full(G_OBJECT(g_dock_launchpad_img), "pb-master", m1, g_object_unref);
                g_object_set_data_full(G_OBJECT(g_dock_launchpad_img), "loaded-icon-key", g_strdup(custom_lp), g_free);
            }
        }
        if (m1) {
            GdkPixbuf *sc = gdk_pixbuf_scale_simple(m1, base_px, base_px, GDK_INTERP_BILINEAR);
            gtk_image_set_from_pixbuf(GTK_IMAGE(g_dock_launchpad_img), sc);
            g_object_unref(sc);
        }
    }
    if (GTK_IS_IMAGE(g_dock_trash_img)) {
        GdkPixbuf *m2 = g_object_get_data(G_OBJECT(g_dock_trash_img), "pb-master");
        if (!m2) {
            m2 = gtk_icon_theme_load_icon(theme, "user-trash", 96, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
            if (m2) g_object_set_data_full(G_OBJECT(g_dock_trash_img), "pb-master", m2, g_object_unref);
        }
        if (m2) {
            GdkPixbuf *sc = gdk_pixbuf_scale_simple(m2, base_px, base_px, GDK_INTERP_BILINEAR);
            gtk_image_set_from_pixbuf(GTK_IMAGE(g_dock_trash_img), sc);
            g_object_unref(sc);
        }
    }
}

static void sync_dock_button_alignment(UltimateShell *shell) {
    if (!shell) return;
    GtkAlign h_al = GTK_ALIGN_CENTER;
    GtkAlign v_al = GTK_ALIGN_END;

    if (shell->dock_pos == DOCK_POS_LEFT) {
        h_al = GTK_ALIGN_START;
        v_al = GTK_ALIGN_CENTER;
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        h_al = GTK_ALIGN_END;
        v_al = GTK_ALIGN_CENTER;
    }

    gint pad_v   = CLAMP(shell->dock_pad_v, 0, 24);
    gint pad_h   = CLAMP(shell->dock_pad_h, 0, 40);
    gint side_hr = get_plank_side_headroom(shell);
    gboolean fw  = (shell->dock_full_width && shell->dock_pos == DOCK_POS_BOTTOM);
    gint fw_mode = CLAMP(shell->dock_full_width_layout, 0, 3);

    gint lp_margin_start = (shell->dock_pos == DOCK_POS_BOTTOM) ? (fw ? MAX(14, pad_h) : side_hr) : 0;
    gint tr_margin_end   = (shell->dock_pos == DOCK_POS_BOTTOM) ? (fw ? MAX(14, pad_h) : side_hr) : 0;

    if (GTK_IS_WIDGET(g_dock_launchpad_btn)) {
        gtk_widget_set_halign(g_dock_launchpad_btn, h_al);
        gtk_widget_set_valign(g_dock_launchpad_btn, v_al);
        gtk_widget_set_margin_start(g_dock_launchpad_btn, (fw && (fw_mode == 0 || fw_mode == 1)) ? 2 : lp_margin_start);
        gtk_widget_set_margin_top(g_dock_launchpad_btn, (shell->dock_pos != DOCK_POS_BOTTOM) ? side_hr : 0);
        gtk_widget_set_margin_bottom(g_dock_launchpad_btn, (shell->dock_pos == DOCK_POS_BOTTOM) ? (pad_v + 4) : 0);
    }
    if (GTK_IS_WIDGET(shell->dock_trash_btn)) {
        gtk_widget_set_halign(shell->dock_trash_btn, h_al);
        gtk_widget_set_valign(shell->dock_trash_btn, v_al);
        gtk_widget_set_margin_start(shell->dock_trash_btn, (fw && fw_mode == 0) ? MAX(10, pad_h) : 2);
        gtk_widget_set_margin_end(shell->dock_trash_btn, (fw && fw_mode == 0) ? 2 : tr_margin_end);
        gtk_widget_set_margin_bottom(shell->dock_trash_btn, (shell->dock_pos == DOCK_POS_BOTTOM) ? (pad_v + 4) : side_hr);
    }
    if (GTK_IS_WIDGET(shell->taskbar_box)) {
        gtk_widget_set_halign(shell->taskbar_box, taskbar_dock_halign(shell));
        gtk_widget_set_valign(shell->taskbar_box, v_al);
        gtk_widget_set_margin_bottom(shell->taskbar_box, (shell->dock_pos == DOCK_POS_BOTTOM) ? pad_v : 0);
    }

    gboolean ubuntu = (shell->dock_style == 1 && shell->dock_pos != DOCK_POS_BOTTOM);
    if (ubuntu && GTK_IS_BOX(shell->dock_inner_box)) {
        /* Ubuntu dock: [apps + Trash] from the top ... [Show Apps] at the bottom */
        GtkWidget *ib = shell->dock_inner_box;
        GtkWidget *order[5] = { shell->taskbar_box, shell->dock_trash_btn, g_dock_spacer_left, g_dock_launchpad_btn, g_dock_spacer_right };
        gint pos = 0;
        for (gint k = 0; k < 5; k++) {
            if (!GTK_IS_WIDGET(order[k]) || gtk_widget_get_parent(order[k]) != ib) continue;
            gboolean grow = (order[k] == g_dock_spacer_left);
            guint pad = (order[k] == shell->dock_trash_btn || order[k] == g_dock_launchpad_btn) ? 2 : 0;
            gtk_box_set_child_packing(GTK_BOX(ib), order[k], grow, grow, pad, GTK_PACK_START);
            gtk_box_reorder_child(GTK_BOX(ib), order[k], pos++);
        }
        if (GTK_IS_WIDGET(shell->taskbar_box)) {
            gtk_widget_set_valign(shell->taskbar_box, GTK_ALIGN_START);
            gtk_widget_set_margin_top(shell->taskbar_box, 6);
        }
        if (GTK_IS_WIDGET(shell->dock_trash_btn)) {
            gtk_widget_set_valign(shell->dock_trash_btn, GTK_ALIGN_START);
            gtk_widget_set_margin_top(shell->dock_trash_btn, 2);
            gtk_widget_set_margin_bottom(shell->dock_trash_btn, 0);
        }
        if (GTK_IS_WIDGET(shell->dock_trash_btn)) gtk_widget_set_halign(shell->dock_trash_btn, GTK_ALIGN_FILL);
        if (GTK_IS_WIDGET(g_dock_launchpad_btn)) {
            gtk_widget_set_halign(g_dock_launchpad_btn, GTK_ALIGN_FILL);
            gtk_widget_set_valign(g_dock_launchpad_btn, GTK_ALIGN_END);
            gtk_widget_set_margin_top(g_dock_launchpad_btn, 0);
            gtk_widget_set_margin_bottom(g_dock_launchpad_btn, 8);
        }
        return;
    }
    if (GTK_IS_WIDGET(shell->taskbar_box)) gtk_widget_set_margin_top(shell->taskbar_box, 0);
    if (GTK_IS_BOX(shell->dock_inner_box)) {   /* back from the Ubuntu order: Trash packs from the end again */
        if (GTK_IS_WIDGET(shell->dock_trash_btn) && gtk_widget_get_parent(shell->dock_trash_btn) == shell->dock_inner_box)
            gtk_box_set_child_packing(GTK_BOX(shell->dock_inner_box), shell->dock_trash_btn, FALSE, FALSE, 2, GTK_PACK_END);
        if (GTK_IS_WIDGET(g_dock_launchpad_btn) && gtk_widget_get_parent(g_dock_launchpad_btn) == shell->dock_inner_box)
            gtk_box_set_child_packing(GTK_BOX(shell->dock_inner_box), g_dock_launchpad_btn, FALSE, FALSE, 2, GTK_PACK_START);
    }

    if (GTK_IS_BOX(shell->dock_inner_box)) {
        /* fw_mode 0: All Centered       -> spacer_L = TRUE,  spacer_R = TRUE
         * fw_mode 1: LP+Icons Center    -> spacer_L = TRUE,  spacer_R = TRUE
         * fw_mode 2: LP Left, Icons Ctr -> spacer_L = TRUE,  spacer_R = TRUE
         * fw_mode 3: LP+Icons Left      -> spacer_L = FALSE, spacer_R = TRUE
         */
        gboolean exp_left  = (fw && fw_mode != 3);
        gboolean exp_right = fw;
        GtkWidget *ib = shell->dock_inner_box;
        if (GTK_IS_WIDGET(g_dock_spacer_left) && gtk_widget_get_parent(g_dock_spacer_left) == ib) {
            gtk_box_set_child_packing(GTK_BOX(ib), g_dock_spacer_left,
                                      exp_left, exp_left, 0, GTK_PACK_START);
        }
        if (GTK_IS_WIDGET(g_dock_spacer_right) && gtk_widget_get_parent(g_dock_spacer_right) == ib) {
            gtk_box_set_child_packing(GTK_BOX(ib), g_dock_spacer_right,
                                      exp_right, exp_right, 0, GTK_PACK_END);
        }
        if (GTK_IS_WIDGET(shell->taskbar_box) && gtk_widget_get_parent(shell->taskbar_box) == ib) {
            gtk_box_set_child_packing(GTK_BOX(ib), shell->taskbar_box,
                                      FALSE, FALSE, 0, GTK_PACK_START);
        }

        if (fw && fw_mode == 2) {
            if (GTK_IS_WIDGET(g_dock_launchpad_btn) && gtk_widget_get_parent(g_dock_launchpad_btn) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), g_dock_launchpad_btn, 0);
            if (GTK_IS_WIDGET(g_dock_spacer_left) && gtk_widget_get_parent(g_dock_spacer_left) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), g_dock_spacer_left, 1);
        } else {
            if (GTK_IS_WIDGET(g_dock_spacer_left) && gtk_widget_get_parent(g_dock_spacer_left) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), g_dock_spacer_left, 0);
            if (GTK_IS_WIDGET(g_dock_launchpad_btn) && gtk_widget_get_parent(g_dock_launchpad_btn) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), g_dock_launchpad_btn, 1);
        }
        if (GTK_IS_WIDGET(shell->taskbar_box) && gtk_widget_get_parent(shell->taskbar_box) == ib) {
            gtk_box_reorder_child(GTK_BOX(ib), shell->taskbar_box, 2);
        }

        if (fw && fw_mode == 0) {
            if (GTK_IS_WIDGET(g_dock_spacer_right) && gtk_widget_get_parent(g_dock_spacer_right) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), g_dock_spacer_right, 0);
            if (GTK_IS_WIDGET(shell->dock_trash_btn) && gtk_widget_get_parent(shell->dock_trash_btn) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), shell->dock_trash_btn, 1);
        } else {
            if (GTK_IS_WIDGET(shell->dock_trash_btn) && gtk_widget_get_parent(shell->dock_trash_btn) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), shell->dock_trash_btn, 0);
            if (GTK_IS_WIDGET(g_dock_spacer_right) && gtk_widget_get_parent(g_dock_spacer_right) == ib)
                gtk_box_reorder_child(GTK_BOX(ib), g_dock_spacer_right, 1);
        }
    }
}

static void update_dock_x11_struts(UltimateShell *shell) {
    if (!shell || !shell->dock_window || !gtk_widget_get_realized(shell->dock_window)) return;
    GdkWindow *gdk_win = gtk_widget_get_window(shell->dock_window);
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!gdk_win || !GDK_IS_X11_DISPLAY(gdk_dpy)) return;

    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    Window xid = GDK_WINDOW_XID(gdk_win);
    Atom strut_atom = XInternAtom(dpy, "_NET_WM_STRUT", False);
    Atom strut_partial_atom = XInternAtom(dpy, "_NET_WM_STRUT_PARTIAL", False);

    gboolean reserve = (shell->mode == LAYOUT_MACOS &&
                        shell->dock_autohide == DOCK_HIDE_NEVER &&
                        gtk_widget_get_visible(shell->dock_window));

    if (!reserve) {
        gdk_x11_display_error_trap_push(gdk_dpy);
        XDeleteProperty(dpy, xid, strut_atom);
        XDeleteProperty(dpy, xid, strut_partial_atom);
        gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
        return;
    }

    GdkMonitor *monitor = gdk_display_get_primary_monitor(gdk_dpy);
    if (!monitor) monitor = gdk_display_get_monitor(gdk_dpy, 0);
    GdkRectangle geom;
    gdk_monitor_get_geometry(monitor, &geom);
    GdkScreen *scr = gdk_screen_get_default();
    gint root_w = scr ? gdk_screen_get_width(scr) : (geom.x + geom.width);
    gint root_h = scr ? gdk_screen_get_height(scr) : (geom.y + geom.height);

    gint shelf_thick = get_plank_shelf_thickness(shell);
    gint edge_px = shell->dock_full_width ? 0 : CLAMP(shell->dock_edge_dist, 0, 64);

    unsigned long strut[12] = { 0 };
    if (shell->dock_pos == DOCK_POS_LEFT) {
        strut[0] = (geom.x + shelf_thick + edge_px);
        strut[4] = geom.y;
        strut[5] = geom.y + geom.height - 1;
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        strut[1] = (root_w - (geom.x + geom.width)) + shelf_thick + edge_px;
        strut[6] = geom.y;
        strut[7] = geom.y + geom.height - 1;
    } else {
        strut[3] = (root_h - (geom.y + geom.height)) + shelf_thick + edge_px;
        strut[10] = geom.x;
        strut[11] = geom.x + geom.width - 1;
    }

    gdk_x11_display_error_trap_push(gdk_dpy);
    XChangeProperty(dpy, xid, strut_atom, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut, 4);
    XChangeProperty(dpy, xid, strut_partial_atom, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)strut, 12);
    gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
}

static void update_dock_input_shape(UltimateShell *shell) {
    if (!shell || !shell->dock_window || !gtk_widget_get_realized(shell->dock_window)) return;
    GdkWindow *gdk_win = gtk_widget_get_window(shell->dock_window);
    if (!gdk_win) return;

    gint ww = gtk_widget_get_allocated_width(shell->dock_window);
    gint wh = gtk_widget_get_allocated_height(shell->dock_window);
    if (ww <= 4 || wh <= 4) return;

    if (g_dock_should_hide && g_dock_hide_progress > 0.35) {
        cairo_region_t *empty_reg = cairo_region_create();
        gdk_window_input_shape_combine_region(gdk_win, empty_reg, 0, 0);
        cairo_region_destroy(empty_reg);
        return;
    }

    gint shelf_t = get_plank_shelf_thickness(shell);
    gint side_hr = get_plank_side_headroom(shell);
    /* Only grow the click area into the transparent zoom headroom when magnification
       is on. With it off nothing grows, and a full-height input area would swallow
       clicks on popups sitting just above the shelf (Start menu footer: resizer,
       power buttons) and block leave-notify, leaving the dock stuck "hovered". */
    gboolean zoomed = shell->dock_magnify &&
                      (g_dock_hovered || g_dock_zoom_progress > 0.01);

    cairo_rectangle_int_t rect;
    if (zoomed) {
        rect.x = 0;
        rect.y = 0;
        rect.width = ww;
        rect.height = wh;
    } else if (shell->dock_pos == DOCK_POS_LEFT) {
        rect.x = 0;
        rect.y = shell->dock_full_width ? 0 : MAX(0, side_hr - 12);
        rect.width = MIN(ww, shelf_t + 6);
        rect.height = MAX(24, wh - 2 * rect.y);
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        rect.width = MIN(ww, shelf_t + 6);
        rect.x = MAX(0, ww - rect.width);
        rect.y = shell->dock_full_width ? 0 : MAX(0, side_hr - 12);
        rect.height = MAX(24, wh - 2 * rect.y);
    } else {
        rect.height = MIN(wh, shelf_t + 6);
        rect.y = MAX(0, wh - rect.height);
        if (shell->dock_full_width) {
            rect.x = 0;
            rect.width = ww;
        } else {
            rect.x = MAX(0, side_hr - 12);
            rect.width = MAX(24, ww - 2 * rect.x);
        }
    }

    cairo_region_t *region = cairo_region_create_rectangle(&rect);
    gdk_window_input_shape_combine_region(gdk_win, region, 0, 0);
    cairo_region_destroy(region);
}

void dock_window_reposition(UltimateShell *shell) {
    if (!shell || !shell->dock_window || !gtk_widget_get_visible(shell->dock_window)) return;

    sync_dock_button_alignment(shell);

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);

    GdkRectangle geom;
    gdk_monitor_get_geometry(monitor, &geom);

    gint buf_thick = get_plank_buffer_thickness(shell);
    gint edge_px   = shell->dock_full_width ? 0 : CLAMP(shell->dock_edge_dist, 0, 64);
    gboolean vert_full = shell->dock_full_width && shell->dock_pos != DOCK_POS_BOTTOM;
    /* full height (Ubuntu dock): from under the top bar to the bottom of the screen */
    GdkRectangle wa = geom;
    gdk_monitor_get_workarea(monitor, &wa);
    gint full_top = MAX(geom.y, wa.y);
    gint full_h   = MAX(100, geom.y + geom.height - full_top);

    GtkRequisition req;
    gtk_widget_get_preferred_size(shell->dock_window, NULL, &req);
    if (req.width < 120) req.width = 320;
    if (req.height < buf_thick) req.height = buf_thick;

    gint x = 0, y = 0;

    if (shell->dock_pos == DOCK_POS_LEFT) {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(shell->dock_inner_box), GTK_ORIENTATION_VERTICAL);
        gtk_orientable_set_orientation(GTK_ORIENTABLE(shell->taskbar_box), GTK_ORIENTATION_VERTICAL);
        gtk_widget_set_size_request(shell->dock_inner_box, buf_thick, -1);
        gtk_widget_set_size_request(shell->dock_window, -1, vert_full ? full_h : -1);
        x = geom.x + edge_px;
        y = vert_full ? full_top : geom.y + (geom.height - req.height) / 2;
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(shell->dock_inner_box), GTK_ORIENTATION_VERTICAL);
        gtk_orientable_set_orientation(GTK_ORIENTABLE(shell->taskbar_box), GTK_ORIENTATION_VERTICAL);
        gtk_widget_set_size_request(shell->dock_inner_box, buf_thick, -1);
        gtk_widget_set_size_request(shell->dock_window, -1, vert_full ? full_h : -1);
        x = geom.x + geom.width - buf_thick - edge_px;
        y = vert_full ? full_top : geom.y + (geom.height - req.height) / 2;
    } else {
        gtk_orientable_set_orientation(GTK_ORIENTABLE(shell->dock_inner_box), GTK_ORIENTATION_HORIZONTAL);
        gtk_orientable_set_orientation(GTK_ORIENTABLE(shell->taskbar_box), GTK_ORIENTATION_HORIZONTAL);
        gtk_widget_set_size_request(shell->dock_inner_box, -1, buf_thick);
        if (shell->dock_full_width) {
            gtk_widget_set_size_request(shell->dock_window, geom.width, buf_thick);
            x = geom.x;
            y = geom.y + geom.height - buf_thick;
        } else {
            gtk_widget_set_size_request(shell->dock_window, -1, buf_thick);
            x = geom.x + (geom.width - req.width) / 2;
            y = geom.y + geom.height - buf_thick - edge_px;
        }
    }

    gtk_window_move(GTK_WINDOW(shell->dock_window), x, y);
    update_dock_x11_struts(shell);
    update_dock_input_shape(shell);
}

static gboolean dock_reposition_idle_cb(gpointer user_data) {
    UltimateShell *shell = (UltimateShell *)user_data;
    if (shell && shell->mode == LAYOUT_MACOS && shell->dock_window) {
        dock_window_reposition(shell);
    }
    return G_SOURCE_REMOVE;
}

static void on_dock_size_allocate(GtkWidget *widget, GdkRectangle *alloc, gpointer user_data) {
    static gint last_w = 0, last_h = 0;
    if (alloc->width != last_w || alloc->height != last_h) {
        last_w = alloc->width;
        last_h = alloc->height;
        dock_window_reposition((UltimateShell *)user_data);
    }
}

static gboolean dock_zoom_anim_tick(gpointer user_data) {
    UltimateShell *shell = (UltimateShell *)user_data;
    if (!shell || shell->mode != LAYOUT_MACOS || !shell->dock_window || !shell->dock_magnify) {
        g_dock_zoom_progress = 0.0;
        if (shell && shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
        g_dock_zoom_anim_id = 0;
        return G_SOURCE_REMOVE;
    }

    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *pointer = gdk_seat_get_pointer(seat);
    gint px = 0, py = 0;
    if (pointer) {
        gdk_device_get_position(pointer, NULL, &px, &py);
        g_dock_cursor_x = px;
        g_dock_cursor_y = py;
    }

    gint wx = 0, wy = 0, ww = 0, wh = 0;
    if (gtk_widget_get_window(shell->dock_window)) {
        gdk_window_get_origin(gtk_widget_get_window(shell->dock_window), &wx, &wy);
        ww = gtk_widget_get_allocated_width(shell->dock_window);
        wh = gtk_widget_get_allocated_height(shell->dock_window);
    }

    gint shelf_h   = get_plank_shelf_thickness(shell);
    gint side_hr   = get_plank_side_headroom(shell);
    gboolean inside_dock = FALSE;
    if (shell->dock_pos == DOCK_POS_LEFT) {
        inside_dock = (px >= wx - 8 && px <= wx + shelf_h + 18 &&
                       py >= wy + side_hr - 24 && py <= wy + wh - side_hr + 24);
    } else if (shell->dock_pos == DOCK_POS_RIGHT) {
        inside_dock = (px >= wx + ww - shelf_h - 18 && px <= wx + ww + 8 &&
                       py >= wy + side_hr - 24 && py <= wy + wh - side_hr + 24);
    } else {
        gint shelf_top = wy + wh - shelf_h - 14;
        gint x_pad = shell->dock_full_width ? 0 : (side_hr - 24);
        inside_dock = (px >= wx + x_pad && px <= wx + ww - x_pad &&
                       py >= shelf_top && py <= wy + wh + 8);
    }
    g_dock_hovered = inside_dock;
    update_dock_input_shape(shell);

    gdouble target = inside_dock ? 1.0 : 0.0;
    if (fabs(g_dock_zoom_progress - target) < 0.02) {
        g_dock_zoom_progress = target;
    } else {
        g_dock_zoom_progress += (target - g_dock_zoom_progress) * 0.26;
    }

    gtk_widget_queue_draw(shell->dock_inner_box);

    if (!inside_dock && g_dock_zoom_progress <= 0.005) {
        g_dock_zoom_progress = 0.0;
        update_dock_input_shape(shell);
        gtk_widget_queue_draw(shell->dock_inner_box);
        g_dock_zoom_anim_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void ensure_dock_zoom_animator(UltimateShell *shell) {
    if (shell && shell->mode == LAYOUT_MACOS && shell->dock_magnify && g_dock_zoom_anim_id == 0) {
        g_dock_zoom_anim_id = g_timeout_add(16, dock_zoom_anim_tick, shell);
    }
}

static gboolean on_dock_enter(GtkWidget *w, GdkEventCrossing *ev, gpointer data) {
    UltimateShell *shell = (UltimateShell *)data;
    g_dock_hovered = TRUE;
    update_dock_input_shape(shell);
    g_dock_cursor_x = ev->x_root;
    g_dock_cursor_y = ev->y_root;
    ensure_dock_zoom_animator(shell);
    return FALSE;
}

static gboolean on_dock_motion(GtkWidget *w, GdkEventMotion *ev, gpointer data) {
    UltimateShell *shell = (UltimateShell *)data;
    g_dock_hovered = TRUE;
    g_dock_cursor_x = ev->x_root;
    g_dock_cursor_y = ev->y_root;
    ensure_dock_zoom_animator(shell);
    if (shell && shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
    return FALSE;
}

static gboolean on_dock_leave(GtkWidget *w, GdkEventCrossing *ev, gpointer data) {
    UltimateShell *shell = (UltimateShell *)data;
    if (ev->detail != GDK_NOTIFY_INFERIOR) {
        g_dock_hovered = FALSE;
    }
    ensure_dock_zoom_animator(shell);
    return FALSE;
}

static gboolean dock_slide_anim_tick(gpointer user_data) {
    UltimateShell *shell = (UltimateShell *)user_data;
    if (!shell || shell->mode != LAYOUT_MACOS || !shell->dock_window) {
        g_dock_slide_anim_id = 0;
        return G_SOURCE_REMOVE;
    }

    gdouble target = g_dock_should_hide ? 1.0 : 0.0;
    gdouble step   = g_dock_should_hide ? 0.14 : 0.18;

    if (fabs(g_dock_hide_progress - target) <= 0.015) {
        g_dock_hide_progress = target;
        update_dock_input_shape(shell);
        if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
        gtk_widget_queue_draw(shell->dock_window);
        g_dock_slide_anim_id = 0;
        return G_SOURCE_REMOVE;
    }

    g_dock_hide_progress += (target - g_dock_hide_progress) * step;
    if (target > g_dock_hide_progress) g_dock_hide_progress = MIN(1.0, g_dock_hide_progress + 0.035);
    else                               g_dock_hide_progress = MAX(0.0, g_dock_hide_progress - 0.045);

    update_dock_input_shape(shell);
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
    gtk_widget_queue_draw(shell->dock_window);
    return G_SOURCE_CONTINUE;
}

static void start_dock_slide_animation(UltimateShell *shell, gboolean hide_now) {
    if (g_dock_should_hide != hide_now) {
        g_dock_should_hide = hide_now;
        if (hide_now) {
            g_dock_hovered = FALSE;
        }
    }
    if (g_dock_slide_anim_id == 0) {
        g_dock_slide_anim_id = g_timeout_add(16, dock_slide_anim_tick, shell);
    }
}

static gboolean is_any_shell_popup_open(UltimateShell *shell, gboolean *out_fs_launchpad) {
    if (out_fs_launchpad) *out_fs_launchpad = FALSE;
    /* Check if any top-level GTK window belonging to our process (Start Menu, Thumbnail Preview, Context Menu) is mapped & visible */
    GList *toplevels = gtk_window_list_toplevels();
    gboolean popup_open = FALSE;
    for (GList *l = toplevels; l != NULL; l = l->next) {
        GtkWidget *w = GTK_WIDGET(l->data);
        if (!gtk_widget_get_visible(w)) continue;
        if (shell && (w == shell->dock_window || w == GTK_WIDGET(shell->plugin))) continue;

        /* If the Fullscreen Launchpad is open, treat it like a maximized window, NOT a dock-locking popup! */
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "is-fullscreen-start")) == 1) {
            if (out_fs_launchpad) *out_fs_launchpad = TRUE;
            continue;
        }

        /* Any visible GtkMenu popup or compact shell popup window (Start Menu, App Preview) */
        const gchar *wname = gtk_widget_get_name(w);
        GtkStyleContext *ctx = gtk_widget_get_style_context(w);
        if (gtk_style_context_has_class(ctx, "ultimate-popup-win") ||
            gtk_style_context_has_class(ctx, "ultimate-popup-box") ||
            (wname && g_strcmp0(wname, "ultimate-popup-window") == 0) ||
            g_object_get_data(G_OBJECT(w), "is-start-popup") != NULL ||
            g_object_get_data(G_OBJECT(w), "is-ultimate-popup") != NULL) {
            popup_open = TRUE;
            break;
        }
        /* Also check GTK's internal tearoff/popup windows for active right-click GtkMenus */
        if (gtk_window_get_window_type(GTK_WINDOW(w)) == GTK_WINDOW_POPUP) {
            GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
            if (child && GTK_IS_MENU(child)) {
                popup_open = TRUE;
                break;
            }
        }
    }
    g_list_free(toplevels);
    return popup_open;
}

static gboolean dock_autohide_tick(gpointer user_data) {
    UltimateShell *shell = (UltimateShell *)user_data;
    if (shell->mode != LAYOUT_MACOS || !shell->dock_window) return G_SOURCE_CONTINUE;

    if (!shell->tracker || !shell->tracker->screen) return G_SOURCE_CONTINUE;
    WnckWindow *active_fs = wnck_screen_get_active_window(shell->tracker->screen);
    gboolean any_active_app_fs = FALSE;
    if (active_fs && wnck_window_get_window_type(active_fs) != WNCK_WINDOW_DESKTOP &&
        wnck_window_get_window_type(active_fs) != WNCK_WINDOW_DOCK) {
        if (wnck_window_is_fullscreen(active_fs)) {
            any_active_app_fs = TRUE;
        } else {
            WnckWindow *tr = wnck_window_get_transient(active_fs);
            if (tr && wnck_window_is_fullscreen(tr)) any_active_app_fs = TRUE;
            WnckApplication *f_app = wnck_window_get_application(active_fs);
            int f_pid = wnck_window_get_pid(active_fs);
            const gchar *f_cls = wnck_window_get_class_group_name(active_fs);
            WnckWorkspace *f_ws = wnck_screen_get_active_workspace(shell->tracker->screen);
            for (GList *l = wnck_screen_get_windows(shell->tracker->screen); l != NULL && !any_active_app_fs; l = l->next) {
                WnckWindow *w = WNCK_WINDOW(l->data);
                if (!w || wnck_window_is_minimized(w) || !wnck_window_is_fullscreen(w)) continue;
                if (f_ws && !wnck_window_is_on_workspace(w, f_ws)) continue;
                if ((f_app && wnck_window_get_application(w) == f_app) ||
                    (f_pid > 0 && wnck_window_get_pid(w) == f_pid) ||
                    (f_cls && f_cls[0] && g_ascii_strcasecmp(f_cls, wnck_window_get_class_group_name(w) ? wnck_window_get_class_group_name(w) : "") == 0)) {
                    any_active_app_fs = TRUE;
                }
            }
        }
    }
    if (any_active_app_fs) {
        if (gtk_widget_get_visible(shell->dock_window)) {
            gtk_widget_hide(shell->dock_window);
            update_dock_x11_struts(shell);
        }
        return G_SOURCE_CONTINUE;
    }

    if (!gtk_widget_get_visible(shell->dock_window)) {
        gtk_widget_show_all(shell->dock_window);
        if (!shell->dock_show_trash && shell->dock_trash_btn) gtk_widget_hide(shell->dock_trash_btn);
        dock_window_reposition(shell);
    }

    if (shell->dock_autohide == DOCK_HIDE_NEVER) {
        g_dock_hide_deadline_us = 0;
        if (g_dock_should_hide || g_dock_hide_progress > 0.0) {
            start_dock_slide_animation(shell, FALSE);
        }
        update_dock_x11_struts(shell);
        return G_SOURCE_CONTINUE;
    }

    GdkDisplay *display = gdk_display_get_default();
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *pointer = gdk_seat_get_pointer(seat);
    gint px = 0, py = 0;
    if (pointer) gdk_device_get_position(pointer, NULL, &px, &py);

    GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
    if (!monitor) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle geom;
    gdk_monitor_get_geometry(monitor, &geom);

    gboolean near_edge = FALSE;
    if (shell->dock_pos == DOCK_POS_BOTTOM && py >= geom.y + geom.height - 6) near_edge = TRUE;
    if (shell->dock_pos == DOCK_POS_LEFT   && px <= geom.x + 6)               near_edge = TRUE;
    if (shell->dock_pos == DOCK_POS_RIGHT  && px >= geom.x + geom.width - 6)  near_edge = TRUE;

    gboolean cursor_in_shelf = FALSE;
    if (!g_dock_should_hide && g_dock_hide_progress < 0.5 && gtk_widget_get_window(shell->dock_window)) {
        gint wx = 0, wy = 0;
        gdk_window_get_origin(gtk_widget_get_window(shell->dock_window), &wx, &wy);
        gint ww = gtk_widget_get_allocated_width(shell->dock_window);
        gint wh = gtk_widget_get_allocated_height(shell->dock_window);
        gint shelf_h = get_plank_shelf_thickness(shell);
        gint side_hr = get_plank_side_headroom(shell);

        if (shell->dock_pos == DOCK_POS_LEFT) {
            cursor_in_shelf = (px >= wx - 4 && px <= wx + shelf_h + 18 &&
                               py >= wy + side_hr - 24 && py <= wy + wh - side_hr + 24);
        } else if (shell->dock_pos == DOCK_POS_RIGHT) {
            cursor_in_shelf = (px >= wx + ww - shelf_h - 18 && px <= wx + ww + 4 &&
                               py >= wy + side_hr - 24 && py <= wy + wh - side_hr + 24);
        } else {
            gint shelf_top = wy + wh - shelf_h - 14;
            gint x_pad = shell->dock_full_width ? 0 : (side_hr - 24);
            cursor_in_shelf = (px >= wx + x_pad && px <= wx + ww - x_pad &&
                               py >= shelf_top && py <= wy + wh + 8);
        }
    }

    gboolean fs_launchpad_open = FALSE;
    gboolean popup_open = is_any_shell_popup_open(shell, &fs_launchpad_open);
    gboolean want_hide = FALSE;
    gboolean window_overlaps_dock = fs_launchpad_open;

    /* Check if the active window (or its parent app window when a dialog/prefs window is focused)
       is maximized or overlaps the dock zone */
    WnckWindow *active = wnck_screen_get_active_window(shell->tracker->screen);
    WnckWindowType atype = active ? wnck_window_get_window_type(active) : WNCK_WINDOW_DESKTOP;
    if (active &&
        (atype == WNCK_WINDOW_NORMAL || atype == WNCK_WINDOW_DIALOG || atype == WNCK_WINDOW_UTILITY) &&
        !wnck_window_is_minimized(active)) {
        const gchar *aname = wnck_window_get_name(active);
        const gchar *acls  = wnck_window_get_class_group_name(active);
        gboolean is_prefs_win = (aname && g_str_has_prefix(aname, "Ultimate Shell"));
        gboolean is_shell_win = !is_prefs_win &&
                                (acls && (g_ascii_strcasecmp(acls, "Wrapper-2.0") == 0 ||
                                          g_ascii_strcasecmp(acls, "xfce4-panel") == 0));
        if (!is_shell_win) {
            WnckApplication *a_app = wnck_window_get_application(active);
            int a_pid = wnck_window_get_pid(active);
            WnckWindow *a_trans = wnck_window_get_transient(active);
            WnckWorkspace *active_ws = wnck_screen_get_active_workspace(shell->tracker->screen);
            gint reserve_zone = get_plank_shelf_thickness(shell) + CLAMP(shell->dock_edge_dist, 0, 64) + 6;

            for (GList *l = wnck_screen_get_windows(shell->tracker->screen); l != NULL; l = l->next) {
                WnckWindow *w = WNCK_WINDOW(l->data);
                if (!w || wnck_window_is_minimized(w)) continue;
                if (active_ws && !wnck_window_is_on_workspace(w, active_ws)) continue;

                /* Only real app windows can overlap the dock. The dock itself (notification
                   type), panels (dock type) and other shell surfaces share a process/class
                   with the Ultimate Shell settings window and must never count. */
                WnckWindowType wtype = wnck_window_get_window_type(w);
                if (wtype != WNCK_WINDOW_NORMAL && wtype != WNCK_WINDOW_DIALOG &&
                    wtype != WNCK_WINDOW_UTILITY) continue;
                if (w != active) {
                    const gchar *wcls = wnck_window_get_class_group_name(w);
                    if (wcls && (g_ascii_strcasecmp(wcls, "Wrapper-2.0") == 0 ||
                                 g_ascii_strcasecmp(wcls, "xfce4-panel") == 0)) continue;
                }
                if (gtk_widget_get_window(shell->dock_window) &&
                    wnck_window_get_xid(w) == GDK_WINDOW_XID(gtk_widget_get_window(shell->dock_window))) continue;

                gboolean belongs_to_active = (w == active) || (w == a_trans) ||
                    (a_app && wnck_window_get_application(w) == a_app) ||
                    (a_pid > 0 && wnck_window_get_pid(w) == a_pid) ||
                    (acls && acls[0] != '\0' && wnck_window_get_class_group_name(w) &&
                     g_ascii_strcasecmp(acls, wnck_window_get_class_group_name(w)) == 0);

                if (!belongs_to_active) continue;

                if (wnck_window_is_maximized(w) || wnck_window_is_fullscreen(w)) {
                    window_overlaps_dock = TRUE;
                    break;
                } else {
                    gint wx, wy, ww, wh;
                    wnck_window_get_geometry(w, &wx, &wy, &ww, &wh);
                    if (shell->dock_pos == DOCK_POS_BOTTOM && (wy + wh >= geom.y + geom.height - reserve_zone)) { window_overlaps_dock = TRUE; break; }
                    if (shell->dock_pos == DOCK_POS_LEFT   && (wx <= geom.x + reserve_zone))                    { window_overlaps_dock = TRUE; break; }
                    if (shell->dock_pos == DOCK_POS_RIGHT  && (wx + ww >= geom.x + geom.width - reserve_zone))  { window_overlaps_dock = TRUE; break; }
                }
            }
        }
    }

    if (shell->dock_autohide == DOCK_HIDE_ALWAYS) {
        want_hide = !(near_edge || cursor_in_shelf || g_dock_hovered || popup_open);
    } else if (shell->dock_autohide == DOCK_HIDE_INTELLIGENT) {
        want_hide = window_overlaps_dock && !(near_edge || cursor_in_shelf || g_dock_hovered || popup_open);
    }

    gint64 now_us = g_get_monotonic_time();

    if (!want_hide) {
        g_dock_hide_deadline_us = 0;
        if (g_dock_should_hide || g_dock_hide_progress > 0.0) {
            start_dock_slide_animation(shell, FALSE);
        }
    } else {
        if (!g_dock_should_hide) {
            /* Use the snappy 380ms Intel-Hide delay whenever a window or Launchpad is maximized/overlapping,
               even when the dock is set to Auto-Hide (DOCK_HIDE_ALWAYS)! */
            gint64 delay_ms = (shell->dock_autohide == DOCK_HIDE_ALWAYS && !window_overlaps_dock)
                ? (CLAMP(shell->dock_hide_delay_sec > 0 ? shell->dock_hide_delay_sec : 10, 1, 60) * 1000LL)
                : 380LL;
            gint64 target_deadline = now_us + (delay_ms * G_TIME_SPAN_MILLISECOND);

            if (g_dock_hide_deadline_us == 0 || target_deadline < g_dock_hide_deadline_us) {
                g_dock_hide_deadline_us = target_deadline;
            }
            if (now_us >= g_dock_hide_deadline_us) {
                g_dock_hide_deadline_us = 0;
                start_dock_slide_animation(shell, TRUE);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

static gboolean on_launchpad_button_press(GtkWidget *btn, GdkEventButton *ev, UltimateShell *shell) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    start_menu_popup_quick_menu(btn, ev);
    return TRUE;
}

static void on_launchpad_clicked(GtkButton *btn, UltimateShell *shell) {
    start_menu_open_from_dock(shell, GTK_WIDGET(btn));
}

static void on_trash_clicked(GtkButton *btn, gpointer user_data) {
    g_spawn_command_line_async("exo-open --launch FileManager trash:///", NULL);
}

void dock_window_apply_settings(UltimateShell *shell) {
    if (!shell || !shell->dock_window) return;

    gint icon_sz = CLAMP(shell->dock_icon_size, 24, 64);
    update_dock_special_icon_masters(shell, icon_sz);
    sync_dock_button_alignment(shell);

    if (shell->dock_trash_btn) {
        if (shell->dock_show_trash) gtk_widget_show_all(shell->dock_trash_btn);
        else gtk_widget_hide(shell->dock_trash_btn);
    }

    taskbar_update_icon_sizes(shell);
    shell_apply_theme(shell);
    if (shell->dock_inner_box) gtk_widget_queue_draw(shell->dock_inner_box);
    dock_window_reposition(shell);
    g_idle_add(dock_reposition_idle_cb, shell);
}

static void on_dock_icon_theme_changed(GtkIconTheme *theme, UltimateShell *shell) {
    if (!shell || !GTK_IS_WIDGET(shell->dock_window)) return;
    GtkWidget *imgs[2] = { g_dock_launchpad_img, g_dock_trash_img };
    for (gint i = 0; i < 2; i++) {
        if (!GTK_IS_IMAGE(imgs[i])) continue;
        g_object_set_data(G_OBJECT(imgs[i]), "pb-master", NULL);
        g_object_set_data(G_OBJECT(imgs[i]), "loaded-icon-key", NULL);
    }
    update_dock_special_icon_masters(shell, CLAMP(shell->dock_icon_size, 24, 64));
    if (GTK_IS_WIDGET(shell->dock_window)) gtk_widget_queue_draw(shell->dock_window);
}

void dock_window_init(UltimateShell *shell) {
    shell->dock_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_widget_set_name(shell->dock_window, "ultimate-macos-dock");
    g_object_set_data(G_OBJECT(shell->dock_window), "ultimate-dock-shell", shell);
    gtk_window_set_decorated(GTK_WINDOW(shell->dock_window), FALSE);
    gtk_window_set_resizable(GTK_WINDOW(shell->dock_window), FALSE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(shell->dock_window), TRUE);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(shell->dock_window), TRUE);
    gtk_window_set_keep_above(GTK_WINDOW(shell->dock_window), TRUE);
    gtk_window_stick(GTK_WINDOW(shell->dock_window));
    gtk_window_set_accept_focus(GTK_WINDOW(shell->dock_window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(shell->dock_window), GDK_WINDOW_TYPE_HINT_NOTIFICATION);
    g_signal_connect(shell->dock_window, "draw", G_CALLBACK(on_draw_dock_toplevel_clear), NULL);
    g_signal_connect(shell->dock_window, "unmap", G_CALLBACK(on_dock_unmap), NULL);

    GdkScreen *screen = gtk_widget_get_screen(shell->dock_window);
    GdkVisual *visual = gdk_screen_get_rgba_visual(screen);
    if (visual) gtk_widget_set_visual(shell->dock_window, visual);
    gtk_widget_set_app_paintable(shell->dock_window, TRUE);

    shell->dock_inner_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_name(shell->dock_inner_box, "ultimate-dock-box");
    gtk_widget_set_app_paintable(shell->dock_inner_box, TRUE);
    g_signal_connect(shell->dock_inner_box, "draw", G_CALLBACK(on_draw_dock_shelf_bg), shell);
    gtk_container_add(GTK_CONTAINER(shell->dock_window), shell->dock_inner_box);

    g_dock_launchpad_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(g_dock_launchpad_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(g_dock_launchpad_btn), "taskbar-item-btn");
    g_dock_launchpad_img = gtk_image_new();
    g_signal_connect(g_dock_launchpad_img, "draw", G_CALLBACK(on_suppress_special_img_draw), NULL);
    gtk_container_add(GTK_CONTAINER(g_dock_launchpad_btn), g_dock_launchpad_img);
    g_signal_connect(g_dock_launchpad_btn, "clicked", G_CALLBACK(on_launchpad_clicked), shell);
    g_signal_connect(g_dock_launchpad_btn, "button-press-event", G_CALLBACK(on_launchpad_button_press), shell);
    g_dock_spacer_left = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    g_dock_spacer_right = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(shell->dock_inner_box), g_dock_spacer_left, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(shell->dock_inner_box), g_dock_launchpad_btn, FALSE, FALSE, 2);

    shell->dock_trash_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(shell->dock_trash_btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(shell->dock_trash_btn), "taskbar-item-btn");
    g_dock_trash_img = gtk_image_new();
    g_signal_connect(g_dock_trash_img, "draw", G_CALLBACK(on_suppress_special_img_draw), NULL);
    gtk_container_add(GTK_CONTAINER(shell->dock_trash_btn), g_dock_trash_img);
    g_signal_connect(shell->dock_trash_btn, "clicked", G_CALLBACK(on_trash_clicked), shell);
    gtk_box_pack_end(GTK_BOX(shell->dock_inner_box), shell->dock_trash_btn, FALSE, FALSE, 2);
    gtk_box_pack_end(GTK_BOX(shell->dock_inner_box), g_dock_spacer_right, FALSE, FALSE, 0);

    update_dock_special_icon_masters(shell, CLAMP(shell->dock_icon_size, 24, 64));
    sync_dock_button_alignment(shell);
    static gboolean theme_hooked = FALSE;
    if (!theme_hooked) {
        g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_dock_icon_theme_changed), shell);
        theme_hooked = TRUE;
    }

    gtk_widget_add_events(shell->dock_window, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(shell->dock_window, "enter-notify-event", G_CALLBACK(on_dock_enter), shell);
    g_signal_connect(shell->dock_window, "motion-notify-event", G_CALLBACK(on_dock_motion), shell);
    g_signal_connect(shell->dock_window, "leave-notify-event", G_CALLBACK(on_dock_leave), shell);
    g_signal_connect(shell->dock_window, "size-allocate", G_CALLBACK(on_dock_size_allocate), shell);

    if (g_dock_poll_id == 0) {
        g_dock_poll_id = g_timeout_add(60, dock_autohide_tick, shell);
    }
}

/* Stop all dock timers (called when the plugin is freed). */
void dock_window_shutdown(void) {
    if (g_dshadow) { gtk_widget_destroy(g_dshadow); g_dshadow = NULL; }
    if (g_dock_label_win) { gtk_widget_destroy(g_dock_label_win); g_dock_label_win = NULL; g_dock_label_lbl = NULL; }
    if (g_dock_poll_id)       { g_source_remove(g_dock_poll_id);       g_dock_poll_id = 0; }
    if (g_dock_zoom_anim_id)  { g_source_remove(g_dock_zoom_anim_id);  g_dock_zoom_anim_id = 0; }
    if (g_dock_slide_anim_id) { g_source_remove(g_dock_slide_anim_id); g_dock_slide_anim_id = 0; }
}
