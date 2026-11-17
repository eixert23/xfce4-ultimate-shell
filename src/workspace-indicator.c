/* GNOME-style workspace indicator: a row of dots, the active workspace drawn
   as a wider pill that slides smoothly. Click a dot to switch, scroll to move,
   right-click for workspace actions. */
#include "workspace-indicator.h"
#include <math.h>
#include <libwnck/libwnck.h>

#define WS_MAX 32

typedef struct {
    UltimateShell *shell;
    WnckScreen    *screen;
    GtkWidget     *ebox;
    GtkWidget     *area;
    gdouble        cur_w[WS_MAX];
    gdouble        x0[WS_MAX], x1[WS_MAX];
    gint           drawn;
    guint          anim_id;
    gboolean       hover;
    gdouble        scroll_acc;
    gint64         last_scroll_us;
} WsInd;

static WsInd *g_ws = NULL;

static gint ws_count(WsInd *i) {
    return i->screen ? MIN(WS_MAX, wnck_screen_get_workspace_count(i->screen)) : 1;
}

static gint ws_active(WsInd *i) {
    WnckWorkspace *a = i->screen ? wnck_screen_get_active_workspace(i->screen) : NULL;
    return a ? wnck_workspace_get_number(a) : 0;
}

static gdouble ws_dot(WsInd *i) { return CLAMP(i->shell->ws_size, 4, 14); }

static gdouble ws_target_w(WsInd *i, gint k) {
    gdouble d = ws_dot(i);
    switch (i->shell->ws_style) {
    case 1:  return d;                                        /* dots */
    case 2:  return d * 2.6;                                  /* numbers */
    default: return (k == ws_active(i)) ? d * 3.6 : d;        /* GNOME pills */
    }
}

static void ws_update_size(WsInd *i) {
    gint n = ws_count(i);
    gdouble d = ws_dot(i), total = d * 2.4;                  /* side padding */
    for (gint k = 0; k < n; k++) total += ws_target_w(i, k) + (k ? d * 0.9 : 0);
    gtk_widget_set_size_request(i->area, (gint)ceil(total), -1);
}

static gboolean ws_anim_tick(gpointer data) {
    WsInd *i = data;
    gint n = ws_count(i);
    gboolean moving = FALSE;
    for (gint k = 0; k < n; k++) {
        gdouble t = ws_target_w(i, k);
        if (i->cur_w[k] <= 0) i->cur_w[k] = t;
        gdouble diff = t - i->cur_w[k];
        if (fabs(diff) > 0.3 && shell_anim_ms(100) > 0) { i->cur_w[k] += diff * 0.28; moving = TRUE; }
        else i->cur_w[k] = t;
    }
    gtk_widget_queue_draw(i->area);
    if (!moving) { i->anim_id = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

static void ws_refresh(WsInd *i) {
    if (!i) return;
    ws_update_size(i);
    if (!i->anim_id) i->anim_id = g_timeout_add(16, ws_anim_tick, i);
}

static void rounded(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r) {
    r = MIN(r, MIN(w, h) / 2.0);
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static gboolean ws_draw(GtkWidget *w, cairo_t *cr, WsInd *i) {
    gint W = gtk_widget_get_allocated_width(w), H = gtk_widget_get_allocated_height(w);
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    GdkRGBA fg;
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg);

    gint n = ws_count(i), act = ws_active(i);
    gdouble d = ws_dot(i), gap = d * 0.9;
    gdouble total = 0;
    for (gint k = 0; k < n; k++) {
        if (i->cur_w[k] <= 0) i->cur_w[k] = ws_target_w(i, k);
        total += i->cur_w[k] + (k ? gap : 0);
    }

    /* GNOME-like rounded hover background */
    gdouble bh = MIN((gdouble)H - 4, d * 3.2);
    if (i->hover) {
        rounded(cr, (W - total) / 2.0 - d * 1.1, (H - bh) / 2.0, total + d * 2.2, bh, bh / 2.0);
        cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.12);
        cairo_fill(cr);
    }

    gdouble x = (W - total) / 2.0, cy = H / 2.0;
    for (gint k = 0; k < n; k++) {
        gdouble cw = i->cur_w[k];
        i->x0[k] = x - gap / 2; i->x1[k] = x + cw + gap / 2;
        gboolean on = (k == act);
        if (i->shell->ws_style == 2) {
            gdouble ch = d * 2.2;
            if (on) {
                rounded(cr, x, cy - ch / 2, cw, ch, ch / 3);
                cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, 0.22);
                cairo_fill(cr);
            }
            gchar num[8];
            g_snprintf(num, sizeof num, "%d", k + 1);
            cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL,
                                   on ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(cr, MAX(9.0, d * 1.45));
            cairo_text_extents_t e;
            cairo_text_extents(cr, num, &e);
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, on ? 1.0 : 0.6);
            cairo_move_to(cr, x + (cw - e.width) / 2 - e.x_bearing, cy - e.height / 2 - e.y_bearing);
            cairo_show_text(cr, num);
        } else {
            rounded(cr, x, cy - d / 2, cw, d, d / 2);
            cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, on ? 1.0 : 0.45);
            cairo_fill(cr);
        }
        x += cw + gap;
    }
    i->drawn = n;
    return FALSE;
}

static gint ws_hit(WsInd *i, gdouble x) {
    for (gint k = 0; k < i->drawn; k++)
        if (x >= i->x0[k] && x < i->x1[k]) return k;
    return -1;
}

static void ws_go(WsInd *i, gint k, guint32 t) {
    WnckWorkspace *ws = wnck_screen_get_workspace(i->screen, k);
    if (ws) wnck_workspace_activate(ws, t ? t : gtk_get_current_event_time());
}

static void ws_step(WsInd *i, gint dir, guint32 t) {
    gint n = ws_count(i), k = ws_active(i) + dir;
    if (k < 0 || k >= n) {
        if (!i->shell->ws_wrap) return;
        k = (k + n) % n;
    }
    ws_go(i, k, t);
}

/* ---- right-click menu ---- */
static void on_ws_menu_go(GtkMenuItem *mi, WsInd *i) {
    ws_go(i, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "ws-idx")), 0);
}
static void on_ws_add(GtkMenuItem *mi, WsInd *i)    { workspace_indicator_set_count(ws_count(i) + 1); }
static void on_ws_remove(GtkMenuItem *mi, WsInd *i) { workspace_indicator_set_count(ws_count(i) - 1); }
static void on_ws_xfwm(GtkMenuItem *mi, gpointer d) {
    g_spawn_command_line_async("xfwm4-workspace-settings", NULL);
}
static void on_ws_shell_settings(GtkMenuItem *mi, WsInd *i) {
    if (i->shell->plugin) g_signal_emit_by_name(i->shell->plugin, "configure-plugin");
}

static void ws_popup_menu(WsInd *i, GdkEventButton *ev) {
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
    gint n = ws_count(i), act = ws_active(i);
    for (gint k = 0; k < n; k++) {
        WnckWorkspace *ws = wnck_screen_get_workspace(i->screen, k);
        gchar *lbl = g_strdup_printf("%s%s", k == act ? "● " : "", ws ? wnck_workspace_get_name(ws) : "Workspace");
        GtkWidget *it = gtk_menu_item_new_with_label(lbl);
        g_free(lbl);
        g_object_set_data(G_OBJECT(it), "ws-idx", GINT_TO_POINTER(k));
        g_signal_connect(it, "activate", G_CALLBACK(on_ws_menu_go), i);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *add = gtk_menu_item_new_with_label("Add Workspace");
    g_signal_connect(add, "activate", G_CALLBACK(on_ws_add), i);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), add);
    if (n > 1) {
        GtkWidget *rm = gtk_menu_item_new_with_label("Remove Last Workspace");
        g_signal_connect(rm, "activate", G_CALLBACK(on_ws_remove), i);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), rm);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *xs = gtk_menu_item_new_with_label("Workspace Names…");
    g_signal_connect(xs, "activate", G_CALLBACK(on_ws_xfwm), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), xs);
    GtkWidget *ss = gtk_menu_item_new_with_label("Workspace Indicator Settings…");
    g_signal_connect(ss, "activate", G_CALLBACK(on_ws_shell_settings), i);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), ss);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    shell_popup_menu_at_button(GTK_MENU(menu), i->ebox, (GdkEvent *)ev);
}

/* ---- input ---- */
static gboolean on_ws_press(GtkWidget *w, GdkEventButton *ev, WsInd *i) {
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    if (ev->button == 3) { ws_popup_menu(i, ev); return TRUE; }
    if (ev->button == 1) {
        gint wx = 0, wy = 0;
        gtk_widget_translate_coordinates(w, i->area, (gint)ev->x, (gint)ev->y, &wx, &wy);
        gint k = ws_hit(i, wx);
        if (k < 0) ws_step(i, +1, ev->time);         /* click beside the dots: next workspace */
        else if (k != ws_active(i)) ws_go(i, k, ev->time);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_ws_scroll(GtkWidget *w, GdkEventScroll *ev, WsInd *i) {
    if (!i->shell->ws_scroll) return FALSE;
    gint dir = 0;
    if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT) dir = -1;
    else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT) dir = +1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        i->scroll_acc += (fabs(ev->delta_y) > fabs(ev->delta_x)) ? ev->delta_y : ev->delta_x;
        if (i->scroll_acc >= 1.0)       { dir = +1; i->scroll_acc = 0; }
        else if (i->scroll_acc <= -1.0) { dir = -1; i->scroll_acc = 0; }
    }
    if (!dir) return TRUE;
    gint64 now = g_get_monotonic_time();
    if (now - i->last_scroll_us < 160000) return TRUE;      /* one step per notch */
    i->last_scroll_us = now;
    ws_step(i, dir, ev->time);
    return TRUE;
}

static gboolean on_ws_crossing(GtkWidget *w, GdkEventCrossing *ev, WsInd *i) {
    if (ev->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    i->hover = (ev->type == GDK_ENTER_NOTIFY);
    gtk_widget_queue_draw(i->area);
    return FALSE;
}

static gboolean on_ws_tooltip(GtkWidget *w, gint x, gint y, gboolean kb, GtkTooltip *tip, WsInd *i) {
    gint ax = x, ay = y;
    gtk_widget_translate_coordinates(w, i->area, x, y, &ax, &ay);
    gint k = ws_hit(i, ax);
    if (k < 0) k = ws_active(i);
    WnckWorkspace *ws = wnck_screen_get_workspace(i->screen, k);
    if (!ws) return FALSE;
    gtk_tooltip_set_text(tip, wnck_workspace_get_name(ws));
    return TRUE;
}

static void ws_update_visibility(WsInd *i) {
    UltimateShell *sh = i->shell;
    gboolean show = sh->ws_enabled && !(sh->ws_hide_single && ws_count(i) <= 1);
    gtk_widget_set_visible(i->ebox, show);
    if (show) gtk_widget_show(i->area);
}

static void on_ws_changed(WnckScreen *s, gpointer arg, WsInd *i) {
    ws_refresh(i);                 /* the pill slides to the new workspace */
    ws_update_visibility(i);       /* hide-when-single may change */
}

static void on_ws_destroy(GtkWidget *w, WsInd *i) {
    if (i->anim_id) g_source_remove(i->anim_id);
    if (i->screen) g_signal_handlers_disconnect_by_data(i->screen, i);
    if (g_ws == i) g_ws = NULL;
    g_free(i);
}

GtkWidget *workspace_indicator_new(UltimateShell *shell) {
    WsInd *i = g_new0(WsInd, 1);
    g_ws = i;
    i->shell = shell;
    i->screen = shell->tracker ? shell->tracker->screen : NULL;   /* same WnckHandle as the rest of the shell */
    if (i->screen) wnck_screen_force_update(i->screen);

    i->ebox = gtk_event_box_new();
    gtk_widget_set_name(i->ebox, "ultimate-workspace-indicator");
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(i->ebox), FALSE);
    /* The drawing area has its own window, which swallowed scroll (and click) events
       before they reached the event box. Put the event box's input window on top. */
    gtk_event_box_set_above_child(GTK_EVENT_BOX(i->ebox), TRUE);
    gtk_widget_add_events(i->ebox, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                                   GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    i->area = gtk_drawing_area_new();
    gtk_widget_set_has_tooltip(i->ebox, TRUE);
    gtk_container_add(GTK_CONTAINER(i->ebox), i->area);

    g_signal_connect(i->area, "draw", G_CALLBACK(ws_draw), i);
    g_signal_connect(i->ebox, "query-tooltip", G_CALLBACK(on_ws_tooltip), i);
    g_signal_connect(i->ebox, "button-press-event", G_CALLBACK(on_ws_press), i);
    g_signal_connect(i->ebox, "scroll-event", G_CALLBACK(on_ws_scroll), i);
    g_signal_connect(i->ebox, "enter-notify-event", G_CALLBACK(on_ws_crossing), i);
    g_signal_connect(i->ebox, "leave-notify-event", G_CALLBACK(on_ws_crossing), i);
    g_signal_connect(i->ebox, "destroy", G_CALLBACK(on_ws_destroy), i);

    if (i->screen) {
        g_signal_connect(i->screen, "active-workspace-changed", G_CALLBACK(on_ws_changed), i);
        g_signal_connect(i->screen, "workspace-created",        G_CALLBACK(on_ws_changed), i);
        g_signal_connect(i->screen, "workspace-destroyed",      G_CALLBACK(on_ws_changed), i);
    }
    ws_update_size(i);
    return i->ebox;
}

void workspace_indicator_apply_settings(UltimateShell *shell) {
    WsInd *i = g_ws;
    if (!i || !shell) return;
    ws_update_visibility(i);
    for (gint k = 0; k < WS_MAX; k++) i->cur_w[k] = 0;   /* re-measure on style/size change */
    ws_refresh(i);
}

gint workspace_indicator_get_count(void) {
    WnckScreen *s = g_ws ? g_ws->screen : NULL;
    if (!s) return 1;
    wnck_screen_force_update(s);
    return wnck_screen_get_workspace_count(s);
}

void workspace_indicator_set_count(gint n) {
    WnckScreen *s = g_ws ? g_ws->screen : NULL;
    if (s && n >= 1 && n <= 16) wnck_screen_change_workspace_count(s, n);
}
