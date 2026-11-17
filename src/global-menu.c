#include "global-menu.h"
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * ULTIMATE SHELL GLOBAL MENU — BUILT FROM SCRATCH
 * Pure GDBus + Native GtkMenuBar/GtkMenu Engine (Zero wrapper libraries)
 *
 * 1. Pre-builds complete GtkMenu submenus when window focus changes so
 *    GTK3 always calculates full popup height (ZERO scroll arrows).
 * 2. Sends direct D-Bus "Event(id, 'clicked', ...)" for DBusMenu apps
 *    (Qt, Electron, GTK+appmenu-gtk-module) and direct D-Bus
 *    "org.gtk.Actions.Activate(...)" for native GMenuModel GTK3 apps.
 * ========================================================================= */

#define APPMENU_CACHE_FILE "/tmp/ultimate-shell-appmenu.cache"

typedef struct {
    gchar *bus_name;
    gchar *object_path;
} RegisteredMenu;

typedef struct {
    UltimateWindowTracker *tracker;
    GtkWidget             *holder_box;
    GtkWidget             *app_name_eb;
    GtkWidget             *app_name_lbl;
    GtkWidget             *menubar;
    WnckWindow            *tracked_active;
    gulong                 name_sig_id;

    GHashTable            *registry;
    guint                  owner_id;
    guint                  reg_obj_id;
    guint                  dbusmenu_sig_sub_id;
    GDBusNodeInfo         *introspection_data;
    GDBusConnection       *session_bus;
    GDBusProxy            *ext_registrar_proxy;

    guint                  active_xid;
    gchar                 *active_bus;
    gchar                 *active_path;
    gboolean               active_is_gtk_menus;
    guint32                active_revision;
    gint                   open_menus_count;
    gboolean               reload_pending;
    guint                  reload_timer_id;
    guint                  retry_timer_id;
    gint                   retry_count;
    gboolean               registrar_enabled;
    gboolean               menubar_is_fallback;   /* app shares no menus: Window/Help-only bar */
    guint                  fallback_xid;
    guint                  hotkey_code;           /* Ctrl+Alt+M, 0 if not grabbed */
    GHashTable            *accel_map;             /* label -> "<Primary>r" from ultimate-accels-gtk-module */
    gulong                 watched_xid;           /* window we listen to for _ULTIMATE_MENU_ACCELS */
    guint                  accel_refresh_id;
    gboolean               accel_filter_added;
} GlobalMenuWidget;

static const gchar *app_display_name_for_window(WnckWindow *win);
static void gm_refresh_published_accels(GlobalMenuWidget *gm);

/* ---- Optional debug log: touch ~/.cache/ultimate-shell-gm-debug to enable.
        Writes every menu item's fields to ~/ultimate-shell-menu-debug.log ---- */
static gboolean gm_debug_enabled(void) {
    static gint cached = -1;
    if (cached < 0) {
        gchar *flag = g_build_filename(g_get_user_cache_dir(), "ultimate-shell-gm-debug", NULL);
        cached = g_file_test(flag, G_FILE_TEST_EXISTS) ? 1 : 0;
        g_free(flag);
    }
    return cached == 1;
}

static void gm_debug_log_item(const gchar *engine, GVariant *dict) {
    if (!gm_debug_enabled() || !dict) return;
    gchar *path = g_build_filename(g_get_home_dir(), "ultimate-shell-menu-debug.log", NULL);
    FILE *f = fopen(path, "a");
    g_free(path);
    if (!f) return;
    gchar *txt = g_variant_print(dict, TRUE);
    fprintf(f, "[%s] %s\n", engine, txt);
    g_free(txt);
    fclose(f);
}


static GlobalMenuWidget *g_gm = NULL;

/* =========================================================================
 * Overflow: menus that don't fit go into a "»" menu at the end (like macOS)
 * A GtkMenuBar never shrinks, so on a crowded top bar the menus used to push
 * the tray and clock off the panel.
 * ========================================================================= */
static guint gm_fit_idle_id = 0;

/* Space the menu bar may use: up to the first thing on its right in the top bar */
static gint gm_available_width(GlobalMenuWidget *gm) {
    GtkWidget *root = gm->holder_box;
    while (root && g_strcmp0(gtk_widget_get_name(root), "ultimate-root-box") != 0)
        root = gtk_widget_get_parent(root);
    if (!root || !GTK_IS_CONTAINER(root) || !gtk_widget_get_realized(gm->menubar)) return -1;

    gint mx = 0, my = 0;
    if (!gtk_widget_translate_coordinates(gm->menubar, root, 0, 0, &mx, &my)) return -1;
    GtkAllocation ra;
    gtk_widget_get_allocation(root, &ra);
    gint limit = ra.width;

    GList *kids = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *l = kids; l; l = l->next) {
        GtkWidget *k = l->data;
        if (!gtk_widget_get_visible(k) || gtk_widget_is_ancestor(gm->menubar, k)) continue;
        GtkAllocation ka;
        gtk_widget_get_allocation(k, &ka);
        if (ka.width <= 1) continue;
        gint kx = 0, ky = 0;
        if (!gtk_widget_translate_coordinates(k, root, 0, 0, &kx, &ky)) continue;
        if (kx > mx && kx < limit) limit = kx;        /* first widget to the right */
    }
    g_list_free(kids);
    return MAX(0, limit - mx - 12);
}

static gint gm_item_width(GtkWidget *w) {
    gint min = 0, nat = 0;
    gtk_widget_get_preferred_width(w, &min, &nat);
    return nat;
}

static GtkWidget *gm_overflow_item(GtkWidget *bar, gboolean create) {
    GtkWidget *ovf = g_object_get_data(G_OBJECT(bar), "gm-overflow-item");
    if (ovf || !create) return ovf;
    ovf = gtk_menu_item_new_with_label("»");
    gtk_widget_set_tooltip_text(ovf, "More menus");
    GtkWidget *sub = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "ultimate-themed-menu");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(ovf), sub);
    g_object_set_data(G_OBJECT(bar), "gm-overflow-item", ovf);
    return ovf;
}

static void gm_fit_menubar(GlobalMenuWidget *gm) {
    GtkWidget *bar = gm ? gm->menubar : NULL;
    if (!GTK_IS_MENU_SHELL(bar) || gm->open_menus_count > 0) return;   /* never while a menu is open */
    gint avail = gm_available_width(gm);
    if (avail < 0) return;

    /* 1. everything back on the bar, in order, overflow item out */
    GtkWidget *ovf = gm_overflow_item(bar, FALSE);
    GtkWidget *sub = ovf ? gtk_menu_item_get_submenu(GTK_MENU_ITEM(ovf)) : NULL;
    gint was_hidden = 0;
    if (ovf) {
        GList *moved = sub ? gtk_container_get_children(GTK_CONTAINER(sub)) : NULL;
        was_hidden = g_list_length(moved);
        g_object_ref(ovf);
        gtk_container_remove(GTK_CONTAINER(bar), ovf);
        for (GList *l = moved; l; l = l->next) {
            GtkWidget *it = l->data;
            g_object_ref(it);
            gtk_container_remove(GTK_CONTAINER(sub), it);
            gtk_menu_shell_append(GTK_MENU_SHELL(bar), it);
            g_object_unref(it);
        }
        g_list_free(moved);
    }

    /* 2. what fits */
    GList *items = gtk_container_get_children(GTK_CONTAINER(bar));
    gint total = 0, n = 0;
    for (GList *l = items; l; l = l->next) if (gtk_widget_get_visible(l->data)) { total += gm_item_width(l->data); n++; }

    if (total <= avail || n == 0) {                  /* all fit: no » */
        if (ovf) g_object_unref(ovf);
        g_list_free(items);
        return;
    }
    if (!ovf) { ovf = gm_overflow_item(bar, TRUE); g_object_ref(ovf); }
    sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(ovf));
    gint ow = gm_item_width(ovf) + 6;
    gint used = 0;
    gboolean overflowing = FALSE;
    for (GList *l = items; l; l = l->next) {
        GtkWidget *it = l->data;
        if (!gtk_widget_get_visible(it)) continue;
        gint w = gm_item_width(it);
        if (!overflowing && used + w + ow <= avail) { used += w; continue; }
        overflowing = TRUE;                           /* keep the order: once one moves, the rest follow */
        g_object_ref(it);
        gtk_container_remove(GTK_CONTAINER(bar), it);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), it);
        g_object_unref(it);
    }
    g_list_free(items);
    gtk_menu_shell_append(GTK_MENU_SHELL(bar), ovf);
    gtk_widget_show_all(ovf);
    g_object_unref(ovf);
    (void)was_hidden;
}

static gboolean gm_fit_idle(gpointer d) {
    gm_fit_idle_id = 0;
    gm_fit_menubar(d);
    return G_SOURCE_REMOVE;
}

/* Re-fit after anything in the top bar changes size; the check is cheap and a
   second pass finds the same answer, so it settles at once */
static void gm_queue_fit(GlobalMenuWidget *gm) {
    if (!gm_fit_idle_id) gm_fit_idle_id = g_timeout_add(60, gm_fit_idle, gm);
}

static void on_gm_holder_allocate(GtkWidget *w, GdkRectangle *a, GlobalMenuWidget *gm) {
    static gint last_w = -1, last_x = -1, last_hw = -1;
    GtkWidget *top = gtk_widget_get_toplevel(w);
    GtkAllocation ta;
    gtk_widget_get_allocation(top, &ta);
    /* same place, same size, same panel: nothing to do (this is also what stops the loop) */
    if (a->x == last_x && a->width == last_hw && ta.width == last_w && !g_object_get_data(G_OBJECT(w), "gm-refit")) return;
    last_x = a->x; last_w = ta.width; last_hw = a->width;
    g_object_set_data(G_OBJECT(w), "gm-refit", NULL);
    gm_queue_fit(gm);
}

static const gchar registrar_xml[] =
    "<node>"
    "  <interface name='com.canonical.AppMenu.Registrar'>"
    "    <method name='RegisterWindow'>"
    "      <arg type='u' name='windowId' direction='in'/>"
    "      <arg type='o' name='menuObjectPath' direction='in'/>"
    "    </method>"
    "    <method name='UnregisterWindow'>"
    "      <arg type='u' name='windowId' direction='in'/>"
    "    </method>"
    "    <method name='GetMenuForWindow'>"
    "      <arg type='u' name='windowId' direction='in'/>"
    "      <arg type='s' name='service' direction='out'/>"
    "      <arg type='o' name='menuObjectPath' direction='out'/>"
    "    </method>"
    "    <signal name='WindowRegistered'>"
    "      <arg type='u' name='windowId'/>"
    "      <arg type='s' name='service'/>"
    "      <arg type='o' name='menuObjectPath'/>"
    "    </signal>"
    "    <signal name='WindowUnregistered'>"
    "      <arg type='u' name='windowId'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static void update_menu_for_active_window(GlobalMenuWidget *gm, gboolean force);

static void free_registered_menu(gpointer data) {
    RegisteredMenu *rm = (RegisteredMenu *)data;
    if (!rm) return;
    g_free(rm->bus_name);
    g_free(rm->object_path);
    g_free(rm);
}

static void save_registry_cache(GlobalMenuWidget *gm) {
    if (!gm || !gm->registry) return;
    FILE *fp = fopen(APPMENU_CACHE_FILE, "w");
    if (!fp) return;
    GHashTableIter iter;
    gpointer k, v;
    g_hash_table_iter_init(&iter, gm->registry);
    while (g_hash_table_iter_next(&iter, &k, &v)) {
        guint xid = GPOINTER_TO_UINT(k);
        RegisteredMenu *rm = (RegisteredMenu *)v;
        if (xid > 0 && rm && rm->bus_name && rm->object_path) {
            fprintf(fp, "%u %s %s\n", xid, rm->bus_name, rm->object_path);
        }
    }
    fclose(fp);
}

static void load_registry_cache(GlobalMenuWidget *gm) {
    if (!gm || !gm->registry) return;
    FILE *fp = fopen(APPMENU_CACHE_FILE, "r");
    if (!fp) return;
    guint xid = 0;
    char bus[256], path[512];
    while (fscanf(fp, "%u %255s %511s", &xid, bus, path) == 3) {
        if (xid > 0 && bus[0] != '\0' && path[0] != '\0') {
            RegisteredMenu *rm = g_new0(RegisteredMenu, 1);
            rm->bus_name = g_strdup(bus);
            rm->object_path = g_strdup(path);
            g_hash_table_replace(gm->registry, GUINT_TO_POINTER(xid), rm);
        }
    }
    fclose(fp);
}

/* =========================================================================
 * Helper: Strip Mnemonic Underscores ("_File" -> "File", "Save __As" -> "Save _As")
 * ========================================================================= */
static gchar *clean_menu_label(const gchar *raw) {
    if (!raw) return g_strdup("");
    GString *out = g_string_new("");
    for (const gchar *p = raw; *p != '\0'; p++) {
        if (*p == '_') {
            if (*(p + 1) == '_') {
                g_string_append_c(out, '_');
                p++;
            }
            continue;
        }
        g_string_append_c(out, *p);
    }
    return g_string_free(out, FALSE);
}

/* =========================================================================
 * Helper: Active Window Detection & Bold App Title
 * ========================================================================= */
/* The Ultimate Shell settings window is treated as a normal app of its own. */
static gboolean is_shell_settings_window(WnckWindow *w) {
    const gchar *name = w ? wnck_window_get_name(w) : NULL;
    return name && g_str_has_prefix(name, "Ultimate Shell");
}

static gboolean is_shell_own_window(WnckWindow *w) {
    if (!w) return TRUE;
    if (is_shell_settings_window(w)) return FALSE;
    const gchar *cls = wnck_window_get_class_group_name(w);
    if (cls && (g_ascii_strcasecmp(cls, "Wrapper-2.0") == 0 ||
                g_ascii_strcasecmp(cls, "xfce4-panel") == 0)) {
        return TRUE;
    }
    return FALSE;
}

static WnckWindow *resolve_to_main_app_window(WnckScreen *screen, WnckWindow *win) {
    if (!win || !screen) return NULL;

    /* 1. Walk up WM_TRANSIENT_FOR chain if set */
    WnckWindow *curr = win;
    for (int depth = 0; depth < 6 && curr; depth++) {
        WnckWindow *parent = wnck_window_get_transient(curr);
        if (!parent || parent == curr) break;
        curr = parent;
    }
    if (curr != win && !wnck_window_is_minimized(curr) && !is_shell_own_window(curr)) {
        return curr;
    }

    /* 2. If it's already a normal taskbar window, return it directly */
    WnckWindowType type = wnck_window_get_window_type(win);
    if (type == WNCK_WINDOW_NORMAL && !wnck_window_is_skip_tasklist(win)) {
        return win;
    }

    /* 3. For dialogs/sub-windows without WM_TRANSIENT_FOR (like VLC/Qt or detached prefs),
          find the topmost visible WNCK_WINDOW_NORMAL belonging to the same App, PID, or Class Group */
    WnckApplication *app = wnck_window_get_application(win);
    int pid = wnck_window_get_pid(win);
    const gchar *cls = wnck_window_get_class_group_name(win);

    GList *stacked = wnck_screen_get_windows_stacked(screen);
    for (GList *l = g_list_last(stacked); l != NULL; l = l->prev) {
        WnckWindow *cand = WNCK_WINDOW(l->data);
        if (!cand || cand == win || wnck_window_is_minimized(cand)) continue;
        if (wnck_window_get_window_type(cand) != WNCK_WINDOW_NORMAL) continue;
        if (is_shell_own_window(cand)) continue;

        WnckApplication *c_app = wnck_window_get_application(cand);
        int c_pid = wnck_window_get_pid(cand);
        const gchar *c_cls = wnck_window_get_class_group_name(cand);

        gboolean same_app = (app && c_app && app == c_app);
        gboolean same_pid = (pid > 0 && c_pid > 0 && pid == c_pid);
        gboolean same_cls = (cls && cls[0] != '\0' && c_cls && g_ascii_strcasecmp(cls, c_cls) == 0);

        if (same_app || same_pid || same_cls) {
            return cand;
        }
    }

    /* 4. If it's a standalone dialog (e.g. a settings tool with no main window), still return it so it doesn't say Finder */
    if (type == WNCK_WINDOW_DIALOG || type == WNCK_WINDOW_NORMAL) {
        return win;
    }
    return NULL;
}

static WnckWindow *get_valid_active_win(GlobalMenuWidget *gm) {
    if (!gm || !gm->tracker || !gm->tracker->screen) return NULL;
    WnckWindow *win = wnck_screen_get_active_window(gm->tracker->screen);
    if (!win || wnck_window_is_minimized(win)) return NULL;

    WnckWindowType type = wnck_window_get_window_type(win);
    if (type == WNCK_WINDOW_DESKTOP || type == WNCK_WINDOW_DOCK) return NULL;
    if (is_shell_own_window(win)) return NULL;

    return resolve_to_main_app_window(gm->tracker->screen, win);
}

static void refresh_active_app_bold_title(GlobalMenuWidget *gm) {
    if (!gm || !GTK_IS_LABEL(gm->app_name_lbl)) return;
    WnckWindow *win = get_valid_active_win(gm);
    if (!win) {
        gtk_label_set_markup(GTK_LABEL(gm->app_name_lbl), "<b>Finder</b>");
        gtk_widget_set_tooltip_text(gm->app_name_eb, "Finder (Desktop)");
        gtk_label_set_width_chars(GTK_LABEL(gm->app_name_lbl), 6);
        return;
    }

    WnckApplication *app = wnck_window_get_application(win);
    const gchar *app_name = app ? wnck_application_get_name(app) : NULL;
    const gchar *win_title = wnck_window_get_name(win);
    const gchar *cls_group = wnck_window_get_class_group_name(win);

    const gchar *best = is_shell_settings_window(win) ? "Ultimate Shell"
                      : app_display_name_for_window(win);
    (void)app_name; (void)cls_group;

    gchar *markup = g_markup_printf_escaped("<b>%s</b>", best);
    gtk_label_set_markup(GTK_LABEL(gm->app_name_lbl), markup);
    g_free(markup);

    /* Keep the whole app name visible (up to 24 characters), like macOS: an
       ellipsized label may otherwise shrink to "..." when the window buttons
       join the top bar on a maximized window. The menus give way instead. */
    gtk_label_set_width_chars(GTK_LABEL(gm->app_name_lbl), (gint)MIN(g_utf8_strlen(best, -1) + 1, 25));   /* +1: the name is bold */

    gtk_widget_set_tooltip_text(gm->app_name_eb, win_title ? win_title : best);
}

static void on_tracked_win_name_changed(WnckWindow *window, GlobalMenuWidget *gm) {
    refresh_active_app_bold_title(gm);
}


static gchar *get_x11_string_property(Window xid, const gchar *prop_name) {
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy) || !xid) return NULL;

    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    Atom prop = XInternAtom(dpy, prop_name, False);

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char *data = NULL;

    gdk_x11_display_error_trap_push(gdk_dpy);
    int status = XGetWindowProperty(dpy, xid, prop, 0, 1024, False, AnyPropertyType,
                                    &actual_type, &actual_format,
                                    &nitems, &bytes_after, &data);
    int err = gdk_x11_display_error_trap_pop(gdk_dpy);

    if (err != 0 || status != Success || !data || nitems == 0) {
        if (data) XFree(data);
        return NULL;
    }

    gchar *result = g_strdup((const gchar *)data);
    XFree(data);
    return result;
}


static void apply_clean_menubar_css(GtkWidget *menubar) {
    static GtkCssProvider *provider = NULL;
    if (!provider) {
        provider = gtk_css_provider_new();
        gtk_css_provider_load_from_data(provider,
            "menubar, #ultimate-global-menubar {"
            "  border: none;"
            "  border-bottom: none;"
            "  border-width: 0px;"
            "  box-shadow: none;"
            "  background: transparent;"
            "  background-image: none;"
            "  padding: 0px;"
            "  margin: 0px;"
            "}"
            "menubar > menuitem, #ultimate-global-menubar > menuitem {"
            "  border: none;"
            "  box-shadow: none;"
            "  padding: 2px 7px;"
            "  color: @theme_fg_color;"
            "}"
            "menubar > menuitem > label, #ultimate-global-menubar > menuitem > label {"
            "  color: @theme_fg_color;"
            "  text-shadow: none;"
            "}"
            "menubar > menuitem:hover, #ultimate-global-menubar > menuitem:hover {"
            "  background-color: @theme_selected_bg_color;"
            "  color: @theme_fg_color;"
            "  border-radius: 4px;"
            "}"
            "window.popup, .popup.csd, decoration {"
            "  background-color: transparent;"
            "  background: transparent;"
            "  box-shadow: none;"
            "}"
            "menu.ultimate-themed-menu menuitem,"
            ".ultimate-themed-menu menuitem,"
            "menu.ultimate-themed-menu menuitem label,"
            ".ultimate-themed-menu menuitem label {"
            "  color: @theme_fg_color;"
            "  opacity: 1.0;"
            "  text-shadow: none;"
            "  -gtk-icon-shadow: none;"
            "}"
            "menu.ultimate-themed-menu menuitem:hover,"
            ".ultimate-themed-menu menuitem:hover {"
            "  background-color: @theme_selected_bg_color;"
            "  background-image: none;"
            "  color: @theme_fg_color;"
            "}"
            "menu.ultimate-themed-menu menuitem:hover label,"
            ".ultimate-themed-menu menuitem:hover label {"
            "  color: @theme_fg_color;"
            "  opacity: 1.0;"
            "}"
            "menu.ultimate-themed-menu menuitem:disabled,"
            ".ultimate-themed-menu menuitem:disabled,"
            "menu.ultimate-themed-menu menuitem:disabled label,"
            ".ultimate-themed-menu menuitem:disabled label,"
            "menu.ultimate-themed-menu menuitem:backdrop:disabled label,"
            ".ultimate-themed-menu menuitem:backdrop:disabled label {"
            "  color: alpha(@theme_fg_color, 0.45);"
            "  opacity: 1.0;"
            "  background: transparent;"
            "  box-shadow: none;"
            "  text-shadow: none;"
            "}",
            -1, NULL);
        GdkScreen *screen = gdk_screen_get_default();
        if (screen) {
            gtk_style_context_add_provider_for_screen(
                screen,
                GTK_STYLE_PROVIDER(provider),
                GTK_STYLE_PROVIDER_PRIORITY_USER + 100
            );
        }
    }
    if (GTK_IS_WIDGET(menubar)) {
        GtkStyleContext *ctx = gtk_widget_get_style_context(menubar);
        gtk_style_context_add_provider(ctx, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER + 100);
    }
}

static void clear_current_menubar(GlobalMenuWidget *gm) {
    if (!gm) return;
    if (gm->reload_timer_id != 0) {
        g_source_remove(gm->reload_timer_id);
        gm->reload_timer_id = 0;
    }
    if (gm->dbusmenu_sig_sub_id != 0 && gm->session_bus) {
        g_dbus_connection_signal_unsubscribe(gm->session_bus, gm->dbusmenu_sig_sub_id);
        gm->dbusmenu_sig_sub_id = 0;
    }
    gm->open_menus_count = 0;
    gm->reload_pending = FALSE;
    gm->active_revision = 0;

    if (gm->active_is_gtk_menus && gm->session_bus && gm->active_bus && gm->active_path) {
        GVariantBuilder eb;
        g_variant_builder_init(&eb, G_VARIANT_TYPE("au"));
        for (guint u = 0; u < 64; u++) g_variant_builder_add(&eb, "u", u);
        g_dbus_connection_call(gm->session_bus, gm->active_bus, gm->active_path,
                               "org.gtk.Menus", "End", g_variant_new("(au)", &eb),
                               NULL, G_DBUS_CALL_FLAGS_NONE, 300, NULL, NULL, NULL);
    }
    gm->active_is_gtk_menus = FALSE;

    if (gm->menubar) {
        gtk_widget_destroy(gm->menubar);
        gm->menubar = NULL;
    }
    g_clear_pointer(&gm->active_bus, g_free);
    g_clear_pointer(&gm->active_path, g_free);
    gm->active_xid = 0;
    gm->menubar_is_fallback = FALSE;
    gm->fallback_xid = 0;
}
/* =========================================================================
 * macOS-style extras: proper app names, app menu, Window menu, menu search,
 * keyboard access (Ctrl+Alt+M), fallback bar for apps without menus
 * ========================================================================= */

/* ---- Proper app name from the app's .desktop file (cached by window class) ---- */
static GHashTable *g_app_name_cache = NULL;

static gchar *pretty_from_class(const gchar *cls) {
    if (!cls || !cls[0]) return g_strdup("Application");
    gchar *s = g_strdup(cls);
    /* "firefox-esr" -> "Firefox-esr" (only used when no .desktop file matches) */
    s[0] = g_ascii_toupper(s[0]);
    return s;
}

static const gchar *app_display_name_for_window(WnckWindow *win) {
    if (!win) return "Application";
    const gchar *cls = wnck_window_get_class_group_name(win);
    const gchar *inst = wnck_window_get_class_instance_name(win);
    const gchar *key = (cls && cls[0]) ? cls : ((inst && inst[0]) ? inst : "");
    if (!g_app_name_cache) g_app_name_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    const gchar *hit = g_hash_table_lookup(g_app_name_cache, key);
    if (hit) return hit;

    gchar *name = NULL;
    /* 1. <class>.desktop / <instance>.desktop (lower-case) */
    const gchar *cands[] = { cls, inst, NULL };
    for (gint i = 0; cands[i] && !name; i++) {
        if (!cands[i][0]) continue;
        gchar *lc = g_ascii_strdown(cands[i], -1);
        gchar *id = g_strdup_printf("%s.desktop", lc);
        GDesktopAppInfo *di = g_desktop_app_info_new(id);
        if (di) { name = g_strdup(g_app_info_get_name(G_APP_INFO(di))); g_object_unref(di); }
        g_free(id); g_free(lc);
    }
    /* 2. any .desktop whose StartupWMClass or executable matches */
    if (!name) {
        GList *all = g_app_info_get_all();
        for (GList *l = all; l && !name; l = l->next) {
            if (!G_IS_DESKTOP_APP_INFO(l->data)) continue;
            GDesktopAppInfo *di = G_DESKTOP_APP_INFO(l->data);
            const gchar *wm = g_desktop_app_info_get_startup_wm_class(di);
            const gchar *exe = g_app_info_get_executable(G_APP_INFO(di));
            gchar *exe_base = exe ? g_path_get_basename(exe) : NULL;
            if ((wm && ((cls && g_ascii_strcasecmp(wm, cls) == 0) || (inst && g_ascii_strcasecmp(wm, inst) == 0))) ||
                (exe_base && ((cls && g_ascii_strcasecmp(exe_base, cls) == 0) || (inst && g_ascii_strcasecmp(exe_base, inst) == 0)))) {
                name = g_strdup(g_app_info_get_name(G_APP_INFO(di)));
            }
            g_free(exe_base);
        }
        g_list_free_full(all, g_object_unref);
    }
    if (!name) {
        WnckApplication *app = wnck_window_get_application(win);
        const gchar *an = app ? wnck_application_get_name(app) : NULL;
        name = (an && an[0] && strlen(an) < 40) ? g_strdup(an) : pretty_from_class(key);
    }
    g_hash_table_insert(g_app_name_cache, g_strdup(key), name);
    return name;
}

/* ---- windows of the same app ---- */
static gboolean same_app(WnckWindow *a, WnckWindow *b) {
    if (!a || !b) return FALSE;
    if (a == b) return TRUE;
    WnckApplication *aa = wnck_window_get_application(a), *ba = wnck_window_get_application(b);
    if (aa && aa == ba) return TRUE;
    gint ap = wnck_window_get_pid(a), bp = wnck_window_get_pid(b);
    if (ap > 0 && ap == bp) return TRUE;
    const gchar *ac = wnck_window_get_class_group_name(a), *bc = wnck_window_get_class_group_name(b);
    return (ac && bc && ac[0] && g_ascii_strcasecmp(ac, bc) == 0);
}

static GList *app_windows(GlobalMenuWidget *gm, WnckWindow *ref) {
    GList *out = NULL;
    if (!gm->tracker || !gm->tracker->screen || !ref) return NULL;
    for (GList *l = wnck_screen_get_windows(gm->tracker->screen); l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        WnckWindowType t = wnck_window_get_window_type(w);
        if (t != WNCK_WINDOW_NORMAL && t != WNCK_WINDOW_DIALOG && t != WNCK_WINDOW_UTILITY) continue;
        if (same_app(w, ref)) out = g_list_append(out, w);
    }
    return out;
}

static WnckWindow *window_from_item(GtkWidget *mi) {
    gulong xid = (gulong)GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(mi), "gm-xid"));
    if (!xid || !g_gm || !g_gm->tracker || !g_gm->tracker->screen) return NULL;
    /* look it up on our own screen (wnck_window_get is deprecated in libwnck 43) */
    for (GList *l = wnck_screen_get_windows(g_gm->tracker->screen); l; l = l->next) {
        if (wnck_window_get_xid(WNCK_WINDOW(l->data)) == xid) return WNCK_WINDOW(l->data);
    }
    return NULL;
}

/* ---- App menu (click on the bold app name) ---- */
static void on_app_hide(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    WnckWindow *ref = window_from_item(GTK_WIDGET(mi));
    GList *ws = app_windows(gm, ref);
    for (GList *l = ws; l; l = l->next) wnck_window_minimize(WNCK_WINDOW(l->data));
    g_list_free(ws);
}

static void on_app_hide_others(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    WnckWindow *ref = window_from_item(GTK_WIDGET(mi));
    if (!ref || !gm->tracker) return;
    for (GList *l = wnck_screen_get_windows(gm->tracker->screen); l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        if (wnck_window_get_window_type(w) != WNCK_WINDOW_NORMAL || wnck_window_is_skip_tasklist(w)) continue;
        if (!same_app(w, ref) && !is_shell_own_window(w)) wnck_window_minimize(w);
    }
}

static void on_app_show_all(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    if (!gm->tracker) return;
    guint32 ts = gtk_get_current_event_time();
    for (GList *l = wnck_screen_get_windows(gm->tracker->screen); l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        if (wnck_window_get_window_type(w) == WNCK_WINDOW_NORMAL && wnck_window_is_minimized(w))
            wnck_window_unminimize(w, ts);
    }
}

static void on_app_quit(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    WnckWindow *ref = window_from_item(GTK_WIDGET(mi));
    guint32 ts = gtk_get_current_event_time();
    GList *ws = app_windows(gm, ref);
    for (GList *l = ws; l; l = l->next) wnck_window_close(WNCK_WINDOW(l->data), ts);   /* asks the app nicely */
    g_list_free(ws);
}

static GtkWidget *gm_item(const gchar *label, WnckWindow *ref, GCallback cb, GlobalMenuWidget *gm) {
    GtkWidget *mi = gtk_menu_item_new_with_label(label);
    if (ref) g_object_set_data(G_OBJECT(mi), "gm-xid", GSIZE_TO_POINTER((gsize)wnck_window_get_xid(ref)));
    if (cb) g_signal_connect(mi, "activate", cb, gm);
    return mi;
}

static gboolean on_app_name_clicked(GtkWidget *eb, GdkEventButton *event, GlobalMenuWidget *gm) {
    if (event->type != GDK_BUTTON_PRESS || event->button != 1) return FALSE;
    WnckWindow *win = get_valid_active_win(gm);
    if (!win) return FALSE;      /* desktop: nothing to show */
    const gchar *name = app_display_name_for_window(win);

    GtkWidget *menu = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
    gchar *l_hide = g_strdup_printf("Hide %s", name);
    gchar *l_quit = g_strdup_printf("Quit %s", name);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gm_item(l_hide, win, G_CALLBACK(on_app_hide), gm));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gm_item("Hide Others", win, G_CALLBACK(on_app_hide_others), gm));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gm_item("Show All", win, G_CALLBACK(on_app_show_all), gm));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gm_item(l_quit, win, G_CALLBACK(on_app_quit), gm));
    g_free(l_hide); g_free(l_quit);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), eb, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, (GdkEvent *)event);
    return TRUE;
}

/* ---- Window menu ---- */
static void on_win_minimize(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    WnckWindow *w = window_from_item(GTK_WIDGET(mi));
    if (w) wnck_window_minimize(w);
}
static void on_win_zoom(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    WnckWindow *w = window_from_item(GTK_WIDGET(mi));
    if (!w) return;
    if (wnck_window_is_maximized(w)) wnck_window_unmaximize(w); else wnck_window_maximize(w);
}
static void on_win_activate(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    WnckWindow *w = window_from_item(GTK_WIDGET(mi));
    if (w) wnck_window_activate(w, gtk_get_current_event_time());
}
static void on_win_bring_all(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    WnckWindow *ref = window_from_item(GTK_WIDGET(mi));
    guint32 ts = gtk_get_current_event_time();
    GList *ws = app_windows(gm, ref);
    for (GList *l = ws; l; l = l->next) wnck_window_activate(WNCK_WINDOW(l->data), ts);
    if (ref) wnck_window_activate(ref, ts);   /* keep the current one on top */
    g_list_free(ws);
}

static void fill_window_menu(GlobalMenuWidget *gm, GtkWidget *sub, WnckWindow *win) {
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gm_item("Minimize", win, G_CALLBACK(on_win_minimize), gm));
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gm_item("Zoom", win, G_CALLBACK(on_win_zoom), gm));
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
    GList *ws = app_windows(gm, win);
    for (GList *l = ws; l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        const gchar *t = wnck_window_get_name(w);
        GtkWidget *ci = gtk_check_menu_item_new_with_label(t && t[0] ? t : "Untitled");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(ci), w == win);
        g_object_set_data(G_OBJECT(ci), "gm-xid", GSIZE_TO_POINTER((gsize)wnck_window_get_xid(w)));
        g_signal_connect(ci, "activate", G_CALLBACK(on_win_activate), gm);
        GtkWidget *lbl = gtk_bin_get_child(GTK_BIN(ci));
        if (GTK_IS_LABEL(lbl)) { gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(lbl), 40); }
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), ci);
    }
    g_list_free(ws);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gm_item("Bring All to Front", win, G_CALLBACK(on_win_bring_all), gm));
}

/* ---- Search all menu items (Help ▸ Search Menus…) ---- */
typedef struct { gchar *path; GtkWidget *item; } MenuHit;

static void menu_hit_free(gpointer p) {
    MenuHit *h = (MenuHit *)p;
    g_free(h->path);
    if (h->item) g_object_unref(h->item);
    g_free(h);
}

static void collect_menu_items(GtkWidget *shell, const gchar *prefix, GPtrArray *out) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(shell));
    for (GList *l = kids; l; l = l->next) {
        GtkWidget *mi = GTK_WIDGET(l->data);
        if (!GTK_IS_MENU_ITEM(mi) || GTK_IS_SEPARATOR_MENU_ITEM(mi)) continue;
        if (g_object_get_data(G_OBJECT(mi), "gm-search-item")) continue;
        const gchar *lbl = gtk_menu_item_get_label(GTK_MENU_ITEM(mi));
        if (!lbl || !lbl[0]) continue;
        gchar *path = prefix ? g_strdup_printf("%s ▸ %s", prefix, lbl) : g_strdup(lbl);
        GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(mi));
        if (sub) {
            collect_menu_items(sub, path, out);
        } else if (gtk_widget_get_sensitive(mi)) {
            MenuHit *h = g_new0(MenuHit, 1);
            h->path = g_strdup(path);
            h->item = g_object_ref(mi);
            g_ptr_array_add(out, h);
        }
        g_free(path);
    }
    g_list_free(kids);
}

static GtkWidget *g_search_win = NULL;

static void search_refilter(GtkSearchEntry *entry, GtkWidget *list) {
    GPtrArray *hits = g_object_get_data(G_OBJECT(list), "hits");
    const gchar *q = gtk_entry_get_text(GTK_ENTRY(entry));
    gchar *qd = g_utf8_casefold(q ? q : "", -1);
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = rows; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(rows);
    gint shown = 0;
    for (guint i = 0; hits && i < hits->len && shown < 40; i++) {
        MenuHit *h = g_ptr_array_index(hits, i);
        gchar *pd = g_utf8_casefold(h->path, -1);
        if (!qd[0] || strstr(pd, qd)) {
            GtkWidget *lbl = gtk_label_new(h->path);
            gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
            gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
            gtk_widget_set_margin_start(lbl, 6);
            gtk_widget_set_margin_top(lbl, 4);
            gtk_widget_set_margin_bottom(lbl, 4);
            gtk_list_box_insert(GTK_LIST_BOX(list), lbl, -1);
            GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), shown);
            if (row) g_object_set_data(G_OBJECT(row), "hit", h);
            shown++;
        }
        g_free(pd);
    }
    g_free(qd);
    gtk_widget_show_all(list);
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
    if (first) gtk_list_box_select_row(GTK_LIST_BOX(list), first);
}

static void search_run_hit(MenuHit *h) {
    if (!h || !h->item) return;
    GtkWidget *item = h->item;
    g_object_ref(item);
    if (g_search_win) gtk_widget_destroy(g_search_win);
    if (gtk_widget_get_parent(item)) gtk_menu_item_activate(GTK_MENU_ITEM(item));   /* still alive */
    g_object_unref(item);
}

static void on_search_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
    search_run_hit(g_object_get_data(G_OBJECT(row), "hit"));
}

static void on_search_entry_activate(GtkEntry *entry, GtkWidget *list) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
    if (!row) row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
    if (row) search_run_hit(g_object_get_data(G_OBJECT(row), "hit"));
}

static gboolean on_search_key(GtkWidget *w, GdkEventKey *ev, GtkWidget *list) {
    if (ev->keyval == GDK_KEY_Escape) { gtk_widget_destroy(w); return TRUE; }
    if (ev->keyval == GDK_KEY_Down || ev->keyval == GDK_KEY_Up) {
        GtkListBoxRow *cur = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
        gint idx = cur ? gtk_list_box_row_get_index(cur) : -1;
        idx += (ev->keyval == GDK_KEY_Down) ? 1 : -1;
        GtkListBoxRow *nr = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), MAX(idx, 0));
        if (nr) gtk_list_box_select_row(GTK_LIST_BOX(list), nr);
        return TRUE;
    }
    return FALSE;
}

static gboolean on_search_focus_out(GtkWidget *w, GdkEvent *ev, gpointer data) {
    gtk_widget_destroy(w);
    return FALSE;
}

static void open_menu_search(GlobalMenuWidget *gm) {
    if (g_search_win) { gtk_window_present(GTK_WINDOW(g_search_win)); return; }
    if (!gm->menubar) return;

    GPtrArray *hits = g_ptr_array_new_with_free_func(menu_hit_free);
    collect_menu_items(gm->menubar, NULL, hits);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_decorated(GTK_WINDOW(win), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(win), GDK_WINDOW_TYPE_HINT_DIALOG);
    gtk_window_set_keep_above(GTK_WINDOW(win), TRUE);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 460, 340);
    gtk_style_context_add_class(gtk_widget_get_style_context(win), "ultimate-popup-win");
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_search_win);
    g_search_win = win;

    GtkWidget *vb = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vb), 10);
    GtkWidget *entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Search menus…");
    gtk_box_pack_start(GTK_BOX(vb), entry, FALSE, FALSE, 0);
    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *list = gtk_list_box_new();
    g_object_set_data_full(G_OBJECT(list), "hits", hits, (GDestroyNotify)g_ptr_array_unref);
    gtk_container_add(GTK_CONTAINER(sc), list);
    gtk_box_pack_start(GTK_BOX(vb), sc, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(win), vb);

    g_signal_connect(entry, "search-changed", G_CALLBACK(search_refilter), list);
    g_signal_connect(entry, "activate", G_CALLBACK(on_search_entry_activate), list);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_search_row_activated), NULL);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_search_key), list);
    g_signal_connect(win, "focus-out-event", G_CALLBACK(on_search_focus_out), NULL);

    /* under the left part of the top bar */
    GdkWindow *gw = gtk_widget_get_window(gm->holder_box);
    gint x = 80, y = 32;
    if (gw) {
        gdk_window_get_origin(gw, &x, &y);
        GtkAllocation a;
        gtk_widget_get_allocation(gm->holder_box, &a);
        x += a.x; y += a.y + a.height + 4;
    }
    gtk_window_move(GTK_WINDOW(win), x, y);
    gtk_widget_show_all(win);
    search_refilter(GTK_SEARCH_ENTRY(entry), list);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(entry);
}

static void on_search_menus_activate(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    open_menu_search(gm);
}

/* ---- decorate whatever menubar is current: Window menu + Help ▸ Search ---- */
static GtkWidget *find_top_item(GtkWidget *menubar, const gchar *label) {
    GtkWidget *found = NULL;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menubar));
    for (GList *l = kids; l && !found; l = l->next) {
        if (!GTK_IS_MENU_ITEM(l->data)) continue;
        const gchar *t = gtk_menu_item_get_label(GTK_MENU_ITEM(l->data));
        if (t && g_ascii_strcasecmp(t, label) == 0) found = GTK_WIDGET(l->data);
    }
    g_list_free(kids);
    return found;
}

static GtkWidget *new_top_menu(GtkWidget *menubar, const gchar *label, gint pos) {
    GtkWidget *top = gtk_menu_item_new_with_label(label);
    GtkWidget *sub = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "ultimate-themed-menu");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    if (pos < 0) gtk_menu_shell_append(GTK_MENU_SHELL(menubar), top);
    else gtk_menu_shell_insert(GTK_MENU_SHELL(menubar), top, pos);
    return top;
}

static gint top_item_index(GtkWidget *menubar, GtkWidget *item) {
    GList *kids = gtk_container_get_children(GTK_CONTAINER(menubar));
    gint idx = g_list_index(kids, item);
    g_list_free(kids);
    return idx;
}

static void gm_decorate_menubar(GlobalMenuWidget *gm) {
    if (!gm || !gm->menubar) return;
    if (g_object_get_data(G_OBJECT(gm->menubar), "gm-decorated")) return;
    g_object_set_data(G_OBJECT(gm->menubar), "gm-decorated", GINT_TO_POINTER(1));

    WnckWindow *win = (gm->active_xid == 0xFFFFFFFF) ? NULL : get_valid_active_win(gm);
    GtkWidget *help = find_top_item(gm->menubar, "Help");
    if (win) gm_refresh_published_accels(gm);

    /* Window menu for every app (merged into the app's own "Window" menu if it has one) */
    if (win) {
        GtkWidget *wtop = find_top_item(gm->menubar, "Window");
        GtkWidget *wsub = NULL;
        if (wtop && gtk_menu_item_get_submenu(GTK_MENU_ITEM(wtop))) {
            wsub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(wtop));
            gtk_menu_shell_append(GTK_MENU_SHELL(wsub), gtk_separator_menu_item_new());
        } else {
            wtop = new_top_menu(gm->menubar, "Window", help ? top_item_index(gm->menubar, help) : -1);
            wsub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(wtop));
        }
        fill_window_menu(gm, wsub, win);
    }

    /* Help ▸ Search Menus… (first item) */
    if (!help) help = new_top_menu(gm->menubar, "Help", -1);
    GtkWidget *hsub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(help));
    if (hsub) {
        GtkWidget *search = gtk_menu_item_new_with_label("Search Menus…");
        g_object_set_data(G_OBJECT(search), "gm-search-item", GINT_TO_POINTER(1));
        g_signal_connect(search, "activate", G_CALLBACK(on_search_menus_activate), gm);
        GList *hk = gtk_container_get_children(GTK_CONTAINER(hsub));
        gboolean had_items = (hk != NULL);
        g_list_free(hk);
        gtk_menu_shell_prepend(GTK_MENU_SHELL(hsub), search);
        if (had_items) gtk_menu_shell_insert(GTK_MENU_SHELL(hsub), gtk_separator_menu_item_new(), 1);
    }
    gtk_widget_show_all(gm->menubar);
}

/* ---- Fallback bar for apps that don't share their menus ---- */
static void on_fallback_hint_activate(GtkMenuItem *mi, gpointer data) {
    GtkWidget *d = gtk_message_dialog_new(NULL, 0, GTK_MESSAGE_INFO, GTK_BUTTONS_CLOSE,
        "This app doesn't share its menus");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        "GTK apps (e.g. Firefox, LibreOffice) need the app-menu helper to show their menus in the top bar.\n\n"
        "Install it, then log out and back in:\n"
        "    sudo apt install appmenu-gtk3-module appmenu-gtk2-module appmenu-registrar\n\n"
        "Some apps (GTK4, many Electron apps) never share their menus; their own menus stay inside the window.");
    g_signal_connect_swapped(d, "response", G_CALLBACK(gtk_widget_destroy), d);
    gtk_widget_show(d);
}

static void gm_build_fallback(GlobalMenuWidget *gm, guint xid) {
    if (gm->menubar_is_fallback && gm->fallback_xid == xid && gm->menubar) return;
    clear_current_menubar(gm);
    gm->menubar = gtk_menu_bar_new();
    gtk_widget_set_name(gm->menubar, "ultimate-global-menubar");
    apply_clean_menubar_css(gm->menubar);
    GtkWidget *help = new_top_menu(gm->menubar, "Help", -1);
    GtkWidget *hsub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(help));
    GtkWidget *hint = gtk_menu_item_new_with_label("Why are this app's menus missing?");
    g_signal_connect(hint, "activate", G_CALLBACK(on_fallback_hint_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(hsub), hint);
    gm_decorate_menubar(gm);                         /* adds Window + Search */
    gtk_box_pack_start(GTK_BOX(gm->holder_box), gm->menubar, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(gm->holder_box), "gm-refit", GINT_TO_POINTER(1));
    gm_queue_fit(gm);
    gtk_widget_show_all(gm->menubar);
    gm->menubar_is_fallback = TRUE;
    gm->fallback_xid = xid;
    gm->active_xid = 0;          /* keep retrying for the real menus */
}

/* ---- Keyboard: Ctrl+Alt+M opens the first menu of the bar ---- */
static void gm_open_first_menu(GlobalMenuWidget *gm) {
    if (!gm || !gm->menubar) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(gm->menubar));
    GtkWidget *first = kids ? GTK_WIDGET(kids->data) : NULL;
    g_list_free(kids);
    if (!first || !GTK_IS_MENU_ITEM(first)) return;
    GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(first));
    if (!sub) return;
    gtk_menu_popup_at_widget(GTK_MENU(sub), first, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
    gtk_menu_shell_select_first(GTK_MENU_SHELL(sub), TRUE);
}

static GdkFilterReturn gm_key_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data) {
    GlobalMenuWidget *gm = (GlobalMenuWidget *)data;
    XEvent *xev = (XEvent *)xevent;
    if (xev->type != KeyPress || !gm->hotkey_code) return GDK_FILTER_CONTINUE;
    if (xev->xkey.keycode == gm->hotkey_code &&
        (xev->xkey.state & (ControlMask | Mod1Mask)) == (ControlMask | Mod1Mask)) {
        gm_open_first_menu(gm);
        return GDK_FILTER_REMOVE;
    }
    return GDK_FILTER_CONTINUE;
}

static void gm_grab_hotkey(GlobalMenuWidget *gm) {
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    KeyCode kc = XKeysymToKeycode(dpy, XStringToKeysym("m"));
    if (!kc) return;
    static const unsigned int extra[] = { 0, LockMask, Mod2Mask, LockMask | Mod2Mask };
    gdk_x11_display_error_trap_push(gdk_dpy);
    for (guint i = 0; i < G_N_ELEMENTS(extra); i++) {
        XGrabKey(dpy, kc, ControlMask | Mod1Mask | extra[i], DefaultRootWindow(dpy), False,
                 GrabModeAsync, GrabModeAsync);
    }
    XSync(dpy, False);
    if (gdk_x11_display_error_trap_pop(gdk_dpy) == 0) {   /* nobody else owns Ctrl+Alt+M */
        gm->hotkey_code = kc;
        gdk_window_add_filter(NULL, gm_key_filter, gm);
    }
}

/* ---- keyboard shortcuts shown next to items ---- */
static void gm_ensure_accel_css(void) {
    static gboolean done = FALSE;
    if (done) return;
    done = TRUE;
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css,
        "menu.ultimate-themed-menu menuitem accelerator { margin-left: 28px; opacity: 0.6; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    g_object_unref(css);
}

static void gm_set_item_accel(GtkWidget *item, guint key, GdkModifierType mods) {
    gm_ensure_accel_css();
    if (!key) return;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(item));
    if (GTK_IS_ACCEL_LABEL(child)) gtk_accel_label_set_accel(GTK_ACCEL_LABEL(child), key, mods);
}


/* ---- Shortcuts published by our GTK module (ultimate-accels-gtk-module) ----
   appmenu-gtk-module drops shortcuts, so GTK apps (Thunar, Mousepad…) publish
   "label\t<accel>\n" lines in _ULTIMATE_MENU_ACCELS on their window. */
static void gm_load_accel_map(GlobalMenuWidget *gm, Window xid) {
    if (gm->accel_map) { g_hash_table_destroy(gm->accel_map); gm->accel_map = NULL; }
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy) || !xid) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);

    Atom prop = XInternAtom(dpy, "_ULTIMATE_MENU_ACCELS", False);
    Atom type_ret; int fmt_ret; unsigned long n_items = 0, after = 0;
    unsigned char *data = NULL;
    gdk_x11_display_error_trap_push(gdk_dpy);
    int st = XGetWindowProperty(dpy, xid, prop, 0, 65536, False, AnyPropertyType,
                                &type_ret, &fmt_ret, &n_items, &after, &data);
    gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
    if (st != Success || !data || fmt_ret != 8 || n_items == 0) { if (data) XFree(data); return; }

    gchar *txt = g_strndup((const gchar *)data, n_items);
    XFree(data);
    gm->accel_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gchar **lines = g_strsplit(txt, "\n", -1);
    for (gint i = 0; lines[i]; i++) {
        gchar *tab = strchr(lines[i], '\t');
        if (!tab) continue;
        *tab = '\0';
        if (lines[i][0] && tab[1]) g_hash_table_replace(gm->accel_map, g_strdup(lines[i]), g_strdup(tab + 1));
    }
    g_strfreev(lines);
    g_free(txt);
}

static void gm_apply_published_accel(GlobalMenuWidget *gm, GtkWidget *item, const gchar *clean_label) {
    if (!gm || !gm->accel_map || !clean_label) return;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(item));
    if (GTK_IS_ACCEL_LABEL(child)) {
        guint k0 = 0; GdkModifierType m0 = 0;
        gtk_accel_label_get_accel(GTK_ACCEL_LABEL(child), &k0, &m0);
        if (k0 != 0) return;                     /* app already sent one */
    }
    const gchar *accel = g_hash_table_lookup(gm->accel_map, clean_label);
    if (!accel) return;
    guint k = 0;
    GdkModifierType m = 0;
    gtk_accelerator_parse(accel, &k, &m);
    gm_set_item_accel(item, k, m);
}


/* Walk the whole bar and add any published shortcut an item doesn't have yet */
static void gm_apply_accels_to_shell(GlobalMenuWidget *gm, GtkWidget *shell) {
    if (!GTK_IS_CONTAINER(shell)) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(shell));
    for (GList *l = kids; l; l = l->next) {
        GtkWidget *mi = GTK_WIDGET(l->data);
        if (!GTK_IS_MENU_ITEM(mi) || GTK_IS_SEPARATOR_MENU_ITEM(mi)) continue;
        GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(mi));
        if (sub) { gm_apply_accels_to_shell(gm, sub); continue; }
        if (GTK_IS_MENU_BAR(shell)) continue;
        const gchar *lbl = gtk_menu_item_get_label(GTK_MENU_ITEM(mi));
        if (lbl && lbl[0]) gm_apply_published_accel(gm, mi, lbl);
    }
    g_list_free(kids);
}

static gulong gm_current_app_xid(GlobalMenuWidget *gm) {
    WnckWindow *w = get_valid_active_win(gm);
    return w ? (gulong)wnck_window_get_xid(w) : 0;
}

static void gm_refresh_published_accels(GlobalMenuWidget *gm) {
    gulong xid = gm_current_app_xid(gm);
    if (!xid || !gm->menubar) return;
    gm_load_accel_map(gm, (Window)xid);
    if (gm->accel_map) gm_apply_accels_to_shell(gm, gm->menubar);
}

static gboolean on_accel_refresh_idle(gpointer data) {
    GlobalMenuWidget *gm = (GlobalMenuWidget *)data;
    gm->accel_refresh_id = 0;
    gm_refresh_published_accels(gm);
    return G_SOURCE_REMOVE;
}

/* The app published (or changed) its shortcut list after we built the menus */
static GdkFilterReturn gm_accel_prop_filter(GdkXEvent *xevent, GdkEvent *event, gpointer data) {
    GlobalMenuWidget *gm = (GlobalMenuWidget *)data;
    XEvent *xev = (XEvent *)xevent;
    if (xev->type != PropertyNotify || !gm->watched_xid) return GDK_FILTER_CONTINUE;
    if (xev->xproperty.window != (Window)gm->watched_xid) return GDK_FILTER_CONTINUE;
    static Atom accel_atom = None;
    if (accel_atom == None) accel_atom = XInternAtom(xev->xproperty.display, "_ULTIMATE_MENU_ACCELS", False);
    if (xev->xproperty.atom == accel_atom && !gm->accel_refresh_id) {
        gm->accel_refresh_id = g_idle_add(on_accel_refresh_idle, gm);
    }
    return GDK_FILTER_CONTINUE;
}

/* Listen for property changes on the focused app window, keeping the event mask
   libwnck already set on it (we only ADD PropertyChangeMask) */
static void gm_watch_accels_on(GlobalMenuWidget *gm, gulong xid) {
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy)) return;
    if (!gm->accel_filter_added) {
        gdk_window_add_filter(NULL, gm_accel_prop_filter, gm);
        gm->accel_filter_added = TRUE;
    }
    gm->watched_xid = xid;
    if (!xid) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    XWindowAttributes wa;
    gdk_x11_display_error_trap_push(gdk_dpy);
    if (XGetWindowAttributes(dpy, (Window)xid, &wa)) {
        if (!(wa.your_event_mask & PropertyChangeMask)) {
            XSelectInput(dpy, (Window)xid, wa.your_event_mask | PropertyChangeMask);
        }
    }
    XSync(dpy, False);
    gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
}

/* dbusmenu "shortcut": aas, e.g. [["Control","Shift","s"]] */
static void gm_apply_dbusmenu_shortcut(GtkWidget *item, GVariant *props) {
    GVariant *sc = g_variant_lookup_value(props, "shortcut", G_VARIANT_TYPE("aas"));
    if (!sc) return;
    if (g_variant_n_children(sc) > 0) {
        GVariant *first = g_variant_get_child_value(sc, 0);
        gsize n = 0;
        const gchar **parts = g_variant_get_strv(first, &n);
        GdkModifierType mods = 0;
        guint key = 0;
        for (gsize i = 0; i < n; i++) {
            const gchar *p = parts[i];
            if (g_ascii_strcasecmp(p, "Control") == 0) mods |= GDK_CONTROL_MASK;
            else if (g_ascii_strcasecmp(p, "Shift") == 0) mods |= GDK_SHIFT_MASK;
            else if (g_ascii_strcasecmp(p, "Alt") == 0) mods |= GDK_MOD1_MASK;
            else if (g_ascii_strcasecmp(p, "Super") == 0) mods |= GDK_SUPER_MASK;
            else key = gdk_keyval_from_name(p);
        }
        if (key == GDK_KEY_VoidSymbol) key = 0;
        gm_set_item_accel(item, gdk_keyval_to_lower(key), mods);
        g_free(parts);
        g_variant_unref(first);
    }
    g_variant_unref(sc);
}


/* =========================================================================
 * Built-in Finder / Desktop Menu Bar when no application window is active
 * ========================================================================= */
static void on_finder_cmd_activate(GtkMenuItem *mi, const gchar *cmd) {
    if (cmd && cmd[0] != '\0') {
        g_spawn_command_line_async(cmd, NULL);
    }
}

static void on_finder_show_desktop(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    if (gm && gm->tracker && gm->tracker->screen) {
        gboolean showing = wnck_screen_get_showing_desktop(gm->tracker->screen);
        wnck_screen_toggle_showing_desktop(gm->tracker->screen, !showing);
    }
}

static void on_finder_restore_all(GtkMenuItem *mi, GlobalMenuWidget *gm) {
    if (!gm || !gm->tracker || !gm->tracker->screen) return;
    WnckWorkspace *ws = wnck_screen_get_active_workspace(gm->tracker->screen);
    GList *wins = wnck_screen_get_windows(gm->tracker->screen);
    for (GList *l = wins; l != NULL; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        if (wnck_window_get_window_type(w) == WNCK_WINDOW_NORMAL &&
            (!ws || wnck_window_is_on_workspace(w, ws)) &&
            wnck_window_is_minimized(w)) {
            wnck_window_unminimize(w, gtk_get_current_event_time());
        }
    }
}

static GtkWidget *add_finder_top_menu(GtkWidget *menubar, const gchar *title) {
    GtkWidget *top = gtk_menu_item_new_with_label(title);
    GtkWidget *sub = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(sub), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "ultimate-themed-menu");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(top), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), top);
    return sub;
}

static void add_finder_sub_item(GtkWidget *sub, const gchar *label, const gchar *cmd) {
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", G_CALLBACK(on_finder_cmd_activate), (gpointer)cmd);
    gtk_menu_shell_append(GTK_MENU_SHELL(sub), item);
}

static void load_finder_desktop_menubar(GlobalMenuWidget *gm) {
    if (gm->active_xid == 0xFFFFFFFF && gm->menubar != NULL) return;
    clear_current_menubar(gm);
    gm->active_xid = 0xFFFFFFFF;

    gm->menubar = gtk_menu_bar_new();
    gtk_widget_set_name(gm->menubar, "ultimate-global-menubar");
    apply_clean_menubar_css(gm->menubar);

    GtkWidget *m_file = add_finder_top_menu(gm->menubar, "File");
    add_finder_sub_item(m_file, "New Finder Window", "exo-open --launch FileManager");
    add_finder_sub_item(m_file, "New Folder on Desktop", "sh -c 'mkdir -p \"$HOME/Desktop/New Folder\"'");
    gtk_menu_shell_append(GTK_MENU_SHELL(m_file), gtk_separator_menu_item_new());
    add_finder_sub_item(m_file, "Open Terminal", "exo-open --launch TerminalEmulator");

    GtkWidget *m_edit = add_finder_top_menu(gm->menubar, "Edit");
    add_finder_sub_item(m_edit, "Preferences...", "xfce4-settings-manager");

    GtkWidget *m_view = add_finder_top_menu(gm->menubar, "View");
    add_finder_sub_item(m_view, "Desktop Settings...", "xfdesktop-settings");
    add_finder_sub_item(m_view, "Display Settings...", "xfce4-display-settings");

    GtkWidget *m_go = add_finder_top_menu(gm->menubar, "Go");
    add_finder_sub_item(m_go, "Home", "exo-open --launch FileManager ~");
    add_finder_sub_item(m_go, "Desktop", "sh -c 'exo-open --launch FileManager \"$HOME/Desktop\"'");
    add_finder_sub_item(m_go, "Documents", "sh -c 'exo-open --launch FileManager \"$HOME/Documents\"'");
    add_finder_sub_item(m_go, "Downloads", "sh -c 'exo-open --launch FileManager \"$HOME/Downloads\"'");
    add_finder_sub_item(m_go, "Trash", "exo-open --launch FileManager trash:///");

    GtkWidget *m_win = add_finder_top_menu(gm->menubar, "Window");
    GtkWidget *it_show_desk = gtk_menu_item_new_with_label("Toggle Show Desktop");
    g_signal_connect(it_show_desk, "activate", G_CALLBACK(on_finder_show_desktop), gm);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_win), it_show_desk);

    GtkWidget *it_restore = gtk_menu_item_new_with_label("Restore Minimized Windows");
    g_signal_connect(it_restore, "activate", G_CALLBACK(on_finder_restore_all), gm);
    gtk_menu_shell_append(GTK_MENU_SHELL(m_win), it_restore);

    GtkWidget *m_help = add_finder_top_menu(gm->menubar, "Help");
    add_finder_sub_item(m_help, "About XFCE", "xfce4-about");

    gm_decorate_menubar(gm);

    gtk_box_pack_start(GTK_BOX(gm->holder_box), gm->menubar, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(gm->holder_box), "gm-refit", GINT_TO_POINTER(1));
    gm_queue_fit(gm);
    gtk_widget_show_all(gm->menubar);
}

/* =========================================================================
 * ENGINE 1: Direct com.canonical.dbusmenu Parser & Event Dispatcher
 * Works with Qt5/Qt6, Electron, LibreOffice, GTK2, and GTK3 (appmenu-gtk-module)
 * ========================================================================= */
typedef struct {
    gchar *bus_name;
    gchar *obj_path;
    gint32 item_id;
} DBusMenuCallData;

static void free_dbusmenu_call_data(gpointer data, GClosure *closure) {
    DBusMenuCallData *d = (DBusMenuCallData *)data;
    if (!d) return;
    g_free(d->bus_name);
    g_free(d->obj_path);
    g_free(d);
}

static void on_dbusmenu_leaf_activate(GtkMenuItem *mi, DBusMenuCallData *d) {
    if (!g_gm || !g_gm->session_bus || !d || !d->bus_name || !d->obj_path) return;

    /* Pass 0 (GDK_CURRENT_TIME) so appmenu-gtk-module / GTK3 never rejects cross-process X11 event timestamps */
    g_dbus_connection_call(
        g_gm->session_bus,
        d->bus_name,
        d->obj_path,
        "com.canonical.dbusmenu",
        "Event",
        g_variant_new("(isvu)", d->item_id, "clicked", g_variant_new_int32(0), (guint32)0),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        NULL,
        NULL
    );
}

static gboolean on_delayed_layout_reload(gpointer user_data) {
    GlobalMenuWidget *gm = (GlobalMenuWidget *)user_data;
    gm->reload_timer_id = 0;
    if (gm->open_menus_count > 0) {
        gm->reload_pending = TRUE;
        return G_SOURCE_REMOVE;
    }
    if (gm->active_bus && gm->active_path && gm->active_xid != 0 && gm->active_xid != 0xFFFFFFFF) {
        update_menu_for_active_window(gm, TRUE);
    }
    return G_SOURCE_REMOVE;
}

static void sync_submenu_children_sensitivity(GtkWidget *menu, GVariant *node_tuple) {
    if (!GTK_IS_CONTAINER(menu) || !node_tuple || !g_variant_is_of_type(node_tuple, G_VARIANT_TYPE("(ia{sv}av)"))) return;
    gint32 pid = 0;
    GVariant *pprops = NULL, *children = NULL;
    g_variant_get(node_tuple, "(i@a{sv}@av)", &pid, &pprops, &children);

    GHashTable *en_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    gsize n = g_variant_n_children(children);
    for (gsize i = 0; i < n; i++) {
        GVariant *cv = g_variant_get_child_value(children, i);
        GVariant *unboxed = g_variant_get_variant(cv);
        if (g_variant_is_of_type(unboxed, G_VARIANT_TYPE("(ia{sv}av)"))) {
            gint32 cid = 0;
            GVariant *cprops = NULL, *cch = NULL;
            g_variant_get(unboxed, "(i@a{sv}@av)", &cid, &cprops, &cch);
            gboolean en = TRUE;
            g_variant_lookup(cprops, "enabled", "b", &en);
            g_hash_table_replace(en_map, GINT_TO_POINTER(cid + 1), GINT_TO_POINTER(en ? 1 : 0));
            g_variant_unref(cprops);
            g_variant_unref(cch);
        }
        g_variant_unref(unboxed);
        g_variant_unref(cv);
    }

    GList *w_children = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *l = w_children; l != NULL; l = l->next) {
        GtkWidget *w = GTK_WIDGET(l->data);
        gpointer tag = g_object_get_data(G_OBJECT(w), "dm-item-id");
        if (tag && g_hash_table_contains(en_map, tag)) {
            gboolean en = (GPOINTER_TO_INT(g_hash_table_lookup(en_map, tag)) != 0);
            gtk_widget_set_sensitive(w, en);
        }
    }
    g_list_free(w_children);
    g_hash_table_destroy(en_map);
    g_variant_unref(pprops);
    g_variant_unref(children);
}

static void on_submenu_Shown(GtkWidget *menu, DBusMenuCallData *d) {
    if (!g_gm) return;
    g_gm->open_menus_count++;
    if (d && g_gm->session_bus && d->bus_name && d->obj_path) {
        /* Query AboutToShow + immediate 1-level GetLayout for this submenu so enabled/disabled states are live */
        g_dbus_connection_call_sync(
            g_gm->session_bus, d->bus_name, d->obj_path,
            "com.canonical.dbusmenu", "AboutToShow",
            g_variant_new("(i)", d->item_id), NULL, G_DBUS_CALL_FLAGS_NONE, 80, NULL, NULL
        );
        const gchar *props_req[] = { "enabled", "toggle-state", NULL };
        GVariant *lret = g_dbus_connection_call_sync(
            g_gm->session_bus, d->bus_name, d->obj_path,
            "com.canonical.dbusmenu", "GetLayout",
            g_variant_new("(ii^as)", d->item_id, 1, props_req),
            G_VARIANT_TYPE("(u(ia{sv}av))"),
            G_DBUS_CALL_FLAGS_NONE, 80, NULL, NULL
        );
        if (lret) {
            guint32 rev = 0;
            GVariant *sub_node = NULL;
            g_variant_get(lret, "(u@(ia{sv}av))", &rev, &sub_node);
            sync_submenu_children_sensitivity(menu, sub_node);
            g_variant_unref(sub_node);
            g_variant_unref(lret);
        }
        g_dbus_connection_call(
            g_gm->session_bus,
            d->bus_name,
            d->obj_path,
            "com.canonical.dbusmenu",
            "Event",
            g_variant_new("(isvu)", d->item_id, "opened", g_variant_new_int32(0), (guint32)0),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            NULL,
            NULL,
            NULL
        );
    }
}

static gboolean send_delayed_submenu_closed_cb(gpointer data) {
    DBusMenuCallData *d = (DBusMenuCallData *)data;
    if (g_gm && g_gm->session_bus && d && d->bus_name && d->obj_path) {
        g_dbus_connection_call(
            g_gm->session_bus,
            d->bus_name,
            d->obj_path,
            "com.canonical.dbusmenu",
            "Event",
            g_variant_new("(isvu)", d->item_id, "closed", g_variant_new_int32(0), (guint32)0),
            NULL,
            G_DBUS_CALL_FLAGS_NONE,
            1000,
            NULL,
            NULL,
            NULL
        );
    }
    if (d) {
        g_free(d->bus_name);
        g_free(d->obj_path);
        g_free(d);
    }
    return G_SOURCE_REMOVE;
}

static void on_submenu_Hidden(GtkWidget *menu, DBusMenuCallData *d) {
    if (!g_gm) return;
    if (g_gm->open_menus_count > 0) g_gm->open_menus_count--;
    if (d && g_gm->session_bus && d->bus_name && d->obj_path) {
        /* Delay "closed" by 60ms so leaf item "clicked" events are processed while the GTK submenu is still open */
        DBusMenuCallData *copy = g_new0(DBusMenuCallData, 1);
        copy->bus_name = g_strdup(d->bus_name);
        copy->obj_path = g_strdup(d->obj_path);
        copy->item_id  = d->item_id;
        g_timeout_add(60, send_delayed_submenu_closed_cb, copy);
    }
    if (g_gm->open_menus_count == 0 && g_gm->reload_pending) {
        g_gm->reload_pending = FALSE;
        if (g_gm->reload_timer_id == 0) {
            g_gm->reload_timer_id = g_timeout_add(150, on_delayed_layout_reload, g_gm);
        }
    }
}

/* Recursively builds native GtkMenu items from a D-Bus (ia{sv}av) node */
static void build_gtk_menu_from_layout_tuple(
    GlobalMenuWidget *gm,
    GtkMenuShell     *parent_shell,
    GVariant         *node_tuple,
    const gchar      *bus_name,
    const gchar      *obj_path,
    gboolean          is_root)
{
    if (!node_tuple || !g_variant_is_of_type(node_tuple, G_VARIANT_TYPE("(ia{sv}av)"))) return;

    gint32 item_id = 0;
    GVariant *props = NULL;
    GVariant *children = NULL;
    g_variant_get(node_tuple, "(i@a{sv}@av)", &item_id, &props, &children);
    gm_debug_log_item("dbusmenu", props);

    if (is_root) {
        gsize n_children = g_variant_n_children(children);
        for (gsize i = 0; i < n_children; i++) {
            GVariant *child_v = g_variant_get_child_value(children, i);
            GVariant *unboxed = g_variant_get_variant(child_v);
            build_gtk_menu_from_layout_tuple(gm, parent_shell, unboxed, bus_name, obj_path, FALSE);
            g_variant_unref(unboxed);
            g_variant_unref(child_v);
        }
        g_variant_unref(props);
        g_variant_unref(children);
        return;
    }

    /* Extract properties from a{sv} */
    gboolean visible = TRUE;
    gboolean enabled = TRUE;
    const gchar *type_str = NULL;
    const gchar *raw_label = NULL;
    const gchar *children_display = NULL;
    const gchar *toggle_type = NULL;
    gint32 toggle_state = 0;

    g_variant_lookup(props, "visible", "b", &visible);
    g_variant_lookup(props, "enabled", "b", &enabled);
    g_variant_lookup(props, "type", "&s", &type_str);
    g_variant_lookup(props, "label", "&s", &raw_label);
    g_variant_lookup(props, "children-display", "&s", &children_display);
    g_variant_lookup(props, "toggle-type", "&s", &toggle_type);
    g_variant_lookup(props, "toggle-state", "i", &toggle_state);

    if (!visible) {
        g_variant_unref(props);
        g_variant_unref(children);
        return;
    }

    if (g_strcmp0(type_str, "separator") == 0) {
        GtkWidget *sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(parent_shell, sep);
        gtk_widget_show(sep);
        g_variant_unref(props);
        g_variant_unref(children);
        return;
    }

    gchar *clean_lbl = clean_menu_label(raw_label ? raw_label : "");
    gsize n_children = g_variant_n_children(children);
    gboolean has_submenu = (n_children > 0) || (g_strcmp0(children_display, "submenu") == 0);

    /* Skip empty non-separator items on the top menu bar */
    if (GTK_IS_MENU_BAR(parent_shell) && clean_lbl[0] == '\0') {
        g_free(clean_lbl);
        g_variant_unref(props);
        g_variant_unref(children);
        return;
    }

    GtkWidget *item_w = NULL;
    if (!GTK_IS_MENU_BAR(parent_shell) && toggle_type &&
        (g_strcmp0(toggle_type, "checkmark") == 0 || g_strcmp0(toggle_type, "radio") == 0)) {
        item_w = gtk_check_menu_item_new_with_label(clean_lbl);
        if (g_strcmp0(toggle_type, "radio") == 0) {
            gtk_check_menu_item_set_draw_as_radio(GTK_CHECK_MENU_ITEM(item_w), TRUE);
        }
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item_w), (toggle_state == 1));
    } else {
        item_w = gtk_menu_item_new_with_label(clean_lbl);
    }
    g_free(clean_lbl);

    GtkWidget *lbl_child = gtk_bin_get_child(GTK_BIN(item_w));
    if (GTK_IS_LABEL(lbl_child)) {
        gtk_label_set_use_underline(GTK_LABEL(lbl_child), FALSE);
    }

    gtk_widget_set_sensitive(item_w, enabled);
    if (!GTK_IS_MENU_BAR(parent_shell)) gm_apply_dbusmenu_shortcut(item_w, props);

    if (has_submenu) {
        GtkWidget *sub_menu = gtk_menu_new();
        gtk_menu_set_reserve_toggle_size(GTK_MENU(sub_menu), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(sub_menu), "ultimate-themed-menu");

        for (gsize i = 0; i < n_children; i++) {
            GVariant *child_v = g_variant_get_child_value(children, i);
            GVariant *unboxed = g_variant_get_variant(child_v);
            build_gtk_menu_from_layout_tuple(gm, GTK_MENU_SHELL(sub_menu), unboxed, bus_name, obj_path, FALSE);
            g_variant_unref(unboxed);
            g_variant_unref(child_v);
        }

        gtk_menu_item_set_submenu(GTK_MENU_ITEM(item_w), sub_menu);

        DBusMenuCallData *sub_data = g_new0(DBusMenuCallData, 1);
        sub_data->bus_name = g_strdup(bus_name);
        sub_data->obj_path = g_strdup(obj_path);
        sub_data->item_id  = item_id;

        g_signal_connect_data(sub_menu, "show", G_CALLBACK(on_submenu_Shown), sub_data, free_dbusmenu_call_data, 0);
        g_signal_connect(sub_menu, "hide", G_CALLBACK(on_submenu_Hidden), sub_data);

        /* Pre-trigger AboutToShow in background when building top bar so apps that lazily
           populate submenus send their items before the user even clicks the menu! */
        if (GTK_IS_MENU_BAR(parent_shell) && n_children == 0 && gm->session_bus) {
            g_dbus_connection_call(
                gm->session_bus,
                bus_name,
                obj_path,
                "com.canonical.dbusmenu",
                "AboutToShow",
                g_variant_new("(i)", item_id),
                NULL,
                G_DBUS_CALL_FLAGS_NONE,
                1000,
                NULL,
                NULL,
                NULL
            );
        }
    } else {
        DBusMenuCallData *click_data = g_new0(DBusMenuCallData, 1);
        click_data->bus_name = g_strdup(bus_name);
        click_data->obj_path = g_strdup(obj_path);
        click_data->item_id  = item_id;

        g_object_set_data(G_OBJECT(item_w), "dm-item-id", GINT_TO_POINTER(item_id + 1));
        g_signal_connect_data(item_w, "activate", G_CALLBACK(on_dbusmenu_leaf_activate),
                              click_data, free_dbusmenu_call_data, 0);
    }

    gtk_menu_shell_append(parent_shell, item_w);
    gtk_widget_show_all(item_w);

    g_variant_unref(props);
    g_variant_unref(children);
}


/* Find the menu item built for dbusmenu id (tag = id + 1) anywhere in the bar,
   its open sub-menus and the » overflow menu */
static GtkWidget *gm_find_item(GtkWidget *shell, gpointer tag) {
    if (!GTK_IS_CONTAINER(shell)) return NULL;
    GtkWidget *hit = NULL;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(shell));
    for (GList *l = kids; l && !hit; l = l->next) {
        GtkWidget *w = l->data;
        if (g_object_get_data(G_OBJECT(w), "dm-item-id") == tag) { hit = w; break; }
        if (GTK_IS_MENU_ITEM(w)) {
            GtkWidget *sub = gtk_menu_item_get_submenu(GTK_MENU_ITEM(w));
            if (sub) hit = gm_find_item(sub, tag);
        }
    }
    g_list_free(kids);
    return hit;
}

static void gm_apply_live_properties(GlobalMenuWidget *gm, GVariant *parameters) {
    if (!gm || !GTK_IS_WIDGET(gm->menubar)) return;
    GVariant *updated = g_variant_get_child_value(parameters, 0);
    GVariantIter it;
    gint32 id;
    GVariant *props;
    g_variant_iter_init(&it, updated);
    while (g_variant_iter_next(&it, "(i@a{sv})", &id, &props)) {
        GtkWidget *w = gm_find_item(gm->menubar, GINT_TO_POINTER(id + 1));
        if (w) {
            gboolean en;
            gint32 ts;
            if (g_variant_lookup(props, "enabled", "b", &en)) gtk_widget_set_sensitive(w, en);
            if (GTK_IS_CHECK_MENU_ITEM(w) && g_variant_lookup(props, "toggle-state", "i", &ts)) {
                g_signal_handlers_block_matched(w, G_SIGNAL_MATCH_ID, g_signal_lookup("activate", GTK_TYPE_MENU_ITEM), 0, NULL, NULL, NULL);
                gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(w), ts == 1);
                g_signal_handlers_unblock_matched(w, G_SIGNAL_MATCH_ID, g_signal_lookup("activate", GTK_TYPE_MENU_ITEM), 0, NULL, NULL, NULL);
            }
        }
        g_variant_unref(props);
    }
    g_variant_unref(updated);
}

static void on_dbusmenu_signal_received(
    GDBusConnection *connection,
    const gchar     *sender_name,
    const gchar     *object_path,
    const gchar     *interface_name,
    const gchar     *signal_name,
    GVariant        *parameters,
    gpointer         user_data)
{
    GlobalMenuWidget *gm = (GlobalMenuWidget *)user_data;
    if (!gm || !gm->active_path || g_strcmp0(object_path, gm->active_path) != 0) return;

    /* A menu is open: the app may be enabling its items right now (it does that when
       it hears AboutToShow). Apply those changes to the open menus at once instead of
       waiting for a reload after they close - otherwise items stay greyed out. */
    if (g_strcmp0(signal_name, "ItemsPropertiesUpdated") == 0 && gm->open_menus_count > 0
        && g_variant_is_of_type(parameters, G_VARIANT_TYPE("(a(ia{sv})a(ias))"))) {
        gm_apply_live_properties(gm, parameters);
        gm->reload_pending = TRUE;          /* still refresh fully once the menus close */
        return;
    }
    if (g_strcmp0(signal_name, "LayoutUpdated") == 0 ||
        g_strcmp0(signal_name, "ItemsPropertiesUpdated") == 0) {
        if (gm->open_menus_count > 0) {
            gm->reload_pending = TRUE;
        } else if (gm->reload_timer_id == 0) {
            gm->reload_timer_id = g_timeout_add(120, on_delayed_layout_reload, gm);
        }
    }
}

static gboolean load_direct_dbusmenu(GlobalMenuWidget *gm, guint xid, const gchar *bus_name, const gchar *obj_path) {
    if (!gm || !gm->session_bus || !bus_name || !obj_path || bus_name[0] == '\0' || g_strcmp0(obj_path, "/") == 0) {
        return FALSE;
    }

    /* Wake up root menu (id=0) first so cold-start GTK/Qt apps populate their top-level items */
    g_dbus_connection_call_sync(
        gm->session_bus,
        bus_name,
        obj_path,
        "com.canonical.dbusmenu",
        "AboutToShow",
        g_variant_new("(i)", 0),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        120,
        NULL,
        NULL
    );

    /* Call GetLayout(0, -1, []) synchronously with a short 250ms timeout to fetch the entire tree */
    const gchar *empty_props[] = { NULL };
    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        gm->session_bus,
        bus_name,
        obj_path,
        "com.canonical.dbusmenu",
        "GetLayout",
        g_variant_new("(ii^as)", 0, -1, empty_props),
        G_VARIANT_TYPE("(u(ia{sv}av))"),
        G_DBUS_CALL_FLAGS_NONE,
        250,
        NULL,
        &err
    );

    if (err || !ret) {
        if (err) g_error_free(err);
        return FALSE;
    }

    guint32 revision = 0;
    GVariant *root_layout = NULL;
    g_variant_get(ret, "(u@(ia{sv}av))", &revision, &root_layout);

    GtkWidget *test_bar = gtk_menu_bar_new();
    gtk_widget_set_name(test_bar, "ultimate-global-menubar");
    apply_clean_menubar_css(test_bar);

    build_gtk_menu_from_layout_tuple(gm, GTK_MENU_SHELL(test_bar), root_layout, bus_name, obj_path, TRUE);

    GList *top_children = gtk_container_get_children(GTK_CONTAINER(test_bar));
    guint n_top = g_list_length(top_children);
    g_list_free(top_children);

    if (n_top == 0) {
        /* Menu tree is still empty on cold start; destroy temporary bar and return FALSE so retry poller continues! */
        g_object_ref_sink(test_bar);
        gtk_widget_destroy(test_bar);
        g_variant_unref(root_layout);
        g_variant_unref(ret);
        return FALSE;
    }

    clear_current_menubar(gm);
    gm->active_xid = xid;
    gm->active_bus = g_strdup(bus_name);
    gm->active_path = g_strdup(obj_path);
    gm->active_revision = revision;
    gm->menubar = test_bar;

    gm_decorate_menubar(gm);

    gtk_box_pack_start(GTK_BOX(gm->holder_box), gm->menubar, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(gm->holder_box), "gm-refit", GINT_TO_POINTER(1));
    gm_queue_fit(gm);
    gtk_widget_show_all(gm->menubar);

    /* Subscribe to LayoutUpdated and ItemsPropertiesUpdated when menus are closed */
    gm->dbusmenu_sig_sub_id = g_dbus_connection_signal_subscribe(
        gm->session_bus,
        bus_name,
        "com.canonical.dbusmenu",
        NULL,
        obj_path,
        NULL,
        G_DBUS_SIGNAL_FLAGS_NONE,
        on_dbusmenu_signal_received,
        gm,
        NULL
    );

    g_variant_unref(root_layout);
    g_variant_unref(ret);
    return TRUE;
}

/* =========================================================================
 * ENGINE 2: Direct org.gtk.Menus + org.gtk.Actions Client
 * For Native GTK3 Apps (_GTK_MENUBAR_OBJECT_PATH / _GTK_UNITY_OBJECT_PATH)
 * ========================================================================= */
typedef struct {
    gchar    *bus_name;
    gchar    *action_obj_path;
    gchar    *action_name;
    GVariant *target_param;
} GtkActionCallData;

static void free_gtk_action_call_data(gpointer data, GClosure *closure) {
    GtkActionCallData *d = (GtkActionCallData *)data;
    if (!d) return;
    g_free(d->bus_name);
    g_free(d->action_obj_path);
    g_free(d->action_name);
    if (d->target_param) g_variant_unref(d->target_param);
    g_free(d);
}

static void on_gtk_direct_action_activate(GtkMenuItem *mi, GtkActionCallData *d) {
    if (!g_gm || !g_gm->session_bus || !d || !d->bus_name || !d->action_obj_path || !d->action_name) return;

    GVariantBuilder param_builder;
    g_variant_builder_init(&param_builder, G_VARIANT_TYPE("av"));
    if (d->target_param) {
        g_variant_builder_add(&param_builder, "v", d->target_param);
    }

    GVariantBuilder platform_builder;
    g_variant_builder_init(&platform_builder, G_VARIANT_TYPE("a{sv}"));

    /* Call org.gtk.Actions.Activate(action_name, [target_param], platform_data) directly! */
    g_dbus_connection_call(
        g_gm->session_bus,
        d->bus_name,
        d->action_obj_path,
        "org.gtk.Actions",
        "Activate",
        g_variant_new("(sav@a{sv})", d->action_name, &param_builder, g_variant_builder_end(&platform_builder)),
        NULL,
        G_DBUS_CALL_FLAGS_NONE,
        2000,
        NULL,
        NULL,
        NULL
    );
}

/* Fetch all action enabled states from org.gtk.Actions.DescribeAll() */
static void fetch_gtk_actions_into_table(GDBusConnection *bus, const gchar *bus_name, const gchar *obj_path,
                                         const gchar *prefix, GHashTable *enabled_table) {
    if (!bus || !bus_name || !obj_path || obj_path[0] == '\0') return;
    GVariant *ret = g_dbus_connection_call_sync(
        bus, bus_name, obj_path, "org.gtk.Actions", "DescribeAll",
        NULL, G_VARIANT_TYPE("(a{s(bgav)})"), G_DBUS_CALL_FLAGS_NONE, 200, NULL, NULL
    );
    if (!ret) return;

    GVariant *dict = g_variant_get_child_value(ret, 0);
    GVariantIter iter;
    const gchar *act_name = NULL;
    GVariant *desc_tuple = NULL;
    g_variant_iter_init(&iter, dict);
    while (g_variant_iter_next(&iter, "{&s@(bgav)}", &act_name, &desc_tuple)) {
        gboolean enabled = TRUE;
        g_variant_get_child(desc_tuple, 0, "b", &enabled);
        gint val = enabled ? 1 : -1;
        g_hash_table_replace(enabled_table, g_strdup_printf("%s.%s", prefix, act_name), GINT_TO_POINTER(val));
        g_hash_table_replace(enabled_table, g_strdup(act_name), GINT_TO_POINTER(val));
        g_variant_unref(desc_tuple);
    }
    g_variant_unref(dict);
    g_variant_unref(ret);
}

static void populate_gtk_menus_group(
    GlobalMenuWidget *gm,
    GtkMenuShell     *shell,
    GHashTable       *groups_map,
    GHashTable       *enabled_table,
    const gchar      *bus_name,
    const gchar      *app_path,
    const gchar      *win_path,
    const gchar      *unity_path,
    guint             sub_id,
    guint             menu_idx,
    gint              depth)
{
    if (depth > 8) return;
    gchar *key = g_strdup_printf("%u:%u", sub_id, menu_idx);
    GVariant *items_array = g_hash_table_lookup(groups_map, key);
    g_free(key);
    if (!items_array) return;

    gsize n_items = g_variant_n_children(items_array);
    for (gsize i = 0; i < n_items; i++) {
        GVariant *item_dict = g_variant_get_child_value(items_array, i);
        gm_debug_log_item("gtk-menus", item_dict);

        const gchar *raw_label = NULL;
        const gchar *action_str = NULL;
        GVariant *target_val = NULL;
        GVariant *section_tup = NULL;
        GVariant *submenu_tup = NULL;

        g_variant_lookup(item_dict, "label", "&s", &raw_label);
        g_variant_lookup(item_dict, "action", "&s", &action_str);
        target_val  = g_variant_lookup_value(item_dict, "target", NULL);
        section_tup = g_variant_lookup_value(item_dict, ":section", G_VARIANT_TYPE("(uu)"));
        submenu_tup = g_variant_lookup_value(item_dict, ":submenu", G_VARIANT_TYPE("(uu)"));

        if (section_tup) {
            guint sec_sub = 0, sec_idx = 0;
            g_variant_get(section_tup, "(uu)", &sec_sub, &sec_idx);
            if (!GTK_IS_MENU_BAR(shell)) {
                GList *existing = gtk_container_get_children(GTK_CONTAINER(shell));
                if (existing != NULL) {
                    GtkWidget *sep = gtk_separator_menu_item_new();
                    gtk_menu_shell_append(shell, sep);
                    gtk_widget_show(sep);
                }
                g_list_free(existing);
            }
            populate_gtk_menus_group(gm, shell, groups_map, enabled_table,
                                     bus_name, app_path, win_path, unity_path,
                                     sec_sub, sec_idx, depth + 1);
            g_variant_unref(section_tup);
            if (submenu_tup) g_variant_unref(submenu_tup);
            if (target_val) g_variant_unref(target_val);
            g_variant_unref(item_dict);
            continue;
        }

        if (raw_label) {
            gchar *clean_lbl = clean_menu_label(raw_label);
            GtkWidget *mi = gtk_menu_item_new_with_label(clean_lbl);
            gchar *clean_for_accel = g_strdup(clean_lbl);
            g_free(clean_lbl);

            GtkWidget *lbl_w = gtk_bin_get_child(GTK_BIN(mi));
            if (GTK_IS_LABEL(lbl_w)) gtk_label_set_use_underline(GTK_LABEL(lbl_w), FALSE);
            {
                const gchar *accel = NULL;
                /* GTK's own GMenuModel uses "accel"; appmenu-gtk-module / unity-gtk-module (how GTK apps
                   like Thunar export their menus) uses Canonical's "x-canonical-accel" */
                if (GTK_IS_MENU(shell) &&
                    ((g_variant_lookup(item_dict, "accel", "&s", &accel) && accel && accel[0]) ||
                     (g_variant_lookup(item_dict, "x-canonical-accel", "&s", &accel) && accel && accel[0]))) {
                    guint k = 0;
                    GdkModifierType m = 0;
                    gtk_accelerator_parse(accel, &k, &m);
                    gm_set_item_accel(mi, k, m);
                }
                if (GTK_IS_MENU(shell)) gm_apply_published_accel(gm, mi, clean_for_accel);
                g_free(clean_for_accel);
            }

            if (submenu_tup) {
                guint sm_sub = 0, sm_idx = 0;
                g_variant_get(submenu_tup, "(uu)", &sm_sub, &sm_idx);
                GtkWidget *sub_menu = gtk_menu_new();
                gtk_menu_set_reserve_toggle_size(GTK_MENU(sub_menu), FALSE);
                gtk_style_context_add_class(gtk_widget_get_style_context(sub_menu), "ultimate-themed-menu");

                populate_gtk_menus_group(gm, GTK_MENU_SHELL(sub_menu), groups_map, enabled_table,
                                         bus_name, app_path, win_path, unity_path,
                                         sm_sub, sm_idx, depth + 1);
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(mi), sub_menu);
            } else if (action_str && action_str[0] != '\0') {
                const gchar *target_obj_path = NULL;
                const gchar *short_act = action_str;
                if (g_str_has_prefix(action_str, "unity.")) {
                    target_obj_path = unity_path ? unity_path : (win_path ? win_path : app_path);
                    short_act = action_str + 6;
                } else if (g_str_has_prefix(action_str, "win.")) {
                    target_obj_path = win_path ? win_path : (unity_path ? unity_path : app_path);
                    short_act = action_str + 4;
                } else if (g_str_has_prefix(action_str, "app.")) {
                    target_obj_path = app_path ? app_path : (unity_path ? unity_path : win_path);
                    short_act = action_str + 4;
                } else {
                    const gchar *dot = strchr(action_str, '.');
                    if (dot) short_act = dot + 1;
                    target_obj_path = unity_path ? unity_path : (win_path ? win_path : app_path);
                }

                gint en_val = GPOINTER_TO_INT(g_hash_table_lookup(enabled_table, action_str));
                if (en_val == 0 && short_act) {
                    en_val = GPOINTER_TO_INT(g_hash_table_lookup(enabled_table, short_act));
                }
                gtk_widget_set_sensitive(mi, (en_val == -1) ? FALSE : TRUE);

                if (target_obj_path) {
                    GtkActionCallData *cd = g_new0(GtkActionCallData, 1);
                    cd->bus_name = g_strdup(bus_name);
                    cd->action_obj_path = g_strdup(target_obj_path);
                    cd->action_name = g_strdup(short_act);
                    cd->target_param = target_val ? g_variant_ref(target_val) : NULL;
                    g_signal_connect_data(mi, "activate", G_CALLBACK(on_gtk_direct_action_activate),
                                          cd, free_gtk_action_call_data, 0);
                }
            }

            gtk_menu_shell_append(shell, mi);
            gtk_widget_show_all(mi);
        }

        if (submenu_tup) g_variant_unref(submenu_tup);
        if (target_val) g_variant_unref(target_val);
        g_variant_unref(item_dict);
    }
}

static gboolean load_direct_gtk_menus(GlobalMenuWidget *gm, Window xid) {
    if (!gm || !gm->session_bus || !xid) return FALSE;

    gchar *bus_name   = get_x11_string_property(xid, "_GTK_UNIQUE_BUS_NAME");
    gchar *menubar_p  = get_x11_string_property(xid, "_GTK_MENUBAR_OBJECT_PATH");
    gchar *appmenu_p  = get_x11_string_property(xid, "_GTK_APP_MENU_OBJECT_PATH");
    gchar *app_path   = get_x11_string_property(xid, "_GTK_APPLICATION_OBJECT_PATH");
    gchar *win_path   = get_x11_string_property(xid, "_GTK_WINDOW_OBJECT_PATH");
    gchar *unity_path = get_x11_string_property(xid, "_GTK_UNITY_OBJECT_PATH");

    const gchar *menu_obj_path = (menubar_p && menubar_p[0] != '\0') ? menubar_p
                               : ((appmenu_p && appmenu_p[0] != '\0') ? appmenu_p : NULL);

    if (!unity_path && menu_obj_path) {
        unity_path = g_strdup(menu_obj_path);
    }
    if (!win_path && menu_obj_path) {
        win_path = g_strdup(menu_obj_path);
    }

    if (!bus_name || !menu_obj_path) {
        g_free(bus_name); g_free(menubar_p); g_free(appmenu_p);
        g_free(app_path); g_free(win_path); g_free(unity_path);
        return FALSE;
    }

    /* Subscribe to groups 0..64 in one Start() call on org.gtk.Menus */
    GVariantBuilder sb;
    g_variant_builder_init(&sb, G_VARIANT_TYPE("au"));
    for (guint u = 0; u < 64; u++) {
        g_variant_builder_add(&sb, "u", u);
    }

    GError *err = NULL;
    GVariant *ret = g_dbus_connection_call_sync(
        gm->session_bus,
        bus_name,
        menu_obj_path,
        "org.gtk.Menus",
        "Start",
        g_variant_new("(au)", &sb),
        G_VARIANT_TYPE("(a(uuaa{sv}))"),
        G_DBUS_CALL_FLAGS_NONE,
        250,
        NULL,
        &err
    );

    if (err || !ret) {
        if (err) g_error_free(err);
        g_free(bus_name); g_free(menubar_p); g_free(appmenu_p);
        g_free(app_path); g_free(win_path); g_free(unity_path);
        return FALSE;
    }

    /* Keep org.gtk.Menus subscription active while this window is focused so unity.* / win.* actions stay bound! */

    GHashTable *groups_map = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_variant_unref);
    GVariant *groups_arr = g_variant_get_child_value(ret, 0);
    gsize n_groups = g_variant_n_children(groups_arr);
    for (gsize i = 0; i < n_groups; i++) {
        GVariant *gtup = g_variant_get_child_value(groups_arr, i);
        guint sub_id = 0, menu_idx = 0;
        GVariant *items_arr = NULL;
        g_variant_get(gtup, "(uu@aa{sv})", &sub_id, &menu_idx, &items_arr);
        gchar *k = g_strdup_printf("%u:%u", sub_id, menu_idx);
        g_hash_table_replace(groups_map, k, items_arr);
        g_variant_unref(gtup);
    }
    g_variant_unref(groups_arr);
    g_variant_unref(ret);

    if (n_groups == 0) {
        g_hash_table_destroy(groups_map);
        g_free(bus_name); g_free(menubar_p); g_free(appmenu_p);
        g_free(app_path); g_free(win_path); g_free(unity_path);
        return FALSE;
    }

    GHashTable *enabled_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    fetch_gtk_actions_into_table(gm->session_bus, bus_name, app_path, "app", enabled_table);
    fetch_gtk_actions_into_table(gm->session_bus, bus_name, win_path, "win", enabled_table);
    fetch_gtk_actions_into_table(gm->session_bus, bus_name, unity_path, "unity", enabled_table);

    clear_current_menubar(gm);
    gm->active_xid = (guint)xid;
    gm->active_bus = g_strdup(bus_name);
    gm->active_path = g_strdup(menu_obj_path);
    gm->active_is_gtk_menus = TRUE;
    gm_load_accel_map(gm, xid);

    gm->menubar = gtk_menu_bar_new();
    gtk_widget_set_name(gm->menubar, "ultimate-global-menubar");
    apply_clean_menubar_css(gm->menubar);

    populate_gtk_menus_group(gm, GTK_MENU_SHELL(gm->menubar), groups_map, enabled_table,
                             bus_name, app_path, win_path, unity_path, 0, 0, 0);

    gm_decorate_menubar(gm);

    gtk_box_pack_start(GTK_BOX(gm->holder_box), gm->menubar, FALSE, FALSE, 0);
    g_object_set_data(G_OBJECT(gm->holder_box), "gm-refit", GINT_TO_POINTER(1));
    gm_queue_fit(gm);
    gtk_widget_show_all(gm->menubar);

    g_hash_table_destroy(enabled_table);
    g_hash_table_destroy(groups_map);
    g_free(bus_name); g_free(menubar_p); g_free(appmenu_p);
    g_free(app_path); g_free(win_path); g_free(unity_path);
    return TRUE;
}

/* =========================================================================
 * External Registrar Query + Active Window Dispatcher
 * ========================================================================= */
static gboolean query_external_registrar(GlobalMenuWidget *gm, guint xid) {
    if (!gm || !gm->ext_registrar_proxy) return FALSE;
    GVariant *ret = g_dbus_proxy_call_sync(
        gm->ext_registrar_proxy,
        "GetMenuForWindow",
        g_variant_new("(u)", xid),
        G_DBUS_CALL_FLAGS_NONE,
        150,
        NULL,
        NULL
    );
    if (!ret) return FALSE;

    const gchar *service = NULL;
    const gchar *obj_path = NULL;
    g_variant_get(ret, "(&s&o)", &service, &obj_path);
    gboolean ok = FALSE;
    if (service && service[0] != '\0' && obj_path && obj_path[0] != '\0' && g_strcmp0(obj_path, "/") != 0) {
        RegisteredMenu *rm = g_new0(RegisteredMenu, 1);
        rm->bus_name = g_strdup(service);
        rm->object_path = g_strdup(obj_path);
        g_hash_table_replace(gm->registry, GUINT_TO_POINTER(xid), rm);
        save_registry_cache(gm);
        ok = load_direct_dbusmenu(gm, xid, service, obj_path);
    }
    g_variant_unref(ret);
    return ok;
}

static gboolean on_cold_start_menu_retry_tick(gpointer user_data) {
    GlobalMenuWidget *gm = (GlobalMenuWidget *)user_data;
    if (!gm || !gm->registrar_enabled) {
        if (gm) gm->retry_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    WnckWindow *active = get_valid_active_win(gm);
    if (!active) {
        gm->retry_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    guint xid = (guint)wnck_window_get_xid(active);
    if (gm->active_xid == xid && gm->menubar != NULL) {
        gm->retry_timer_id = 0;
        return G_SOURCE_REMOVE;
    }

    gm->retry_count++;
    update_menu_for_active_window(gm, TRUE);

    if ((gm->active_xid == xid && gm->menubar != NULL) || gm->retry_count >= 8) {
        gm->retry_timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void schedule_cold_start_menu_retry(GlobalMenuWidget *gm) {
    if (!gm) return;
    gm->retry_count = 0;
    if (gm->retry_timer_id == 0) {
        gm->retry_timer_id = g_timeout_add(250, on_cold_start_menu_retry_tick, gm);
    }
}

static void stop_cold_start_menu_retry(GlobalMenuWidget *gm) {
    if (!gm) return;
    if (gm->retry_timer_id != 0) {
        g_source_remove(gm->retry_timer_id);
        gm->retry_timer_id = 0;
    }
    gm->retry_count = 0;
}

static void update_menu_for_active_window(GlobalMenuWidget *gm, gboolean force) {
    if (!gm || !gm->registrar_enabled) {
        stop_cold_start_menu_retry(gm);
        clear_current_menubar(gm);
        return;
    }

    refresh_active_app_bold_title(gm);

    WnckWindow *active = get_valid_active_win(gm);
    if (!active) {
        stop_cold_start_menu_retry(gm);
        load_finder_desktop_menubar(gm);
        return;
    }

    /* Our own settings window exports no app menu: show its name, no menus, no retry polling. */
    if (is_shell_settings_window(active)) {
        stop_cold_start_menu_retry(gm);
        clear_current_menubar(gm);
        return;
    }

    guint xid = (guint)wnck_window_get_xid(active);
    if (!force && gm->active_xid == xid && gm->menubar != NULL) {
        return;
    }

    /* 1. Try local Registrar table (com.canonical.dbusmenu) */
    RegisteredMenu *rm = g_hash_table_lookup(gm->registry, GUINT_TO_POINTER(xid));
    if (rm && rm->bus_name && rm->object_path) {
        if (load_direct_dbusmenu(gm, xid, rm->bus_name, rm->object_path)) {
            stop_cold_start_menu_retry(gm);
            return;
        }
    }

    /* 2. Try external com.canonical.AppMenu.Registrar daemon if running */
    if (query_external_registrar(gm, xid)) {
        stop_cold_start_menu_retry(gm);
        return;
    }

    /* 3. Try KDE/Qt X11 window properties (_KDE_NET_WM_APPMENU_*) */
    gchar *kde_bus  = get_x11_string_property(xid, "_KDE_NET_WM_APPMENU_SERVICE_NAME");
    gchar *kde_path = get_x11_string_property(xid, "_KDE_NET_WM_APPMENU_OBJECT_PATH");
    if (kde_bus && kde_bus[0] != '\0' && kde_path && kde_path[0] != '\0') {
        RegisteredMenu *new_rm = g_new0(RegisteredMenu, 1);
        new_rm->bus_name = kde_bus;
        new_rm->object_path = kde_path;
        g_hash_table_replace(gm->registry, GUINT_TO_POINTER(xid), new_rm);
        save_registry_cache(gm);
        if (load_direct_dbusmenu(gm, xid, new_rm->bus_name, new_rm->object_path)) {
            stop_cold_start_menu_retry(gm);
            return;
        }
    } else {
        g_free(kde_bus);
        g_free(kde_path);
    }

    /* 4. Try Direct GTK3 org.gtk.Menus + org.gtk.Actions */
    if (load_direct_gtk_menus(gm, xid)) {
        stop_cold_start_menu_retry(gm);
        return;
    }

    /* No menus (yet): show a Window/Help bar so it's never empty; keep retrying for the real ones */
    gm_build_fallback(gm, xid);
    /* Start cold-start retry poller so late D-Bus / X11 property exports appear automatically! */
    if (gm->retry_timer_id == 0) {
        schedule_cold_start_menu_retry(gm);
    }
}

static void on_active_window_changed(WnckScreen *screen, WnckWindow *prev, GlobalMenuWidget *gm) {
    stop_cold_start_menu_retry(gm);
    if (gm->tracked_active) {
        if (gm->name_sig_id) g_signal_handler_disconnect(gm->tracked_active, gm->name_sig_id);
        gm->name_sig_id = 0;
        g_object_remove_weak_pointer(G_OBJECT(gm->tracked_active), (gpointer *)&gm->tracked_active);
    }

    gm->tracked_active = get_valid_active_win(gm);
    if (gm->tracked_active) {
        g_object_add_weak_pointer(G_OBJECT(gm->tracked_active), (gpointer *)&gm->tracked_active);
        gm->name_sig_id = g_signal_connect(gm->tracked_active, "name-changed",
                                           G_CALLBACK(on_tracked_win_name_changed), gm);
    }

    update_menu_for_active_window(gm, FALSE);
    gm_watch_accels_on(gm, gm_current_app_xid(gm));
}

/* =========================================================================
 * com.canonical.AppMenu.Registrar Server & Signal Handlers
 * ========================================================================= */
static void on_ext_registrar_signal(GDBusProxy *proxy, const gchar *sender, const gchar *signal_name,
                                    GVariant *parameters, gpointer user_data) {
    GlobalMenuWidget *gm = (GlobalMenuWidget *)user_data;
    if (g_strcmp0(signal_name, "WindowRegistered") == 0) {
        guint32 xid = 0;
        const gchar *service = NULL;
        const gchar *path = NULL;
        g_variant_get(parameters, "(u&s&o)", &xid, &service, &path);
        if (xid > 0 && service && service[0] != '\0' && path) {
            RegisteredMenu *rm = g_new0(RegisteredMenu, 1);
            rm->bus_name = g_strdup(service);
            rm->object_path = g_strdup(path);
            g_hash_table_replace(gm->registry, GUINT_TO_POINTER((guint)xid), rm);
            save_registry_cache(gm);
            WnckWindow *active = get_valid_active_win(gm);
            if (active && (guint)wnck_window_get_xid(active) == (guint)xid) {
                update_menu_for_active_window(gm, TRUE);
            } else {
                schedule_cold_start_menu_retry(gm);
            }
        }
    } else if (g_strcmp0(signal_name, "WindowUnregistered") == 0) {
        guint32 xid = 0;
        g_variant_get(parameters, "(u)", &xid);
        g_hash_table_remove(gm->registry, GUINT_TO_POINTER((guint)xid));
        save_registry_cache(gm);
    }
}

static void handle_registrar_method_call(
    GDBusConnection       *connection,
    const gchar           *sender,
    const gchar           *object_path,
    const gchar           *interface_name,
    const gchar           *method_name,
    GVariant              *parameters,
    GDBusMethodInvocation *invocation,
    gpointer               user_data)
{
    GlobalMenuWidget *gm = (GlobalMenuWidget *)user_data;

    if (g_strcmp0(method_name, "RegisterWindow") == 0) {
        guint32 xid = 0;
        const gchar *menu_path = NULL;
        g_variant_get(parameters, "(u&o)", &xid, &menu_path);

        if (xid > 0 && menu_path) {
            RegisteredMenu *rm = g_new0(RegisteredMenu, 1);
            rm->bus_name = g_strdup(sender);
            rm->object_path = g_strdup(menu_path);
            g_hash_table_replace(gm->registry, GUINT_TO_POINTER((guint)xid), rm);
            save_registry_cache(gm);

            WnckWindow *active = get_valid_active_win(gm);
            if (active && (guint)wnck_window_get_xid(active) == (guint)xid) {
                update_menu_for_active_window(gm, TRUE);
            } else {
                schedule_cold_start_menu_retry(gm);
            }
        }
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "UnregisterWindow") == 0) {
        guint32 xid = 0;
        g_variant_get(parameters, "(u)", &xid);
        g_hash_table_remove(gm->registry, GUINT_TO_POINTER((guint)xid));
        save_registry_cache(gm);

        WnckWindow *active = wnck_screen_get_active_window(gm->tracker->screen);
        if (active && (guint)wnck_window_get_xid(active) == xid) {
            clear_current_menubar(gm);
        }
        g_dbus_method_invocation_return_value(invocation, NULL);

    } else if (g_strcmp0(method_name, "GetMenuForWindow") == 0) {
        guint32 xid = 0;
        g_variant_get(parameters, "(u)", &xid);
        RegisteredMenu *rm = g_hash_table_lookup(gm->registry, GUINT_TO_POINTER((guint)xid));
        if (rm) {
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(so)", rm->bus_name, rm->object_path)
            );
        } else {
            g_dbus_method_invocation_return_value(
                invocation,
                g_variant_new("(so)", "", "/")
            );
        }
    }
}

static const GDBusInterfaceVTable registrar_vtable = {
    handle_registrar_method_call,
    NULL,
    NULL,
    { 0 }
};

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    GlobalMenuWidget *gm = (GlobalMenuWidget *)user_data;
    if (gm->session_bus) g_object_unref(gm->session_bus);
    gm->session_bus = g_object_ref(connection);

    if (gm->reg_obj_id == 0) {
        gm->reg_obj_id = g_dbus_connection_register_object(
            connection,
            "/com/canonical/AppMenu/Registrar",
            gm->introspection_data->interfaces[0],
            &registrar_vtable,
            gm,
            NULL,
            NULL
        );
    }

    update_menu_for_active_window(gm, TRUE);
}

void global_menu_set_enabled(gboolean enabled) {
    if (!g_gm) return;
    GlobalMenuWidget *gm = g_gm;
    gm->registrar_enabled = enabled;

    if (enabled) {
        if (!gm->session_bus) {
            gm->session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        }
        if (gm->owner_id == 0) {
            gm->owner_id = g_bus_own_name(
                G_BUS_TYPE_SESSION,
                "com.canonical.AppMenu.Registrar",
                G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT | G_BUS_NAME_OWNER_FLAGS_REPLACE,
                on_bus_acquired,
                NULL,
                NULL,
                gm,
                NULL
            );
        }
        update_menu_for_active_window(gm, TRUE);
    } else {
        clear_current_menubar(gm);
        if (gm->reg_obj_id != 0 && gm->session_bus) {
            g_dbus_connection_unregister_object(gm->session_bus, gm->reg_obj_id);
            gm->reg_obj_id = 0;
        }
        if (gm->owner_id != 0) {
            g_bus_unown_name(gm->owner_id);
            gm->owner_id = 0;
        }
    }
}

GtkWidget *global_menu_widget_new(UltimateWindowTracker *tracker) {
    GlobalMenuWidget *gm = g_new0(GlobalMenuWidget, 1);
    g_gm = gm;
    gm->tracker = tracker;
    gm->registrar_enabled = TRUE;
    gm->session_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);

    gm->holder_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_valign(gm->holder_box, GTK_ALIGN_CENTER);

    gm->app_name_eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(gm->app_name_eb), FALSE);
    gm->app_name_lbl = gtk_label_new(NULL);
    gtk_label_set_ellipsize(GTK_LABEL(gm->app_name_lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(gm->app_name_lbl), 24);
    gtk_widget_set_margin_start(gm->app_name_lbl, 4);
    gtk_widget_set_margin_end(gm->app_name_lbl, 4);
    gtk_container_add(GTK_CONTAINER(gm->app_name_eb), gm->app_name_lbl);
    g_signal_connect(gm->app_name_eb, "button-press-event", G_CALLBACK(on_app_name_clicked), gm);

    gtk_box_pack_start(GTK_BOX(gm->holder_box), gm->app_name_eb, FALSE, FALSE, 0);
    g_signal_connect(gm->holder_box, "size-allocate", G_CALLBACK(on_gm_holder_allocate), gm);

    gm->registry = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free_registered_menu);
    load_registry_cache(gm);
    gm->introspection_data = g_dbus_node_info_new_for_xml(registrar_xml, NULL);

    if (gm->session_bus) {
        gm->ext_registrar_proxy = g_dbus_proxy_new_sync(
            gm->session_bus,
            G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES,
            NULL,
            "com.canonical.AppMenu.Registrar",
            "/com/canonical/AppMenu/Registrar",
            "com.canonical.AppMenu.Registrar",
            NULL,
            NULL
        );
        if (gm->ext_registrar_proxy) {
            g_signal_connect(gm->ext_registrar_proxy, "g-signal", G_CALLBACK(on_ext_registrar_signal), gm);
        }
    }

    global_menu_set_enabled(TRUE);

    g_signal_connect(tracker->screen, "active-window-changed",
                     G_CALLBACK(on_active_window_changed), gm);

    on_active_window_changed(tracker->screen, NULL, gm);
    gm_grab_hotkey(gm);
    return gm->holder_box;
}
