/*
 * ultimate-accels-gtk-module
 *
 * A tiny GTK3 module, loaded into GTK apps through the "Gtk/Modules" xsettings list
 * (next to appmenu-gtk-module). appmenu-gtk-module exports each menu item's label and
 * action to the top bar but drops its keyboard shortcut. This module reads the app's own
 * menu bar (the one it would draw in the window), collects "label -> shortcut" pairs and
 * publishes them on the toplevel X11 window as the UTF-8 property _ULTIMATE_MENU_ACCELS:
 *
 *     Reload\t<Primary>r\n
 *     Show Hidden Files\t<Primary>h\n
 *
 * Labels are cleaned exactly like the Ultimate Shell global menu does (mnemonic '_' removed),
 * so the top bar can match them 1:1.
 */
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <string.h>

#define PROP_NAME      "_ULTIMATE_MENU_ACCELS"
#define RESCAN_SECONDS 4

static gchar *clean_label(const gchar *raw) {
    GString *out = g_string_new("");
    for (const gchar *p = raw ? raw : ""; *p; p++) {
        if (*p == '_') {
            if (p[1] == '_') { g_string_append_c(out, '_'); p++; }
            continue;
        }
        g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

static void collect_menu(GtkWidget *shell, GString *out, GHashTable *seen);

static void collect_item(GtkWidget *w, GString *out, GHashTable *seen) {
    if (!GTK_IS_MENU_ITEM(w) || GTK_IS_SEPARATOR_MENU_ITEM(w)) return;

    GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(w));
    if (sub) collect_menu(sub, out, seen);

    GtkWidget *child = gtk_bin_get_child(GTK_BIN(w));
    if (!GTK_IS_ACCEL_LABEL(child)) return;

    guint key = 0;
    GdkModifierType mods = 0;
    gtk_accel_label_refetch(GTK_ACCEL_LABEL(child));
    gtk_accel_label_get_accel(GTK_ACCEL_LABEL(child), &key, &mods);
    if (key == 0) return;

    const gchar *raw = gtk_menu_item_get_label(GTK_MENU_ITEM(w));
    if (!raw || !raw[0]) return;
    gchar *label = clean_label(raw);
    if (label[0] && !strchr(label, '\t') && !strchr(label, '\n') && !g_hash_table_contains(seen, label)) {
        gchar *accel = gtk_accelerator_name(key, mods);
        if (accel && accel[0]) {
            g_string_append_printf(out, "%s\t%s\n", label, accel);
            g_hash_table_add(seen, label);
            label = NULL;                       /* owned by the set now */
        }
        g_free(accel);
    }
    g_free(label);
}

static void collect_menu(GtkWidget *shell, GString *out, GHashTable *seen) {
    if (!GTK_IS_CONTAINER(shell)) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(shell));
    for (GList *l = kids; l; l = l->next) collect_item(GTK_WIDGET(l->data), out, seen);
    g_list_free(kids);
}

/* Find every GtkMenuBar inside the window (including ones the app keeps hidden) */
static void find_menubars(GtkWidget *w, gpointer data) {
    GString *out = ((gpointer *)data)[0];
    GHashTable *seen = ((gpointer *)data)[1];
    if (GTK_IS_MENU_BAR(w)) {
        collect_menu(w, out, seen);
        return;
    }
    if (GTK_IS_CONTAINER(w)) gtk_container_forall(GTK_CONTAINER(w), find_menubars, data);
}


/* GtkApplication apps (Mousepad, GNOME-style apps) build their menu bar from a GMenuModel.
   When the top bar shows the menus, GTK doesn't even create the in-window menu bar, so
   read the model itself and ask the application for each action's shortcut. */
static void collect_model(GtkApplication *app, GMenuModel *model, GString *out, GHashTable *seen, gint depth) {
    if (!model || depth > 8) return;
    gint n = g_menu_model_get_n_items(model);
    for (gint i = 0; i < n; i++) {
        GMenuModel *link;
        if ((link = g_menu_model_get_item_link(model, i, G_MENU_LINK_SECTION))) {
            collect_model(app, link, out, seen, depth + 1);
            g_object_unref(link);
        }
        if ((link = g_menu_model_get_item_link(model, i, G_MENU_LINK_SUBMENU))) {
            collect_model(app, link, out, seen, depth + 1);
            g_object_unref(link);
        }

        gchar *raw = NULL, *action = NULL, *accel_attr = NULL;
        if (!g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_LABEL, "s", &raw)) continue;
        g_menu_model_get_item_attribute(model, i, G_MENU_ATTRIBUTE_ACTION, "s", &action);
        g_menu_model_get_item_attribute(model, i, "accel", "s", &accel_attr);

        gchar *accel = NULL;
        if (accel_attr && accel_attr[0]) {
            accel = g_strdup(accel_attr);
        } else if (action && action[0]) {
            GVariant *target = g_menu_model_get_item_attribute_value(model, i, G_MENU_ATTRIBUTE_TARGET, NULL);
            gchar *detailed = g_action_print_detailed_name(action, target);
            gchar **accels = gtk_application_get_accels_for_action(app, detailed);
            if (accels && accels[0]) accel = g_strdup(accels[0]);
            g_strfreev(accels);
            g_free(detailed);
            if (target) g_variant_unref(target);
        }

        gchar *label = clean_label(raw);
        if (accel && label[0] && !strchr(label, '\t') && !strchr(label, '\n') &&
            !g_hash_table_contains(seen, label)) {
            g_string_append_printf(out, "%s\t%s\n", label, accel);
            g_hash_table_add(seen, label);
            label = NULL;
        }
        g_free(label); g_free(accel); g_free(raw); g_free(action); g_free(accel_attr);
    }
}

static void publish(GtkWidget *win) {
    GdkWindow *gw = gtk_widget_get_window(win);
    if (!gw || !GDK_IS_X11_WINDOW(gw)) return;

    GString *out = g_string_new("");
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpointer data[2] = { out, seen };
    gtk_container_forall(GTK_CONTAINER(win), find_menubars, data);
    GtkApplication *app = gtk_window_get_application(GTK_WINDOW(win));
    if (app) {
        collect_model(app, gtk_application_get_menubar(app), out, seen, 0);
        collect_model(app, gtk_application_get_app_menu(app), out, seen, 0);
    }
    g_hash_table_destroy(seen);

    /* Only touch the X server when something changed */
    const gchar *prev = g_object_get_data(G_OBJECT(win), "ult-accels-last");
    if (g_strcmp0(prev ? prev : "", out->str) != 0) {
        GdkDisplay *disp = gdk_window_get_display(gw);
        Display *dpy = GDK_DISPLAY_XDISPLAY(disp);
        Atom prop = gdk_x11_get_xatom_by_name_for_display(disp, PROP_NAME);
        Atom utf8 = gdk_x11_get_xatom_by_name_for_display(disp, "UTF8_STRING");
        gdk_x11_display_error_trap_push(disp);
        if (out->len > 0) {
            XChangeProperty(dpy, GDK_WINDOW_XID(gw), prop, utf8, 8, PropModeReplace,
                            (const unsigned char *)out->str, (int)out->len);
        } else {
            XDeleteProperty(dpy, GDK_WINDOW_XID(gw), prop);
        }
        gdk_x11_display_error_trap_pop_ignored(disp);
        g_object_set_data_full(G_OBJECT(win), "ult-accels-last", g_strdup(out->str), g_free);
    }
    g_string_free(out, TRUE);
}

static gboolean on_rescan(gpointer user_data) {
    GtkWidget *win = GTK_WIDGET(user_data);
    if (!gtk_widget_get_realized(win)) return G_SOURCE_CONTINUE;
    publish(win);
    return G_SOURCE_CONTINUE;
}

static void on_window_destroy(GtkWidget *win, gpointer data) {
    guint id = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(win), "ult-accels-timer"));
    if (id) g_source_remove(id);
    g_object_set_data(G_OBJECT(win), "ult-accels-timer", NULL);
}

static gboolean on_first_scan(gpointer user_data) {
    GtkWidget *win = GTK_WIDGET(user_data);
    if (gtk_widget_get_realized(win)) publish(win);
    g_object_unref(win);
    return G_SOURCE_REMOVE;
}

/* Every toplevel window that gets mapped: publish now-ish, then keep it fresh
   (menus can change: recent files, plugins, user-edited shortcuts) */
static gboolean on_map_hook(GSignalInvocationHint *hint, guint n, const GValue *params, gpointer data) {
    GObject *obj = g_value_get_object(&params[0]);
    if (!GTK_IS_WINDOW(obj)) return TRUE;
    GtkWidget *win = GTK_WIDGET(obj);
    if (gtk_window_get_window_type(GTK_WINDOW(win)) != GTK_WINDOW_TOPLEVEL) return TRUE;
    if (g_object_get_data(obj, "ult-accels-timer")) return TRUE;

    g_idle_add(on_first_scan, g_object_ref(win));   /* right after the window is up */
    guint id = g_timeout_add_seconds(RESCAN_SECONDS, on_rescan, win);
    g_object_set_data(obj, "ult-accels-timer", GUINT_TO_POINTER(id));
    g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), NULL);
    return TRUE;
}

G_MODULE_EXPORT void gtk_module_init(gint *argc, gchar ***argv) {
    GdkDisplay *disp = gdk_display_get_default();
    if (disp && !GDK_IS_X11_DISPLAY(disp)) return;        /* X11 only */
    gpointer klass = g_type_class_ref(GTK_TYPE_WIDGET);
    guint sig = g_signal_lookup("map", GTK_TYPE_WIDGET);
    if (sig) g_signal_add_emission_hook(sig, 0, on_map_hook, NULL, NULL);
    g_type_class_unref(klass);
}
