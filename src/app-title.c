/* GNOME mode: the focused app's icon and name in the top bar, with a small app menu
   (New Window, Quit). Hidden on the desktop, like GNOME. */
#include "app-title.h"
#include "global-menu.h"
#include "taskbar.h"

typedef struct {
    UltimateShell *shell;
    GtkWidget     *btn, *icon, *lbl;
    WnckWindow    *win;          /* the app window shown (not owned) */
    gulong         name_id, icon_id;
} AppTitle;

static AppTitle *g_at = NULL;

static GAppInfo *at_app_info(WnckWindow *w) {
    if (!w) return NULL;
    const gchar *cls = wnck_window_get_class_group_name(w);
    const gchar *inst = wnck_window_get_class_instance_name(w);
    GAppInfo *ai = (cls && cls[0]) ? taskbar_find_app_info(cls) : NULL;
    if (!ai && inst && inst[0]) ai = taskbar_find_app_info(inst);
    return ai;
}

static void at_disconnect(AppTitle *a) {
    if (a->win) {
        if (a->name_id) g_signal_handler_disconnect(a->win, a->name_id);
        if (a->icon_id) g_signal_handler_disconnect(a->win, a->icon_id);
        g_object_remove_weak_pointer(G_OBJECT(a->win), (gpointer *)&a->win);
    }
    a->win = NULL; a->name_id = a->icon_id = 0;
}

static void at_refresh(AppTitle *a) {
    if (!a || !GTK_IS_WIDGET(a->btn)) return;
    gboolean on = a->shell && a->shell->mode == LAYOUT_MACOS && a->shell->gnome_style;
    WnckWindow *w = on ? global_menu_active_app_window() : NULL;
    if (!w) { gtk_widget_hide(a->btn); return; }

    const gchar *name = global_menu_app_name(w);
    gchar *mk = g_markup_printf_escaped("<b>%s</b>", name ? name : "");
    gtk_label_set_markup(GTK_LABEL(a->lbl), mk);
    g_free(mk);
    const gchar *title = wnck_window_get_name(w);
    gtk_widget_set_tooltip_text(a->btn, title && title[0] ? title : name);

    gint px = 16;
    GAppInfo *ai = at_app_info(w);
    GIcon *gi = ai ? g_app_info_get_icon(ai) : NULL;
    if (gi) {
        gtk_image_set_from_gicon(GTK_IMAGE(a->icon), gi, GTK_ICON_SIZE_MENU);
        gtk_image_set_pixel_size(GTK_IMAGE(a->icon), px);
    } else {
        GdkPixbuf *pb = wnck_window_get_icon(w);
        if (pb) {
            GdkPixbuf *sc = gdk_pixbuf_scale_simple(pb, px, px, GDK_INTERP_BILINEAR);
            gtk_image_set_from_pixbuf(GTK_IMAGE(a->icon), sc);
            if (sc) g_object_unref(sc);
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(a->icon), "application-x-executable", GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(a->icon), px);
        }
    }
    if (ai) g_object_unref(ai);
    /* show_all() stops at a no-show-all widget, so show the button and its contents directly */
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(a->btn)));
    gtk_widget_show(a->btn);
}

static void on_at_win_changed(WnckWindow *w, AppTitle *a) { at_refresh(a); }

static void on_at_active_changed(WnckScreen *s, WnckWindow *prev, AppTitle *a) {
    WnckWindow *w = global_menu_active_app_window();
    if (w != a->win) {
        at_disconnect(a);
        if (w) {
            a->win = w;
            g_object_add_weak_pointer(G_OBJECT(w), (gpointer *)&a->win);
            a->name_id = g_signal_connect(w, "name-changed", G_CALLBACK(on_at_win_changed), a);
            a->icon_id = g_signal_connect(w, "icon-changed", G_CALLBACK(on_at_win_changed), a);
        }
    }
    at_refresh(a);
}

/* ---- the app menu ---- */
static void on_at_new_window(GtkMenuItem *mi, AppTitle *a) {
    GAppInfo *ai = at_app_info(a->win);
    if (ai) { shell_launch_app_info(ai); g_object_unref(ai); }
}
static void on_at_quit(GtkMenuItem *mi, AppTitle *a) { global_menu_quit_app(a->win); }

static void on_at_clicked(GtkButton *b, AppTitle *a) {
    if (!a->win) return;
    const gchar *name = global_menu_app_name(a->win);
    GtkWidget *menu = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
    GAppInfo *ai = at_app_info(a->win);
    if (ai) {
        GtkWidget *nw = gtk_menu_item_new_with_label("New Window");
        g_signal_connect(nw, "activate", G_CALLBACK(on_at_new_window), a);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), nw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        g_object_unref(ai);
    }
    gchar *lq = g_strdup_printf("Quit %s", name ? name : "");
    GtkWidget *q = gtk_menu_item_new_with_label(lq);
    g_free(lq);
    g_signal_connect(q, "activate", G_CALLBACK(on_at_quit), a);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), q);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b), GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static void on_at_destroy(GtkWidget *w, AppTitle *a) {
    at_disconnect(a);
    if (a->shell && a->shell->tracker && a->shell->tracker->screen)
        g_signal_handlers_disconnect_by_data(a->shell->tracker->screen, a);
    if (g_at == a) g_at = NULL;
    g_free(a);
}

GtkWidget *app_title_new(UltimateShell *shell) {
    AppTitle *a = g_new0(AppTitle, 1);
    a->shell = shell;
    a->btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(a->btn), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(a->btn), "ultimate-module-btn");
    gtk_widget_set_name(a->btn, "ultimate-app-title");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    a->icon = gtk_image_new();
    a->lbl = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(a->lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(a->lbl), 28);
    gtk_box_pack_start(GTK_BOX(box), a->icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), a->lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(a->btn), box);
    gtk_widget_set_no_show_all(a->btn, TRUE);    /* only shows with a focused app */
    g_signal_connect(a->btn, "clicked", G_CALLBACK(on_at_clicked), a);
    g_signal_connect(a->btn, "destroy", G_CALLBACK(on_at_destroy), a);
    if (shell && shell->tracker && shell->tracker->screen)
        g_signal_connect(shell->tracker->screen, "active-window-changed", G_CALLBACK(on_at_active_changed), a);
    g_at = a;
    return a->btn;
}

void app_title_refresh(void) {
    if (g_at) on_at_active_changed(NULL, NULL, g_at);
}
