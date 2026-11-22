#include "shell-plugin.h"
void shell_save_layout_and_theme(UltimateShell *shell);
#include "start-menu.h"
#include "taskbar.h"
#include "dock-window.h"
#include <gio/gio.h>
#include <gio/gdesktopappinfo.h>
#include <cairo.h>
#include <math.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <gdk/gdkx.h>

typedef struct {
    const gchar *title;
    const gchar *icon;
    const gchar *filter;
} CategorySpec;

static const CategorySpec g_category_specs[] = {
    { "Favorites",    "starred",                  "Favorites" },
    { "All",          "applications-other",       "All" },
    { "Accessories",  "applications-utilities",   "Utility" },
    { "Development",  "applications-development", "Development" },
    { "Education",    "applications-education",   "Education" },
    { "Games",        "applications-games",       "Game" },
    { "Graphics",     "applications-graphics",    "Graphics" },
    { "Internet",     "network-workgroup",        "Network" },
    { "Multimedia",   "applications-multimedia",  "AudioVideo" },
    { "Office",       "applications-office",      "Office" },
    { "Science",      "applications-science",     "Science" },
    { "System",       "applications-system",      "System" },
    { "Settings",     "preferences-system",       "Settings" },
    { "Wine / Other", "application-x-executable", "Other" }
};

typedef struct {
    UltimateShell *shell;
    GtkWidget     *btn;
    GtkWidget     *btn_box;
    GtkWidget     *btn_icon_widget;
    GtkWidget     *btn_label_widget;
    GtkWidget     *popup_win;
    GtkWidget     *root_container;
    GtkWidget     *search_entry;
    GtkWidget     *list_box;
    GtkWidget     *flow_box;
    GtkWidget     *win11_stack;
    GtkWidget     *win11_pinned_btn;
    GtkWidget     *win11_all_btn;
    gchar         *active_category;

    /* Windows 7 Left Stack (Categories Default + All Programs Toggle) */
    GtkWidget     *win7_left_stack;
    GtkWidget     *win7_toggle_btn;

    /* Fullscreen Paged Deck State */
    GtkWidget     *fs_mode_stack;
    GtkWidget     *fs_page_stack;
    GtkWidget     *fs_dots_da;
    GtkWidget     *fs_cat_scroll;
    GPtrArray     *fs_cat_buttons;
    GPtrArray     *fs_cat_filters;
    GArray        *fs_cat_first_page;
    GArray        *fs_page_cat_idx;
    gint           fs_total_pages;
    gint           fs_current_page;
    guint32        fs_last_scroll_time;

    /* App Info Cache for Fast Zero-Lag Rebuilds */
    GList           *cached_apps;
    GAppInfoMonitor *app_monitor;

    /* search extras (calculator, web) and Recommended */
    GtkWidget     *search_extras, *calc_row, *calc_lbl, *web_btn;
    GtkWidget     *rec_widget;
    gboolean       rebuild_pending;
    guint          rebuild_id, fade_id;
    gdouble        fade_step;

    /* Windows 10 Tiles Menu / GNOME App Grid */
    GtkWidget     *win10_tiles_grid, *win10_tiles_pane, *win10_list_stack, *win10_list_scroll;
    GtkWidget     *gnome_overview, *gnome_overview_scroll;
    GtkWidget     *gnome_peek_prev, *gnome_peek_next;   /* edges of the neighbouring pages */
    GPtrArray     *fs_page_apps;                         /* per page: GPtrArray of GAppInfo (GNOME) */
    gint           fs_page_cols;

    gboolean       resizing;
    gdouble        drag_start_x;
    gdouble        drag_start_y;
    gint           start_w;
    gint           start_h;
} StartMenuUI;

static StartMenuUI *g_start_ui = NULL;

static gint get_active_style_idx(UltimateShell *shell) {
    if (!shell) return 0;
    return CLAMP((gint)shell->start_style, 0, START_STYLE_COUNT - 1);
}

/* Launchpad and the GNOME App Grid both fill the screen and page their apps */
static gboolean sm_style_fullscreen(UltimateShell *shell) {
    return shell && (shell->start_style == START_STYLE_FULLSCREEN || shell->start_style == START_STYLE_GNOME);
}

static void win10_fill_tiles(StartMenuUI *ui);
static void win10_append_resize_menu(GtkWidget *menu, GAppInfo *app);
static void win10_show_letter_picker(StartMenuUI *ui);
static void on_start_popup_show(GtkWidget *win, StartMenuUI *ui);
static void gnome_make_tile_draggable(GtkWidget *eb, GAppInfo *app);
static void gnome_overview_refresh(StartMenuUI *ui);
static void gnome_hold_open(StartMenuUI *ui, guint ms);
static void gnome_update_peeks(StartMenuUI *ui);
static GtkWidget *gnome_make_peek(StartMenuUI *ui, gint dir);
static void gnome_ws_resize_start(StartMenuUI *ui, gint sw, gint sh);
static void gnome_play_intro(StartMenuUI *ui);
static void on_w10_tile_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer d);
static void on_w10_tile_drag_end(GtkWidget *w, GdkDragContext *ctx, StartMenuUI *ui);
static const gchar *resolve_app_primary_title(GAppInfo *app, gboolean use_generic) {
    if (use_generic && G_IS_DESKTOP_APP_INFO(app)) {
        const gchar *gen = g_desktop_app_info_get_generic_name(G_DESKTOP_APP_INFO(app));
        if (gen && gen[0] != '\0') return gen;
    }
    return g_app_info_get_display_name(app);
}

static gint compare_app_info_alpha(gconstpointer a, gconstpointer b) {
    gboolean use_gen = (g_start_ui && g_start_ui->shell) ? g_start_ui->shell->start_generic_names : FALSE;
    const gchar *na = resolve_app_primary_title(G_APP_INFO(a), use_gen);
    const gchar *nb = resolve_app_primary_title(G_APP_INFO(b), use_gen);
    return g_utf8_collate(na ? na : "", nb ? nb : "");
}

/* =========================================================================
 * Fast In-Memory App List Cache (Eliminates Slider Lag)
 * ========================================================================= */
/* Apps were installed / removed: rebuild the menu (later, if it is open right now) */
static gboolean on_app_rebuild_idle(gpointer d) {
    StartMenuUI *ui = d;
    ui->rebuild_id = 0;
    if (ui->popup_win && gtk_widget_get_visible(ui->popup_win)) { ui->rebuild_pending = TRUE; return G_SOURCE_REMOVE; }
    ui->rebuild_pending = FALSE;
    start_menu_rebuild_style(ui->shell);
    return G_SOURCE_REMOVE;
}

static void on_app_monitor_changed(GAppInfoMonitor *mon, StartMenuUI *ui) {
    if (ui->cached_apps) {
        g_list_free_full(ui->cached_apps, g_object_unref);
        ui->cached_apps = NULL;
    }
    if (ui->rebuild_id) g_source_remove(ui->rebuild_id);
    ui->rebuild_id = g_timeout_add(1200, on_app_rebuild_idle, ui);   /* installs touch many files */
}

/* Icon theme switched: rebuild so every tile picks up the new icons */
static void on_sm_icon_theme_changed(GtkIconTheme *theme, StartMenuUI *ui) {
    if (ui->rebuild_id) g_source_remove(ui->rebuild_id);
    ui->rebuild_id = g_timeout_add(300, on_app_rebuild_idle, ui);
}

static void usage_note_seen(GList *apps);

static GList *get_sorted_apps_cached(StartMenuUI *ui) {
    if (!ui->app_monitor) {
        ui->app_monitor = g_app_info_monitor_get();
        g_signal_connect(ui->app_monitor, "changed", G_CALLBACK(on_app_monitor_changed), ui);
        g_signal_connect(gtk_icon_theme_get_default(), "changed", G_CALLBACK(on_sm_icon_theme_changed), ui);
    }
    if (!ui->cached_apps) {
        ui->cached_apps = g_app_info_get_all();
        usage_note_seen(ui->cached_apps);
    }
    ui->cached_apps = g_list_sort(ui->cached_apps, compare_app_info_alpha);
    return ui->cached_apps;
}

/* =========================================================================
 * Favorites / Pinned Management Engine
 * ========================================================================= */
static const gchar *get_app_id_token(GAppInfo *app) {
    if (!app) return NULL;
    const gchar *id = g_app_info_get_id(app);
    if (id && id[0] != '\0') return id;
    return g_app_info_get_executable(app);
}

static void ensure_default_favorites(UltimateShell *shell) {
    if (!shell) return;
    if (shell->start_favorites && shell->start_favorites[0] != '\0') return;
    g_free(shell->start_favorites);
    shell->start_favorites = g_strdup(
        "xfce4-terminal.desktop;exo-terminal-emulator.desktop;thunar.desktop;"
        "exo-file-manager.desktop;firefox.desktop;firefox-esr.desktop;"
        "exo-web-browser.desktop;xfce4-settings-manager.desktop;mousepad.desktop"
    );
}

static gboolean is_app_in_favorites(UltimateShell *shell, GAppInfo *app) {
    if (!shell || !app) return FALSE;
    ensure_default_favorites(shell);
    const gchar *id = g_app_info_get_id(app);
    const gchar *exec = g_app_info_get_executable(app);

    gboolean found = FALSE;
    gchar **tokens = g_strsplit(shell->start_favorites, ";", -1);
    for (int i = 0; tokens && tokens[i]; i++) {
        gchar *t = g_strstrip(tokens[i]);
        if (t[0] == '\0') continue;
        if ((id && g_ascii_strcasecmp(t, id) == 0) ||
            (exec && g_ascii_strcasecmp(t, exec) == 0)) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(tokens);
    return found;
}

static void refresh_win11_pinned(StartMenuUI *ui);

/* List style: show a new pin at once (after the right-click menu is gone) */
static gboolean start_menu_rebuild_idle_once(gpointer shell) {
    start_menu_rebuild_style((UltimateShell *)shell);
    return G_SOURCE_REMOVE;
}

static void toggle_app_favorite(UltimateShell *shell, GAppInfo *app) {
    if (!shell || !app) return;
    ensure_default_favorites(shell);
    const gchar *tok = get_app_id_token(app);
    if (!tok) return;

    gboolean currently_fav = is_app_in_favorites(shell, app);
    const gchar *id = g_app_info_get_id(app);
    const gchar *exec = g_app_info_get_executable(app);

    GString *next = g_string_new("");
    gchar **tokens = g_strsplit(shell->start_favorites, ";", -1);
    for (int i = 0; tokens && tokens[i]; i++) {
        gchar *t = g_strstrip(tokens[i]);
        if (t[0] == '\0') continue;
        if (currently_fav &&
            ((id && g_ascii_strcasecmp(t, id) == 0) ||
             (exec && g_ascii_strcasecmp(t, exec) == 0))) {
            continue;
        }
        if (next->len > 0) g_string_append_c(next, ';');
        g_string_append(next, t);
    }
    g_strfreev(tokens);

    if (!currently_fav) {
        if (next->len > 0) g_string_append_c(next, ';');
        g_string_append(next, tok);
    }

    g_free(shell->start_favorites);
    shell->start_favorites = g_string_free(next, FALSE);
    if (shell->channel) {
        xfconf_channel_set_string(shell->channel, "/plugins/ultimateshell/start_favorites", shell->start_favorites);
    }
    /* Don't tear the open menu down (it jumped to the top and lost the page/category):
       Win11 refreshes its Pinned grid in place, other styles update the next time they open. */
    StartMenuUI *ui = g_start_ui;
    if (ui && ui->popup_win && gtk_widget_get_visible(ui->popup_win)) {
        if (shell->start_style == START_STYLE_WIN11 || shell->start_style == START_STYLE_WIN10) refresh_win11_pinned(ui);
        else if (shell->start_style == START_STYLE_DEFAULT) g_idle_add((GSourceFunc)start_menu_rebuild_idle_once, shell);
        else ui->rebuild_pending = TRUE;
    } else {
        start_menu_rebuild_style(shell);
    }
}

/* =========================================================================
 * App usage history (Most used / Recently added), stored locally:
 *   ~/.config/ultimate-shell/app-usage.ini   [count] [last] [seen]
 * ========================================================================= */
static GKeyFile *g_usage = NULL;

static gchar *usage_path(void) {
    return g_build_filename(g_get_user_config_dir(), "ultimate-shell", "app-usage.ini", NULL);
}
static GKeyFile *usage_kf(void) {
    if (!g_usage) {
        g_usage = g_key_file_new();
        gchar *p = usage_path();
        g_key_file_load_from_file(g_usage, p, G_KEY_FILE_NONE, NULL);
        g_free(p);
    }
    return g_usage;
}
static void usage_save(void) {
    gchar *p = usage_path(), *d = g_path_get_dirname(p);
    g_mkdir_with_parents(d, 0700);
    g_key_file_save_to_file(usage_kf(), p, NULL);
    g_free(d); g_free(p);
}
static void usage_record_launch(GAppInfo *app) {
    const gchar *id = app ? g_app_info_get_id(app) : NULL;
    if (!id) return;
    GKeyFile *kf = usage_kf();
    gint n = g_key_file_get_integer(kf, "count", id, NULL);
    g_key_file_set_integer(kf, "count", id, n + 1);
    g_key_file_set_int64(kf, "last", id, g_get_real_time() / G_USEC_PER_SEC);
    usage_save();
}
void start_menu_clear_usage_history(void) {
    GKeyFile *kf = usage_kf();
    g_key_file_remove_group(kf, "count", NULL);
    g_key_file_remove_group(kf, "last", NULL);
    usage_save();
}
/* Remember when each app was first seen; on the very first run nothing counts as "new". */
static void usage_note_seen(GList *apps) {
    GKeyFile *kf = usage_kf();
    gboolean first_run = !g_key_file_has_group(kf, "seen");
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gboolean changed = FALSE;
    for (GList *l = apps; l; l = l->next) {
        const gchar *id = g_app_info_get_id(G_APP_INFO(l->data));
        if (!id || g_key_file_has_key(kf, "seen", id, NULL)) continue;
        g_key_file_set_int64(kf, "seen", id, first_run ? 0 : now);
        changed = TRUE;
    }
    if (first_run && !changed) g_key_file_set_int64(kf, "seen", "_init", 0), changed = TRUE;
    if (changed) usage_save();
}

/* Installed in the last 7 days and not opened yet */
static gboolean app_is_new(GAppInfo *app) {
    const gchar *id = app ? g_app_info_get_id(app) : NULL;
    if (!id) return FALSE;
    GKeyFile *kf = usage_kf();
    gint64 t = g_key_file_get_int64(kf, "seen", id, NULL);
    if (t <= 0 || t < g_get_real_time() / G_USEC_PER_SEC - 7 * 86400) return FALSE;
    return g_key_file_get_integer(kf, "count", id, NULL) == 0;
}

/* =========================================================================
 * Recent & Recommended section (optional, Start Menu settings)
 * ========================================================================= */
typedef struct { gchar *id; gint count; gint64 last; } UsageEntry;
static gint usage_cmp(gconstpointer a, gconstpointer b) {
    const UsageEntry *x = a, *y = b;
    if (x->count != y->count) return y->count - x->count;
    return (y->last > x->last) - (y->last < x->last);
}
static gint recent_info_cmp(gconstpointer a, gconstpointer b) {
    time_t x = gtk_recent_info_get_modified((GtkRecentInfo *)a);
    time_t y = gtk_recent_info_get_modified((GtkRecentInfo *)b);
    return (y > x) - (y < x);
}

static void launch_app_and_close(StartMenuUI *ui, GAppInfo *app);
static void start_menu_close(StartMenuUI *ui);

static void on_rec_app_clicked(GtkButton *b, StartMenuUI *ui) {
    GAppInfo *app = g_object_get_data(G_OBJECT(b), "app-info");
    if (app) launch_app_and_close(ui, app);
}
static void on_rec_file_clicked(GtkButton *b, StartMenuUI *ui) {
    const gchar *uri = g_object_get_data(G_OBJECT(b), "uri");
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, NULL);
    start_menu_close(ui);
}

static GtkWidget *rec_item_button(const gchar *title, const gchar *sub, GIcon *gicon, const gchar *icon_name) {
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "start-rec-item");
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *img = gicon ? gtk_image_new_from_gicon(gicon, GTK_ICON_SIZE_DND)
                           : gtk_image_new_from_icon_name(icon_name ? icon_name : "text-x-generic", GTK_ICON_SIZE_DND);
    gtk_image_set_pixel_size(GTK_IMAGE(img), 28);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    GtkWidget *t = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(t), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 22);
    GtkWidget *s = gtk_label_new(sub);
    gtk_label_set_xalign(GTK_LABEL(s), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(s), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(s), 24);
    gtk_style_context_add_class(gtk_widget_get_style_context(s), "dim-label");
    gtk_style_context_add_class(gtk_widget_get_style_context(s), "start-rec-sub");
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v), s, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(b), h);
    return b;
}

static gchar *rec_age_text(gint64 then) {
    gint64 d = g_get_real_time() / G_USEC_PER_SEC - then;
    if (d < 3600) return g_strdup("Just now");
    if (d < 86400) return g_strdup_printf("%d hours ago", (gint)(d / 3600));
    if (d < 172800) return g_strdup("Yesterday");
    return g_strdup_printf("%d days ago", (gint)(d / 86400));
}

/* columns: 2 for the Win11 grid, 1 for list styles, 4 for Launchpad */
static GtkWidget *build_recommended_section(StartMenuUI *ui, gint columns) {
    UltimateShell *sh = ui->shell;
    if (!sh) return NULL;
    /* "Recently added" is always on; Most used + recent files only when Recommended is enabled */
    gboolean full = sh->start_show_recommended;
    GKeyFile *kf = usage_kf();
    gboolean use_gen = sh->start_generic_names;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "start-rec-section");
    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), full ? "<b>Recommended</b>" : "<b>Recently added</b>");
    gtk_label_set_xalign(GTK_LABEL(hdr), 0.0);
    gtk_widget_set_margin_start(hdr, 8);
    gtk_box_pack_start(GTK_BOX(box), hdr, FALSE, FALSE, 0);

    GtkWidget *fb = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(fb), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(fb), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(fb), columns);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(fb), columns);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(fb), 4);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(fb), 2);
    gtk_box_pack_start(GTK_BOX(box), fb, FALSE, FALSE, 0);

    gint max_items = (columns >= 4) ? 8 : (columns == 2 ? 6 : 4);
    gint added = 0;
    GHashTable *used = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* 1. Most used apps */
    gsize n = 0;
    gchar **keys = g_key_file_get_keys(kf, "count", &n, NULL);
    GArray *arr = g_array_new(FALSE, FALSE, sizeof(UsageEntry));
    for (gsize i = 0; keys && i < n; i++) {
        UsageEntry e = { keys[i], g_key_file_get_integer(kf, "count", keys[i], NULL),
                         g_key_file_get_int64(kf, "last", keys[i], NULL) };
        if (e.count > 0) g_array_append_val(arr, e);
    }
    g_array_sort(arr, usage_cmp);
    gint most_cap = full ? MAX(2, max_items - 2) : 0;
    for (guint i = 0; i < arr->len && added < most_cap; i++) {
        UsageEntry *e = &g_array_index(arr, UsageEntry, i);
        GDesktopAppInfo *di = g_desktop_app_info_new(e->id);
        if (!di) continue;
        if (!g_app_info_should_show(G_APP_INFO(di))) { g_object_unref(di); continue; }
        gchar *sub = g_strdup_printf("Opened %d time%s", e->count, e->count == 1 ? "" : "s");
        GtkWidget *b = rec_item_button(resolve_app_primary_title(G_APP_INFO(di), use_gen), sub,
                                       g_app_info_get_icon(G_APP_INFO(di)), NULL);
        g_free(sub);
        g_object_set_data_full(G_OBJECT(b), "app-info", di, g_object_unref);
        g_signal_connect(b, "clicked", G_CALLBACK(on_rec_app_clicked), ui);
        gtk_container_add(GTK_CONTAINER(fb), b);
        g_hash_table_add(used, g_strdup(e->id));
        added++;
    }
    g_array_free(arr, TRUE);
    g_strfreev(keys);

    /* 2. Recently added apps (first seen in the last 7 days) */
    gint64 week_ago = g_get_real_time() / G_USEC_PER_SEC - 7 * 86400;
    keys = g_key_file_get_keys(kf, "seen", &n, NULL);
    for (gsize i = 0; keys && i < n && added < max_items; i++) {
        gint64 t = g_key_file_get_int64(kf, "seen", keys[i], NULL);
        if (t <= week_ago || g_hash_table_contains(used, keys[i])) continue;
        if (g_key_file_get_integer(kf, "count", keys[i], NULL) > 0 && !full) continue;   /* already opened */
        GDesktopAppInfo *di = g_desktop_app_info_new(keys[i]);
        if (!di) continue;
        if (!g_app_info_should_show(G_APP_INFO(di))) { g_object_unref(di); continue; }
        gchar *age = rec_age_text(t), *sub = g_strdup_printf("New · %s", age);
        GtkWidget *b = rec_item_button(resolve_app_primary_title(G_APP_INFO(di), use_gen), sub,
                                       g_app_info_get_icon(G_APP_INFO(di)), NULL);
        g_free(age); g_free(sub);
        g_object_set_data_full(G_OBJECT(b), "app-info", di, g_object_unref);
        g_signal_connect(b, "clicked", G_CALLBACK(on_rec_app_clicked), ui);
        gtk_container_add(GTK_CONTAINER(fb), b);
        g_hash_table_add(used, g_strdup(keys[i]));
        added++;
    }
    g_strfreev(keys);

    /* 3. Recent files */
    if (full && sh->start_recommend_files) {
        GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
        items = g_list_sort(items, recent_info_cmp);
        gint files = 0;
        for (GList *l = items; l && files < 4 && added < max_items + 2; l = l->next) {
            GtkRecentInfo *ri = l->data;
            if (!gtk_recent_info_is_local(ri) || !gtk_recent_info_exists(ri)) continue;
            GIcon *gi = gtk_recent_info_get_gicon(ri);
            time_t md = gtk_recent_info_get_modified(ri);
            gchar *age = md > 0 ? rec_age_text((gint64)md) : g_strdup("");
            GtkWidget *b = rec_item_button(gtk_recent_info_get_display_name(ri), age, gi, "text-x-generic");
            g_free(age);
            if (gi) g_object_unref(gi);
            g_object_set_data_full(G_OBJECT(b), "uri", g_strdup(gtk_recent_info_get_uri(ri)), g_free);
            gtk_widget_set_tooltip_text(b, gtk_recent_info_get_uri_display(ri));
            g_signal_connect(b, "clicked", G_CALLBACK(on_rec_file_clicked), ui);
            gtk_container_add(GTK_CONTAINER(fb), b);
            files++; added++;
        }
        g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    }
    g_hash_table_destroy(used);

    if (added == 0 && !full) {            /* nothing new: no section at all */
        gtk_widget_destroy(box);
        return NULL;
    }
    if (added == 0) {
        GtkWidget *e = gtk_label_new("Apps you open often and newly installed apps will show up here.");
        gtk_label_set_line_wrap(GTK_LABEL(e), TRUE);
        gtk_label_set_xalign(GTK_LABEL(e), 0.0);
        gtk_widget_set_margin_start(e, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(e), "dim-label");
        gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 4);
    }
    return box;
}

/* =========================================================================
 * Search: ranking, Enter, arrow keys, calculator, web search
 * ========================================================================= */
static gint search_rank(GObject *item, const gchar *q) {
    if (!q || !q[0]) return 0;
    const gchar *nm = g_object_get_data(item, "search-name");
    const gchar *key = g_object_get_data(item, "search-key");
    if (nm && g_str_has_prefix(nm, q)) return 0;
    if (nm) {
        gchar *pat = g_strconcat(" ", q, NULL);
        gboolean word = strstr(nm, pat) != NULL;
        g_free(pat);
        if (word) return 1;
        if (strstr(nm, q)) return 2;
    }
    if (key && strstr(key, q)) return 3;
    return 4;
}

/* ---- favourites order, with a live preview while one is dragged (Modern pinned grid, Categories) ---- */
static gchar *g_fav_preview = NULL;       /* the order shown during a drag; NULL = the saved one */
static gchar *g_fav_moving  = NULL;       /* id being dragged */
static guint  g_fav_restore_id = 0;

static gint fav_index_of(UltimateShell *sh, GAppInfo *app) {
    if (!sh || !app) return 100000;
    const gchar *order = g_fav_preview ? g_fav_preview : sh->start_favorites;
    const gchar *id = g_app_info_get_id(app), *ex = g_app_info_get_executable(app);
    gchar **toks = g_strsplit(order ? order : "", ";", -1);
    gint res = 100000, n = 0;
    for (gint i = 0; toks && toks[i]; i++) {
        gchar *t = g_strstrip(toks[i]);
        if (!t[0]) continue;
        if ((id && g_ascii_strcasecmp(t, id) == 0) || (ex && g_ascii_strcasecmp(t, ex) == 0)) { res = n; break; }
        n++;
    }
    g_strfreev(toks);
    return res;
}

/* the saved order with `moving` placed before (or after) `target` */
static gchar *fav_order_with_move(UltimateShell *sh, const gchar *moving, const gchar *target, gboolean after) {
    gchar **toks = g_strsplit(sh->start_favorites ? sh->start_favorites : "", ";", -1);
    GString *out = g_string_new("");
    gboolean placed = FALSE;
    for (gint i = 0; toks && toks[i]; i++) {
        gchar *t = g_strstrip(toks[i]);
        if (!t[0] || g_ascii_strcasecmp(t, moving) == 0) continue;
        gboolean here = (g_ascii_strcasecmp(t, target) == 0);
        if (here && !after) { if (out->len) g_string_append_c(out, ';'); g_string_append(out, moving); placed = TRUE; }
        if (out->len) g_string_append_c(out, ';');
        g_string_append(out, t);
        if (here && after) { g_string_append_c(out, ';'); g_string_append(out, moving); placed = TRUE; }
    }
    g_strfreev(toks);
    if (!placed) { if (out->len) g_string_append_c(out, ';'); g_string_append(out, moving); }
    return g_string_free(out, FALSE);
}

static gboolean fav_order_applies(GObject *child, StartMenuUI *ui) {
    GtkWidget *par = gtk_widget_get_parent(GTK_WIDGET(child));
    if (!par) return FALSE;
    if (g_object_get_data(G_OBJECT(par), "fav-order")) return TRUE;
    return g_object_get_data(G_OBJECT(par), "fav-order-cat") && g_strcmp0(ui->active_category, "Favorites") == 0;
}

static gint search_sort_common(GObject *a, GObject *b, StartMenuUI *ui) {
    const gchar *q0 = ui->search_entry ? gtk_entry_get_text(GTK_ENTRY(ui->search_entry)) : "";
    if (q0 && q0[0]) {
        gchar *q = g_utf8_strdown(q0, -1);
        gint ra = search_rank(a, q), rb = search_rank(b, q);
        g_free(q);
        if (ra != rb) return ra - rb;
    } else if (fav_order_applies(a, ui)) {
        gint fa = fav_index_of(ui->shell, g_object_get_data(a, "app-info"));
        gint fb = fav_index_of(ui->shell, g_object_get_data(b, "app-info"));
        if (fa != fb) return fa - fb;
    }
    return GPOINTER_TO_INT(g_object_get_data(a, "order-idx")) - GPOINTER_TO_INT(g_object_get_data(b, "order-idx"));
}
static gint search_sort_list(GtkListBoxRow *a, GtkListBoxRow *b, gpointer ui) {
    return search_sort_common(G_OBJECT(a), G_OBJECT(b), ui);
}
static gint search_sort_flow(GtkFlowBoxChild *a, GtkFlowBoxChild *b, gpointer ui) {
    return search_sort_common(G_OBJECT(a), G_OBJECT(b), ui);
}

/* first time a container is searched: remember the original order, then rank by relevance */
static void ensure_search_sort(StartMenuUI *ui, GtkWidget *c) {
    if (!c || g_object_get_data(G_OBJECT(c), "search-sort-on")) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(c));
    gint i = 1;
    for (GList *l = kids; l; l = l->next) g_object_set_data(G_OBJECT(l->data), "order-idx", GINT_TO_POINTER(i++));
    g_list_free(kids);
    if (GTK_IS_LIST_BOX(c)) gtk_list_box_set_sort_func(GTK_LIST_BOX(c), search_sort_list, ui, NULL);
    else if (GTK_IS_FLOW_BOX(c)) gtk_flow_box_set_sort_func(GTK_FLOW_BOX(c), search_sort_flow, ui, NULL);
    g_object_set_data(G_OBJECT(c), "search-sort-on", GINT_TO_POINTER(1));
}

/* first visible, launchable result in the container that is on screen */
static GtkWidget *first_search_result(StartMenuUI *ui) {
    GtkWidget *cands[2] = { ui->list_box, ui->flow_box };
    if (ui->shell && (sm_style_fullscreen(ui->shell) || ui->shell->start_style == START_STYLE_WIN11)) {
        /* Launchpad searches its grid; Win11 shows the All-apps list while searching */
        if (sm_style_fullscreen(ui->shell)) { cands[0] = ui->flow_box; cands[1] = NULL; }
    }
    for (gint c = 0; c < 2; c++) {
        GtkWidget *box = cands[c];
        if (!box || !gtk_widget_get_mapped(box)) continue;
        GtkWidget *best = NULL;
        gint best_idx = G_MAXINT;
        GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
        for (GList *l = kids; l; l = l->next) {
            GtkWidget *w = l->data;
            if (!gtk_widget_get_child_visible(w) || !gtk_widget_get_visible(w)) continue;
            if (!g_object_get_data(G_OBJECT(w), "app-info")) continue;
            gint idx = GTK_IS_LIST_BOX_ROW(w) ? gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(w))
                                              : gtk_flow_box_child_get_index(GTK_FLOW_BOX_CHILD(w));
            if (idx < best_idx) { best_idx = idx; best = w; }
        }
        g_list_free(kids);
        if (best) return best;
    }
    return NULL;
}

/* --- tiny calculator: + - * / % ^ ( ) and decimals --- */
typedef struct { const gchar *p; gboolean ok; } CalcP;
static gdouble calc_expr(CalcP *c);
static void calc_ws(CalcP *c) { while (*c->p == ' ') c->p++; }
static gdouble calc_atom(CalcP *c) {
    calc_ws(c);
    if (*c->p == '(') { c->p++; gdouble v = calc_expr(c); calc_ws(c); if (*c->p == ')') c->p++; else c->ok = FALSE; return v; }
    if (*c->p == '-') { c->p++; return -calc_atom(c); }
    if (*c->p == '+') { c->p++; return calc_atom(c); }
    gchar *end = NULL;
    gdouble v = g_ascii_strtod(c->p, &end);
    if (end == c->p) { c->ok = FALSE; return 0; }
    c->p = end;
    return v;
}
static gdouble calc_pow(CalcP *c) {
    gdouble b = calc_atom(c);
    calc_ws(c);
    if (*c->p == '^') { c->p++; return pow(b, calc_pow(c)); }
    return b;
}
static gdouble calc_term(CalcP *c) {
    gdouble v = calc_pow(c);
    for (;;) {
        calc_ws(c);
        gchar op = *c->p;
        if (op == '*' || op == 'x' || op == '/' || op == '%') {
            c->p++;
            gdouble r = calc_pow(c);
            if (op == '/') { if (r == 0) c->ok = FALSE; else v /= r; }
            else if (op == '%') { if (r == 0) c->ok = FALSE; else v = fmod(v, r); }
            else v *= r;
        } else return v;
    }
}
static gdouble calc_expr(CalcP *c) {
    gdouble v = calc_term(c);
    for (;;) {
        calc_ws(c);
        if (*c->p == '+') { c->p++; v += calc_term(c); }
        else if (*c->p == '-') { c->p++; v -= calc_term(c); }
        else return v;
    }
}
/* returns a newly allocated result string, or NULL when the text is not a calculation */
static gchar *calc_try(const gchar *q) {
    if (!q || !q[0]) return NULL;
    gboolean has_op = FALSE, has_digit = FALSE;
    for (const gchar *s = q; *s; s++) {
        if (g_ascii_isdigit(*s)) has_digit = TRUE;
        else if (strchr("+-*/%^x()", *s)) { if (s != q || *s == '(') has_op = TRUE; }
        else if (*s != '.' && *s != ' ' && *s != ',') return NULL;
    }
    if (!has_op || !has_digit) return NULL;
    gchar *norm = g_strdup(q);
    for (gchar *s = norm; *s; s++) if (*s == ',') *s = '.';
    CalcP c = { norm, TRUE };
    gdouble v = calc_expr(&c);
    calc_ws(&c);
    gboolean ok = c.ok && *c.p == '\0' && isfinite(v);
    g_free(norm);
    if (!ok) return NULL;
    gchar buf[64];
    if (fabs(v - llround(v)) < 1e-9 && fabs(v) < 1e15) g_snprintf(buf, sizeof buf, "%lld", (long long)llround(v));
    else g_snprintf(buf, sizeof buf, "%.10g", v);
    return g_strdup(buf);
}

static void on_search_web_clicked(GtkButton *b, StartMenuUI *ui) {
    const gchar *q = ui->search_entry ? gtk_entry_get_text(GTK_ENTRY(ui->search_entry)) : "";
    if (!q || !q[0]) return;
    gchar *esc = g_uri_escape_string(q, NULL, TRUE);
    gchar *uri = g_strdup_printf("https://duckduckgo.com/?q=%s", esc);
    g_app_info_launch_default_for_uri(uri, NULL, NULL);
    g_free(uri); g_free(esc);
    start_menu_close(ui);
}

static void on_calc_copy_clicked(GtkButton *b, StartMenuUI *ui) {
    const gchar *v = g_object_get_data(G_OBJECT(ui->search_extras), "calc-value");
    if (!v) return;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), v, -1);
    start_menu_close(ui);
}

static void update_search_extras(StartMenuUI *ui) {
    if (!ui->search_extras) return;
    const gchar *q = ui->search_entry ? gtk_entry_get_text(GTK_ENTRY(ui->search_entry)) : "";
    gchar *res = calc_try(q);
    if (res) {
        gchar *mk = g_markup_printf_escaped("<b>= %s</b>   <small>Enter copies the result</small>", res);
        gtk_label_set_markup(GTK_LABEL(ui->calc_lbl), mk);
        g_free(mk);
        g_object_set_data_full(G_OBJECT(ui->search_extras), "calc-value", res, g_free);
    } else {
        g_object_set_data(G_OBJECT(ui->search_extras), "calc-value", NULL);
    }
    gtk_widget_set_visible(ui->calc_row, res != NULL);
    gboolean has_q = q && q[0];
    if (has_q) {
        gchar *t = g_strdup_printf("Search the web for “%s”", q);
        gtk_button_set_label(GTK_BUTTON(ui->web_btn), t);
        g_free(t);
    }
    gtk_widget_set_visible(ui->web_btn, has_q);
    gtk_widget_set_visible(ui->search_extras, has_q);
}

static void on_search_activate(GtkEntry *e, StartMenuUI *ui) {
    if (ui->search_extras && g_object_get_data(G_OBJECT(ui->search_extras), "calc-value")) {
        on_calc_copy_clicked(NULL, ui);
        return;
    }
    GtkWidget *w = first_search_result(ui);
    GAppInfo *app = w ? g_object_get_data(G_OBJECT(w), "app-info") : NULL;
    if (app) launch_app_and_close(ui, app);
}

/* Down / Tab from the search box moves into the results */
static GtkWidget *fs_current_page_flow(StartMenuUI *ui);

static gboolean on_search_key(GtkWidget *w, GdkEventKey *ev, StartMenuUI *ui) {
    if (ev->keyval != GDK_KEY_Down) return FALSE;
    const gchar *q = gtk_entry_get_text(GTK_ENTRY(w));
    if (sm_style_fullscreen(ui->shell) && (!q || !q[0])) {
        /* full-screen grids: into the page that is showing */
        GtkWidget *fb = fs_current_page_flow(ui);
        GtkFlowBoxChild *c = GTK_IS_FLOW_BOX(fb) ? gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(fb), 0) : NULL;
        if (c) { gtk_widget_grab_focus(GTK_WIDGET(c)); return TRUE; }
        return FALSE;
    }
    GtkWidget *r = first_search_result(ui);
    if (!r) return FALSE;
    if (GTK_IS_FLOW_BOX_CHILD(r)) {
        GtkWidget *fb = gtk_widget_get_parent(r);
        if (GTK_IS_FLOW_BOX(fb)) gtk_flow_box_select_child(GTK_FLOW_BOX(fb), GTK_FLOW_BOX_CHILD(r));
    }
    gtk_widget_grab_focus(r);
    return TRUE;
}

/* place the calculator / web row right under the search box, whatever the style */
static void attach_search_extras(StartMenuUI *ui) {
    ui->search_extras = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->search_extras), "start-search-extras");
    ui->calc_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *ci = gtk_image_new_from_icon_name("accessories-calculator-symbolic", GTK_ICON_SIZE_MENU);
    ui->calc_lbl = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(ui->calc_lbl), 0.0);
    GtkWidget *cb = gtk_button_new_with_label("Copy");
    gtk_button_set_relief(GTK_BUTTON(cb), GTK_RELIEF_NONE);
    g_signal_connect(cb, "clicked", G_CALLBACK(on_calc_copy_clicked), ui);
    gtk_box_pack_start(GTK_BOX(ui->calc_row), ci, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(ui->calc_row), ui->calc_lbl, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(ui->calc_row), cb, FALSE, FALSE, 0);
    ui->web_btn = gtk_button_new_with_label("Search the web");
    gtk_button_set_relief(GTK_BUTTON(ui->web_btn), GTK_RELIEF_NONE);
    gtk_widget_set_halign(ui->web_btn, GTK_ALIGN_START);
    g_signal_connect(ui->web_btn, "clicked", G_CALLBACK(on_search_web_clicked), ui);
    GtkWidget *lbl = gtk_bin_get_child(GTK_BIN(ui->web_btn));
    if (GTK_IS_LABEL(lbl)) gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(ui->search_extras), ui->calc_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ui->search_extras), ui->web_btn, FALSE, FALSE, 0);

    /* nearest vertical box above the search entry */
    GtkWidget *child = ui->search_entry, *parent = gtk_widget_get_parent(child);
    while (parent && !(GTK_IS_BOX(parent) &&
           gtk_orientable_get_orientation(GTK_ORIENTABLE(parent)) == GTK_ORIENTATION_VERTICAL)) {
        child = parent;
        parent = gtk_widget_get_parent(parent);
    }
    if (!parent) { gtk_widget_destroy(ui->search_extras); ui->search_extras = NULL; return; }
    GValue pos = G_VALUE_INIT;
    g_value_init(&pos, G_TYPE_INT);
    gtk_container_child_get_property(GTK_CONTAINER(parent), child, "position", &pos);
    gtk_box_pack_start(GTK_BOX(parent), ui->search_extras, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(parent), ui->search_extras, g_value_get_int(&pos) + 1);
    g_value_unset(&pos);
    gtk_widget_set_no_show_all(ui->search_extras, TRUE);
    gtk_widget_show_all(ui->calc_row);
    gtk_widget_show(ui->web_btn);
    gtk_widget_hide(ui->search_extras);
}

/* =========================================================================
 * Pinned apps: drag to reorder (Win11 grid)
 * ========================================================================= */
static const GtkTargetEntry g_fav_target[] = { { "ULTIMATE_FAV_ID", GTK_TARGET_SAME_APP, 0 } };

static void on_fav_drag_data_get(GtkWidget *w, GdkDragContext *ctx, GtkSelectionData *sd, guint info, guint t, GAppInfo *app) {
    const gchar *id = get_app_id_token(app);
    if (id) gtk_selection_data_set(sd, gtk_selection_data_get_target(sd), 8, (const guchar *)id, strlen(id));
}

static void favorites_move_before(UltimateShell *sh, const gchar *moving, const gchar *before) {
    if (!sh || !moving || !before || g_strcmp0(moving, before) == 0) return;
    gchar **toks = g_strsplit(sh->start_favorites ? sh->start_favorites : "", ";", -1);
    GString *out = g_string_new("");
    for (gint i = 0; toks && toks[i]; i++) {
        gchar *t = g_strstrip(toks[i]);
        if (!t[0] || g_ascii_strcasecmp(t, moving) == 0) continue;
        if (g_ascii_strcasecmp(t, before) == 0) {
            if (out->len) g_string_append_c(out, ';');
            g_string_append(out, moving);
        }
        if (out->len) g_string_append_c(out, ';');
        g_string_append(out, t);
    }
    g_strfreev(toks);
    g_free(sh->start_favorites);
    sh->start_favorites = g_string_free(out, FALSE);
    if (sh->channel) xfconf_channel_set_string(sh->channel, "/plugins/ultimateshell/start_favorites", sh->start_favorites);
}

static void refresh_win11_pinned(StartMenuUI *ui);

static gboolean refresh_pinned_idle(gpointer d) {
    if (g_start_ui) refresh_win11_pinned(g_start_ui);
    return G_SOURCE_REMOVE;
}

static void on_fav_drag_received(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *sd,
                                 guint info, guint t, GAppInfo *target_app) {
    const guchar *data = gtk_selection_data_get_data(sd);
    gint len = gtk_selection_data_get_length(sd);
    if (data && len > 0 && g_start_ui) {
        gchar *moving = g_strndup((const gchar *)data, len);
        favorites_move_before(g_start_ui->shell, moving, get_app_id_token(target_app));
        g_free(moving);
        g_idle_add(refresh_pinned_idle, NULL);   /* not inside the target tile's own handler: it gets rebuilt */
    }
    gtk_drag_finish(ctx, TRUE, FALSE, t);
}

static gboolean clear_context_menu_guard_idle(gpointer data);

/* A drag takes the pointer and keyboard, so the menu loses focus - which normally closes it.
   Keep it open for the whole drag (like for the right-click menu), dim the tile being moved. */
static void on_sm_drag_begin_keep_open(GtkWidget *w, GdkDragContext *ctx, gpointer d) {
    if (g_start_ui && g_start_ui->popup_win)
        g_object_set_data(G_OBJECT(g_start_ui->popup_win), "context-menu-open", GINT_TO_POINTER(1));
    gtk_widget_set_opacity(w, 0.4);
}

static void on_sm_drag_end_keep_open(GtkWidget *w, GdkDragContext *ctx, gpointer d) {
    gtk_widget_set_opacity(w, 1.0);
    if (!g_start_ui || !g_start_ui->popup_win) return;
    /* a drop on a GNOME workspace holds the grid open a while longer itself */
    if (g_object_get_data(G_OBJECT(g_start_ui->popup_win), "gnome-hold-id")) return;
    g_idle_add(clear_context_menu_guard_idle, g_start_ui->popup_win);   /* focus back, guard off */
}

static void sm_drag_keep_open(GtkWidget *src) {
    g_signal_connect(src, "drag-begin", G_CALLBACK(on_sm_drag_begin_keep_open), NULL);
    g_signal_connect(src, "drag-end", G_CALLBACK(on_sm_drag_end_keep_open), NULL);
}

static void fav_drag_resort(GtkWidget *c) {
    if (GTK_IS_LIST_BOX(c)) gtk_list_box_invalidate_sort(GTK_LIST_BOX(c));
    else if (GTK_IS_FLOW_BOX(c)) gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(c));
}

static void on_fav_src_drag_begin(GtkWidget *w, GdkDragContext *ctx, GAppInfo *app) {
    g_free(g_fav_moving);
    g_fav_moving = g_strdup(get_app_id_token(app));
}
static void on_fav_src_drag_end(GtkWidget *w, GdkDragContext *ctx, GtkWidget *container) {
    g_free(g_fav_moving); g_fav_moving = NULL;
    if (g_fav_preview) { g_free(g_fav_preview); g_fav_preview = NULL; fav_drag_resort(container); }   /* cancelled */
}

/* the container (pinned grid / Favourites list) is the drop target; its children make room live */
static gboolean on_fav_area_motion(GtkWidget *c, GdkDragContext *ctx, gint x, gint y, guint t, StartMenuUI *ui) {
    if (!g_fav_moving) { gdk_drag_status(ctx, 0, t); return FALSE; }
    if (g_object_get_data(G_OBJECT(c), "fav-order-cat") && g_strcmp0(ui->active_category, "Favorites") != 0) {
        gdk_drag_status(ctx, 0, t);               /* Categories: reordering only inside Favourites */
        return TRUE;
    }
    if (g_fav_restore_id) { g_source_remove(g_fav_restore_id); g_fav_restore_id = 0; }
    GtkWidget *target = NULL;
    gboolean after = FALSE;
    if (GTK_IS_LIST_BOX(c)) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_y(GTK_LIST_BOX(c), y);
        if (r) { GtkAllocation a; gtk_widget_get_allocation(GTK_WIDGET(r), &a); after = (y > a.y + a.height / 2); target = GTK_WIDGET(r); }
    } else if (GTK_IS_FLOW_BOX(c)) {
        GtkFlowBoxChild *ch = gtk_flow_box_get_child_at_pos(GTK_FLOW_BOX(c), x, y);
        if (ch) { GtkAllocation a; gtk_widget_get_allocation(GTK_WIDGET(ch), &a); after = (x > a.x + a.width / 2); target = GTK_WIDGET(ch); }
    }
    GAppInfo *tapp = target ? g_object_get_data(G_OBJECT(target), "app-info") : NULL;
    const gchar *tid = tapp ? get_app_id_token(tapp) : NULL;
    if (tid && g_ascii_strcasecmp(tid, g_fav_moving) != 0) {
        gchar *ord = fav_order_with_move(ui->shell, g_fav_moving, tid, after);
        if (g_strcmp0(ord, g_fav_preview) != 0) { g_free(g_fav_preview); g_fav_preview = ord; fav_drag_resort(c); }
        else g_free(ord);
    }
    gdk_drag_status(ctx, GDK_ACTION_MOVE, t);
    return TRUE;
}

static gboolean fav_restore_idle(gpointer c) {
    g_fav_restore_id = 0;
    if (g_fav_preview) { g_free(g_fav_preview); g_fav_preview = NULL; fav_drag_resort(c); }
    return G_SOURCE_REMOVE;
}
static void on_fav_area_leave(GtkWidget *c, GdkDragContext *ctx, guint t, StartMenuUI *ui) {
    if (g_fav_restore_id) g_source_remove(g_fav_restore_id);
    g_fav_restore_id = g_timeout_add(120, fav_restore_idle, c);    /* GTK sends leave right before a drop too */
}
static gboolean on_fav_area_drop(GtkWidget *c, GdkDragContext *ctx, gint x, gint y, guint t, StartMenuUI *ui) {
    if (g_fav_restore_id) { g_source_remove(g_fav_restore_id); g_fav_restore_id = 0; }
    if (g_fav_preview) {                          /* what you saw is what is kept */
        g_free(ui->shell->start_favorites);
        ui->shell->start_favorites = g_fav_preview;
        g_fav_preview = NULL;
        if (ui->shell->channel)
            xfconf_channel_set_string(ui->shell->channel, "/plugins/ultimateshell/start_favorites", ui->shell->start_favorites);
        fav_drag_resort(c);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, t);
    return TRUE;
}

/* make a container's children reorderable as favourites (they must carry "app-info") */
static void fav_area_setup(StartMenuUI *ui, GtkWidget *c) {
    gtk_drag_dest_set(c, 0, g_fav_target, 1, GDK_ACTION_MOVE);
    g_signal_connect(c, "drag-motion", G_CALLBACK(on_fav_area_motion), ui);
    g_signal_connect(c, "drag-leave", G_CALLBACK(on_fav_area_leave), ui);
    g_signal_connect(c, "drag-drop", G_CALLBACK(on_fav_area_drop), ui);
}
static void fav_source_setup(GtkWidget *src, GAppInfo *app, GtkWidget *container) {
    if (!GTK_IS_WIDGET(src) || !app) return;
    gtk_drag_source_set(src, GDK_BUTTON1_MASK, g_fav_target, 1, GDK_ACTION_MOVE);
    g_object_set_data(G_OBJECT(src), "fav-draggable", GINT_TO_POINTER(1));   /* launch on release, not press */
    GIcon *gi = g_app_info_get_icon(app);
    if (gi) gtk_drag_source_set_icon_gicon(src, gi);
    g_signal_connect(src, "drag-data-get", G_CALLBACK(on_fav_drag_data_get), app);
    g_signal_connect(src, "drag-begin", G_CALLBACK(on_fav_src_drag_begin), app);
    g_signal_connect(src, "drag-end", G_CALLBACK(on_fav_src_drag_end), container);
    sm_drag_keep_open(src);
}


static void on_tile_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer d) {
    /* carry a picture of the tile, not just its icon */
    GtkAllocation a; gtk_widget_get_allocation(w, &a);
    cairo_surface_t *sf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, MAX(1, a.width), MAX(1, a.height));
    cairo_t *cr = cairo_create(sf);
    gtk_widget_draw(w, cr);
    cairo_destroy(cr);
    cairo_surface_set_device_offset(sf, -a.width / 2.0, -a.height / 2.0);
    gtk_drag_set_icon_surface(ctx, sf);
    cairo_surface_destroy(sf);
}

static void make_tile_reorderable(GtkWidget *eb, GAppInfo *app) {
    gtk_drag_source_set(eb, GDK_BUTTON1_MASK, g_fav_target, 1, GDK_ACTION_MOVE);
    gtk_drag_dest_set(eb, GTK_DEST_DEFAULT_ALL, g_fav_target, 1, GDK_ACTION_MOVE);
    GIcon *gi = g_app_info_get_icon(app);
    if (gi) gtk_drag_source_set_icon_gicon(eb, gi);
    g_signal_connect(eb, "drag-data-get", G_CALLBACK(on_fav_drag_data_get), app);
    g_signal_connect(eb, "drag-data-received", G_CALLBACK(on_fav_drag_received), app);
    sm_drag_keep_open(eb);
}

static gboolean app_matches_category_filter(UltimateShell *shell, GAppInfo *app, const gchar *filter) {
    if (!filter || g_strcmp0(filter, "All") == 0) return TRUE;
    if (g_strcmp0(filter, "Favorites") == 0) {
        return is_app_in_favorites(shell, app);
    }
    const gchar *cats = "";
    if (G_IS_DESKTOP_APP_INFO(app)) {
        const gchar *c = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(app));
        if (c) cats = c;
    }
    /* Categories=Audio;AudioVideo;Player;  -> compare whole entries, never substrings */
    gchar **toks = g_strsplit(cats, ";", -1);
    gboolean hit = FALSE;
    if (g_strcmp0(filter, "Other") == 0) {
        gboolean known = FALSE, wine = FALSE;
        for (gint t = 0; toks && toks[t]; t++) {
            if (g_strcmp0(toks[t], "Wine") == 0 || g_strcmp0(toks[t], "X-Wine") == 0) wine = TRUE;
            for (guint i = 2; i < G_N_ELEMENTS(g_category_specs) - 1; i++)
                if (g_strcmp0(toks[t], g_category_specs[i].filter) == 0) known = TRUE;
        }
        hit = wine || !known;
    } else {
        for (gint t = 0; toks && toks[t] && !hit; t++) hit = (g_strcmp0(toks[t], filter) == 0);
    }
    g_strfreev(toks);
    return hit;
}

static PangoWeight weight_idx_to_pango_enum(gint w) {
    if (w >= 700 || w == 2) return PANGO_WEIGHT_BOLD;
    if (w >= 500 || w == 1) return PANGO_WEIGHT_MEDIUM;
    if (w == 3) return PANGO_WEIGHT_HEAVY;
    return PANGO_WEIGHT_NORMAL;
}

static void apply_label_font_size_and_weight(GtkWidget *lbl, gint pt_size, gint weight_idx) {
    gint clamped_pt = (pt_size > 0) ? CLAMP(pt_size, 8, 22) : 11;
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_size_new(clamped_pt * PANGO_SCALE));
    pango_attr_list_insert(attrs, pango_attr_weight_new(weight_idx_to_pango_enum(weight_idx)));
    gtk_label_set_attributes(GTK_LABEL(lbl), attrs);
    pango_attr_list_unref(attrs);
}

static void apply_label_font_size(GtkWidget *lbl, gint pt_size, gboolean bold) {
    apply_label_font_size_and_weight(lbl, pt_size, bold ? 2 : 0);
}

/* =========================================================================
 * App Icon Pixbuf Loader & Smooth Hover Zoom Effect
 * ========================================================================= */
static GdkPixbuf *load_app_pixbuf_at_size(GAppInfo *app, gint px_size) {
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    GdkPixbuf *pb = NULL;
    GIcon *gicon = g_app_info_get_icon(app);

    if (gicon) {
        GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(theme, gicon, px_size, GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info) {
            pb = gtk_icon_info_load_icon(info, NULL);
            g_object_unref(info);
        }
    }
    if (!pb) {
        pb = gtk_icon_theme_load_icon(theme, "application-x-executable", px_size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
    return pb;
}

static gboolean on_app_tile_hover_enter(GtkWidget *eb, GdkEventCrossing *event, StartMenuUI *ui) {
    GtkWidget *child_box = gtk_bin_get_child(GTK_BIN(eb));
    if (child_box) gtk_style_context_add_class(gtk_widget_get_style_context(child_box), "app-tile-hovered");
    GtkWidget *parent_row = gtk_widget_get_parent(eb);
    if (parent_row) gtk_style_context_add_class(gtk_widget_get_style_context(parent_row), "app-tile-hovered");
    if (ui && ui->shell && !ui->shell->start_hover_effect) return FALSE;

    GtkWidget *img = g_object_get_data(G_OBJECT(eb), "icon-img");
    GdkPixbuf *hover_pb = g_object_get_data(G_OBJECT(eb), "pb-hover");
    if (GTK_IS_IMAGE(img) && hover_pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(img), hover_pb);
    }
    return FALSE;
}

static gboolean on_app_tile_hover_leave(GtkWidget *eb, GdkEventCrossing *event, StartMenuUI *ui) {
    if (event->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    GtkWidget *child_box = gtk_bin_get_child(GTK_BIN(eb));
    if (child_box) gtk_style_context_remove_class(gtk_widget_get_style_context(child_box), "app-tile-hovered");
    GtkWidget *parent_row = gtk_widget_get_parent(eb);
    if (parent_row) gtk_style_context_remove_class(gtk_widget_get_style_context(parent_row), "app-tile-hovered");

    GtkWidget *img = g_object_get_data(G_OBJECT(eb), "icon-img");
    GdkPixbuf *norm_pb = g_object_get_data(G_OBJECT(eb), "pb-normal");
    if (GTK_IS_IMAGE(img) && norm_pb) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(img), norm_pb);
    }
    return FALSE;
}

/* =========================================================================
 * Enhanced Right-Click Context Menu (Stays Open + Favorites + Pin + Desktop)
 * ========================================================================= */
static void ensure_polkit_agent_running(void) {
    g_spawn_command_line_async(
        "sh -c \"pgrep -f 'polkit.*agent|lxpolkit|xfce-polkit|mate-polkit' >/dev/null 2>&1 || "
        "/usr/lib/policykit-1-gnome/polkit-gnome-authentication-agent-1 >/dev/null 2>&1 & "
        "pgrep -f 'polkit.*agent|lxpolkit|xfce-polkit|mate-polkit' >/dev/null 2>&1 || "
        "/usr/libexec/polkit-gnome-authentication-agent-1 >/dev/null 2>&1 & "
        "pgrep -f 'polkit.*agent|lxpolkit|xfce-polkit|mate-polkit' >/dev/null 2>&1 || "
        "lxpolkit >/dev/null 2>&1 & "
        "pgrep -f 'polkit.*agent|lxpolkit|xfce-polkit|mate-polkit' >/dev/null 2>&1 || "
        "/usr/lib/xfce-polkit/xfce-polkit >/dev/null 2>&1 &\"",
        NULL);
}

static gchar *strip_desktop_field_codes(const gchar *cmd_line) {
    if (!cmd_line) return g_strdup("");
    GString *s = g_string_new("");
    for (const gchar *p = cmd_line; *p != '\0'; p++) {
        if (*p == '%' && *(p + 1) != '\0') {
            p++;
            continue;
        }
        g_string_append_c(s, *p);
    }
    return g_strstrip(g_string_free(s, FALSE));
}

static void on_spawned_child_exited(GPid pid, gint status, gpointer user_data) {
    g_spawn_close_pid(pid);
}

static void spawn_command_with_live_parent(const gchar *cmd_line) {
    if (!cmd_line || cmd_line[0] == '\0') return;
    gint argc = 0;
    gchar **argv = NULL;
    if (!g_shell_parse_argv(cmd_line, &argc, &argv, NULL) || !argv) return;

    GPid child_pid = 0;
    /* CRITICAL (WhiskerMenu / libxfce4ui method):
       G_SPAWN_DO_NOT_REAP_CHILD prevents GLib from double-forking, so the child process
       keeps xfce4-panel as its live PPID instead of being reparented to PID 1 (which pkexec rejects). */
    if (g_spawn_async(NULL, argv, NULL,
                      G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                      NULL, NULL, &child_pid, NULL)) {
        g_child_watch_add(child_pid, on_spawned_child_exited, NULL);
    }
    g_strfreev(argv);
}

/* Close the menu with the chosen exit animation (instant when animations are off) */
static void start_menu_close(StartMenuUI *ui) {
    if (!ui || !ui->popup_win) return;
    shell_popup_hide(ui->popup_win);   /* before forgetting the dock anchor: Slide uses it */
    g_object_set_data(G_OBJECT(ui->popup_win), "dock-anchor-btn", NULL);
    if (GTK_IS_TOGGLE_BUTTON(ui->btn)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->btn), FALSE);
}

/* Same launcher as the taskbar/dock: honours Terminal=true and Path=, and pkexec apps */
static void launch_app_and_close(StartMenuUI *ui, GAppInfo *app) {
    if (app) usage_record_launch(app);
    start_menu_close(ui);
    if (app) shell_launch_app_info(app);
}

static void on_fav_menu_item_activated(GtkMenuItem *item, GAppInfo *app) {
    if (g_start_ui && g_start_ui->shell) {
        toggle_app_favorite(g_start_ui->shell, app);
    }
}

static void on_pin_menu_item_activated(GtkMenuItem *item, GAppInfo *app) {
    taskbar_pin_app_info(app);
}

static void on_add_to_desktop_activated(GtkMenuItem *item, GAppInfo *app) {
    if (!G_IS_DESKTOP_APP_INFO(app)) return;
    const gchar *src_path = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app));
    const gchar *desktop_dir = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);
    if (!src_path || !desktop_dir) return;

    gchar *basename = g_path_get_basename(src_path);
    gchar *dest_path = g_build_filename(desktop_dir, basename, NULL);
    /* Copy with GIO (no shell quoting issues with apostrophes etc.) */
    GFile *src = g_file_new_for_path(src_path);
    GFile *dst = g_file_new_for_path(dest_path);
    if (g_file_copy(src, dst, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, NULL)) {
        g_file_set_attribute_uint32(dst, G_FILE_ATTRIBUTE_UNIX_MODE, 0755,
                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);
        g_file_set_attribute_string(dst, "metadata::trusted", "true",
                                    G_FILE_QUERY_INFO_NONE, NULL, NULL);
    }
    g_object_unref(src);
    g_object_unref(dst);
    g_free(dest_path);
    g_free(basename);
}

static void on_run_as_root_activated(GtkMenuItem *item, GAppInfo *app) {
    ensure_polkit_agent_running();
    const gchar *raw_cmd = g_app_info_get_commandline(app);
    if (!raw_cmd) raw_cmd = g_app_info_get_executable(app);
    if (!raw_cmd) return;

    gchar *clean = strip_desktop_field_codes(raw_cmd);

    if (strstr(clean, "pkexec")) {
        spawn_command_with_live_parent(clean);
    } else {
        const gchar *disp = g_getenv("DISPLAY");
        const gchar *xauth = g_getenv("XAUTHORITY");
        if (!disp || disp[0] == '\0') disp = ":0";
        gchar *cmd = NULL;
        if (xauth && xauth[0] != '\0') {
            cmd = g_strdup_printf("pkexec env DISPLAY=%s XAUTHORITY=%s %s", disp, xauth, clean);
        } else {
            cmd = g_strdup_printf("pkexec %s", clean);
        }
        spawn_command_with_live_parent(cmd);
        g_free(cmd);
    }
    g_free(clean);
    if (g_start_ui) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_start_ui->btn), FALSE);
}

static gboolean clear_context_menu_guard_idle(gpointer data) {
    GtkWidget *popup_win = GTK_WIDGET(data);
    if (GTK_IS_WIDGET(popup_win)) {
        if (gtk_widget_get_visible(popup_win)) {
            gtk_window_present(GTK_WINDOW(popup_win));
            if (g_start_ui && GTK_IS_WIDGET(g_start_ui->search_entry) &&
                gtk_widget_get_visible(g_start_ui->search_entry)) {
                gtk_widget_grab_focus(g_start_ui->search_entry);
            }
        }
        g_object_set_data(G_OBJECT(popup_win), "context-menu-open", GINT_TO_POINTER(0));
    }
    return G_SOURCE_REMOVE;
}

static void on_app_context_menu_deactivate(GtkMenuShell *ms, GtkWidget *popup_win) {
    g_timeout_add(120, clear_context_menu_guard_idle, popup_win);
}

static gboolean on_app_widget_right_click(GtkWidget *widget, GdkEventButton *event, GAppInfo *app);

static gboolean on_grid_tile_button_press(GtkWidget *widget, GdkEventButton *event, GAppInfo *app) {
    if (event->type == GDK_BUTTON_PRESS && event->button == 1 && app && g_start_ui && g_start_ui->shell &&
        (g_start_ui->shell->start_style == START_STYLE_GNOME || g_object_get_data(G_OBJECT(widget), "fav-draggable"))) {
        /* GNOME App Grid: click = flow-box activation on release, so apps can be dragged.
           Mark this press so the grid background (same event, bubbling up) doesn't close the grid. */
        if (g_start_ui->popup_win)
            g_object_set_data(G_OBJECT(g_start_ui->popup_win), "tile-press-time", GUINT_TO_POINTER(event->time));
        return FALSE;
    }
    if (event->type == GDK_BUTTON_PRESS && event->button == 1 && app && g_start_ui) {
        if (g_start_ui->popup_win) {
            gtk_widget_hide(g_start_ui->popup_win);
        }
        launch_app_and_close(g_start_ui, app);
        return TRUE;
    }
    return on_app_widget_right_click(widget, event, app);
}

static const gchar *find_app_store_command(void);

/* =========================================================================
 * Shared app actions (Start menu + taskbar/dock right-click menus)
 * ========================================================================= */
void app_run_as_admin(GAppInfo *app) {
    if (app) on_run_as_root_activated(NULL, app);
}

static void on_jump_action_activate(GtkMenuItem *mi, GDesktopAppInfo *info) {
    const gchar *action = g_object_get_data(G_OBJECT(mi), "jump-action");
    if (!info || !action) return;
    GdkAppLaunchContext *ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    g_desktop_app_info_launch_action(info, action, G_APP_LAUNCH_CONTEXT(ctx));
    g_object_unref(ctx);
}

/* The app's own quick actions from its .desktop file (e.g. "New Private Window") */
gint app_append_jump_actions(GtkWidget *menu, GAppInfo *app) {
    if (!G_IS_DESKTOP_APP_INFO(app)) return 0;
    GDesktopAppInfo *info = G_DESKTOP_APP_INFO(app);
    const gchar *const *actions = g_desktop_app_info_list_actions(info);
    gint n = 0;
    for (gint i = 0; actions && actions[i]; i++) {
        gchar *name = g_desktop_app_info_get_action_name(info, actions[i]);
        if (!name || !name[0]) { g_free(name); continue; }
        GtkWidget *mi = gtk_menu_item_new_with_label(name);
        g_free(name);
        g_object_set_data_full(G_OBJECT(mi), "jump-action", g_strdup(actions[i]), g_free);
        g_object_set_data_full(G_OBJECT(mi), "jump-info", g_object_ref(info), g_object_unref);
        g_signal_connect(mi, "activate", G_CALLBACK(on_jump_action_activate), info);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
        n++;
    }
    return n;
}

/* Edit the launcher's name/icon/command. System launchers are copied to
   ~/.local/share/applications first (that copy overrides the system one). */
void app_edit_launcher(GAppInfo *app) {
    if (!G_IS_DESKTOP_APP_INFO(app)) return;
    const gchar *path = g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app));
    if (!path) return;
    gchar *user_dir = g_build_filename(g_get_user_data_dir(), "applications", NULL);
    gchar *target = NULL;
    if (g_str_has_prefix(path, user_dir)) {
        target = g_strdup(path);
    } else {
        g_mkdir_with_parents(user_dir, 0755);
        gchar *base = g_path_get_basename(path);
        target = g_build_filename(user_dir, base, NULL);
        g_free(base);
        if (!g_file_test(target, G_FILE_TEST_EXISTS)) {
            GFile *src = g_file_new_for_path(path), *dst = g_file_new_for_path(target);
            g_file_copy(src, dst, G_FILE_COPY_NONE, NULL, NULL, NULL, NULL);
            g_object_unref(src); g_object_unref(dst);
        }
    }
    gchar *argv[] = { "exo-desktop-item-edit", target, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_free(target);
    g_free(user_dir);
}

/* Open the installed app store on this app (details page where the store supports it) */
void app_open_uninstall(GAppInfo *app) {
    const gchar *store = find_app_store_command();
    if (!store) return;
    const gchar *id = app ? g_app_info_get_id(app) : NULL;
    if (id && g_strcmp0(store, "gnome-software") == 0) {
        gchar *arg = g_strdup_printf("--details=%s", id);
        gchar *argv[] = { "gnome-software", arg, NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
        g_free(arg);
    } else {
        spawn_command_with_live_parent(store);
    }
}

gboolean app_store_available(void) {
    return find_app_store_command() != NULL;
}

static void on_sm_edit_launcher(GtkMenuItem *mi, GAppInfo *app) { app_edit_launcher(app); }
static void on_sm_uninstall(GtkMenuItem *mi, GAppInfo *app) { app_open_uninstall(app); }

static gboolean on_app_widget_right_click(GtkWidget *widget, GdkEventButton *event, GAppInfo *app) {
    if (event->type == GDK_BUTTON_PRESS && event->button == 3 && app && g_start_ui) {
        /* Guard popup_win so focus-out-event does NOT close the Start Menu when right-clicking */
        g_object_set_data(G_OBJECT(g_start_ui->popup_win), "context-menu-open", GINT_TO_POINTER(1));

        GtkWidget *menu = gtk_menu_new();
        gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
        g_signal_connect(menu, "deactivate", G_CALLBACK(on_app_context_menu_deactivate), g_start_ui->popup_win);

        {   /* which app this menu is for */
            gchar *mk = g_markup_printf_escaped("<b>%s</b>", g_app_info_get_display_name(app));
            GtkWidget *hi = gtk_menu_item_new_with_label("");
            gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(hi))), mk);
            g_free(mk);
            gtk_widget_set_sensitive(hi, FALSE);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), hi);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        }

        gboolean in_fav = is_app_in_favorites(g_start_ui->shell, app);
        gboolean list_style = (g_start_ui->shell->start_style == START_STYLE_DEFAULT);
        GtkWidget *fav_item = gtk_menu_item_new_with_label(
            list_style ? (in_fav ? "📌 Unpin from top" : "📌 Pin to top")
                       : (in_fav ? "❌ Remove from Favorites" : "⭐ Add to Favorites"));
        g_signal_connect(fav_item, "activate", G_CALLBACK(on_fav_menu_item_activated), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), fav_item);

        win10_append_resize_menu(menu, app);

        GtkWidget *pin_item = gtk_menu_item_new_with_label("📌 Pin to Taskbar / Dock");
        g_signal_connect(pin_item, "activate", G_CALLBACK(on_pin_menu_item_activated), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), pin_item);

        if (G_IS_DESKTOP_APP_INFO(app) && g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app))) {
            GtkWidget *desk_item = gtk_menu_item_new_with_label("🖥️ Add to Desktop");
            g_signal_connect(desk_item, "activate", G_CALLBACK(on_add_to_desktop_activated), app);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), desk_item);
        }

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        GtkWidget *root_item = gtk_menu_item_new_with_label("⚡ Run as Administrator (pkexec)");
        g_signal_connect(root_item, "activate", G_CALLBACK(on_run_as_root_activated), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), root_item);

        /* The app's own quick actions */
        GtkWidget *jump_sep = gtk_separator_menu_item_new();
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), jump_sep);
        if (app_append_jump_actions(menu, app) == 0) gtk_widget_destroy(jump_sep);

        gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
        if (G_IS_DESKTOP_APP_INFO(app) && g_desktop_app_info_get_filename(G_DESKTOP_APP_INFO(app))) {
            GtkWidget *edit_item = gtk_menu_item_new_with_label("✏️ Edit Launcher…");
            g_signal_connect(edit_item, "activate", G_CALLBACK(on_sm_edit_launcher), app);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), edit_item);
        }
        if (app_store_available()) {
            GtkWidget *un_item = gtk_menu_item_new_with_label("🗑 Uninstall…");
            gtk_widget_set_tooltip_text(un_item, "Open your app store / package manager on this app");
            g_signal_connect(un_item, "activate", G_CALLBACK(on_sm_uninstall), app);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), un_item);
        }

        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
        return TRUE;
    }
    return FALSE;
}

static void on_app_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    StartMenuUI *ui = (StartMenuUI *)user_data;
    const gchar *cat_jump = g_object_get_data(G_OBJECT(row), "win7-cat-jump");
    if (cat_jump) {
        g_free(ui->active_category);
        ui->active_category = g_strdup(cat_jump);
        if (ui->list_box) gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui->list_box));
        if (ui->win7_left_stack) gtk_stack_set_visible_child_name(GTK_STACK(ui->win7_left_stack), "apps");
        if (ui->win7_toggle_btn) gtk_button_set_label(GTK_BUTTON(ui->win7_toggle_btn), "◂ Back to Categories");
        return;
    }
    if (g_object_get_data(G_OBJECT(row), "is-alpha-header")) {
        if (ui->shell && ui->shell->start_style == START_STYLE_WIN10) win10_show_letter_picker(ui);
        return;
    }
    GAppInfo *app = g_object_get_data(G_OBJECT(row), "app-info");
    if (app) launch_app_and_close(ui, app);
}

static void on_flow_child_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user_data) {
    StartMenuUI *ui = (StartMenuUI *)user_data;
    GAppInfo *app = g_object_get_data(G_OBJECT(child), "app-info");
    if (app) launch_app_and_close(ui, app);
}

static gboolean match_app_item(GObject *item, StartMenuUI *ui) {
    const gchar *query = ui->search_entry ? gtk_entry_get_text(GTK_ENTRY(ui->search_entry)) : "";

    if (g_object_get_data(item, "is-alpha-header")) {
        return (!query || query[0] == '\0');
    }
    if (g_object_get_data(item, "is-rec-row")) {
        return (!query || query[0] == '\0') &&
               (!ui->active_category || g_strcmp0(ui->active_category, "All") == 0);
    }

    if (g_object_get_data(item, "is-pinned-row") && query && query[0]) return FALSE;   /* it is in A–Z too */
    const gchar *search_key = g_object_get_data(item, "search-key");
    GAppInfo *app = g_object_get_data(item, "app-info");

    if (!sm_style_fullscreen(ui->shell)) {
        if ((!query || query[0] == '\0') && ui->active_category && g_strcmp0(ui->active_category, "All") != 0) {
            if (!app || !app_matches_category_filter(ui->shell, app, ui->active_category)) return FALSE;
        }
    }

    if (!query || query[0] == '\0') return TRUE;
    if (!search_key) return FALSE;

    gchar *q_down = g_utf8_strdown(query, -1);
    gboolean match = (strstr(search_key, q_down) != NULL);
    g_free(q_down);
    return match;
}

static gboolean filter_list_func(GtkListBoxRow *row, gpointer user_data) {
    return match_app_item(G_OBJECT(row), (StartMenuUI *)user_data);
}

static gboolean filter_flow_func(GtkFlowBoxChild *child, gpointer user_data) {
    return match_app_item(G_OBJECT(child), (StartMenuUI *)user_data);
}

/* =========================================================================
 * Fullscreen Horizontal Deck Navigation & Option B Tapering Circle Indicator
 * ========================================================================= */
static void fs_sync_active_category_pill(StartMenuUI *ui) {
    if (!ui->fs_page_cat_idx || !ui->fs_cat_buttons || ui->fs_total_pages <= 0) return;
    gint page = CLAMP(ui->fs_current_page, 0, ui->fs_total_pages - 1);
    gint active_cat = g_array_index(ui->fs_page_cat_idx, gint, page);

    for (guint i = 0; i < ui->fs_cat_buttons->len; i++) {
        GtkWidget *btn = g_ptr_array_index(ui->fs_cat_buttons, i);
        GtkStyleContext *ctx = gtk_widget_get_style_context(btn);
        if ((gint)i == active_cat) {
            gtk_widget_set_state_flags(btn, GTK_STATE_FLAG_CHECKED, FALSE);
            gtk_style_context_add_class(ctx, "suggested-action");

            if (GTK_IS_SCROLLED_WINDOW(ui->fs_cat_scroll)) {
                GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(ui->fs_cat_scroll));
                if (hadj) {
                    GtkAllocation alloc;
                    gtk_widget_get_allocation(btn, &alloc);
                    gdouble page_sz = gtk_adjustment_get_page_size(hadj);
                    gdouble target = alloc.x + (alloc.width / 2.0) - (page_sz / 2.0);
                    gdouble max_val = gtk_adjustment_get_upper(hadj) - page_sz;
                    gtk_adjustment_set_value(hadj, CLAMP(target, 0.0, MAX(0.0, max_val)));
                }
            }
        } else {
            gtk_widget_unset_state_flags(btn, GTK_STATE_FLAG_CHECKED);
            gtk_style_context_remove_class(ctx, "suggested-action");
        }
    }
}

static GtkWidget *find_flow_box(GtkWidget *w) {
    if (GTK_IS_FLOW_BOX(w)) return w;
    if (!GTK_IS_CONTAINER(w)) return NULL;
    GtkWidget *hit = NULL;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
    for (GList *l = kids; l && !hit; l = l->next) hit = find_flow_box(l->data);
    g_list_free(kids);
    return hit;
}
static GtkWidget *fs_current_page_flow(StartMenuUI *ui) {
    if (!ui->fs_page_stack) return NULL;
    return find_flow_box(gtk_stack_get_visible_child(GTK_STACK(ui->fs_page_stack)));
}

static void fs_go_to_page(StartMenuUI *ui, gint target_page);

/* arrow keys past the page edge: next / previous page, same row */
static gboolean on_fs_page_keynav_failed(GtkWidget *fb, GtkDirectionType dir, StartMenuUI *ui) {
    if (dir != GTK_DIR_LEFT && dir != GTK_DIR_RIGHT) {
        if (dir == GTK_DIR_UP && ui->search_entry) { gtk_widget_grab_focus(ui->search_entry); return TRUE; }
        return FALSE;
    }
    gint cols = MAX(1, ui->fs_page_cols);
    GtkWidget *foc = ui->popup_win ? gtk_window_get_focus(GTK_WINDOW(ui->popup_win)) : NULL;
    gint idx = GTK_IS_FLOW_BOX_CHILD(foc) ? gtk_flow_box_child_get_index(GTK_FLOW_BOX_CHILD(foc)) : 0;
    gint row = idx / cols;
    gint target = ui->fs_current_page + (dir == GTK_DIR_RIGHT ? 1 : -1);
    if (target < 0 || target >= ui->fs_total_pages) return TRUE;
    fs_go_to_page(ui, target);
    GtkWidget *nfb = fs_current_page_flow(ui);
    if (!GTK_IS_FLOW_BOX(nfb)) return TRUE;
    gint want = row * cols + (dir == GTK_DIR_RIGHT ? 0 : cols - 1);
    GtkFlowBoxChild *c = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(nfb), want);
    while (!c && want > 0) c = gtk_flow_box_get_child_at_index(GTK_FLOW_BOX(nfb), --want);
    if (c) gtk_widget_grab_focus(GTK_WIDGET(c));
    return TRUE;
}

static void fs_go_to_page(StartMenuUI *ui, gint target_page) {
    if (!ui->fs_page_stack || ui->fs_total_pages <= 0) return;
    gint clamped = CLAMP(target_page, 0, ui->fs_total_pages - 1);
    if (clamped == ui->fs_current_page) return;

    GtkStackTransitionType trans = (clamped > ui->fs_current_page)
        ? GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT
        : GTK_STACK_TRANSITION_TYPE_SLIDE_RIGHT;

    ui->fs_current_page = clamped;
    gchar *pname = g_strdup_printf("page_%d", clamped);
    gtk_stack_set_visible_child_full(GTK_STACK(ui->fs_page_stack), pname, trans);
    g_free(pname);

    fs_sync_active_category_pill(ui);
    if (ui->fs_dots_da) gtk_widget_queue_draw(ui->fs_dots_da);
    if (ui->gnome_peek_prev) gnome_update_peeks(ui);
}

static gboolean on_fs_deck_scroll(GtkWidget *widget, GdkEventScroll *event, StartMenuUI *ui) {
    if (!ui || !ui->shell || !sm_style_fullscreen(ui->shell) || ui->fs_total_pages <= 1) return FALSE;

    if (ui->fs_mode_stack && g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(ui->fs_mode_stack)), "search") == 0) {
        return FALSE;
    }

    if (event->time > 0 && ui->fs_last_scroll_time > 0 &&
        (event->time - ui->fs_last_scroll_time) < 150u) {                 /* one page per wheel notch (GNOME's scroll timeout) */
        return TRUE;
    }

    gint delta = 0;
    if (event->direction == GDK_SCROLL_DOWN || event->direction == GDK_SCROLL_RIGHT) {
        delta = 1;
    } else if (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_LEFT) {
        delta = -1;
    } else if (event->direction == GDK_SCROLL_SMOOTH) {
        gdouble dx = 0.0, dy = 0.0;
        if (gdk_event_get_scroll_deltas((GdkEvent *)event, &dx, &dy)) {
            if (dy > 0.12 || dx > 0.12) delta = 1;
            else if (dy < -0.12 || dx < -0.12) delta = -1;
        }
    }

    if (delta != 0) {
        ui->fs_last_scroll_time = event->time;
        fs_go_to_page(ui, ui->fs_current_page + delta);
        return TRUE;
    }
    return FALSE;
}

static void start_popup_close(StartMenuUI *ui) {
    if (ui->popup_win) {
        shell_popup_hide(ui->popup_win);   /* before forgetting the dock anchor: Slide uses it */
        g_object_set_data(G_OBJECT(ui->popup_win), "dock-anchor-btn", NULL);
    }
    if (GTK_IS_TOGGLE_BUTTON(ui->btn) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->btn)))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->btn), FALSE);
}

/* While the menu is open it holds the keyboard, so the Super shortcut never reaches
   Keyboard Shortcuts: the menu closes itself when Super is tapped alone (pressed and
   released with no other key - Super+E and the like still work as before). */
static gboolean on_start_popup_key_release(GtkWidget *win, GdkEventKey *event, StartMenuUI *ui) {
    if (!ui || !ui->popup_win) return FALSE;
    gboolean super = (event->keyval == GDK_KEY_Super_L || event->keyval == GDK_KEY_Super_R);
    if (super && g_object_get_data(G_OBJECT(ui->popup_win), "super-armed")) {
        g_object_set_data(G_OBJECT(ui->popup_win), "super-armed", NULL);
        if (ui->shell && ui->shell->super_key_start) { start_popup_close(ui); return TRUE; }
    }
    return FALSE;
}

static gboolean on_start_popup_key_press(GtkWidget *win, GdkEventKey *event, StartMenuUI *ui) {
    if (!ui || !ui->shell) return FALSE;
    if (ui->popup_win)   /* armed by a lone Super press, cleared by any other key */
        g_object_set_data(G_OBJECT(ui->popup_win), "super-armed",
                          (event->keyval == GDK_KEY_Super_L || event->keyval == GDK_KEY_Super_R) ? GINT_TO_POINTER(1) : NULL);

    if (event->keyval == GDK_KEY_Escape && ui->search_entry &&
        gtk_entry_get_text(GTK_ENTRY(ui->search_entry))[0] != '\0') {
        gtk_entry_set_text(GTK_ENTRY(ui->search_entry), "");      /* Esc: clear the search first */
        gtk_widget_grab_focus(ui->search_entry);
        return TRUE;
    }
    if (event->keyval == GDK_KEY_Escape) {
        start_popup_close(ui);
        return TRUE;
    }

    if (sm_style_fullscreen(ui->shell)) {
        const gchar *q = ui->search_entry ? gtk_entry_get_text(GTK_ENTRY(ui->search_entry)) : "";
        gboolean search_empty = (!q || q[0] == '\0');

        GtkWidget *foc = ui->popup_win ? gtk_window_get_focus(GTK_WINDOW(ui->popup_win)) : NULL;
        if (GTK_IS_FLOW_BOX_CHILD(foc)) search_empty = FALSE;   /* arrows move between apps then */
        if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_Page_Down) { fs_go_to_page(ui, ui->fs_current_page + 1); return TRUE; }
        if ((event->state & GDK_CONTROL_MASK) && event->keyval == GDK_KEY_Page_Up)   { fs_go_to_page(ui, ui->fs_current_page - 1); return TRUE; }
        if (event->keyval == GDK_KEY_Page_Down || (search_empty && event->keyval == GDK_KEY_Right)) {
            fs_go_to_page(ui, ui->fs_current_page + 1);
            return TRUE;
        }
        if (event->keyval == GDK_KEY_Page_Up || (search_empty && event->keyval == GDK_KEY_Left)) {
            fs_go_to_page(ui, ui->fs_current_page - 1);
            return TRUE;
        }
        if (search_empty && (event->keyval == GDK_KEY_Tab || event->keyval == GDK_KEY_ISO_Left_Tab)) {
            if (ui->fs_page_cat_idx && ui->fs_cat_first_page && ui->fs_cat_buttons && ui->fs_cat_buttons->len > 1) {
                gint cur_cat = g_array_index(ui->fs_page_cat_idx, gint, CLAMP(ui->fs_current_page, 0, ui->fs_total_pages - 1));
                gint dir = (event->keyval == GDK_KEY_ISO_Left_Tab || (event->state & GDK_SHIFT_MASK)) ? -1 : 1;
                gint next_cat = (cur_cat + dir + (gint)ui->fs_cat_buttons->len) % (gint)ui->fs_cat_buttons->len;
                gint target_p = g_array_index(ui->fs_cat_first_page, gint, next_cat);
                fs_go_to_page(ui, target_p);
                return TRUE;
            }
        }
    }

    if (ui->search_entry && !gtk_widget_has_focus(ui->search_entry)) {
        guint32 uc = gdk_keyval_to_unicode(event->keyval);
        if (g_unichar_isgraph(uc) && !(event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK))) {
            gtk_widget_grab_focus(ui->search_entry);
            gtk_editable_set_position(GTK_EDITABLE(ui->search_entry), -1);
        }
    }
    return FALSE;
}

static gboolean on_fs_background_button_press(GtkWidget *eb, GdkEventButton *event, StartMenuUI *ui) {
    if (event->type == GDK_BUTTON_PRESS && event->button == 1 &&
        ui && ui->shell && sm_style_fullscreen(ui->shell)) {
        if (ui->popup_win && GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(ui->popup_win), "tile-press-time")) == event->time)
            return FALSE;                      /* that press was on an app tile, not on empty space */
        if (ui->popup_win) shell_popup_hide(ui->popup_win);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->btn), FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_fs_prev_page_clicked(GtkButton *btn, StartMenuUI *ui) {
    fs_go_to_page(ui, ui->fs_current_page - 1);
}

static void on_fs_next_page_clicked(GtkButton *btn, StartMenuUI *ui) {
    fs_go_to_page(ui, ui->fs_current_page + 1);
}

static void on_fs_cat_left_clicked(GtkButton *btn, StartMenuUI *ui) {
    if (!GTK_IS_SCROLLED_WINDOW(ui->fs_cat_scroll)) return;
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(ui->fs_cat_scroll));
    if (hadj) {
        gdouble cur = gtk_adjustment_get_value(hadj);
        gtk_adjustment_set_value(hadj, MAX(0.0, cur - 180.0));
    }
}

static void on_fs_cat_right_clicked(GtkButton *btn, StartMenuUI *ui) {
    if (!GTK_IS_SCROLLED_WINDOW(ui->fs_cat_scroll)) return;
    GtkAdjustment *hadj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(ui->fs_cat_scroll));
    if (hadj) {
        gdouble cur = gtk_adjustment_get_value(hadj);
        gdouble max_v = gtk_adjustment_get_upper(hadj) - gtk_adjustment_get_page_size(hadj);
        gtk_adjustment_set_value(hadj, MIN(MAX(0.0, max_v), cur + 180.0));
    }
}

static void compute_dots_window(gint total, gint current, gint *out_start, gint *out_count) {
    const gint max_vis = 11;
    if (total <= max_vis) {
        *out_start = 0;
        *out_count = total;
        return;
    }
    gint start = current - (max_vis / 2);
    if (start < 0) start = 0;
    if (start + max_vis > total) start = total - max_vis;
    *out_start = start;
    *out_count = max_vis;
}

/* GNOME-style page indicator: small dots, the current page drawn as a wider
   pill that slides (animated) when the page changes. */
#define FS_DOT      7.0
#define FS_DOT_GAP  7.0
#define FS_PILL_W   (FS_DOT * 3.6)
#define FS_DOTS_MAX 64

static gdouble   g_fs_dot_w[FS_DOTS_MAX];
static gint      g_fs_dot_win_start = -1;
static guint     g_fs_dot_anim_id   = 0;
static GtkWidget *g_fs_dot_widget   = NULL;

static gboolean fs_dots_anim_tick(gpointer data) {
    StartMenuUI *ui = data;
    if (!ui || !g_fs_dot_widget) { g_fs_dot_anim_id = 0; return G_SOURCE_REMOVE; }
    gint win_start = 0, vis = 0;
    compute_dots_window(ui->fs_total_pages, ui->fs_current_page, &win_start, &vis);
    gboolean moving = FALSE;
    for (gint i = 0; i < vis && i < FS_DOTS_MAX; i++) {
        gdouble t = (win_start + i == ui->fs_current_page) ? FS_PILL_W : FS_DOT;
        gdouble d = t - g_fs_dot_w[i];
        if (fabs(d) > 0.25) { g_fs_dot_w[i] += d * 0.28; moving = TRUE; }
        else g_fs_dot_w[i] = t;
    }
    gtk_widget_queue_draw(g_fs_dot_widget);
    if (!moving) { g_fs_dot_anim_id = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}

/* Current widths (animated) and the x where dot i starts; returns total width */
static gdouble fs_dots_layout(StartMenuUI *ui, GtkWidget *widget, gint *win_start, gint *vis, gdouble *xs) {
    compute_dots_window(ui->fs_total_pages, ui->fs_current_page, win_start, vis);
    gboolean need_anim = FALSE;
    if (*win_start != g_fs_dot_win_start) {           /* window scrolled: snap */
        for (gint i = 0; i < FS_DOTS_MAX; i++) g_fs_dot_w[i] = 0;
        g_fs_dot_win_start = *win_start;
    }
    gdouble total = 0;
    for (gint i = 0; i < *vis && i < FS_DOTS_MAX; i++) {
        gdouble t = (*win_start + i == ui->fs_current_page) ? FS_PILL_W : FS_DOT;
        if (g_fs_dot_w[i] <= 0) g_fs_dot_w[i] = t;
        if (fabs(g_fs_dot_w[i] - t) > 0.25) need_anim = TRUE;
        total += g_fs_dot_w[i] + (i ? FS_DOT_GAP : 0);
    }
    gdouble x = (gtk_widget_get_allocated_width(widget) - total) / 2.0;
    for (gint i = 0; i < *vis && i < FS_DOTS_MAX; i++) {
        if (xs) xs[i] = x;
        x += g_fs_dot_w[i] + FS_DOT_GAP;
    }
    if (g_fs_dot_widget != widget) {               /* the deck is rebuilt on style changes */
        if (g_fs_dot_widget) g_object_remove_weak_pointer(G_OBJECT(g_fs_dot_widget), (gpointer *)&g_fs_dot_widget);
        g_fs_dot_widget = widget;
        g_object_add_weak_pointer(G_OBJECT(widget), (gpointer *)&g_fs_dot_widget);
    }
    if (need_anim && !g_fs_dot_anim_id) g_fs_dot_anim_id = g_timeout_add(16, fs_dots_anim_tick, ui);
    return total;
}

static void fs_rounded_rect(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h) {
    gdouble r = MIN(w, h) / 2.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

static gboolean on_draw_fs_dots(GtkWidget *widget, cairo_t *cr, StartMenuUI *ui) {
    if (!ui || ui->fs_total_pages <= 0) return FALSE;
    gint h = gtk_widget_get_allocated_height(widget);
    gint win_start = 0, vis = 0;
    gdouble xs[FS_DOTS_MAX];
    gdouble total = fs_dots_layout(ui, widget, &win_start, &vis, xs);

    /* soft rounded track behind the dots */
    gdouble th = FS_DOT * 3.0;
    fs_rounded_rect(cr, xs[0] - FS_DOT * 1.4, (h - th) / 2.0, total + FS_DOT * 2.8, th);
    cairo_set_source_rgba(cr, 0.12, 0.12, 0.14, 0.55);
    cairo_fill(cr);

    ShellPalette pl;
    shell_palette_get(ui->shell, &pl);
    GdkRGBA ac = pl.accent;

    gdouble cy = h / 2.0;
    for (gint i = 0; i < vis && i < FS_DOTS_MAX; i++) {
        gboolean on = (win_start + i == ui->fs_current_page);
        /* pages beyond the visible window fade at the ends */
        gdouble a = 0.45;
        if ((win_start > 0 && i == 0) || (win_start + vis < ui->fs_total_pages && i == vis - 1)) a = 0.22;
        fs_rounded_rect(cr, xs[i], cy - FS_DOT / 2.0, g_fs_dot_w[i], FS_DOT);
        if (on) cairo_set_source_rgba(cr, ac.red, ac.green, ac.blue, 1.0);
        else    cairo_set_source_rgba(cr, 0.92, 0.92, 0.94, a);
        cairo_fill(cr);
    }
    return FALSE;
}

static gboolean on_press_fs_dots(GtkWidget *widget, GdkEventButton *event, StartMenuUI *ui) {
    if (!ui || ui->fs_total_pages <= 0 || event->button != 1) return FALSE;
    gint win_start = 0, vis = 0;
    gdouble xs[FS_DOTS_MAX];
    fs_dots_layout(ui, widget, &win_start, &vis, xs);
    for (gint i = 0; i < vis && i < FS_DOTS_MAX; i++) {
        if (event->x >= xs[i] - FS_DOT_GAP / 2.0 && event->x <= xs[i] + g_fs_dot_w[i] + FS_DOT_GAP / 2.0) {
            fs_go_to_page(ui, win_start + i);
            return TRUE;
        }
    }
    return FALSE;
}

static void on_search_entry_stop_search(GtkSearchEntry *entry, StartMenuUI *ui) {
    if (!ui) return;
    if (ui->popup_win) {
        shell_popup_hide(ui->popup_win);   /* before forgetting the dock anchor: Slide uses it */
        g_object_set_data(G_OBJECT(ui->popup_win), "dock-anchor-btn", NULL);
    }
    if (GTK_IS_TOGGLE_BUTTON(ui->btn) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->btn))) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->btn), FALSE);
    }
}

static void on_search_changed(GtkSearchEntry *entry, StartMenuUI *ui) {
    const gchar *q = gtk_entry_get_text(GTK_ENTRY(entry));
    update_search_extras(ui);
    if (ui->rec_widget) gtk_widget_set_visible(ui->rec_widget, !(q && q[0]));
    if (ui->win10_tiles_pane) gtk_widget_set_visible(ui->win10_tiles_pane, !(q && q[0]));
    if (ui->win10_list_stack && q && q[0]) gtk_stack_set_visible_child_name(GTK_STACK(ui->win10_list_stack), "list");
    if (ui->gnome_overview_scroll) gtk_widget_set_visible(ui->gnome_overview_scroll, !(q && q[0]));
    /* rank results: names starting with the text first, then words, then keywords */
    if (q && q[0]) { ensure_search_sort(ui, ui->list_box); ensure_search_sort(ui, ui->flow_box); }
    if (ui->list_box && g_object_get_data(G_OBJECT(ui->list_box), "search-sort-on")) gtk_list_box_invalidate_sort(GTK_LIST_BOX(ui->list_box));
    if (ui->flow_box && g_object_get_data(G_OBJECT(ui->flow_box), "search-sort-on")) gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(ui->flow_box));
    if (ui->shell && sm_style_fullscreen(ui->shell) && ui->fs_mode_stack) {
        if (q && q[0] != '\0') {
            gtk_stack_set_visible_child_name(GTK_STACK(ui->fs_mode_stack), "search");
            if (ui->flow_box) gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(ui->flow_box));
        } else {
            gtk_stack_set_visible_child_name(GTK_STACK(ui->fs_mode_stack), "deck");
        }
        return;
    }
    if (ui->shell && ui->shell->start_style == START_STYLE_WIN7 && ui->win7_left_stack) {
        if (q && q[0] != '\0') {
            gtk_stack_set_visible_child_name(GTK_STACK(ui->win7_left_stack), "apps");
            if (ui->win7_toggle_btn) gtk_button_set_label(GTK_BUTTON(ui->win7_toggle_btn), "◂ Back to Categories");
        }
    }
    if (ui->shell && ui->shell->start_style == START_STYLE_WIN11 && ui->win11_stack) {
        if (q && q[0] != '\0') {
            gtk_stack_set_visible_child_name(GTK_STACK(ui->win11_stack), "all");
            if (ui->win11_pinned_btn) gtk_label_set_markup(GTK_LABEL(ui->win11_pinned_btn), "<b>Search Results</b>");
            if (ui->win11_all_btn) gtk_button_set_label(GTK_BUTTON(ui->win11_all_btn), "‹  Pinned");
        } else {
            gtk_stack_set_visible_child_name(GTK_STACK(ui->win11_stack), "pinned");
            if (ui->win11_pinned_btn) gtk_label_set_markup(GTK_LABEL(ui->win11_pinned_btn), "<b>Pinned</b>");
            if (ui->win11_all_btn) gtk_button_set_label(GTK_BUTTON(ui->win11_all_btn), "All apps  ›");
        }
    }
    if (ui->list_box) gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui->list_box));
    if (ui->flow_box) gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(ui->flow_box));
}

static void apply_category_filter_from_btn(GtkWidget *btn, StartMenuUI *ui) {
    if (ui->shell && sm_style_fullscreen(ui->shell)) {
        gint target_page = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "fs-first-page"));
        if (ui->search_entry && gtk_entry_get_text(GTK_ENTRY(ui->search_entry))[0] != '\0') {
            gtk_entry_set_text(GTK_ENTRY(ui->search_entry), "");
        }
        fs_go_to_page(ui, target_page);
        return;
    }

    const gchar *cat = g_object_get_data(G_OBJECT(btn), "cat-filter");
    const gchar *next_cat = cat ? cat : "All";
    if (g_strcmp0(ui->active_category, next_cat) == 0) return;
    g_free(ui->active_category);
    ui->active_category = g_strdup(next_cat);
    if (ui->rec_widget) gtk_widget_set_visible(ui->rec_widget, g_strcmp0(next_cat, "All") == 0);
    if (ui->list_box) gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui->list_box));
    if (ui->list_box) gtk_list_box_invalidate_sort(GTK_LIST_BOX(ui->list_box));   /* Favourites: your order */
    if (ui->flow_box) gtk_flow_box_invalidate_sort(GTK_FLOW_BOX(ui->flow_box));
    if (ui->flow_box) gtk_flow_box_invalidate_filter(GTK_FLOW_BOX(ui->flow_box));
}

static void on_category_clicked(GtkButton *btn, StartMenuUI *ui) {
    apply_category_filter_from_btn(GTK_WIDGET(btn), ui);
}

static gboolean on_category_hover_enter(GtkWidget *btn, GdkEventCrossing *event, StartMenuUI *ui) {
    if (ui && ui->shell) {
        gint st = get_active_style_idx(ui->shell);
        if (ui->shell->start_style_hover_cat[st]) {
            apply_category_filter_from_btn(btn, ui);
        }
    }
    return FALSE;
}

static void on_place_shortcut_clicked(GtkButton *btn, const gchar *place_id) {
    if (!place_id) return;
    if (g_start_ui) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_start_ui->btn), FALSE);
    }

    if (g_strcmp0(place_id, "term") == 0) {
        g_spawn_command_line_async("exo-open --launch TerminalEmulator", NULL);
        return;
    }
    if (g_strcmp0(place_id, "settings") == 0) {
        g_spawn_command_line_async("xfce4-settings-manager", NULL);
        return;
    }
    if (g_strcmp0(place_id, "run") == 0) {
        g_spawn_command_line_async("xfce4-appfinder --collapsed", NULL);
        return;
    }
    if (g_strcmp0(place_id, "trash") == 0) {
        g_spawn_command_line_async("sh -c \"thunar trash:/// 2>/dev/null || exo-open --launch FileManager trash:///\"", NULL);
        return;
    }

    const gchar *home = g_get_home_dir();
    const gchar *target_dir = NULL;
    gchar *fallback_dir = NULL;

    if (g_strcmp0(place_id, "home") == 0) {
        target_dir = home;
    } else if (g_strcmp0(place_id, "docs") == 0) {
        target_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
        if (!target_dir) fallback_dir = g_build_filename(home, "Documents", NULL);
    } else if (g_strcmp0(place_id, "dl") == 0) {
        target_dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
        if (!target_dir) fallback_dir = g_build_filename(home, "Downloads", NULL);
    } else if (g_strcmp0(place_id, "pic") == 0) {
        target_dir = g_get_user_special_dir(G_USER_DIRECTORY_PICTURES);
        if (!target_dir) fallback_dir = g_build_filename(home, "Pictures", NULL);
    } else if (g_strcmp0(place_id, "mus") == 0) {
        target_dir = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
        if (!target_dir) fallback_dir = g_build_filename(home, "Music", NULL);
    } else if (g_strcmp0(place_id, "vid") == 0) {
        target_dir = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        if (!target_dir) fallback_dir = g_build_filename(home, "Videos", NULL);
    }

    const gchar *final_dir = target_dir ? target_dir : (fallback_dir ? fallback_dir : home);
    gchar *quoted = g_shell_quote(final_dir);
    gchar *cmd = g_strdup_printf("exo-open --launch FileManager %s", quoted);
    g_spawn_command_line_async(cmd, NULL);
    g_free(cmd);
    g_free(quoted);
    g_free(fallback_dir);
}

static void on_session_cmd(GtkButton *btn, const gchar *cmd) {
    g_spawn_command_line_async(cmd, NULL);
    if (g_start_ui) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_start_ui->btn), FALSE);
}

/* =========================================================================
 * Item 9: Confirmation Dialog for Log Out, Restart, and Shut Down
 * ========================================================================= */
static void on_confirm_power_action(GtkButton *btn, const gchar *action_type) {
    if (g_start_ui) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_start_ui->btn), FALSE);
    }

    const gchar *title = "Session Confirmation";
    const gchar *msg   = "Are you sure you want to continue?";
    const gchar *ok_lbl = "_Confirm";
    const gchar *cmd   = "xfce4-session-logout";

    if (g_strcmp0(action_type, "logout") == 0) {
        title  = "Confirm Log Out";
        msg    = "Are you sure you want to log out of your current XFCE session?";
        ok_lbl = "_Log Out";
        cmd    = "xfce4-session-logout --logout";
    } else if (g_strcmp0(action_type, "restart") == 0) {
        title  = "Confirm Restart";
        msg    = "Are you sure you want to restart your computer now?";
        ok_lbl = "_Restart";
        cmd    = "xfce4-session-logout --reboot || systemctl reboot";
    } else if (g_strcmp0(action_type, "shutdown") == 0) {
        title  = "Confirm Shut Down";
        msg    = "Are you sure you want to shut down your computer now?";
        ok_lbl = "_Shut Down";
        cmd    = "xfce4-session-logout --halt || systemctl poweroff";
    }

    GtkWidget *dlg = gtk_message_dialog_new(
        NULL, 0, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dlg), "%s", msg);
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
                           "_Cancel", GTK_RESPONSE_CANCEL,
                           ok_lbl, GTK_RESPONSE_ACCEPT,
                           NULL);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_type_hint(GTK_WINDOW(dlg), GDK_WINDOW_TYPE_HINT_NORMAL);
    gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);

    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp == GTK_RESPONSE_ACCEPT) {
        gchar *argv[] = { "/bin/sh", "-c", (gchar *)cmd, NULL };
        g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, NULL);
    }
}

static gboolean on_draw_circular_avatar(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    gint size = GPOINTER_TO_INT(user_data);
    GdkPixbuf *pix = NULL;

    gchar *face_path = g_build_filename(g_get_home_dir(), ".face", NULL);
    if (g_file_test(face_path, G_FILE_TEST_EXISTS)) {
        pix = gdk_pixbuf_new_from_file_at_scale(face_path, size, size, FALSE, NULL);
    }
    g_free(face_path);

    if (!pix) {
        GtkIconTheme *theme = gtk_icon_theme_get_default();
        pix = gtk_icon_theme_load_icon(theme, "avatar-default", size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }

    cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0 - 1.5, 0, 2 * G_PI);
    cairo_clip_preserve(cr);

    if (pix) {
        gdk_cairo_set_source_pixbuf(cr, pix, 0, 0);
        cairo_paint(cr);
        g_object_unref(pix);
    } else {
        cairo_set_source_rgb(cr, 0.10, 0.45, 0.91);
        cairo_fill_preserve(cr);
    }

    cairo_reset_clip(cr);
    cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0 - 1.5, 0, 2 * G_PI);
    cairo_set_source_rgb(cr, 0.10, 0.45, 0.91);
    cairo_set_line_width(cr, 2.0);
    cairo_stroke(cr);
    return FALSE;
}

static GtkWidget *create_circular_avatar_widget(gint px_size) {
    GtkWidget *da = gtk_drawing_area_new();
    gtk_widget_set_size_request(da, px_size, px_size);
    gtk_widget_set_halign(da, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
    g_signal_connect(da, "draw", G_CALLBACK(on_draw_circular_avatar), GINT_TO_POINTER(px_size));
    return da;
}

static GtkWidget *create_grid_tile_for_app(StartMenuUI *ui, GAppInfo *app, gint grid_icon_px, gint label_pt, gint cell_w, gint cell_h) {
    gboolean use_gen = ui->shell ? ui->shell->start_generic_names : FALSE;
    const gchar *formal_name = g_app_info_get_display_name(app);
    const gchar *name = resolve_app_primary_title(app, use_gen);
    const gchar *desc = g_app_info_get_description(app);

    gint hover_px = grid_icon_px + MAX(6, (grid_icon_px * 18) / 100);
    GdkPixbuf *pb_norm  = load_app_pixbuf_at_size(app, grid_icon_px);
    GdkPixbuf *pb_hover = load_app_pixbuf_at_size(app, hover_px);

    GtkWidget *img = pb_norm ? gtk_image_new_from_pixbuf(pb_norm)
                             : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_size_request(img, hover_px, hover_px);
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);

    GtkWidget *child = gtk_flow_box_child_new();
    gtk_widget_set_size_request(child, cell_w, cell_h);
    gtk_widget_set_valign(child, GTK_ALIGN_START);
    gtk_widget_set_halign(child, GTK_ALIGN_CENTER);

    GtkWidget *eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
    gtk_widget_set_size_request(eb, cell_w, cell_h);
    gtk_widget_add_events(eb, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(vbox), "app-grid-tile");
    gtk_widget_set_size_request(vbox, cell_w - 2, cell_h - 2);
    gtk_widget_set_halign(vbox, GTK_ALIGN_FILL);
    gtk_widget_set_valign(vbox, GTK_ALIGN_FILL);
    gtk_widget_set_margin_top(vbox, 1);
    gtk_widget_set_margin_bottom(vbox, 1);
    gtk_widget_set_margin_start(vbox, 1);
    gtk_widget_set_margin_end(vbox, 1);

    GtkWidget *lbl = gtk_label_new(name);
    if (app_is_new(app)) {
        const gchar *acc = (ui->shell && ui->shell->custom_accent && ui->shell->custom_accent[0]) ? ui->shell->custom_accent : "#3894FF";
        gchar *mk = g_markup_printf_escaped("<span foreground='%s'>●</span> %s", acc, name);
        gtk_label_set_markup(GTK_LABEL(lbl), mk);
        g_free(mk);
        gtk_widget_set_tooltip_text(lbl, "Newly installed");
    }
    apply_label_font_size(lbl, label_pt, FALSE);
    gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(lbl), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_lines(GTK_LABEL(lbl), 2);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(lbl), 11);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 11);
    gtk_label_set_justify(GTK_LABEL(lbl), GTK_JUSTIFY_CENTER);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.5);
    gtk_label_set_yalign(GTK_LABEL(lbl), 0.0);
    gtk_widget_set_size_request(lbl, cell_w - 8, label_pt * 2 + 14);

    if (desc && desc[0] != '\0') {
        gtk_widget_set_tooltip_text(eb, desc);
    }

    gtk_box_pack_start(GTK_BOX(vbox), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), lbl, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(eb), vbox);
    gtk_container_add(GTK_CONTAINER(child), eb);

    g_object_set_data(G_OBJECT(eb), "icon-img", img);
    if (pb_norm)  g_object_set_data_full(G_OBJECT(eb), "pb-normal", pb_norm, g_object_unref);
    if (pb_hover) g_object_set_data_full(G_OBJECT(eb), "pb-hover", pb_hover, g_object_unref);

    g_signal_connect(eb, "enter-notify-event", G_CALLBACK(on_app_tile_hover_enter), ui);
    g_signal_connect(eb, "leave-notify-event", G_CALLBACK(on_app_tile_hover_leave), ui);
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_grid_tile_button_press), app);

    GString *kws = g_string_new("");
    if (G_IS_DESKTOP_APP_INFO(app)) {
        const gchar *const *kw = g_desktop_app_info_get_keywords(G_DESKTOP_APP_INFO(app));
        for (gint k = 0; kw && kw[k]; k++) { g_string_append_c(kws, ' '); g_string_append(kws, kw[k]); }
    }
    const gchar *exe = g_app_info_get_executable(app);
    gchar *exe_base = exe ? g_path_get_basename(exe) : g_strdup("");
    gchar *combined_search = g_strdup_printf("%s %s %s %s %s",
        name, formal_name ? formal_name : "", desc ? desc : "", kws->str, exe_base);
    g_object_set_data_full(G_OBJECT(child), "app-info", g_object_ref(app), g_object_unref);
    g_object_set_data_full(G_OBJECT(child), "search-key", g_utf8_strdown(combined_search, -1), g_free);
    gchar *nm2 = g_strdup_printf("%s %s", name, formal_name ? formal_name : "");
    g_object_set_data_full(G_OBJECT(child), "search-name", g_utf8_strdown(nm2, -1), g_free);
    g_free(nm2);
    g_free(combined_search);
    g_string_free(kws, TRUE);
    g_free(exe_base);

    return child;
}

static GtkWidget *create_list_row_for_app(StartMenuUI *ui, GAppInfo *app, gint list_icon_px, gint hover_px, gint label_pt, gboolean show_desc, gboolean use_gen) {
    const gchar *formal_name = g_app_info_get_display_name(app);
    const gchar *primary_name = resolve_app_primary_title(app, use_gen);
    const gchar *desc = g_app_info_get_description(app);
    if ((!desc || desc[0] == '\0') && use_gen && formal_name && g_strcmp0(primary_name, formal_name) != 0) {
        desc = formal_name;
    }

    GdkPixbuf *pb_norm  = load_app_pixbuf_at_size(app, list_icon_px);
    GdkPixbuf *pb_hover = load_app_pixbuf_at_size(app, hover_px);
    GtkWidget *img = pb_norm ? gtk_image_new_from_pixbuf(pb_norm)
                             : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_size_request(img, hover_px, hover_px);

    GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "app-grid-tile");
    GtkWidget *eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
    gtk_widget_add_events(eb, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK | GDK_BUTTON_PRESS_MASK);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_start(hbox, 4);
    gtk_widget_set_margin_end(hbox, 4);
    gtk_widget_set_margin_top(hbox, 2);
    gtk_widget_set_margin_bottom(hbox, 2);
    gtk_container_set_border_width(GTK_CONTAINER(hbox), 4);

    gtk_box_pack_start(GTK_BOX(hbox), img, FALSE, FALSE, 0);

    if (show_desc && desc && desc[0] != '\0') {
        GtkWidget *text_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_set_valign(text_vbox, GTK_ALIGN_CENTER);

        GtkWidget *title_lbl = gtk_label_new(primary_name);
        apply_label_font_size(title_lbl, label_pt, TRUE);
        gtk_label_set_xalign(GTK_LABEL(title_lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(title_lbl), PANGO_ELLIPSIZE_END);

        GtkWidget *sub_lbl = gtk_label_new(desc);
        apply_label_font_size(sub_lbl, MAX(8, label_pt - 1), FALSE);
        gtk_label_set_xalign(GTK_LABEL(sub_lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(sub_lbl), PANGO_ELLIPSIZE_END);
        gtk_style_context_add_class(gtk_widget_get_style_context(sub_lbl), "dim-label");

        gtk_box_pack_start(GTK_BOX(text_vbox), title_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(text_vbox), sub_lbl, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), text_vbox, TRUE, TRUE, 0);
    } else {
        GtkWidget *lbl = gtk_label_new(primary_name);
        apply_label_font_size(lbl, label_pt, FALSE);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(hbox), lbl, TRUE, TRUE, 0);
    }
    if (app_is_new(app)) {
        GtkWidget *nb = gtk_label_new("New");
        gtk_style_context_add_class(gtk_widget_get_style_context(nb), "start-new-badge");
        gtk_widget_set_valign(nb, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(hbox), nb, FALSE, FALSE, 6);
    }

    if (desc && desc[0] != '\0') {
        gtk_widget_set_tooltip_text(eb, desc);
    }

    gtk_container_add(GTK_CONTAINER(eb), hbox);
    gtk_container_add(GTK_CONTAINER(row), eb);

    g_object_set_data(G_OBJECT(eb), "icon-img", img);
    if (pb_norm)  g_object_set_data_full(G_OBJECT(eb), "pb-normal", pb_norm, g_object_unref);
    if (pb_hover) g_object_set_data_full(G_OBJECT(eb), "pb-hover", pb_hover, g_object_unref);

    g_signal_connect(eb, "enter-notify-event", G_CALLBACK(on_app_tile_hover_enter), ui);
    g_signal_connect(eb, "leave-notify-event", G_CALLBACK(on_app_tile_hover_leave), ui);
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_app_widget_right_click), app);

    GString *kws = g_string_new("");
    if (G_IS_DESKTOP_APP_INFO(app)) {
        const gchar *const *kw = g_desktop_app_info_get_keywords(G_DESKTOP_APP_INFO(app));
        for (gint k = 0; kw && kw[k]; k++) { g_string_append_c(kws, ' '); g_string_append(kws, kw[k]); }
    }
    const gchar *exe = g_app_info_get_executable(app);
    gchar *exe_base = exe ? g_path_get_basename(exe) : g_strdup("");
    gchar *combined_search = g_strdup_printf("%s %s %s %s %s",
        primary_name, formal_name ? formal_name : "", desc ? desc : "", kws->str, exe_base);
    g_object_set_data_full(G_OBJECT(row), "app-info", g_object_ref(app), g_object_unref);
    g_object_set_data_full(G_OBJECT(row), "search-key", g_utf8_strdown(combined_search, -1), g_free);
    gchar *nm2 = g_strdup_printf("%s %s", primary_name, formal_name ? formal_name : "");
    g_object_set_data_full(G_OBJECT(row), "search-name", g_utf8_strdown(nm2, -1), g_free);
    g_free(nm2);
    g_free(combined_search);
    g_string_free(kws, TRUE);
    g_free(exe_base);

    return row;
}

static gboolean on_list_keynav_failed(GtkWidget *lb, GtkDirectionType dir, StartMenuUI *ui) {
    if (dir == GTK_DIR_UP && ui && ui->search_entry) {
        gtk_widget_grab_focus(ui->search_entry);
        gtk_editable_set_position(GTK_EDITABLE(ui->search_entry), -1);
        return TRUE;
    }
    return FALSE;
}

static void populate_app_list(StartMenuUI *ui, GtkWidget *target_listbox, gboolean with_alpha_headers) {
    if (!g_object_get_data(G_OBJECT(target_listbox), "keynav-hooked")) {
        g_signal_connect(target_listbox, "keynav-failed", G_CALLBACK(on_list_keynav_failed), ui);
        g_object_set_data(G_OBJECT(target_listbox), "keynav-hooked", GINT_TO_POINTER(1));
    }
    GList *apps = get_sorted_apps_cached(ui);
    gunichar current_letter = 0;

    gint st = get_active_style_idx(ui->shell);
    gint list_icon_px = (ui->shell && ui->shell->start_style_list_icon[st] > 0)
                        ? CLAMP(ui->shell->start_style_list_icon[st], 16, 48) : 24;
    gint hover_px     = list_icon_px + MAX(6, (list_icon_px * 20) / 100);
    gboolean show_desc = ui->shell ? ui->shell->start_style_show_desc[st] : FALSE;
    gboolean use_gen   = ui->shell ? ui->shell->start_generic_names : FALSE;

    /* List style: the pinned apps first, in your order (right-click ▸ Pin to top) */
    if (ui->shell && ui->shell->start_style == START_STYLE_DEFAULT) {
        ensure_default_favorites(ui->shell);
        gboolean any = FALSE;
        gchar **toks = g_strsplit(ui->shell->start_favorites ? ui->shell->start_favorites : "", ";", -1);
        for (gint t = 0; toks && toks[t]; t++) {
            gchar *tok = g_strstrip(toks[t]);
            if (!tok[0]) continue;
            for (GList *l = apps; l; l = l->next) {
                GAppInfo *app = G_APP_INFO(l->data);
                const gchar *id = g_app_info_get_id(app), *ex = g_app_info_get_executable(app);
                if (!((id && g_ascii_strcasecmp(id, tok) == 0) || (ex && g_ascii_strcasecmp(ex, tok) == 0))) continue;
                if (!g_app_info_should_show(app)) break;
                if (!any) {
                    GtkWidget *hr = gtk_list_box_row_new();
                    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(hr), FALSE);
                    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(hr), FALSE);
                    GtkWidget *hl = gtk_label_new(NULL);
                    gtk_label_set_markup(GTK_LABEL(hl), "<b>Pinned</b>");
                    gtk_label_set_xalign(GTK_LABEL(hl), 0.0);
                    gtk_widget_set_margin_start(hl, 10); gtk_widget_set_margin_top(hl, 4); gtk_widget_set_margin_bottom(hl, 2);
                    gtk_style_context_add_class(gtk_widget_get_style_context(hl), "start-alpha-header");
                    gtk_container_add(GTK_CONTAINER(hr), hl);
                    g_object_set_data(G_OBJECT(hr), "is-alpha-header", GINT_TO_POINTER(1));   /* hidden while searching */
                    gtk_container_add(GTK_CONTAINER(target_listbox), hr);
                    any = TRUE;
                }
                gint lpt = (ui->shell->start_style_label_pt[st] > 0) ? CLAMP(ui->shell->start_style_label_pt[st], 8, 20) : 11;
                GtkWidget *pr = create_list_row_for_app(ui, app, list_icon_px, hover_px, lpt, show_desc, use_gen);
                g_object_set_data(G_OBJECT(pr), "is-pinned-row", GINT_TO_POINTER(1));
                gtk_container_add(GTK_CONTAINER(target_listbox), pr);
                break;
            }
        }
        g_strfreev(toks);
        if (any) {
            GtkWidget *sep = gtk_list_box_row_new();
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(sep), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(sep), FALSE);
            gtk_container_add(GTK_CONTAINER(sep), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            g_object_set_data(G_OBJECT(sep), "is-alpha-header", GINT_TO_POINTER(1));
            gtk_container_add(GTK_CONTAINER(target_listbox), sep);
        }
    }

    /* Recent & Recommended on top of the list (list styles; Win11 / Launchpad place their own) */
    if (ui->shell && ui->shell->start_style != START_STYLE_WIN11 && !sm_style_fullscreen(ui->shell)) {
        GtkWidget *rec = build_recommended_section(ui, 1);
        if (rec) {
            GtkWidget *rrow = gtk_list_box_row_new();
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(rrow), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(rrow), FALSE);
            g_object_set_data(G_OBJECT(rrow), "is-rec-row", GINT_TO_POINTER(1));
            gtk_widget_set_margin_bottom(rec, 6);
            gtk_container_add(GTK_CONTAINER(rrow), rec);
            gtk_container_add(GTK_CONTAINER(target_listbox), rrow);
        }
    }

    for (GList *l = apps; l != NULL; l = l->next) {
        GAppInfo *app = G_APP_INFO(l->data);
        if (!g_app_info_should_show(app)) continue;

        const gchar *primary_name = resolve_app_primary_title(app, use_gen);
        if (!primary_name || primary_name[0] == '\0') continue;

        if (with_alpha_headers) {
            gunichar first = g_unichar_toupper(g_utf8_get_char_validated(primary_name, -1));
            if (first == (gunichar)-1 || first == (gunichar)-2 || !g_unichar_isalpha(first)) first = '#';

            if (first != current_letter) {
                current_letter = first;
                GtkWidget *hdr_row = gtk_list_box_row_new();
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(hdr_row), FALSE);
                gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(hdr_row), FALSE);

                gchar ubuf[8] = { 0 };
                g_unichar_to_utf8(current_letter, ubuf);
                gchar *h_mk = g_markup_printf_escaped("<span weight='bold' size='large'>%s</span>", ubuf);
                GtkWidget *hdr_lbl = gtk_label_new(NULL);
                gtk_label_set_markup(GTK_LABEL(hdr_lbl), h_mk);
                gtk_style_context_add_class(gtk_widget_get_style_context(hdr_lbl), "start-alpha-header");
                gtk_label_set_xalign(GTK_LABEL(hdr_lbl), 0.0);
                gtk_widget_set_margin_start(hdr_lbl, 10);
                gtk_widget_set_margin_top(hdr_lbl, 8);
                gtk_widget_set_margin_bottom(hdr_lbl, 2);
                g_free(h_mk);

                g_object_set_data(G_OBJECT(hdr_row), "is-alpha-header", GINT_TO_POINTER(1));
                g_object_set_data_full(G_OBJECT(hdr_row), "alpha-letter", g_strdup(ubuf), g_free);
                gtk_container_add(GTK_CONTAINER(hdr_row), hdr_lbl);
                gtk_container_add(GTK_CONTAINER(target_listbox), hdr_row);
            }
        }

        gint label_pt = (ui->shell && ui->shell->start_style_label_pt[st] > 0) ? CLAMP(ui->shell->start_style_label_pt[st], 8, 20) : 11;
        GtkWidget *row = create_list_row_for_app(ui, app, list_icon_px, hover_px, label_pt, show_desc, use_gen);
        gtk_container_add(GTK_CONTAINER(target_listbox), row);
    }
}

/* Item 2: Windows 7 Default Left Pane (Favorites Category Folder + All Other Categories) */
static void populate_win7_categories_default_list(StartMenuUI *ui, GtkWidget *target_listbox) {
    GList *apps = get_sorted_apps_cached(ui);
    gint st = START_STYLE_WIN7;
    gint cat_icon_px  = (ui->shell && ui->shell->start_style_cat_icon[st] > 0)
                        ? CLAMP(ui->shell->start_style_cat_icon[st], 16, 32) : 22;
    gint hover_px     = cat_icon_px + MAX(6, (cat_icon_px * 20) / 100);

    GtkIconTheme *theme = gtk_icon_theme_get_default();

    for (guint i = 0; i < G_N_ELEMENTS(g_category_specs); i++) {
        /* Skip "All" in the folder list since the bottom "All Programs ▸" button opens All */
        if (i == 1) continue;
        const CategorySpec *spec = &g_category_specs[i];

        gboolean has_any = FALSE;
        for (GList *l = apps; l != NULL; l = l->next) {
            GAppInfo *app = G_APP_INFO(l->data);
            if (g_app_info_should_show(app) && app_matches_category_filter(ui->shell, app, spec->filter)) {
                has_any = TRUE;
                break;
            }
        }
        /* Always show Favorites folder at the top; prune empty non-Favorite categories */
        if (!has_any && i != 0) continue;

        GdkPixbuf *pb_norm  = gtk_icon_theme_load_icon(theme, spec->icon, cat_icon_px, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
        GdkPixbuf *pb_hover = gtk_icon_theme_load_icon(theme, spec->icon, hover_px, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
        GtkWidget *cimg = pb_norm ? gtk_image_new_from_pixbuf(pb_norm)
                                  : gtk_image_new_from_icon_name(spec->icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_widget_set_size_request(cimg, hover_px, hover_px);

        GtkWidget *row = gtk_list_box_row_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(row), "app-grid-tile");
        GtkWidget *eb = gtk_event_box_new();
        gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
        gtk_widget_add_events(eb, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

        gint cat_sp = (ui->shell && ui->shell->start_style_cat_spacing[st] >= 0)
                      ? CLAMP(ui->shell->start_style_cat_spacing[st], 0, 16) : 5;
        gtk_widget_set_margin_top(row, cat_sp / 2);
        gtk_widget_set_margin_bottom(row, (cat_sp + 1) / 2);

        GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_start(hbox, 4);
        gtk_widget_set_margin_end(hbox, 4);
        gtk_widget_set_margin_top(hbox, 2 + (cat_sp / 3));
        gtk_widget_set_margin_bottom(hbox, 2 + (cat_sp / 3));
        gtk_container_set_border_width(GTK_CONTAINER(hbox), 4);

        gint cat_pt = (ui->shell && ui->shell->start_style_cat_pt[st] > 0) ? ui->shell->start_style_cat_pt[st] : 11;
        gint cat_wt = ui->shell ? ui->shell->start_style_cat_weight[st] : 2;
        GtkWidget *clbl = gtk_label_new(spec->title);
        apply_label_font_size_and_weight(clbl, cat_pt, cat_wt);
        gtk_label_set_xalign(GTK_LABEL(clbl), 0.0);

        GtkWidget *arrow = gtk_label_new("▸");
        gtk_style_context_add_class(gtk_widget_get_style_context(arrow), "dim-label");

        gtk_box_pack_start(GTK_BOX(hbox), cimg, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hbox), clbl, TRUE, TRUE, 0);
        gtk_box_pack_end(GTK_BOX(hbox), arrow, FALSE, FALSE, 4);

        gtk_container_add(GTK_CONTAINER(eb), hbox);
        gtk_container_add(GTK_CONTAINER(row), eb);

        g_object_set_data(G_OBJECT(eb), "icon-img", cimg);
        if (pb_norm)  g_object_set_data_full(G_OBJECT(eb), "pb-normal", pb_norm, g_object_unref);
        if (pb_hover) g_object_set_data_full(G_OBJECT(eb), "pb-hover", pb_hover, g_object_unref);
        g_signal_connect(eb, "enter-notify-event", G_CALLBACK(on_app_tile_hover_enter), ui);
        g_signal_connect(eb, "leave-notify-event", G_CALLBACK(on_app_tile_hover_leave), ui);

        g_object_set_data(G_OBJECT(row), "win7-cat-jump", (gpointer)spec->filter);
        gtk_container_add(GTK_CONTAINER(target_listbox), row);

        /* Put a subtle separator right below Favorites */
        if (i == 0) {
            GtkWidget *sep_row = gtk_list_box_row_new();
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(sep_row), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(sep_row), FALSE);
            gtk_container_add(GTK_CONTAINER(sep_row), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
            gtk_container_add(GTK_CONTAINER(target_listbox), sep_row);
        }
    }
}

static void on_win7_all_programs_toggled(GtkButton *btn, StartMenuUI *ui) {
    if (!ui->win7_left_stack) return;
    const gchar *vis = gtk_stack_get_visible_child_name(GTK_STACK(ui->win7_left_stack));
    if (g_strcmp0(vis, "categories") == 0) {
        g_free(ui->active_category);
        ui->active_category = g_strdup("All");
        if (ui->list_box) gtk_list_box_invalidate_filter(GTK_LIST_BOX(ui->list_box));
        gtk_stack_set_visible_child_name(GTK_STACK(ui->win7_left_stack), "apps");
        gtk_button_set_label(btn, "◂ Back to Categories");
    } else {
        if (ui->search_entry) gtk_entry_set_text(GTK_ENTRY(ui->search_entry), "");
        gtk_stack_set_visible_child_name(GTK_STACK(ui->win7_left_stack), "categories");
        gtk_button_set_label(btn, "All Programs ▸");
    }
}

/* Item 7: Windows 11 Pinned Grid (Favorites Only) vs Fullscreen Search Grid (All Apps) */
static void populate_win11_grid(StartMenuUI *ui, GtkWidget *target_flowbox, gboolean favorites_only) {
    GList *apps = get_sorted_apps_cached(ui);

    gint st = get_active_style_idx(ui->shell);
    gint max_icon = (st == START_STYLE_FULLSCREEN || st == START_STYLE_GNOME) ? 96 : 64;
    gint grid_icon_px = (ui->shell && ui->shell->start_style_grid_icon[st] > 0)
                        ? CLAMP(ui->shell->start_style_grid_icon[st], 24, max_icon) : 36;
    gboolean use_gen  = ui->shell ? ui->shell->start_generic_names : FALSE;
    gint cell_w = (st == START_STYLE_WIN11) ? MAX(80, grid_icon_px + 38) : MAX(88, grid_icon_px + 46);
    gint label_pt = (ui->shell && ui->shell->start_style_label_pt[st] > 0) ? CLAMP(ui->shell->start_style_label_pt[st], 8, 20) : 10;
    gint cell_h = (st == START_STYLE_WIN11) ? MAX(78, grid_icon_px + 26 + label_pt * 2) : MAX(84, grid_icon_px + 30 + label_pt * 2);

    if (favorites_only) {
        /* Pinned: the user's own order (drag a tile onto another to move it) */
        ensure_default_favorites(ui->shell);
        gchar **toks = g_strsplit(ui->shell->start_favorites ? ui->shell->start_favorites : "", ";", -1);
        for (gint t = 0; toks && toks[t]; t++) {
            gchar *tok = g_strstrip(toks[t]);
            if (!tok[0]) continue;
            for (GList *l = apps; l != NULL; l = l->next) {
                GAppInfo *app = G_APP_INFO(l->data);
                const gchar *id = g_app_info_get_id(app), *ex = g_app_info_get_executable(app);
                if (!((id && g_ascii_strcasecmp(id, tok) == 0) || (ex && g_ascii_strcasecmp(ex, tok) == 0))) continue;
                if (!g_app_info_should_show(app)) break;
                GtkWidget *child = create_grid_tile_for_app(ui, app, grid_icon_px, label_pt, cell_w, cell_h);
                GtkWidget *eb = gtk_bin_get_child(GTK_BIN(child));
                if (eb) fav_source_setup(eb, app, target_flowbox);   /* drag with a live preview */
                gtk_container_add(GTK_CONTAINER(target_flowbox), child);
                break;
            }
        }
        g_strfreev(toks);
        return;
    }

    for (GList *l = apps; l != NULL; l = l->next) {
        GAppInfo *app = G_APP_INFO(l->data);
        if (!g_app_info_should_show(app)) continue;

        const gchar *name = resolve_app_primary_title(app, use_gen);
        if (!name || name[0] == '\0') continue;

        GtkWidget *child = create_grid_tile_for_app(ui, app, grid_icon_px, label_pt, cell_w, cell_h);
        gtk_container_add(GTK_CONTAINER(target_flowbox), child);
    }
}

static void refresh_win11_pinned(StartMenuUI *ui) {
    if (ui && ui->shell && ui->shell->start_style == START_STYLE_WIN10) { win10_fill_tiles(ui); return; }
    if (!ui || !ui->shell || ui->shell->start_style != START_STYLE_WIN11) return;
    GtkWidget *fb = g_object_get_data(G_OBJECT(ui->popup_win), "win11-pinned-flow");
    if (!GTK_IS_FLOW_BOX(fb)) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(fb));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    g_object_set_data(G_OBJECT(fb), "search-sort-on", NULL);
    gtk_flow_box_set_sort_func(GTK_FLOW_BOX(fb), NULL, NULL, NULL);
    populate_win11_grid(ui, fb, TRUE);
    ensure_search_sort(ui, fb);                    /* favourites order (+ search ranking) */
    gtk_widget_show_all(fb);
}

/* --- Per-Style Persistent Start Menu Resize Logic --- */
static void get_default_size_for_style(StartMenuStyle style, gint *out_w, gint *out_h) {
    switch (style) {
        case START_STYLE_WIN7:
            *out_w = 480; *out_h = 490; break;
        case START_STYLE_WIN11:
            *out_w = 540; *out_h = 520; break;
        case START_STYLE_WHISKER:
            *out_w = 540; *out_h = 500; break;
        case START_STYLE_WIN10:
            *out_w = 760; *out_h = 600; break;
        case START_STYLE_DEFAULT:
        default:
            *out_w = 350; *out_h = 470; break;
    }
}

static void get_saved_or_default_style_size(UltimateShell *shell, StartMenuStyle style, gint *out_w, gint *out_h) {
    gint def_w = 350, def_h = 470;
    get_default_size_for_style(style, &def_w, &def_h);
    if (!shell || style < 0 || style >= 4) {
        *out_w = def_w;
        *out_h = def_h;
        return;
    }
    *out_w = (shell->start_style_w[style] > 200) ? shell->start_style_w[style] : def_w;
    *out_h = (shell->start_style_h[style] > 200) ? shell->start_style_h[style] : def_h;
}

static gint g_resize_orig_win_x = 0;
static gint g_resize_orig_win_y = 0;
static gint g_resize_start_x = 0;
static gint g_resize_start_y = 0;
static gint g_resize_start_w = 0;
static gint g_resize_start_h = 0;

/* ---- Resizing: the ‡ corner grip and the menu's edges ----
   EDGE_* tell which sides move. The opposite sides stay where they are. */
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8, EDGE_GRIP = 16 };
static gint g_resize_edges = 0;

/* Win11 always opens on Pinned, wherever it was left last time (panel button and dock alike) */
static void win11_reset_to_pinned(StartMenuUI *ui) {
    if (!ui || !ui->win11_stack) return;
    gtk_stack_set_visible_child_full(GTK_STACK(ui->win11_stack), "pinned", GTK_STACK_TRANSITION_TYPE_NONE);
    if (ui->win11_pinned_btn) gtk_label_set_markup(GTK_LABEL(ui->win11_pinned_btn), "<b>Pinned</b>");
    if (ui->win11_all_btn) gtk_button_set_label(GTK_BUTTON(ui->win11_all_btn), "All apps  ›");
}

/* Is the menu hanging from the bottom of the screen (bottom panel or bottom dock)? */
static gboolean menu_anchored_bottom(StartMenuUI *ui) {
    gboolean from_dock = (g_object_get_data(G_OBJECT(ui->popup_win), "dock-anchor-btn") != NULL);
    if (from_dock) return ui->shell->dock_pos == DOCK_POS_BOTTOM;
    if (ui->btn && gtk_widget_get_window(ui->btn)) {
        gint bx = 0, by = 0;
        gdk_window_get_origin(gtk_widget_get_window(ui->btn), &bx, &by);
        GdkMonitor *mon = gdk_display_get_monitor_at_point(gdk_display_get_default(), bx, by);
        if (mon) {
            GdkRectangle g;
            gdk_monitor_get_geometry(mon, &g);
            return by > g.y + g.height / 2;
        }
    }
    return ui->shell->mode == LAYOUT_WINDOWS;
}

static void menu_size_limits(StartMenuUI *ui, gint *max_w, gint *max_h) {
    GdkMonitor *mon = NULL;
    if (ui->popup_win && gtk_widget_get_window(ui->popup_win))
        mon = gdk_display_get_monitor_at_window(gdk_display_get_default(), gtk_widget_get_window(ui->popup_win));
    if (!mon) mon = gdk_display_get_primary_monitor(gdk_display_get_default());
    GdkRectangle wa = { 0, 0, 1280, 720 };
    if (mon) gdk_monitor_get_workarea(mon, &wa);
    *max_w = MAX(400, (gint)(wa.width * 0.9));
    *max_h = MAX(420, (gint)(wa.height * 0.9));
}

static gboolean begin_resize(StartMenuUI *ui, GdkEventButton *event, gint edges) {
    if (event->button != 1 || !ui->popup_win) return FALSE;
    ui->resizing = TRUE;
    if (edges & EDGE_GRIP) {
        /* the corner grip: right side + the free (non-anchored) vertical side */
        edges = EDGE_RIGHT | (menu_anchored_bottom(ui) ? EDGE_TOP : EDGE_BOTTOM);
    }
    g_resize_edges = edges;
    g_resize_start_x = event->x_root;
    g_resize_start_y = event->y_root;
    gtk_window_get_size(GTK_WINDOW(ui->popup_win), &g_resize_start_w, &g_resize_start_h);
    gtk_window_get_position(GTK_WINDOW(ui->popup_win), &g_resize_orig_win_x, &g_resize_orig_win_y);
    return TRUE;
}

static gboolean on_resize_press(GtkWidget *widget, GdkEventButton *event, StartMenuUI *ui) {
    gint edges = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), "resize-edges"));
    return begin_resize(ui, event, edges ? edges : EDGE_GRIP);
}

static gboolean on_resize_motion(GtkWidget *widget, GdkEventMotion *event, StartMenuUI *ui) {
    if (!ui || !ui->resizing || !ui->shell) return FALSE;

    gint dx = (gint)(event->x_root - g_resize_start_x);
    gint dy = (gint)(event->y_root - g_resize_start_y);
    gint max_w = 1200, max_h = 950;
    menu_size_limits(ui, &max_w, &max_h);

    gint new_w = g_resize_start_w;
    gint new_h = g_resize_start_h;
    if (g_resize_edges & EDGE_RIGHT)  new_w = g_resize_start_w + dx;
    if (g_resize_edges & EDGE_LEFT)   new_w = g_resize_start_w - dx;
    if (g_resize_edges & EDGE_BOTTOM) new_h = g_resize_start_h + dy;
    if (g_resize_edges & EDGE_TOP)    new_h = g_resize_start_h - dy;
    new_w = CLAMP(new_w, 320, max_w);
    new_h = CLAMP(new_h, 360, max_h);

    gint st = CLAMP((gint)ui->shell->start_style, 0, START_STYLE_COUNT - 1);
    ui->shell->start_style_w[st] = new_w;
    ui->shell->start_style_h[st] = new_h;
    ui->shell->start_menu_w = new_w;
    ui->shell->start_menu_h = new_h;

    gtk_widget_set_size_request(ui->popup_win, new_w, new_h);
    gtk_window_resize(GTK_WINDOW(ui->popup_win), new_w, new_h);
    if (ui->root_container) gtk_widget_set_size_request(ui->root_container, -1, -1);

    /* Keep the sides that weren't dragged in place */
    gint nx = g_resize_orig_win_x + ((g_resize_edges & EDGE_LEFT) ? (g_resize_start_w - new_w) : 0);
    gint ny = g_resize_orig_win_y + ((g_resize_edges & EDGE_TOP)  ? (g_resize_start_h - new_h) : 0);
    gtk_window_move(GTK_WINDOW(ui->popup_win), MAX(0, nx), MAX(0, ny));
    return TRUE;
}

static gboolean on_resize_release(GtkWidget *widget, GdkEventButton *event, StartMenuUI *ui) {
    if (event->button == 1 && ui->resizing) {
        ui->resizing = FALSE;
        g_resize_edges = 0;
        shell_save_layout_and_theme(ui->shell);
        return TRUE;
    }
    return FALSE;
}


/* ---- invisible edge handles around the menu ---- */
static const gchar *edge_cursor_name(gint e) {
    switch (e) {
        case EDGE_TOP: case EDGE_BOTTOM: return "ns-resize";
        case EDGE_LEFT: case EDGE_RIGHT: return "ew-resize";
        case EDGE_TOP | EDGE_LEFT: case EDGE_BOTTOM | EDGE_RIGHT: return "nwse-resize";
        default: return "nesw-resize";
    }
}

static gboolean on_edge_enter(GtkWidget *w, GdkEventCrossing *ev, StartMenuUI *ui) {
    GdkWindow *gw = ui->popup_win ? gtk_widget_get_window(ui->popup_win) : NULL;
    if (!gw) return FALSE;
    gint e = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(w), "resize-edges"));
    GdkCursor *c = gdk_cursor_new_from_name(gdk_window_get_display(gw), edge_cursor_name(e));
    gdk_window_set_cursor(gw, c);
    if (c) g_object_unref(c);
    return FALSE;
}

static gboolean on_edge_leave(GtkWidget *w, GdkEventCrossing *ev, StartMenuUI *ui) {
    if (ui->resizing) return FALSE;            /* keep the cursor while dragging */
    GdkWindow *gw = ui->popup_win ? gtk_widget_get_window(ui->popup_win) : NULL;
    if (gw) gdk_window_set_cursor(gw, NULL);
    return FALSE;
}

static GtkWidget *make_edge_handle(StartMenuUI *ui, gint edges) {
    GtkWidget *eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
    gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK |
                              GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_widget_set_size_request(eb, 6, 6);
    g_object_set_data(G_OBJECT(eb), "resize-edges", GINT_TO_POINTER(edges));
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_resize_press), ui);
    g_signal_connect(eb, "button-release-event", G_CALLBACK(on_resize_release), ui);
    g_signal_connect(eb, "motion-notify-event", G_CALLBACK(on_resize_motion), ui);
    g_signal_connect(eb, "enter-notify-event", G_CALLBACK(on_edge_enter), ui);
    g_signal_connect(eb, "leave-notify-event", G_CALLBACK(on_edge_leave), ui);
    return eb;
}

/* Put the menu content in the middle of a 3×3 grid of edge/corner handles */
static GtkWidget *wrap_with_edge_handles(StartMenuUI *ui, GtkWidget *content) {
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_TOP | EDGE_LEFT),     0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_TOP),                 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_TOP | EDGE_RIGHT),    2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_LEFT),                0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), content,                                        1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_RIGHT),               2, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_BOTTOM | EDGE_LEFT),  0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_BOTTOM),              1, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), make_edge_handle(ui, EDGE_BOTTOM | EDGE_RIGHT), 2, 2, 1, 1);
    gtk_widget_set_hexpand(content, TRUE);
    gtk_widget_set_vexpand(content, TRUE);
    return grid;
}

static GtkWidget *create_action_buttons_box(StartMenuUI *ui, gboolean include_resizer_right) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    UltimateShell *sh = ui->shell;

    if (!sh || sh->start_show_lock) {
        GtkWidget *lock_btn = gtk_button_new_from_icon_name("system-lock-screen-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(lock_btn, "Lock Screen");
        g_signal_connect(lock_btn, "clicked", G_CALLBACK(on_session_cmd), "xflock4");
        gtk_box_pack_start(GTK_BOX(box), lock_btn, FALSE, FALSE, 0);
    }

    if (!sh || sh->start_show_logout) {
        GtkWidget *logout_btn = gtk_button_new_from_icon_name("system-log-out-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(logout_btn, "Log Out");
        g_signal_connect(logout_btn, "clicked", G_CALLBACK(on_confirm_power_action), "logout");
        gtk_box_pack_start(GTK_BOX(box), logout_btn, FALSE, FALSE, 0);
    }

    if (sh && sh->start_show_restart) {
        GtkWidget *restart_btn = gtk_button_new_from_icon_name("system-reboot-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(restart_btn, "Restart");
        g_signal_connect(restart_btn, "clicked", G_CALLBACK(on_confirm_power_action), "restart");
        gtk_box_pack_start(GTK_BOX(box), restart_btn, FALSE, FALSE, 0);
    }

    if (!sh || sh->start_show_shutdown) {
        GtkWidget *power_btn = gtk_button_new_from_icon_name("system-shutdown-symbolic", GTK_ICON_SIZE_BUTTON);
        gtk_widget_set_tooltip_text(power_btn, "Shut Down");
        g_signal_connect(power_btn, "clicked", G_CALLBACK(on_confirm_power_action), "shutdown");
        gtk_box_pack_start(GTK_BOX(box), power_btn, FALSE, FALSE, 0);
    }

    if (include_resizer_right) {
    }
    return box;
}

static GtkWidget *create_user_avatar_chip(gint avatar_px) {
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *avatar = create_circular_avatar_widget(avatar_px);
    gchar *mk = g_strdup_printf("<b>%s</b>", g_get_user_name());
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), mk);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
    g_free(mk);

    gtk_box_pack_start(GTK_BOX(hbox), avatar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), lbl, FALSE, FALSE, 0);
    return hbox;
}

static void on_win11_toggle_view_clicked(GtkButton *btn, StartMenuUI *ui) {
    if (!ui->win11_stack) return;
    const gchar *cur = gtk_stack_get_visible_child_name(GTK_STACK(ui->win11_stack));
    if (g_strcmp0(cur, "pinned") == 0) {
        gtk_stack_set_visible_child_name(GTK_STACK(ui->win11_stack), "all");
        if (ui->win11_pinned_btn) gtk_label_set_markup(GTK_LABEL(ui->win11_pinned_btn), "<b>All</b>");
        gtk_button_set_label(btn, "‹  Back");
    } else {
        gtk_stack_set_visible_child_name(GTK_STACK(ui->win11_stack), "pinned");
        if (ui->win11_pinned_btn) gtk_label_set_markup(GTK_LABEL(ui->win11_pinned_btn), "<b>Pinned</b>");
        gtk_button_set_label(btn, "All apps  ›");
    }
}

static gboolean should_place_search_top(UltimateShell *shell, gboolean style_default_top) {
    if (!shell) return style_default_top;
    gint st = get_active_style_idx(shell);
    if (shell->start_style_search_pos[st] == START_SEARCH_POS_DEFAULT) return style_default_top;
    return (shell->start_style_search_pos[st] == START_SEARCH_POS_TOP);
}

static void compute_fullscreen_menu_workarea(StartMenuUI *ui, GdkRectangle *out_rect) {
    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *monitor = NULL;
    gint wx = 0, wy = 0;
    GtkAllocation btn_alloc = { 0, 0, 32, 32 };

    if (ui->btn && gtk_widget_get_window(ui->btn)) {
        gdk_window_get_origin(gtk_widget_get_window(ui->btn), &wx, &wy);
        gtk_widget_get_allocation(ui->btn, &btn_alloc);
        monitor = gdk_display_get_monitor_at_point(display, wx + btn_alloc.x, wy + btn_alloc.y);
    }
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);

    GdkRectangle geom, workarea;
    gdk_monitor_get_geometry(monitor, &geom);
    gdk_monitor_get_workarea(monitor, &workarea);

    gint top_edge    = workarea.y;
    gint bottom_edge = workarea.y + workarea.height;
    gint left_edge   = workarea.x;
    gint right_edge  = workarea.x + workarea.width;

    if (ui->shell && ui->shell->plugin) {
        gint panel_size = xfce_panel_plugin_get_size(ui->shell->plugin);
        if (panel_size < 20) panel_size = (ui->shell->mode == LAYOUT_MACOS) ? 28 : 42;

        GtkWidget *panel_top_win = gtk_widget_get_toplevel(GTK_WIDGET(ui->shell->plugin));
        if (GTK_IS_WIDGET(panel_top_win) && gtk_widget_get_window(panel_top_win)) {
            gint px = 0, py = 0, ph = 0;
            gdk_window_get_origin(gtk_widget_get_window(panel_top_win), &px, &py);
            ph = gtk_widget_get_allocated_height(panel_top_win);
            if (ph < panel_size) ph = panel_size;

            if (py <= geom.y + geom.height / 2) {
                top_edge = MAX(top_edge, py + ph);
            } else {
                bottom_edge = MIN(bottom_edge, py);
            }
        } else {
            if (ui->shell->mode == LAYOUT_MACOS) {
                top_edge = MAX(top_edge, geom.y + panel_size);
            } else {
                bottom_edge = MIN(bottom_edge, geom.y + geom.height - panel_size);
            }
        }
    }

    if (ui->shell && ui->shell->mode == LAYOUT_MACOS &&
        GTK_IS_WIDGET(ui->shell->dock_window) && gtk_widget_get_visible(ui->shell->dock_window) &&
        ui->shell->dock_autohide == DOCK_HIDE_NEVER) {
        gint base_icon = CLAMP(ui->shell->dock_icon_size, 24, 64);
        gint pad_v     = CLAMP(ui->shell->dock_pad_v, 0, 24);
        gint edge_px   = ui->shell->dock_full_width   /* full width / full height (Ubuntu): flush with the edge */
                       ? 0 : CLAMP(ui->shell->dock_edge_dist, 0, 64);
        gint d_reserve = base_icon + (pad_v * 2) + 12 + edge_px;
        if (ui->shell->dock_pos == DOCK_POS_BOTTOM) {
            bottom_edge = MIN(bottom_edge, geom.y + geom.height - d_reserve);
        } else if (ui->shell->dock_pos == DOCK_POS_LEFT) {
            left_edge = MAX(left_edge, geom.x + d_reserve);
        } else if (ui->shell->dock_pos == DOCK_POS_RIGHT) {
            right_edge = MIN(right_edge, geom.x + geom.width - d_reserve);
        }
    }

    out_rect->x      = left_edge;
    out_rect->y      = top_edge;
    out_rect->width  = MAX(400, right_edge - left_edge);
    out_rect->height = MAX(320, bottom_edge - top_edge);

    if (GTK_IS_WIDGET(ui->popup_win)) {
        g_object_set_data(G_OBJECT(ui->popup_win), "fs-x", GINT_TO_POINTER(out_rect->x));
        g_object_set_data(G_OBJECT(ui->popup_win), "fs-y", GINT_TO_POINTER(out_rect->y));
        g_object_set_data(G_OBJECT(ui->popup_win), "fs-w", GINT_TO_POINTER(out_rect->width));
        g_object_set_data(G_OBJECT(ui->popup_win), "fs-h", GINT_TO_POINTER(out_rect->height));
    }
}

static void build_fullscreen_paged_deck(StartMenuUI *ui, GtkWidget *cat_box, const GdkRectangle *fs_rect,
                                        gboolean all_only, gint reserve_h) {
    UltimateShell *shell = ui->shell;
    const gint st = all_only ? START_STYLE_GNOME : START_STYLE_FULLSCREEN;
    gint cat_icon_px  = (shell->start_style_cat_icon[st] > 0) ? CLAMP(shell->start_style_cat_icon[st], 16, 32) : 18;
    gint grid_icon_px = (shell->start_style_grid_icon[st] > 0) ? CLAMP(shell->start_style_grid_icon[st], 32, 96) : 52;
    gint label_pt     = (shell->start_style_label_pt[st] > 0) ? CLAMP(shell->start_style_label_pt[st], 8, 20) : 11;
    gint fs_cols      = (shell->start_style_grid_cols[st] > 0) ? CLAMP(shell->start_style_grid_cols[st], 4, 10) : 7;
    gint rows_set     = all_only ? shell->start_gnome_rows : shell->start_style_fs_rows;
    gint fs_rows      = (rows_set > 0) ? CLAMP(rows_set, 2, 6) : (all_only ? 3 : 4);
    StartCatDisplayMode cat_mode = shell->start_style_cat_mode[st];

    gint avail_grid_h = MAX(220, fs_rect->height - 190 - reserve_h);
    gint max_row_h    = (avail_grid_h / fs_rows) - 12;
    gint ideal_cell_h = MAX(76, grid_icon_px + 34 + label_pt * 2);
    gint cell_h       = CLAMP(MIN(ideal_cell_h, max_row_h), 68, 165);
    gint cell_w       = MAX(88, grid_icon_px + 50);
    gint items_per_page = MAX(fs_cols * fs_rows, 6);

    GList *all_raw = get_sorted_apps_cached(ui);
    GList *valid_apps = NULL;
    gboolean use_gen = shell->start_generic_names;

    for (GList *l = all_raw; l != NULL; l = l->next) {
        GAppInfo *app = G_APP_INFO(l->data);
        if (!g_app_info_should_show(app)) continue;
        const gchar *name = resolve_app_primary_title(app, use_gen);
        if (!name || name[0] == '\0') continue;
        valid_apps = g_list_append(valid_apps, app);
    }

    if (ui->fs_cat_buttons) g_ptr_array_free(ui->fs_cat_buttons, TRUE);
    if (ui->fs_cat_filters) g_ptr_array_free(ui->fs_cat_filters, TRUE);
    if (ui->fs_cat_first_page) g_array_free(ui->fs_cat_first_page, TRUE);
    if (ui->fs_page_cat_idx) g_array_free(ui->fs_page_cat_idx, TRUE);

    ui->fs_cat_buttons    = g_ptr_array_new();
    ui->fs_cat_filters    = g_ptr_array_new();
    ui->fs_cat_first_page = g_array_new(FALSE, FALSE, sizeof(gint));
    ui->fs_page_cat_idx   = g_array_new(FALSE, FALSE, sizeof(gint));
    ui->fs_total_pages    = 0;
    ui->fs_current_page   = 0;
    if (ui->fs_page_apps) g_ptr_array_unref(ui->fs_page_apps);
    ui->fs_page_apps = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
    ui->fs_page_cols = fs_cols;

    for (guint c = 0; c < G_N_ELEMENTS(g_category_specs); c++) {
        const CategorySpec *spec = &g_category_specs[c];
        if (all_only && g_strcmp0(spec->filter, "All") != 0) continue;   /* GNOME: one A–Z run of pages */
        GList *cat_apps = NULL;

        for (GList *l = valid_apps; l != NULL; l = l->next) {
            GAppInfo *app = G_APP_INFO(l->data);
            if (app_matches_category_filter(shell, app, spec->filter)) {
                cat_apps = g_list_append(cat_apps, app);
            }
        }

        if (!cat_apps) continue;

        gint active_cat_idx = (gint)ui->fs_cat_buttons->len;
        gint first_page_for_cat = ui->fs_total_pages;
        g_array_append_val(ui->fs_cat_first_page, first_page_for_cat);
        g_ptr_array_add(ui->fs_cat_filters, (gpointer)spec->filter);

        GtkWidget *cbtn = gtk_button_new();
        gtk_button_set_relief(GTK_BUTTON(cbtn), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(cbtn), "ultimate-module-btn");
        gtk_widget_set_tooltip_text(cbtn, spec->title);

        GtkWidget *chbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        if (cat_mode != START_CAT_DISPLAY_LABEL_ONLY) {
            GtkWidget *cimg = gtk_image_new_from_icon_name(spec->icon, GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(cimg), cat_icon_px);
            gtk_box_pack_start(GTK_BOX(chbox), cimg, FALSE, FALSE, 2);
        }
        if (cat_mode != START_CAT_DISPLAY_ICON_ONLY) {
            gint cat_pt = (shell->start_style_cat_pt[st] > 0) ? shell->start_style_cat_pt[st] : 11;
            gint cat_wt = shell->start_style_cat_weight[st];
            GtkWidget *clbl = gtk_label_new(spec->title);
            apply_label_font_size_and_weight(clbl, cat_pt, cat_wt);
            gtk_box_pack_start(GTK_BOX(chbox), clbl, FALSE, FALSE, 2);
        }
        gtk_container_add(GTK_CONTAINER(cbtn), chbox);

        g_object_set_data(G_OBJECT(cbtn), "fs-first-page", GINT_TO_POINTER(first_page_for_cat));
        gtk_widget_add_events(cbtn, GDK_ENTER_NOTIFY_MASK);
        g_signal_connect(cbtn, "clicked", G_CALLBACK(on_category_clicked), ui);
        g_signal_connect(cbtn, "enter-notify-event", G_CALLBACK(on_category_hover_enter), ui);
        gtk_box_pack_start(GTK_BOX(cat_box), cbtn, FALSE, FALSE, 0);
        g_ptr_array_add(ui->fs_cat_buttons, cbtn);

        GList *iter = cat_apps;
        while (iter != NULL) {
            GtkWidget *page_clip = gtk_scrolled_window_new(NULL, NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page_clip), GTK_POLICY_NEVER, GTK_POLICY_NEVER);
            gtk_widget_add_events(page_clip, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
            g_signal_connect(page_clip, "scroll-event", G_CALLBACK(on_fs_deck_scroll), ui);

            GtkWidget *page_eb = gtk_event_box_new();
            gtk_event_box_set_visible_window(GTK_EVENT_BOX(page_eb), FALSE);
            gtk_widget_add_events(page_eb, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK);
            g_signal_connect(page_eb, "scroll-event", G_CALLBACK(on_fs_deck_scroll), ui);
            g_signal_connect(page_eb, "button-press-event", G_CALLBACK(on_fs_background_button_press), ui);

            GtkWidget *page_flow = gtk_flow_box_new();
            gtk_widget_set_halign(page_flow, GTK_ALIGN_CENTER);
            gtk_widget_set_valign(page_flow, GTK_ALIGN_CENTER);
            gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(page_flow), TRUE);
            gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(page_flow), fs_cols);
            gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(page_flow), fs_cols);
            gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(page_flow), 14);
            gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(page_flow), 10);
            gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(page_flow), GTK_SELECTION_SINGLE);
            gtk_widget_add_events(page_flow, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
            g_signal_connect(page_flow, "scroll-event", G_CALLBACK(on_fs_deck_scroll), ui);
            g_signal_connect(page_flow, "child-activated", G_CALLBACK(on_flow_child_activated), ui);
            g_signal_connect(page_flow, "keynav-failed", G_CALLBACK(on_fs_page_keynav_failed), ui);

            gint count_on_page = 0;
            GPtrArray *page_apps = g_ptr_array_new();
            while (iter != NULL && count_on_page < items_per_page) {
                GAppInfo *app = G_APP_INFO(iter->data);
                GtkWidget *tile = create_grid_tile_for_app(ui, app, grid_icon_px, label_pt, cell_w, cell_h);
                if (all_only) gnome_make_tile_draggable(gtk_bin_get_child(GTK_BIN(tile)), app);
                g_ptr_array_add(page_apps, app);
                gtk_container_add(GTK_CONTAINER(page_flow), tile);
                count_on_page++;
                iter = iter->next;
            }

            gtk_container_add(GTK_CONTAINER(page_eb), page_flow);
            gtk_container_add(GTK_CONTAINER(page_clip), page_eb);

            gchar *pname = g_strdup_printf("page_%d", ui->fs_total_pages);
            gtk_stack_add_named(GTK_STACK(ui->fs_page_stack), page_clip, pname);
            g_free(pname);

            g_ptr_array_add(ui->fs_page_apps, page_apps);
            g_array_append_val(ui->fs_page_cat_idx, active_cat_idx);
            ui->fs_total_pages++;
        }

        g_list_free(cat_apps);
    }

    g_list_free(valid_apps);
}

/* =========================================================================
 * Windows 10 Tiles Menu: rail · A–Z list · resizable pinned tiles
 * ========================================================================= */
#define W10_CELL 48            /* one small tile */
#define W10_GAP  4
#define W10_MAX_COLS 12        /* small-tile columns at most (6 medium tiles) */
/* tile-area width in small-tile columns: the Start ▸ Grid Columns setting counts medium tiles (default 4) */
static gint win10_cols(UltimateShell *sh) {
    gint m = (sh && sh->start_style_grid_cols[START_STYLE_WIN10] > 0) ? sh->start_style_grid_cols[START_STYLE_WIN10] : 4;
    return CLAMP(m, 2, 6) * 2;
}

/* tile size of a pinned app: 's' small 1×1, 'm' medium 2×2 (default), 'w' wide 4×2 */
static gchar win10_tile_size(UltimateShell *sh, const gchar *id) {
    if (!sh || !id || !sh->win10_tile_sizes) return 'm';
    gchar **toks = g_strsplit(sh->win10_tile_sizes, ";", -1);
    gchar res = 'm';
    for (gint i = 0; toks && toks[i]; i++) {
        gchar *eq = strrchr(toks[i], '=');
        if (!eq || eq == toks[i]) continue;
        *eq = '\0';
        if (g_ascii_strcasecmp(toks[i], id) == 0 && (eq[1] == 's' || eq[1] == 'm' || eq[1] == 'w')) { res = eq[1]; break; }
    }
    g_strfreev(toks);
    return res;
}

static void win10_set_tile_size(UltimateShell *sh, const gchar *id, gchar size) {
    if (!sh || !id) return;
    GString *out = g_string_new("");
    gchar **toks = g_strsplit(sh->win10_tile_sizes ? sh->win10_tile_sizes : "", ";", -1);
    for (gint i = 0; toks && toks[i]; i++) {
        gchar *eq = strrchr(toks[i], '=');
        if (!eq || eq == toks[i]) continue;
        gchar *key = g_strndup(toks[i], eq - toks[i]);
        if (g_ascii_strcasecmp(key, id) != 0) {
            if (out->len) g_string_append_c(out, ';');
            g_string_append(out, toks[i]);
        }
        g_free(key);
    }
    g_strfreev(toks);
    if (size != 'm') {                         /* medium is the default: not stored */
        if (out->len) g_string_append_c(out, ';');
        g_string_append_printf(out, "%s=%c", id, size);
    }
    g_free(sh->win10_tile_sizes);
    sh->win10_tile_sizes = g_string_free(out, FALSE);
    if (sh->channel) xfconf_channel_set_string(sh->channel, "/plugins/ultimateshell/win10_tile_sizes", sh->win10_tile_sizes);
}

static gboolean on_win10_tile_press(GtkWidget *w, GdkEventButton *ev, GAppInfo *app) {
    if (ev->type == GDK_BUTTON_PRESS && ev->button == 3) return on_app_widget_right_click(w, ev, app);
    return FALSE;
}
static gboolean on_win10_tile_release(GtkWidget *w, GdkEventButton *ev, GAppInfo *app) {
    if (ev->button == 1 && g_start_ui && app) {
        GtkAllocation a; gtk_widget_get_allocation(w, &a);
        if (ev->x >= 0 && ev->y >= 0 && ev->x < a.width && ev->y < a.height) launch_app_and_close(g_start_ui, app);
    }
    return FALSE;
}

/* an event box gets no :hover by itself: set it on enter / leave so the tile CSS hover applies */
static gboolean on_win10_tile_crossing(GtkWidget *w, GdkEventCrossing *ev, gpointer d) {
    if (ev->detail == GDK_NOTIFY_INFERIOR) return FALSE;
    gboolean in = (ev->type == GDK_ENTER_NOTIFY);
    if (in) gtk_widget_set_state_flags(w, GTK_STATE_FLAG_PRELIGHT, FALSE);
    else gtk_widget_unset_state_flags(w, GTK_STATE_FLAG_PRELIGHT);
    /* icon zoom, same as the other menus (Start ▸ hover effect setting) */
    if (g_start_ui && g_start_ui->shell && !g_start_ui->shell->start_hover_effect) in = FALSE;
    GtkWidget *img = g_object_get_data(G_OBJECT(w), "icon-img");
    GdkPixbuf *pb = g_object_get_data(G_OBJECT(w), in ? "pb-hover" : "pb-norm");
    if (GTK_IS_IMAGE(img) && pb) gtk_image_set_from_pixbuf(GTK_IMAGE(img), pb);
    return FALSE;
}

static GtkWidget *win10_make_tile(StartMenuUI *ui, GAppInfo *app, gchar size) {
    gint cw = (size == 's') ? 1 : (size == 'w') ? 4 : 2;
    gint ch = (size == 's') ? 1 : 2;
    gint pw = cw * W10_CELL + (cw - 1) * W10_GAP, ph = ch * W10_CELL + (ch - 1) * W10_GAP;
    gint icon_px = (size == 's') ? 28 : 40;

    GtkWidget *eb = gtk_event_box_new();
    gtk_widget_set_size_request(eb, pw, ph);
    gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_style_context_add_class(gtk_widget_get_style_context(eb), "win10-tile");
    g_signal_connect(eb, "enter-notify-event", G_CALLBACK(on_win10_tile_crossing), NULL);
    g_signal_connect(eb, "leave-notify-event", G_CALLBACK(on_win10_tile_crossing), NULL);
    gtk_widget_set_tooltip_text(eb, g_app_info_get_display_name(app));

    GtkWidget *ov = gtk_overlay_new();
    gint zoom_px = icon_px + MAX(4, icon_px * 20 / 100);
    GdkPixbuf *pb = load_app_pixbuf_at_size(app, icon_px);
    GdkPixbuf *pbz = load_app_pixbuf_at_size(app, zoom_px);
    GtkWidget *img = pb ? gtk_image_new_from_pixbuf(pb) : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DND);
    gtk_widget_set_size_request(img, zoom_px, zoom_px);       /* room for the zoom: nothing shifts */
    g_object_set_data(G_OBJECT(eb), "icon-img", img);
    if (pb)  g_object_set_data_full(G_OBJECT(eb), "pb-norm", pb, g_object_unref);
    if (pbz) g_object_set_data_full(G_OBJECT(eb), "pb-hover", pbz, g_object_unref);
    gtk_widget_set_halign(img, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(img, GTK_ALIGN_CENTER);
    if (size != 's') gtk_widget_set_margin_bottom(img, 12);
    gtk_container_add(GTK_CONTAINER(ov), img);
    if (size != 's') {
        GtkWidget *lbl = gtk_label_new(resolve_app_primary_title(app, ui->shell && ui->shell->start_generic_names));
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 1);
        gtk_widget_set_halign(lbl, GTK_ALIGN_FILL);
        gtk_widget_set_valign(lbl, GTK_ALIGN_END);
        gtk_widget_set_margin_start(lbl, 6); gtk_widget_set_margin_end(lbl, 6); gtk_widget_set_margin_bottom(lbl, 4);
        gtk_style_context_add_class(gtk_widget_get_style_context(lbl), "win10-tile-label");
        gtk_overlay_add_overlay(GTK_OVERLAY(ov), lbl);
    }
    gtk_container_add(GTK_CONTAINER(eb), ov);
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_win10_tile_press), app);
    g_signal_connect(eb, "button-release-event", G_CALLBACK(on_win10_tile_release), app);
    make_tile_reorderable(eb, app);
    gtk_drag_dest_unset(eb);                        /* the whole tile area is the drop target (live preview) */
    g_signal_connect_after(eb, "drag-begin", G_CALLBACK(on_tile_drag_begin), NULL);
    g_signal_connect(eb, "drag-begin", G_CALLBACK(on_w10_tile_drag_begin), NULL);
    g_signal_connect(eb, "drag-end", G_CALLBACK(on_w10_tile_drag_end), ui);
    return eb;
}

/* Pack the pinned apps into the tile grid in the user's order, first free spot wins */
/* ---- tile layout: every tile has its own cell (like Windows 10); tiles in the way move down ---- */
typedef struct { gint c, r, cw, ch; } W10Cell;
#define W10_MAXR 200

static void w10_size_cells(gchar size, gint *cw, gint *ch) {
    *cw = (size == 's') ? 1 : (size == 'w') ? 4 : 2;
    *ch = (size == 's') ? 1 : 2;
}

static gboolean w10_saved_pos3(UltimateShell *sh, const gchar *id, gint *c, gint *r, gint *g) {
    *g = 0;
    if (!sh || !id || !sh->win10_tile_pos) return FALSE;
    gboolean ok = FALSE;
    gchar **toks = g_strsplit(sh->win10_tile_pos, ";", -1);
    for (gint i = 0; toks && toks[i] && !ok; i++) {
        gchar *eq = strrchr(toks[i], '=');
        if (!eq || eq == toks[i]) continue;
        *eq = '\0';
        if (g_ascii_strcasecmp(toks[i], id) != 0) continue;
        gint n = sscanf(eq + 1, "%d,%d,%d", c, r, g);
        if (n >= 2 && *c >= 0 && *r >= 0) { ok = TRUE; if (n < 3 || *g < 0) *g = 0; }
    }
    g_strfreev(toks);
    return ok;
}

static gboolean w10_saved_pos(UltimateShell *sh, const gchar *id, gint *c, gint *r) {
    if (!sh || !id || !sh->win10_tile_pos) return FALSE;
    gboolean ok = FALSE;
    gchar **toks = g_strsplit(sh->win10_tile_pos, ";", -1);
    for (gint i = 0; toks && toks[i] && !ok; i++) {
        gchar *eq = strrchr(toks[i], '=');
        if (!eq || eq == toks[i]) continue;
        *eq = '\0';
        if (g_ascii_strcasecmp(toks[i], id) == 0 && sscanf(eq + 1, "%d,%d", c, r) >= 2 && *c >= 0 && *r >= 0) ok = TRUE;
    }
    g_strfreev(toks);
    return ok;
}

static gboolean w10_free(guint8 occ[][W10_MAX_COLS], gint c, gint r, gint cw, gint ch) {
    if (r < 0 || r + ch > W10_MAXR) return FALSE;
    for (gint rr = r; rr < r + ch; rr++) for (gint cc = c; cc < c + cw; cc++) if (occ[rr][cc]) return FALSE;
    return TRUE;
}
static void w10_mark(guint8 occ[][W10_MAX_COLS], gint c, gint r, gint cw, gint ch) {
    for (gint rr = r; rr < r + ch; rr++) for (gint cc = c; cc < c + cw; cc++) occ[rr][cc] = 1;
}

typedef struct { gint idx, c, r; gboolean has; } W10Want;
static gint w10_want_cmp(gconstpointer a, gconstpointer b) {
    const W10Want *x = a, *y = b;
    if (x->has != y->has) return x->has ? -1 : 1;          /* placed tiles first, new ones after */
    if (!x->has) return x->idx - y->idx;
    if (x->r != y->r) return x->r - y->r;
    return x->c - y->c;
}

/* Where every tile goes. moving (may be NULL) is put at (mc, mr) first; every other tile keeps its
   saved cell, or moves down until it fits; tiles never placed fill the first free cells.
   Returns the rows used. */
static gint w10_layout(StartMenuUI *ui, GPtrArray *tiles, GtkWidget *moving, gint mc, gint mr, W10Cell *out) {
    static guint8 occ[W10_MAXR][W10_MAX_COLS];
    memset(occ, 0, sizeof occ);
    gint cols = win10_cols(ui->shell), rows = 0;
    guint n = tiles->len;
    W10Want *want = g_new0(W10Want, MAX(1, n));
    for (guint i = 0; i < n; i++) {
        GtkWidget *t = g_ptr_array_index(tiles, i);
        gchar size = (gchar)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "tile-size"));
        w10_size_cells(size, &out[i].cw, &out[i].ch);
        want[i].idx = (gint)i;
        want[i].has = w10_saved_pos(ui->shell, g_object_get_data(G_OBJECT(t), "tile-id"), &want[i].c, &want[i].r);
    }
    /* 1. the tile being dragged goes exactly where the pointer is */
    for (guint i = 0; moving && i < n; i++) if (g_ptr_array_index(tiles, i) == moving) {
        gint c = CLAMP(mc, 0, cols - out[i].cw), r = MAX(0, mr);
        out[i].c = c; out[i].r = r;
        w10_mark(occ, c, r, out[i].cw, out[i].ch);
        rows = MAX(rows, r + out[i].ch);
    }
    /* 2. the others, top to bottom: their own cell, else pushed down */
    qsort(want, n, sizeof *want, w10_want_cmp);
    for (guint k = 0; k < n; k++) {
        guint i = (guint)want[k].idx;
        if (g_ptr_array_index(tiles, i) == moving) continue;
        gint cw = out[i].cw, ch = out[i].ch;
        gboolean done = FALSE;
        if (want[k].has) {
            gint c = CLAMP(want[k].c, 0, cols - cw);
            for (gint r = want[k].r; r + ch <= W10_MAXR && !done; r++)
                if (w10_free(occ, c, r, cw, ch)) { out[i].c = c; out[i].r = r; done = TRUE; }
        }
        for (gint r = 0; r + ch <= W10_MAXR && !done; r++)          /* new tile: first free cell */
            for (gint c = 0; c + cw <= cols && !done; c++)
                if (w10_free(occ, c, r, cw, ch)) { out[i].c = c; out[i].r = r; done = TRUE; }
        w10_mark(occ, out[i].c, out[i].r, cw, ch);
        rows = MAX(rows, out[i].r + ch);
    }
    g_free(want);
    return rows;
}

/* ---- groups: stacked bands, each a name and its own tile cells ---- */
#define W10_HDR       30      /* group name row */
#define W10_GROUP_GAP 14
#define W10_MAX_GROUPS 32
typedef struct { gint y0, y1, tiles_y; } W10Band;
static W10Band g_w10_band[W10_MAX_GROUPS + 1];
static gint    g_w10_nbands = 0;            /* bands from the last layout (index = group) */

static gint w10_group_count(UltimateShell *sh) {
    gchar **n = g_strsplit(sh->win10_groups && sh->win10_groups[0] ? sh->win10_groups : "Pinned", ";", -1);
    gint c = MAX(1, (gint)g_strv_length(n));
    g_strfreev(n);
    return MIN(c, W10_MAX_GROUPS);
}
static gchar *w10_group_name(UltimateShell *sh, gint g) {
    gchar **n = g_strsplit(sh->win10_groups && sh->win10_groups[0] ? sh->win10_groups : "Pinned", ";", -1);
    gchar *r = (g >= 0 && g < (gint)g_strv_length(n) && n[g][0]) ? g_strdup(n[g]) : g_strdup(g == 0 ? "Pinned" : "New group");
    g_strfreev(n);
    return r;
}
static void w10_save_group_names(UltimateShell *sh, GPtrArray *names) {
    GString *o = g_string_new("");
    for (guint i = 0; i < names->len; i++) {
        if (i) g_string_append_c(o, ';');
        gchar *clean = g_strdelimit(g_strdup(g_ptr_array_index(names, i)), ";", ',');
        g_string_append(o, clean);
        g_free(clean);
    }
    g_free(sh->win10_groups);
    sh->win10_groups = g_string_free(o, FALSE);
    if (sh->channel) xfconf_channel_set_string(sh->channel, "/plugins/ultimateshell/win10_groups", sh->win10_groups);
}

/* rename: click a group name */
static void on_w10_group_name_done(GtkEntry *e, GtkWidget *hdr) {
    StartMenuUI *ui = g_start_ui;
    gint g = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(hdr), "group-idx"));
    const gchar *txt = gtk_entry_get_text(e);
    if (ui && txt && txt[0]) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        gint n = MAX(w10_group_count(ui->shell), g + 1);
        for (gint i = 0; i < n; i++) g_ptr_array_add(names, i == g ? g_strdup(txt) : w10_group_name(ui->shell, i));
        w10_save_group_names(ui->shell, names);
        g_ptr_array_unref(names);
        gchar *mk = g_markup_printf_escaped("<b>%s</b>", txt);
        gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(hdr))), mk);
        g_free(mk);
    }
    GtkWidget *pop = gtk_widget_get_ancestor(GTK_WIDGET(e), GTK_TYPE_POPOVER);
    if (pop) gtk_popover_popdown(GTK_POPOVER(pop));
}
static void on_w10_group_hdr_clicked(GtkButton *b, gpointer d) {
    GtkWidget *pop = gtk_popover_new(GTK_WIDGET(b));
    GtkWidget *e = gtk_entry_new();
    gchar *cur = w10_group_name(g_start_ui->shell, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "group-idx")));
    gtk_entry_set_text(GTK_ENTRY(e), cur);
    g_free(cur);
    gtk_entry_set_placeholder_text(GTK_ENTRY(e), "Name group");
    gtk_container_set_border_width(GTK_CONTAINER(pop), 6);
    gtk_container_add(GTK_CONTAINER(pop), e);
    g_signal_connect(e, "activate", G_CALLBACK(on_w10_group_name_done), b);
    gtk_widget_show_all(pop);
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_widget_grab_focus(e);
}
static GtkWidget *w10_make_group_header(StartMenuUI *ui, gint g, const gchar *name) {
    GtkWidget *b = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    GtkWidget *l = gtk_label_new(NULL);
    gchar *mk = g_markup_printf_escaped("<b>%s</b>", name);
    gtk_label_set_markup(GTK_LABEL(l), mk);
    g_free(mk);
    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_container_add(GTK_CONTAINER(b), l);
    gtk_widget_set_tooltip_text(b, "Click to rename this group");
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "win10-group-hdr");
    g_object_set_data(G_OBJECT(b), "group-idx", GINT_TO_POINTER(g));
    gtk_widget_set_size_request(b, win10_cols(ui->shell) * (W10_CELL + W10_GAP) - W10_GAP, W10_HDR - 4);
    g_signal_connect(b, "clicked", G_CALLBACK(on_w10_group_hdr_clicked), NULL);
    return b;
}

/* lay every group out; moving (may be NULL) goes to group mg at (mc, mr). mg == number of groups
   means "a new group below the others" (shown while dragging there). */
static void w10_place(StartMenuUI *ui, GtkWidget *moving, gint mg, gint mc, gint mr) {
    GtkWidget *grid = ui->win10_tiles_grid;
    GPtrArray *tiles = GTK_IS_FIXED(grid) ? g_object_get_data(G_OBJECT(grid), "w10-tiles") : NULL;
    GPtrArray *hdrs  = GTK_IS_FIXED(grid) ? g_object_get_data(G_OBJECT(grid), "w10-headers") : NULL;
    if (!tiles || tiles->len == 0 || !hdrs) return;
    gint cols = win10_cols(ui->shell), step = W10_CELL + W10_GAP;
    gint ng = (gint)hdrs->len - 1;                       /* last header = the "new group" placeholder */
    gint y = 0;
    g_w10_nbands = 0;
    W10Cell *pos = g_new0(W10Cell, tiles->len);
    for (gint g = 0; g <= ng; g++) {
        GPtrArray *sub = g_ptr_array_new();
        for (guint i = 0; i < tiles->len; i++) {
            GtkWidget *t = g_ptr_array_index(tiles, i);
            if (t == moving) continue;
            if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(t), "tile-group")) == g) g_ptr_array_add(sub, t);
        }
        if (moving && mg == g) g_ptr_array_add(sub, moving);
        GtkWidget *hdr = g_ptr_array_index(hdrs, g);
        if (sub->len == 0) {                             /* empty group: not shown */
            gtk_widget_hide(hdr);
            g_w10_band[g].y0 = g_w10_band[g].y1 = g_w10_band[g].tiles_y = -1;
            g_ptr_array_unref(sub);
            if (g < W10_MAX_GROUPS + 1) g_w10_nbands = g + 1;
            continue;
        }
        gtk_widget_show(hdr);
        gtk_fixed_move(GTK_FIXED(grid), hdr, 0, y);
        gint rows = w10_layout(ui, sub, (moving && mg == g) ? moving : NULL, mc, mr, pos);
        gint ty = y + W10_HDR;
        for (guint i = 0; i < sub->len; i++) {
            GtkWidget *t = g_ptr_array_index(sub, i);
            gtk_fixed_move(GTK_FIXED(grid), t, pos[i].c * step, ty + pos[i].r * step);
            g_object_set_data(G_OBJECT(t), "cell-c", GINT_TO_POINTER(pos[i].c));
            g_object_set_data(G_OBJECT(t), "cell-r", GINT_TO_POINTER(pos[i].r));
            g_object_set_data(G_OBJECT(t), "cell-g", GINT_TO_POINTER(g));
        }
        g_w10_band[g].y0 = y;
        g_w10_band[g].tiles_y = ty;
        g_w10_band[g].y1 = ty + rows * step;
        y = g_w10_band[g].y1 + W10_GROUP_GAP;
        g_w10_nbands = g + 1;
        g_ptr_array_unref(sub);
    }
    g_free(pos);
    gtk_widget_set_size_request(grid, cols * step - W10_GAP, y + 2 * step);   /* + room for a new group */
}

/* remember cells and groups; empty groups go, the others are renumbered in order */
static void w10_save_positions(StartMenuUI *ui) {
    GtkWidget *grid = ui->win10_tiles_grid;
    GPtrArray *tiles = GTK_IS_FIXED(grid) ? g_object_get_data(G_OBJECT(grid), "w10-tiles") : NULL;
    GPtrArray *hdrs  = GTK_IS_FIXED(grid) ? g_object_get_data(G_OBJECT(grid), "w10-headers") : NULL;
    if (!tiles || !hdrs) return;
    gint total = (gint)hdrs->len;
    gint *map = g_new0(gint, total + 1);
    for (gint g = 0; g <= total; g++) map[g] = -1;
    for (guint i = 0; i < tiles->len; i++) {
        gint g = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g_ptr_array_index(tiles, i)), "cell-g"));
        if (g >= 0 && g <= total) map[g] = 0;
    }
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    gint next = 0;
    for (gint g = 0; g <= total; g++) if (map[g] == 0) { map[g] = next++; g_ptr_array_add(names, w10_group_name(ui->shell, g)); }
    if (names->len == 0) g_ptr_array_add(names, g_strdup("Pinned"));
    w10_save_group_names(ui->shell, names);
    g_ptr_array_unref(names);

    GString *out = g_string_new("");
    for (guint i = 0; i < tiles->len; i++) {
        GObject *t = g_ptr_array_index(tiles, i);
        const gchar *id = g_object_get_data(t, "tile-id");
        if (!id) continue;
        gint g = GPOINTER_TO_INT(g_object_get_data(t, "cell-g"));
        gint ng = (g >= 0 && g <= total && map[g] >= 0) ? map[g] : 0;
        g_object_set_data(t, "tile-group", GINT_TO_POINTER(ng));
        if (out->len) g_string_append_c(out, ';');
        g_string_append_printf(out, "%s=%d,%d,%d", id, GPOINTER_TO_INT(g_object_get_data(t, "cell-c")),
                               GPOINTER_TO_INT(g_object_get_data(t, "cell-r")), ng);
    }
    g_free(map);
    g_free(ui->shell->win10_tile_pos);
    ui->shell->win10_tile_pos = g_string_free(out, FALSE);
    if (ui->shell->channel)
        xfconf_channel_set_string(ui->shell->channel, "/plugins/ultimateshell/win10_tile_pos", ui->shell->win10_tile_pos);
}

/* (re)make the group name buttons: one per group + the hidden "New group" placeholder */
static void w10_make_headers(StartMenuUI *ui) {
    GtkWidget *grid = ui->win10_tiles_grid;
    GPtrArray *old = g_object_get_data(G_OBJECT(grid), "w10-headers");
    for (guint i = 0; old && i < old->len; i++) gtk_widget_destroy(g_ptr_array_index(old, i));
    GPtrArray *hdrs = g_ptr_array_new();
    gint n = w10_group_count(ui->shell);
    for (gint g = 0; g <= n; g++) {
        gchar *nm = (g < n) ? w10_group_name(ui->shell, g) : g_strdup("New group");
        GtkWidget *h = w10_make_group_header(ui, g, nm);
        g_free(nm);
        gtk_fixed_put(GTK_FIXED(grid), h, 0, 0);
        gtk_widget_set_no_show_all(h, TRUE);
        g_ptr_array_add(hdrs, h);
    }
    g_object_set_data_full(G_OBJECT(grid), "w10-headers", hdrs, (GDestroyNotify)g_ptr_array_unref);
}

static void win10_fill_tiles(StartMenuUI *ui) {
    GtkWidget *grid = ui->win10_tiles_grid;
    if (!GTK_IS_FIXED(grid)) return;
    g_object_set_data(G_OBJECT(grid), "w10-headers", NULL);
    GList *kids = gtk_container_get_children(GTK_CONTAINER(grid));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    ensure_default_favorites(ui->shell);
    GList *apps = get_sorted_apps_cached(ui);
    const gint W10_COLS = win10_cols(ui->shell);
    gint ngroups = w10_group_count(ui->shell);
    GPtrArray *tiles = g_ptr_array_new();
    gchar **toks = g_strsplit(ui->shell->start_favorites ? ui->shell->start_favorites : "", ";", -1);
    for (gint t = 0; toks && toks[t]; t++) {
        gchar *tok = g_strstrip(toks[t]);
        if (!tok[0]) continue;
        GAppInfo *app = NULL;
        for (GList *l = apps; l; l = l->next) {
            const gchar *id = g_app_info_get_id(l->data), *ex = g_app_info_get_executable(l->data);
            if ((id && g_ascii_strcasecmp(id, tok) == 0) || (ex && g_ascii_strcasecmp(ex, tok) == 0)) { app = l->data; break; }
        }
        if (!app || !g_app_info_should_show(app)) continue;
        gchar size = win10_tile_size(ui->shell, get_app_id_token(app));
        GtkWidget *tile = win10_make_tile(ui, app, size);
        gint c, r, g;
        w10_saved_pos3(ui->shell, get_app_id_token(app), &c, &r, &g);
        g_object_set_data(G_OBJECT(tile), "tile-size", GINT_TO_POINTER((gint)size));
        g_object_set_data(G_OBJECT(tile), "tile-group", GINT_TO_POINTER(CLAMP(g, 0, ngroups - 1)));
        g_object_set_data_full(G_OBJECT(tile), "tile-id", g_strdup(get_app_id_token(app)), g_free);
        gtk_fixed_put(GTK_FIXED(grid), tile, 0, 0);
        g_ptr_array_add(tiles, tile);
    }
    g_strfreev(toks);
    g_object_set_data_full(G_OBJECT(grid), "w10-tiles", tiles, (GDestroyNotify)g_ptr_array_unref);
    if (tiles->len == 0) {
        GtkWidget *hint = gtk_label_new("Right-click an app and choose\n“Add to Favorites” to pin it here.");
        gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
        gtk_widget_set_size_request(hint, W10_COLS * (W10_CELL + W10_GAP) - W10_GAP, 2 * W10_CELL);
        gtk_fixed_put(GTK_FIXED(grid), hint, 0, 0);
        gtk_widget_set_size_request(grid, W10_COLS * (W10_CELL + W10_GAP) - W10_GAP, 2 * W10_CELL);
    } else {
        w10_make_headers(ui);
        w10_place(ui, NULL, 0, 0, 0);
        w10_save_positions(ui);
        w10_make_headers(ui);              /* names may have been renumbered */
        w10_place(ui, NULL, 0, 0, 0);
    }
    gtk_widget_show_all(grid);
}

/* ---- dragging: the tile follows the pointer into a group's cell, or into a new group below ---- */
static GtkWidget *g_w10_moving = NULL;
static gboolean w10_after_drop(gpointer d) {
    g_w10_moving = NULL;
    if (g_start_ui) {
        refresh_win11_pinned(g_start_ui);
        if (g_start_ui->popup_win) clear_context_menu_guard_idle(g_start_ui->popup_win);   /* focus back */
    }
    return G_SOURCE_REMOVE;
}
static gint       g_w10_tg = -1, g_w10_tc = -1, g_w10_tr = -1;
static guint      g_w10_restore_id = 0;

static gboolean on_w10_area_motion(GtkWidget *box, GdkDragContext *ctx, gint x, gint y, guint t, StartMenuUI *ui) {
    GtkWidget *grid = ui->win10_tiles_grid;
    GPtrArray *hdrs = GTK_IS_FIXED(grid) ? g_object_get_data(G_OBJECT(grid), "w10-headers") : NULL;
    if (!hdrs || !g_w10_moving) { gdk_drag_status(ctx, 0, t); return FALSE; }
    if (g_w10_restore_id) { g_source_remove(g_w10_restore_id); g_w10_restore_id = 0; }
    gint gx = 0, gy = 0;
    gtk_widget_translate_coordinates(box, grid, x, y, &gx, &gy);
    gint step = W10_CELL + W10_GAP;
    gint ng = (gint)hdrs->len - 1;
    /* which group band is under the pointer? below the last one = a new group */
    gint g = -1, last = -1;
    for (gint i = 0; i < g_w10_nbands && i < ng; i++) {
        if (g_w10_band[i].y0 < 0) continue;
        last = i;
        if (gy < g_w10_band[i].y1 + W10_GROUP_GAP / 2) { g = i; break; }
    }
    gint origin = 0;
    if (g < 0) {
        gint bottom = (last >= 0) ? g_w10_band[last].y1 : 0;
        if (last >= 0 && gy < bottom + W10_GROUP_GAP + W10_HDR / 2) g = last;      /* just under it: same group */
        else { g = ng; origin = (last >= 0 ? bottom + W10_GROUP_GAP : 0) + W10_HDR; }
    }
    if (g < ng) origin = g_w10_band[g].tiles_y;
    gchar size = (gchar)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(g_w10_moving), "tile-size"));
    gint cw, ch; w10_size_cells(size, &cw, &ch);
    gint c = (gint)floor((gdouble)gx / step - cw / 2.0 + 0.5);
    gint r = (gint)floor((gdouble)(gy - origin) / step - ch / 2.0 + 0.5);
    c = CLAMP(c, 0, win10_cols(ui->shell) - cw);
    r = CLAMP(r, 0, W10_MAXR - ch);
    if (g == ng) r = 0;                         /* a new group starts with this tile */
    if (g != g_w10_tg || c != g_w10_tc || r != g_w10_tr) {
        g_w10_tg = g; g_w10_tc = c; g_w10_tr = r;
        w10_place(ui, g_w10_moving, g, c, r);
    }
    gdk_drag_status(ctx, GDK_ACTION_MOVE, t);
    return TRUE;
}

static gboolean w10_restore_idle(gpointer d) {
    StartMenuUI *ui = d;
    g_w10_restore_id = 0;
    g_w10_tg = g_w10_tc = g_w10_tr = -1;
    w10_place(ui, NULL, 0, 0, 0);            /* back to the saved cells */
    return G_SOURCE_REMOVE;
}

static void on_w10_area_leave(GtkWidget *box, GdkDragContext *ctx, guint t, StartMenuUI *ui) {
    if (g_w10_restore_id) g_source_remove(g_w10_restore_id);
    g_w10_restore_id = g_timeout_add(120, w10_restore_idle, ui);
}

static gboolean on_w10_area_drop(GtkWidget *box, GdkDragContext *ctx, gint x, gint y, guint t, StartMenuUI *ui) {
    if (g_w10_restore_id) { g_source_remove(g_w10_restore_id); g_w10_restore_id = 0; }
    if (g_w10_moving && g_w10_tc >= 0) {
        GPtrArray *hdrs = g_object_get_data(G_OBJECT(ui->win10_tiles_grid), "w10-headers");
        gboolean new_group = hdrs && g_w10_tg == (gint)hdrs->len - 1;
        w10_place(ui, g_w10_moving, g_w10_tg, g_w10_tc, g_w10_tr);
        if (new_group) {                          /* name it "New group" (click to rename) */
            gint n = w10_group_count(ui->shell);
            GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
            for (gint i = 0; i < n; i++) g_ptr_array_add(names, w10_group_name(ui->shell, i));
            g_ptr_array_add(names, g_strdup("New group"));
            w10_save_group_names(ui->shell, names);
            g_ptr_array_unref(names);
        }
        w10_save_positions(ui);                   /* what you saw is what is kept; empty groups go */
        g_timeout_add(250, w10_after_drop, NULL);   /* rebuild group names once the drag has fully ended */
    }
    g_w10_tg = g_w10_tc = g_w10_tr = -1;
    gtk_drag_finish(ctx, TRUE, FALSE, t);
    return TRUE;
}

static void on_w10_tile_drag_begin(GtkWidget *w, GdkDragContext *ctx, gpointer d) { g_w10_moving = w; g_w10_tg = g_w10_tc = g_w10_tr = -1; }
static void on_w10_tile_drag_end(GtkWidget *w, GdkDragContext *ctx, StartMenuUI *ui) {
    g_w10_moving = NULL;
    if (g_w10_tc >= 0) w10_restore_idle(ui);      /* dropped elsewhere / cancelled: back as it was */
}

static void on_win10_resize_tile(GtkMenuItem *mi, GAppInfo *app) {
    if (!g_start_ui || !app) return;
    if (GTK_IS_CHECK_MENU_ITEM(mi) && !gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(mi))) return;
    gchar size = (gchar)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(mi), "tile-size"));
    win10_set_tile_size(g_start_ui->shell, get_app_id_token(app), size);
    win10_fill_tiles(g_start_ui);
}

/* "Resize ▸ Small / Medium / Wide" for a pinned tile (Win10 style only) */
static void win10_append_resize_menu(GtkWidget *menu, GAppInfo *app) {
    if (!g_start_ui || !g_start_ui->shell || g_start_ui->shell->start_style != START_STYLE_WIN10) return;
    if (!is_app_in_favorites(g_start_ui->shell, app)) return;
    gchar cur = win10_tile_size(g_start_ui->shell, get_app_id_token(app));
    GtkWidget *it = gtk_menu_item_new_with_label("Resize");
    GtkWidget *sub = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "ultimate-themed-menu");
    const struct { const gchar *lbl; gchar s; } opts[] = { { "Small", 's' }, { "Medium", 'm' }, { "Wide", 'w' } };
    GSList *grp = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(opts); i++) {
        GtkWidget *r = gtk_radio_menu_item_new_with_label(grp, opts[i].lbl);
        grp = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(r));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(r), opts[i].s == cur);
        g_object_set_data(G_OBJECT(r), "tile-size", GINT_TO_POINTER((gint)opts[i].s));
        g_signal_connect(r, "toggled", G_CALLBACK(on_win10_resize_tile), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), r);
    }
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it);
}

/* A–Z list: click a letter header → letter picker → jump to that letter */
static void on_win10_letter_clicked(GtkButton *b, StartMenuUI *ui) {
    GtkWidget *target = g_object_get_data(G_OBJECT(b), "hdr-row");
    if (ui->win10_list_stack) gtk_stack_set_visible_child_name(GTK_STACK(ui->win10_list_stack), "list");
    if (!GTK_IS_WIDGET(target) || !GTK_IS_SCROLLED_WINDOW(ui->win10_list_scroll)) return;
    GtkAllocation a; gtk_widget_get_allocation(target, &a);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(ui->win10_list_scroll));
    gtk_adjustment_set_value(adj, a.y);
}

static void win10_show_letter_picker(StartMenuUI *ui) {
    GtkWidget *page = ui->win10_list_stack ? gtk_stack_get_child_by_name(GTK_STACK(ui->win10_list_stack), "letters") : NULL;
    if (!GTK_IS_GRID(page)) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(page));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    const gchar *letters = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    GList *rows = gtk_container_get_children(GTK_CONTAINER(ui->list_box));
    for (gint i = 0; letters[i]; i++) {
        gchar lb[2] = { letters[i], 0 };
        GtkWidget *btn = gtk_button_new_with_label(lb);
        gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(btn), "win10-letter");
        gtk_widget_set_size_request(btn, 44, 40);
        GtkWidget *hit = NULL;
        for (GList *l = rows; l; l = l->next) {
            const gchar *hl = g_object_get_data(G_OBJECT(l->data), "alpha-letter");
            if (hl && g_strcmp0(hl, lb) == 0) { hit = l->data; break; }
        }
        gtk_widget_set_sensitive(btn, hit != NULL);
        g_object_set_data(G_OBJECT(btn), "hdr-row", hit);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_win10_letter_clicked), ui);
        gtk_grid_attach(GTK_GRID(page), btn, i % 4, i / 4, 1, 1);
    }
    g_list_free(rows);
    gtk_widget_show_all(page);
    gtk_stack_set_visible_child_name(GTK_STACK(ui->win10_list_stack), "letters");
}

static GtkWidget *win10_rail_button(const gchar *icon, const gchar *tip, GCallback cb, gpointer data) {
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_style_context_add_class(gtk_widget_get_style_context(b), "win10-rail-btn");
    gtk_widget_set_tooltip_text(b, tip);
    gtk_widget_set_size_request(b, 44, 40);
    if (cb) g_signal_connect(b, "clicked", cb, data);
    return b;
}

static void on_win10_power_item(GtkMenuItem *mi, const gchar *action) {
    if (g_strcmp0(action, "switch") == 0) on_session_cmd(NULL, "dm-tool switch-to-greeter");
    else if (g_strcmp0(action, "lock") == 0) on_session_cmd(NULL, "xflock4");
    else if (g_strcmp0(action, "suspend") == 0) on_session_cmd(NULL, "systemctl suspend");
    else on_confirm_power_action(NULL, action);
}

static void on_win10_power_clicked(GtkButton *b, StartMenuUI *ui) {
    g_object_set_data(G_OBJECT(ui->popup_win), "context-menu-open", GINT_TO_POINTER(1));
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_app_context_menu_deactivate), ui->popup_win);
    const struct { const gchar *lbl, *act; } it[] = {
        { "Lock", "lock" }, { "Sleep", "suspend" }, { "Sign out", "logout" }, { "Restart", "restart" }, { "Shut down", "shutdown" } };
    for (guint i = 0; i < G_N_ELEMENTS(it); i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(it[i].lbl);
        g_signal_connect(mi, "activate", G_CALLBACK(on_win10_power_item), (gpointer)it[i].act);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b), GDK_GRAVITY_NORTH_EAST, GDK_GRAVITY_SOUTH_WEST, NULL);
}

/* Classic (Windows 7): [ Shut down ][ ▸ ] - the arrow opens the other session actions */
static void on_classic_power_more(GtkButton *b, StartMenuUI *ui) {
    g_object_set_data(G_OBJECT(ui->popup_win), "context-menu-open", GINT_TO_POINTER(1));
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_app_context_menu_deactivate), ui->popup_win);
    gchar *dm = g_find_program_in_path("dm-tool");
    const struct { const gchar *lbl, *act; gboolean show; } it[] = {
        { "Switch user", "switch", dm != NULL }, { "Log off", "logout", TRUE }, { "Lock", "lock", TRUE },
        { "Restart", "restart", TRUE }, { "Sleep", "suspend", TRUE } };
    g_free(dm);
    for (guint i = 0; i < G_N_ELEMENTS(it); i++) {
        if (!it[i].show) continue;
        GtkWidget *mi = gtk_menu_item_new_with_label(it[i].lbl);
        g_signal_connect(mi, "activate", G_CALLBACK(on_win10_power_item), (gpointer)it[i].act);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), GTK_WIDGET(b), GDK_GRAVITY_NORTH_EAST, GDK_GRAVITY_SOUTH_EAST, NULL);
}

static GtkWidget *classic_power_button(StartMenuUI *ui) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "linked");
    GtkWidget *sd = gtk_button_new_with_label("Shut down");
    g_signal_connect(sd, "clicked", G_CALLBACK(on_confirm_power_action), "shutdown");
    GtkWidget *more = gtk_button_new_with_label("▸");
    gtk_widget_set_tooltip_text(more, "Switch user, Log off, Lock, Restart, Sleep");
    g_signal_connect(more, "clicked", G_CALLBACK(on_classic_power_more), ui);
    gtk_box_pack_start(GTK_BOX(box), sd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), more, FALSE, FALSE, 0);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    return box;
}

static void build_win10_menu(StartMenuUI *ui) {
    UltimateShell *shell = ui->shell;
    gint init_w = 760, init_h = 600;
    get_saved_or_default_style_size(shell, START_STYLE_WIN10, &init_w, &init_h);
    {   /* width follows Grid Columns: rail + A–Z list + exactly the tile columns */
        gint c = win10_cols(shell);
        gint tiles_w = c * W10_CELL + (c - 1) * W10_GAP;
        init_w = shell->win10_show_tiles ? 52 + 250 + tiles_w + 28 + 14 : 52 + 250 + 16;
    }
    gtk_window_set_resizable(GTK_WINDOW(ui->popup_win), TRUE);
    gtk_widget_set_size_request(ui->popup_win, init_w, init_h);
    gtk_window_resize(GTK_WINDOW(ui->popup_win), init_w, init_h);
    gtk_container_set_border_width(GTK_CONTAINER(ui->root_container), 0);
    gtk_box_set_spacing(GTK_BOX(ui->root_container), 0);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->search_entry), "Type here to search");

    GtkWidget *cols = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

    /* 1. rail: account up top, places + power at the bottom */
    GtkWidget *rail = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_style_context_add_class(gtk_widget_get_style_context(rail), "win10-rail");
    gtk_widget_set_margin_top(rail, 6); gtk_widget_set_margin_bottom(rail, 6);
    gtk_widget_set_margin_start(rail, 4); gtk_widget_set_margin_end(rail, 4);
    if (shell->start_show_user_header) {
        GtkWidget *av = create_circular_avatar_widget(30);
        gtk_widget_set_tooltip_text(av, g_get_user_name());
        gtk_widget_set_margin_bottom(av, 6);
        gtk_box_pack_start(GTK_BOX(rail), av, FALSE, FALSE, 4);
    }
    gtk_box_pack_end(GTK_BOX(rail), win10_rail_button("system-shutdown-symbolic", "Power", G_CALLBACK(on_win10_power_clicked), ui), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(rail), win10_rail_button("preferences-system-symbolic", "Settings", G_CALLBACK(on_place_shortcut_clicked), "settings"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(rail), win10_rail_button("folder-pictures-symbolic", "Pictures", G_CALLBACK(on_place_shortcut_clicked), "pic"), FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(rail), win10_rail_button("folder-documents-symbolic", "Documents", G_CALLBACK(on_place_shortcut_clicked), "docs"), FALSE, FALSE, 0);

    /* 2. A–Z list (with search on top or bottom) */
    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(mid, 10); gtk_widget_set_margin_bottom(mid, 10);
    gtk_widget_set_size_request(mid, 250, -1);
    ui->win10_list_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ui->win10_list_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    ui->list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list_box), GTK_SELECTION_NONE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(ui->list_box), filter_list_func, ui, NULL);
    g_signal_connect(ui->list_box, "row-activated", G_CALLBACK(on_app_row_activated), ui);
    populate_app_list(ui, ui->list_box, TRUE);
    {   /* letter headers open the letter picker */
        GList *rows = gtk_container_get_children(GTK_CONTAINER(ui->list_box));
        for (GList *l = rows; l; l = l->next)
            if (g_object_get_data(G_OBJECT(l->data), "is-alpha-header"))
                gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(l->data), TRUE);
        g_list_free(rows);
    }
    gtk_container_add(GTK_CONTAINER(ui->win10_list_scroll), ui->list_box);
    GtkWidget *letters = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(letters), 4);
    gtk_grid_set_column_spacing(GTK_GRID(letters), 4);
    gtk_widget_set_halign(letters, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(letters, GTK_ALIGN_CENTER);
    ui->win10_list_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui->win10_list_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_add_named(GTK_STACK(ui->win10_list_stack), ui->win10_list_scroll, "list");
    gtk_stack_add_named(GTK_STACK(ui->win10_list_stack), letters, "letters");
    gboolean search_top = should_place_search_top(shell, FALSE);
    if (search_top) gtk_box_pack_start(GTK_BOX(mid), ui->search_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mid), ui->win10_list_stack, TRUE, TRUE, 0);
    if (!search_top) gtk_box_pack_start(GTK_BOX(mid), ui->search_entry, FALSE, FALSE, 0);

    /* 3. tiles */
    ui->win10_tiles_pane = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ui->win10_tiles_pane), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *tiles_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(tiles_box, 14); gtk_widget_set_margin_end(tiles_box, 14);
    gtk_widget_set_margin_top(tiles_box, 12);
    GtkWidget *ttl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(ttl), "<b>Pinned</b>");
    gtk_label_set_xalign(GTK_LABEL(ttl), 0.0);
    ui->win10_tiles_grid = gtk_fixed_new();        /* tiles at exact cell positions */
    gtk_widget_set_halign(ui->win10_tiles_grid, GTK_ALIGN_START);
    gtk_widget_destroy(ttl);                       /* each group shows its own name now */
    gtk_box_pack_start(GTK_BOX(tiles_box), ui->win10_tiles_grid, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ui->win10_tiles_pane), tiles_box);
    gtk_widget_set_vexpand(tiles_box, TRUE);       /* drop anywhere in the pane */
    gtk_drag_dest_set(tiles_box, 0, g_fav_target, 1, GDK_ACTION_MOVE);
    g_signal_connect(tiles_box, "drag-motion", G_CALLBACK(on_w10_area_motion), ui);
    g_signal_connect(tiles_box, "drag-leave", G_CALLBACK(on_w10_area_leave), ui);
    g_signal_connect(tiles_box, "drag-drop", G_CALLBACK(on_w10_area_drop), ui);
    win10_fill_tiles(ui);

    gtk_box_pack_start(GTK_BOX(cols), rail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(cols), mid, FALSE, TRUE, 0);
    if (shell->win10_show_tiles) gtk_box_pack_start(GTK_BOX(cols), ui->win10_tiles_pane, TRUE, TRUE, 0);
    else { gtk_widget_destroy(ui->win10_tiles_pane); ui->win10_tiles_pane = NULL; ui->win10_tiles_grid = NULL; }
    if (!shell->win10_show_tiles) gtk_box_set_child_packing(GTK_BOX(cols), mid, TRUE, TRUE, 0, GTK_PACK_START);
    gtk_box_pack_start(GTK_BOX(ui->root_container), cols, TRUE, TRUE, 0);
}

/* =========================================================================
 * GNOME App Grid: open windows overview above a paged app grid
 * ========================================================================= */
/* ---- page peeks: the edge column of the previous / next page, dimmed ---- */
static gboolean on_gnome_peek_press(GtkWidget *eb, GdkEventButton *ev, StartMenuUI *ui) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1) return FALSE;
    gint dir = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(eb), "peek-dir"));
    fs_go_to_page(ui, ui->fs_current_page + dir);
    return TRUE;
}

static GtkWidget *gnome_make_peek(StartMenuUI *ui, gint dir) {
    GtkWidget *eb = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(eb), FALSE);
    gtk_widget_add_events(eb, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
    g_object_set_data(G_OBJECT(eb), "peek-dir", GINT_TO_POINTER(dir));
    g_signal_connect(eb, "button-press-event", G_CALLBACK(on_gnome_peek_press), ui);
    g_signal_connect(eb, "scroll-event", G_CALLBACK(on_fs_deck_scroll), ui);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_valign(col, GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(eb), col);
    gtk_widget_set_opacity(eb, 0.35);
    return eb;
}

static void gnome_fill_peek(StartMenuUI *ui, GtkWidget *peek, gint page, gboolean last_col) {
    if (!GTK_IS_WIDGET(peek)) return;
    GtkWidget *col = gtk_bin_get_child(GTK_BIN(peek));
    GList *kids = gtk_container_get_children(GTK_CONTAINER(col));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
    gint st = START_STYLE_GNOME;
    gint icon_px = (ui->shell->start_style_grid_icon[st] > 0) ? CLAMP(ui->shell->start_style_grid_icon[st], 32, 96) : 48;
    gtk_widget_set_size_request(peek, icon_px + 24, -1);     /* one icon column of the neighbouring page */
    if (!ui->fs_page_apps || page < 0 || page >= (gint)ui->fs_page_apps->len) return;
    GPtrArray *apps = g_ptr_array_index(ui->fs_page_apps, page);
    gint cols = MAX(1, ui->fs_page_cols), rows = ((gint)apps->len + cols - 1) / cols;
    for (gint r = 0; r < rows; r++) {
        gint idx = r * cols + (last_col ? cols - 1 : 0);
        if (last_col && idx >= (gint)apps->len) idx = (gint)apps->len - 1;   /* short last row */
        if (idx < 0 || idx >= (gint)apps->len) continue;
        GdkPixbuf *pb = load_app_pixbuf_at_size(g_ptr_array_index(apps, idx), icon_px);
        GtkWidget *img = pb ? gtk_image_new_from_pixbuf(pb) : gtk_image_new_from_icon_name("application-x-executable", GTK_ICON_SIZE_DIALOG);
        if (pb) g_object_unref(pb);
        gtk_widget_set_size_request(img, icon_px, icon_px + 30);
        gtk_box_pack_start(GTK_BOX(col), img, FALSE, FALSE, 0);
    }
}

static void gnome_update_peeks(StartMenuUI *ui) {
    gnome_fill_peek(ui, ui->gnome_peek_prev, ui->fs_current_page - 1, TRUE);
    gnome_fill_peek(ui, ui->gnome_peek_next, ui->fs_current_page + 1, FALSE);
    if (ui->gnome_peek_prev) gtk_widget_show_all(ui->gnome_peek_prev);
    if (ui->gnome_peek_next) gtk_widget_show_all(ui->gnome_peek_next);
}

/* ---- opening: the icons grow into place (GNOME's icon scale-in), with the Animations setting ---- */
static gboolean gnome_intro_end(gpointer d) {
    StartMenuUI *ui = d;
    if (ui->popup_win) gtk_style_context_remove_class(gtk_widget_get_style_context(ui->popup_win), "fs-grid-intro");
    return G_SOURCE_REMOVE;
}
static void gnome_play_intro(StartMenuUI *ui) {
    if (!ui->popup_win || shell_anim_ms(300) <= 0) return;
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->popup_win), "fs-grid-intro");
    g_timeout_add(40, gnome_intro_end, ui);     /* the transition back to normal size is the animation */
}

static const GtkTargetEntry g_app_target[] = { { "ULTIMATE_APP_ID", GTK_TARGET_SAME_APP, 0 } };

/* ---- dragging an app to another page: hold it at the left / right edge (or on a peek) for a second
   (GNOME: OVERSHOOT_THRESHOLD 20 px, OVERSHOOT_TIMEOUT 1000 ms) ---- */
static guint g_edge_turn_id = 0;
static gint  g_edge_turn_dir = 0;
static gboolean edge_turn_fire(gpointer d) {
    StartMenuUI *ui = d;
    fs_go_to_page(ui, ui->fs_current_page + g_edge_turn_dir);
    return G_SOURCE_CONTINUE;                    /* still held there: keep turning, one page a second */
}
static void edge_turn_stop(void) {
    if (g_edge_turn_id) { g_source_remove(g_edge_turn_id); g_edge_turn_id = 0; }
    g_edge_turn_dir = 0;
}
static gboolean in_widget_x(GtkWidget *area, GtkWidget *w, gint x) {
    if (!GTK_IS_WIDGET(w) || !gtk_widget_get_visible(w)) return FALSE;
    gint wx = 0, wy = 0;
    if (!gtk_widget_translate_coordinates(w, area, 0, 0, &wx, &wy)) return FALSE;
    return x >= wx && x < wx + gtk_widget_get_allocated_width(w);
}
static gboolean on_deck_drag_motion(GtkWidget *area, GdkDragContext *ctx, gint x, gint y, guint t, StartMenuUI *ui) {
    gint w = gtk_widget_get_allocated_width(area);
    gint dir = 0;
    if (x < 20 || in_widget_x(area, ui->gnome_peek_prev, x)) dir = -1;
    else if (x > w - 20 || in_widget_x(area, ui->gnome_peek_next, x)) dir = 1;
    if (dir != g_edge_turn_dir) {
        edge_turn_stop();
        if (dir) { g_edge_turn_dir = dir; g_edge_turn_id = g_timeout_add(1000, edge_turn_fire, ui); }
    }
    gdk_drag_status(ctx, 0, t);                  /* nothing to drop here: workspaces take drops */
    return TRUE;
}
static void on_deck_drag_leave(GtkWidget *area, GdkDragContext *ctx, guint t, gpointer d) { edge_turn_stop(); }
static gboolean on_deck_drag_drop(GtkWidget *area, GdkDragContext *ctx, gint x, gint y, guint t, gpointer d) {
    edge_turn_stop();
    gtk_drag_finish(ctx, FALSE, FALSE, t);
    return TRUE;
}

static void on_gnome_app_drag_get(GtkWidget *w, GdkDragContext *c, GtkSelectionData *sd, guint i, guint t, GAppInfo *app) {
    const gchar *id = get_app_id_token(app);
    if (id) gtk_selection_data_set(sd, gtk_selection_data_get_target(sd), 8, (const guchar *)id, strlen(id));
}

static void gnome_make_tile_draggable(GtkWidget *eb, GAppInfo *app) {
    if (!GTK_IS_WIDGET(eb) || !app) return;
    gtk_drag_source_set(eb, GDK_BUTTON1_MASK, g_app_target, 1, GDK_ACTION_COPY);
    GIcon *gi = g_app_info_get_icon(app);
    if (gi) gtk_drag_source_set_icon_gicon(eb, gi);
    g_signal_connect(eb, "drag-data-get", G_CALLBACK(on_gnome_app_drag_get), app);
    sm_drag_keep_open(eb);
}

/* workspace map heights follow the screen: others 6.5 %, the current one 8 % of its height */
static gint gnome_ws_h(gboolean active) {
    GdkMonitor *m = gdk_display_get_primary_monitor(gdk_display_get_default());
    if (!m) m = gdk_display_get_monitor(gdk_display_get_default(), 0);
    GdkRectangle g = { 0, 0, 1920, 1080 };
    if (m) gdk_monitor_get_geometry(m, &g);
    return MAX(40, (gint)(g.height * (active ? 0.080 : 0.065)));
}
#define GNOME_WS_H        gnome_ws_h(FALSE)
#define GNOME_WS_ACTIVE_H gnome_ws_h(TRUE)

static gboolean gnome_hold_release(gpointer d) {
    StartMenuUI *ui = d;
    g_object_set_data(G_OBJECT(ui->popup_win), "gnome-hold-id", NULL);
    clear_context_menu_guard_idle(ui->popup_win);     /* back on top with focus, guard off */
    return G_SOURCE_REMOVE;
}

/* xfdesktop's wallpaper for workspace n (first monitor that has one), scaled & cached */
static GHashTable *g_ws_wall_cache = NULL;   /* "path@WxH" → GdkPixbuf */
static GdkPixbuf *gnome_ws_wallpaper(gint n, gint w, gint h) {
    XfconfChannel *ch = xfconf_channel_get("xfce4-desktop");
    if (!ch) return NULL;
    GHashTable *props = xfconf_channel_get_properties(ch, "/backdrop/screen0");
    if (!props) return NULL;
    gboolean single = FALSE; gint single_n = 0;
    gchar *path = NULL;
    GHashTableIter it; gpointer k, v;
    /* single-workspace mode: every workspace shows that one */
    g_hash_table_iter_init(&it, props);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (g_str_has_suffix(k, "/single-workspace-mode") && G_VALUE_HOLDS_BOOLEAN((GValue *)v) && g_value_get_boolean(v)) single = TRUE;
        if (g_str_has_suffix(k, "/single-workspace-number") && G_VALUE_HOLDS_INT((GValue *)v)) single_n = g_value_get_int(v);
    }
    gchar *want = g_strdup_printf("/workspace%d/last-image", single ? single_n : n);
    gchar *best_key = NULL;
    g_hash_table_iter_init(&it, props);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        if (!g_str_has_suffix(k, want) || !G_VALUE_HOLDS_STRING((GValue *)v)) continue;
        if (!best_key || g_strcmp0(k, best_key) < 0) { best_key = k; g_free(path); path = g_value_dup_string(v); }
    }
    g_free(want);
    g_hash_table_destroy(props);
    if (!path || !path[0]) { g_free(path); return NULL; }
    if (!g_ws_wall_cache) g_ws_wall_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    gchar *ck = g_strdup_printf("%s@%dx%d", path, w, h);
    GdkPixbuf *pb = g_hash_table_lookup(g_ws_wall_cache, ck);
    if (!pb) {
        pb = gdk_pixbuf_new_from_file_at_scale(path, w * 2, h * 2, FALSE, NULL);   /* crisp on the scaled map */
        if (pb) g_hash_table_insert(g_ws_wall_cache, g_strdup(ck), pb);
    }
    g_free(ck); g_free(path);
    return pb;
}

/* a small map of the workspace: its windows as frames with their icons */
static gboolean on_gnome_ws_draw(GtkWidget *da, cairo_t *cr, StartMenuUI *ui) {
    WnckWorkspace *ws = g_object_get_data(G_OBJECT(da), "wnck-ws");
    WnckScreen *scr = (ui->shell && ui->shell->tracker) ? ui->shell->tracker->screen : NULL;
    if (!WNCK_IS_WORKSPACE(ws) || !scr) return FALSE;
    gint w = gtk_widget_get_allocated_width(da), h = gtk_widget_get_allocated_height(da);
    ShellPalette p; shell_palette_get(ui->shell, &p);
    gboolean active = (ws == wnck_screen_get_active_workspace(scr));
    gboolean hover = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(da), "drop-hover"));

    gdouble r = CLAMP(h * 0.11, 6.0, 12.0);     /* GNOME's 30 px wallpaper radius, scaled to the map */
    cairo_save(cr);
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r - 1, r + 1, r, -G_PI / 2, 0);
    cairo_arc(cr, w - r - 1, h - r - 1, r, 0, G_PI / 2);
    cairo_arc(cr, r + 1, h - r - 1, r, G_PI / 2, G_PI);
    cairo_arc(cr, r + 1, r + 1, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    GdkPixbuf *wall = g_object_get_data(G_OBJECT(da), "ws-wall");   /* looked up once per opening */
    cairo_clip(cr);                  /* rounded map; clears the path for the window frames */
    if (wall) {
        cairo_save(cr);
        cairo_scale(cr, (gdouble)w / gdk_pixbuf_get_width(wall), (gdouble)h / gdk_pixbuf_get_height(wall));
        gdk_cairo_set_source_pixbuf(cr, wall, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
    } else {
        cairo_set_source_rgba(cr, p.fg.red, p.fg.green, p.fg.blue, 0.10);
        cairo_paint(cr);
    }
    if (hover) { cairo_set_source_rgba(cr, p.accent.red, p.accent.green, p.accent.blue, 0.25); cairo_paint(cr); }

    gint sw = wnck_screen_get_width(scr), sh = wnck_screen_get_height(scr);
    gdouble sx = (gdouble)w / MAX(1, sw), sy = (gdouble)h / MAX(1, sh);
    GList *wins = g_list_copy(wnck_screen_get_windows_stacked(scr));   /* bottom → top */
    for (GList *l = wins; l; l = l->next) {
        WnckWindow *win = l->data;
        if (wnck_window_is_skip_tasklist(win) || wnck_window_is_minimized(win) ||
            wnck_window_get_window_type(win) != WNCK_WINDOW_NORMAL || !wnck_window_is_on_workspace(win, ws)) continue;
        gint x, y, ww, wh;
        wnck_window_get_geometry(win, &x, &y, &ww, &wh);
        x -= wnck_workspace_get_viewport_x(ws); y -= wnck_workspace_get_viewport_y(ws);
        gdouble rx = x * sx, ry = y * sy, rw = MAX(6, ww * sx), rh = MAX(6, wh * sy);
        cairo_rectangle(cr, rx + 0.5, ry + 0.5, rw - 1, rh - 1);
        cairo_set_source_rgba(cr, p.bg.red, p.bg.green, p.bg.blue, 0.95);
        cairo_fill_preserve(cr);
        cairo_set_source_rgba(cr, p.fg.red, p.fg.green, p.fg.blue, 0.30);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
        GdkPixbuf *ic = wnck_window_get_mini_icon(win);
        if (ic && rw > 18 && rh > 18) {
            gdk_cairo_set_source_pixbuf(cr, ic, rx + (rw - gdk_pixbuf_get_width(ic)) / 2, ry + (rh - gdk_pixbuf_get_height(ic)) / 2);
            cairo_paint(cr);
        }
    }
    g_list_free(wins);
    cairo_restore(cr);
    /* frame: accent on the current workspace / while an app hovers over it */
    cairo_new_sub_path(cr);
    cairo_arc(cr, w - r - 1, r + 1, r, -G_PI / 2, 0);
    cairo_arc(cr, w - r - 1, h - r - 1, r, 0, G_PI / 2);
    cairo_arc(cr, r + 1, h - r - 1, r, G_PI / 2, G_PI);
    cairo_arc(cr, r + 1, r + 1, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
    if (active || hover) cairo_set_source_rgba(cr, p.accent.red, p.accent.green, p.accent.blue, 1.0);
    else cairo_set_source_rgba(cr, p.fg.red, p.fg.green, p.fg.blue, 0.25);
    cairo_set_line_width(cr, (active || hover) ? 2.5 : 1.0);
    cairo_stroke(cr);
    return FALSE;
}

static gboolean on_gnome_ws_press(GtkWidget *da, GdkEventButton *ev, StartMenuUI *ui) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 1) return TRUE;
    /* switch to that workspace; the grid stays open (it is sticky) */
    WnckWorkspace *ws = g_object_get_data(G_OBJECT(da), "wnck-ws");
    WnckScreen *scr = (ui->shell && ui->shell->tracker) ? ui->shell->tracker->screen : NULL;
    if (WNCK_IS_WORKSPACE(ws) && scr && ws != wnck_screen_get_active_workspace(scr)) {
        gnome_hold_open(ui, 700);
        wnck_workspace_activate(ws, ev->time);
    }
    return TRUE;
}

static gboolean on_gnome_ws_drag_motion(GtkWidget *da, GdkDragContext *c, gint x, gint y, guint t, gpointer d) {
    if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(da), "drop-hover"))) {
        g_object_set_data(G_OBJECT(da), "drop-hover", GINT_TO_POINTER(1));
        gtk_widget_queue_draw(da);
    }
    gdk_drag_status(c, GDK_ACTION_COPY, t);
    return TRUE;
}
static void on_gnome_ws_drag_leave(GtkWidget *da, GdkDragContext *c, guint t, gpointer d) {
    g_object_set_data(G_OBJECT(da), "drop-hover", NULL);
    gtk_widget_queue_draw(da);
}

/* Keep the grid open for a moment although focus moves (workspace switch, a new window):
   the focus-out close is skipped, then the grid takes focus back */
static void gnome_hold_open(StartMenuUI *ui, guint ms) {
    if (!ui || !ui->popup_win) return;
    g_object_set_data(G_OBJECT(ui->popup_win), "context-menu-open", GINT_TO_POINTER(1));
    guint old = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(ui->popup_win), "gnome-hold-id"));
    if (old) g_source_remove(old);
    guint id = g_timeout_add(ms, gnome_hold_release, ui);
    g_object_set_data(G_OBJECT(ui->popup_win), "gnome-hold-id", GUINT_TO_POINTER(id));
}

typedef struct { GAppInfo *app; } GnomeLaunchLater;
static gboolean gnome_launch_later(gpointer data) {
    GnomeLaunchLater *l = data;
    if (l->app) { usage_record_launch(l->app); shell_launch_app_info(l->app); }
    g_object_unref(l->app);
    g_free(l);
    return G_SOURCE_REMOVE;
}

/* app dropped on a workspace: go there, then start the app (it opens on that workspace) */
static void on_gnome_ws_drag_received(GtkWidget *da, GdkDragContext *c, gint x, gint y, GtkSelectionData *sd,
                                      guint info, guint t, StartMenuUI *ui) {
    const guchar *data = gtk_selection_data_get_data(sd);
    gint len = gtk_selection_data_get_length(sd);
    GAppInfo *hit = NULL;
    if (data && len > 0) {
        gchar *id = g_strndup((const gchar *)data, len);
        for (GList *l = get_sorted_apps_cached(ui); l; l = l->next)
            if (g_strcmp0(get_app_id_token(l->data), id) == 0) { hit = l->data; break; }
        g_free(id);
    }
    if (!hit) return;              /* GTK_DEST_DEFAULT_DROP finishes the drag itself */
    WnckWorkspace *ws = g_object_get_data(G_OBJECT(da), "wnck-ws");
    /* the grid stays open: switch, start the app and watch it appear in the overview */
    gnome_hold_open(ui, 3000);
    if (WNCK_IS_WORKSPACE(ws)) wnck_workspace_activate(ws, t);
    GnomeLaunchLater *l = g_new0(GnomeLaunchLater, 1);
    l->app = g_object_ref(hit);
    g_timeout_add(150, gnome_launch_later, l);    /* let the workspace switch land first */
}

static gboolean on_gnome_strip_scroll(GtkWidget *w, GdkEventScroll *ev, StartMenuUI *ui) {
    WnckScreen *scr = (ui->shell && ui->shell->tracker) ? ui->shell->tracker->screen : NULL;
    if (!scr) return TRUE;
    static gint64 last_us = 0;
    static gdouble acc = 0;
    gint dir = 0;
    if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_LEFT) dir = -1;
    else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_RIGHT) dir = 1;
    else if (ev->direction == GDK_SCROLL_SMOOTH) {
        acc += (fabs(ev->delta_x) > fabs(ev->delta_y)) ? ev->delta_x : ev->delta_y;
        if (acc <= -1.0) { dir = -1; acc = 0; } else if (acc >= 1.0) { dir = 1; acc = 0; }
    }
    gint64 now = g_get_monotonic_time();
    if (!dir || now - last_us < 220000) return TRUE;   /* one workspace per notch */
    last_us = now;
    WnckWorkspace *cur = wnck_screen_get_active_workspace(scr);
    gint n = cur ? wnck_workspace_get_number(cur) : 0;
    WnckWorkspace *next = wnck_screen_get_workspace(scr, CLAMP(n + dir, 0, wnck_screen_get_workspace_count(scr) - 1));
    if (next && next != cur) {
        gnome_hold_open(ui, 700);
        wnck_workspace_activate(next, ev->time);
    }
    return TRUE;
}

/* the overview follows the desktop while the grid is open */
static guint g_gnome_refresh_id = 0;
static gboolean gnome_refresh_idle(gpointer d) {
    g_gnome_refresh_id = 0;
    StartMenuUI *ui = d;
    if (ui->popup_win && gtk_widget_get_visible(ui->popup_win) && ui->shell &&
        ui->shell->start_style == START_STYLE_GNOME) gnome_overview_refresh(ui);
    return G_SOURCE_REMOVE;
}
static void gnome_queue_refresh(StartMenuUI *ui, guint ms) {
    if (g_gnome_refresh_id) g_source_remove(g_gnome_refresh_id);
    g_gnome_refresh_id = g_timeout_add(ms, gnome_refresh_idle, ui);
}
static void on_gnome_screen_changed(WnckScreen *scr, gpointer arg, StartMenuUI *ui) { gnome_queue_refresh(ui, 60); }
static void on_gnome_window_opened(WnckScreen *scr, WnckWindow *w, StartMenuUI *ui) {
    gnome_queue_refresh(ui, 60);
    gnome_queue_refresh(ui, 600);      /* again once it has its size and place */
}

/* the maps grow / shrink to their new size in 200 ms (GNOME's RESCALE_ANIMATION_TIME) */
typedef struct { StartMenuUI *ui; gint sw, sh; gint64 t0, dur; } GnomeWsAnim;
static guint g_gnome_ws_anim_id = 0;
static gboolean gnome_ws_resize_tick(gpointer d) {
    GnomeWsAnim *a = d;
    gdouble t = MIN(1.0, (gdouble)(g_get_monotonic_time() - a->t0) / MAX(1, a->dur));
    gdouble e = 1.0 - pow(1.0 - t, 3.0);
    if (GTK_IS_BOX(a->ui->gnome_overview)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(a->ui->gnome_overview));
        for (GList *l = kids; l; l = l->next) {
            gint f = GPOINTER_TO_INT(g_object_get_data(l->data, "ws-from-h"));
            gint to = GPOINTER_TO_INT(g_object_get_data(l->data, "ws-to-h"));
            if (!to) continue;
            gint h = (gint)lround(f + (to - f) * e);
            gtk_widget_set_size_request(l->data, h * MAX(1, a->sw) / MAX(1, a->sh), h);
        }
        g_list_free(kids);
    }
    if (t >= 1.0) { g_gnome_ws_anim_id = 0; return G_SOURCE_REMOVE; }
    return G_SOURCE_CONTINUE;
}
static void gnome_ws_resize_start(StartMenuUI *ui, gint sw, gint sh) {
    if (g_gnome_ws_anim_id) g_source_remove(g_gnome_ws_anim_id);
    GnomeWsAnim *a = g_new0(GnomeWsAnim, 1);
    a->ui = ui; a->sw = sw; a->sh = sh;
    a->t0 = g_get_monotonic_time();
    a->dur = (gint64)shell_anim_ms(200) * 1000;
    g_gnome_ws_anim_id = g_timeout_add_full(G_PRIORITY_DEFAULT, 15, gnome_ws_resize_tick, a, g_free);
}

/* Rebuilt every time the grid opens: one small map per workspace */
static void gnome_overview_refresh(StartMenuUI *ui) {
    if (!GTK_IS_BOX(ui->gnome_overview)) return;
    GList *kids = gtk_container_get_children(GTK_CONTAINER(ui->gnome_overview));
    for (GList *l = kids; l; l = l->next) gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);

    WnckScreen *scr = (ui->shell && ui->shell->tracker) ? ui->shell->tracker->screen : NULL;
    if (!scr) return;
    if (!g_object_get_data(G_OBJECT(scr), "ult-gnome-hooked")) {
        g_signal_connect(scr, "window-opened", G_CALLBACK(on_gnome_window_opened), ui);
        g_signal_connect(scr, "window-closed", G_CALLBACK(on_gnome_screen_changed), ui);
        g_signal_connect(scr, "active-workspace-changed", G_CALLBACK(on_gnome_screen_changed), ui);
        g_signal_connect(scr, "workspace-created", G_CALLBACK(on_gnome_screen_changed), ui);
        g_signal_connect(scr, "workspace-destroyed", G_CALLBACK(on_gnome_screen_changed), ui);
        g_object_set_data(G_OBJECT(scr), "ult-gnome-hooked", GINT_TO_POINTER(1));
    }
    gint sw = wnck_screen_get_width(scr), sh = wnck_screen_get_height(scr);
    WnckWorkspace *cur = wnck_screen_get_active_workspace(scr);
    static WnckWorkspace *prev_active = NULL;
    gboolean animate = (prev_active && prev_active != cur && shell_anim_ms(200) > 0);
    for (GList *l = wnck_screen_get_workspaces(scr); l; l = l->next) {
        WnckWorkspace *ws = l->data;
        gint th = (ws == cur) ? GNOME_WS_ACTIVE_H : GNOME_WS_H;
        gint from = animate ? ((ws == prev_active) ? GNOME_WS_ACTIVE_H : GNOME_WS_H) : th;
        GtkWidget *da = gtk_drawing_area_new();
        gtk_widget_set_size_request(da, from * MAX(1, sw) / MAX(1, sh), from);
        g_object_set_data(G_OBJECT(da), "ws-from-h", GINT_TO_POINTER(from));
        g_object_set_data(G_OBJECT(da), "ws-to-h", GINT_TO_POINTER(th));
        gtk_widget_set_valign(da, GTK_ALIGN_CENTER);
        g_object_set_data(G_OBJECT(da), "ws-wall",
                          gnome_ws_wallpaper(wnck_workspace_get_number(ws), th * MAX(1, sw) / MAX(1, sh), th));   /* owned by the cache */
        gtk_widget_set_tooltip_text(da, wnck_workspace_get_name(ws));
        g_object_set_data(G_OBJECT(da), "wnck-ws", ws);
        gtk_widget_add_events(da, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(da, "draw", G_CALLBACK(on_gnome_ws_draw), ui);
        g_signal_connect(da, "button-press-event", G_CALLBACK(on_gnome_ws_press), ui);
        gtk_drag_dest_set(da, GTK_DEST_DEFAULT_DROP, g_app_target, 1, GDK_ACTION_COPY);
        g_signal_connect(da, "drag-motion", G_CALLBACK(on_gnome_ws_drag_motion), NULL);
        g_signal_connect(da, "drag-leave", G_CALLBACK(on_gnome_ws_drag_leave), NULL);
        g_signal_connect(da, "drag-data-received", G_CALLBACK(on_gnome_ws_drag_received), ui);
        gtk_box_pack_start(GTK_BOX(ui->gnome_overview), da, FALSE, FALSE, 0);
    }
    prev_active = cur;
    gtk_widget_show_all(ui->gnome_overview);
    if (animate) gnome_ws_resize_start(ui, sw, sh);
}

static void on_start_popup_show(GtkWidget *win, StartMenuUI *ui) {
    if (ui && ui->shell && ui->shell->start_style == START_STYLE_GNOME) gnome_overview_refresh(ui);
    if (ui && sm_style_fullscreen(ui->shell)) gnome_play_intro(ui);    /* Launchpad + GNOME: icons grow in */
}

void start_menu_rebuild_style(UltimateShell *shell) {
    if (!g_start_ui) return;
    StartMenuUI *ui = g_start_ui;

    {
        /* the popup's child is either the content itself or the edge-handle grid around it */
        GtkWidget *old_child = ui->popup_win ? gtk_bin_get_child(GTK_BIN(ui->popup_win)) : NULL;
        if (old_child) gtk_widget_destroy(old_child);
        ui->root_container = NULL;
    }
    ui->list_box = NULL;
    ui->flow_box = NULL;
    ui->win11_stack = NULL;
    ui->win7_left_stack = NULL;
    ui->win7_toggle_btn = NULL;
    ui->fs_mode_stack = NULL;
    ui->fs_page_stack = NULL;
    ui->fs_dots_da = NULL;
    ui->fs_cat_scroll = NULL;
    ui->search_extras = ui->calc_row = ui->calc_lbl = ui->web_btn = NULL;
    ui->rec_widget = NULL;
    ui->win10_tiles_grid = ui->win10_tiles_pane = ui->win10_list_stack = ui->win10_list_scroll = NULL;
    ui->gnome_overview = ui->gnome_overview_scroll = NULL;
    ui->gnome_peek_prev = ui->gnome_peek_next = NULL;
    if (!g_object_get_data(G_OBJECT(ui->popup_win), "show-hooked")) {
        g_signal_connect(ui->popup_win, "show", G_CALLBACK(on_start_popup_show), ui);
        g_object_set_data(G_OBJECT(ui->popup_win), "show-hooked", GINT_TO_POINTER(1));
    }
    ui->rebuild_pending = FALSE;
    g_free(ui->active_category);
    ui->active_category = g_strdup("All");

    ui->root_container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(ui->root_container), 12);

    ui->search_entry = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui->search_entry), "Type to search applications...");
    g_signal_connect(ui->search_entry, "search-changed", G_CALLBACK(on_search_changed), ui);
    g_signal_connect(ui->search_entry, "stop-search", G_CALLBACK(on_search_entry_stop_search), ui);
    g_signal_connect(ui->search_entry, "activate", G_CALLBACK(on_search_activate), ui);
    g_signal_connect(ui->search_entry, "key-press-event", G_CALLBACK(on_search_key), ui);

    StartMenuStyle style = shell->start_style;
    gint st = get_active_style_idx(shell);
    gint cat_icon_px = (shell->start_style_cat_icon[st] > 0) ? CLAMP(shell->start_style_cat_icon[st], 16, 32) : 18;
    gint win11_cols  = (shell->start_style_grid_cols[START_STYLE_WIN11] > 0) ? CLAMP(shell->start_style_grid_cols[START_STYLE_WIN11], 3, 8) : 6;
    gint fs_st       = (style == START_STYLE_GNOME) ? START_STYLE_GNOME : START_STYLE_FULLSCREEN;
    gint fs_cols     = (shell->start_style_grid_cols[fs_st] > 0) ? CLAMP(shell->start_style_grid_cols[fs_st], 4, 10) : 7;
    gboolean is_gnome = (style == START_STYLE_GNOME);
    gboolean show_user = shell->start_show_user_header;
    StartCatDisplayMode cat_mode = shell->start_style_cat_mode[st];

    g_object_set_data(G_OBJECT(ui->popup_win), "is-fullscreen-start",
                      GINT_TO_POINTER(sm_style_fullscreen(shell) ? 1 : 0));
    if (sm_style_fullscreen(shell)) {
        gtk_style_context_add_class(gtk_widget_get_style_context(ui->popup_win), "fullscreen-launchpad");
    } else {
        gtk_style_context_remove_class(gtk_widget_get_style_context(ui->popup_win), "fullscreen-launchpad");
    }
    if (is_gnome) gtk_style_context_add_class(gtk_widget_get_style_context(ui->popup_win), "gnome-app-grid");
    else gtk_style_context_remove_class(gtk_widget_get_style_context(ui->popup_win), "gnome-app-grid");
    g_object_set_data(G_OBJECT(ui->popup_win), "close-class", sm_style_fullscreen(shell) ? (gpointer)"fs-grid-outro" : NULL);
    if (is_gnome) gtk_window_stick(GTK_WINDOW(ui->popup_win));      /* stays up while switching workspaces */
    else gtk_window_unstick(GTK_WINDOW(ui->popup_win));

    if (style == START_STYLE_WIN10) {
        build_win10_menu(ui);
    } else if (sm_style_fullscreen(shell)) {
        GdkRectangle fs_rect;
        compute_fullscreen_menu_workarea(ui, &fs_rect);
        gtk_window_set_resizable(GTK_WINDOW(ui->popup_win), FALSE);
        gtk_widget_set_size_request(ui->popup_win, fs_rect.width, fs_rect.height);
        gtk_window_resize(GTK_WINDOW(ui->popup_win), fs_rect.width, fs_rect.height);
        gtk_container_set_border_width(GTK_CONTAINER(ui->root_container), 16);

        gboolean search_top = is_gnome ? TRUE : should_place_search_top(shell, TRUE);
        if (is_gnome) gtk_entry_set_placeholder_text(GTK_ENTRY(ui->search_entry), "Type to search");

        GtkWidget *search_center_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_set_size_request(ui->search_entry, 420, 34);
        gtk_widget_set_halign(ui->search_entry, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(search_center_box), ui->search_entry, TRUE, FALSE, 0);

        GtkWidget *cat_outer_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_halign(cat_outer_bar, GTK_ALIGN_CENTER);

        GtkWidget *cat_left_btn = gtk_button_new_from_icon_name("pan-start-symbolic", GTK_ICON_SIZE_MENU);
        GtkWidget *cat_right_btn = gtk_button_new_from_icon_name("pan-end-symbolic", GTK_ICON_SIZE_MENU);
        gtk_button_set_relief(GTK_BUTTON(cat_left_btn), GTK_RELIEF_NONE);
        gtk_button_set_relief(GTK_BUTTON(cat_right_btn), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(cat_left_btn), "ultimate-module-btn");
        gtk_style_context_add_class(gtk_widget_get_style_context(cat_right_btn), "ultimate-module-btn");
        g_signal_connect(cat_left_btn, "clicked", G_CALLBACK(on_fs_cat_left_clicked), ui);
        g_signal_connect(cat_right_btn, "clicked", G_CALLBACK(on_fs_cat_right_clicked), ui);

        ui->fs_cat_scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ui->fs_cat_scroll), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
        gtk_widget_set_size_request(ui->fs_cat_scroll, MIN(fs_rect.width - 160, 960), -1);

        GtkWidget *cat_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_halign(cat_box, GTK_ALIGN_CENTER);
        gtk_container_add(GTK_CONTAINER(ui->fs_cat_scroll), cat_box);

        gtk_box_pack_start(GTK_BOX(cat_outer_bar), cat_left_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(cat_outer_bar), ui->fs_cat_scroll, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(cat_outer_bar), cat_right_btn, FALSE, FALSE, 0);

        ui->fs_page_stack = gtk_stack_new();
        gtk_stack_set_transition_duration(GTK_STACK(ui->fs_page_stack),
                                          (guint)shell_anim_ms(250));   /* GNOME's PAGE_SWITCH_TIME, for Launchpad too */
        build_fullscreen_paged_deck(ui, cat_box, &fs_rect, is_gnome, is_gnome ? 130 : 0);

        GtkWidget *deck_eb = gtk_event_box_new();
        gtk_event_box_set_visible_window(GTK_EVENT_BOX(deck_eb), FALSE);
        gtk_widget_add_events(deck_eb, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK);
        g_signal_connect(deck_eb, "scroll-event", G_CALLBACK(on_fs_deck_scroll), ui);
        g_signal_connect(deck_eb, "button-press-event", G_CALLBACK(on_fs_background_button_press), ui);
        if (is_gnome) {   /* hold a dragged app at a side to turn the page */
            gtk_drag_dest_set(deck_eb, 0, g_app_target, 1, GDK_ACTION_COPY);
            g_signal_connect(deck_eb, "drag-motion", G_CALLBACK(on_deck_drag_motion), ui);
            g_signal_connect(deck_eb, "drag-leave", G_CALLBACK(on_deck_drag_leave), NULL);
            g_signal_connect(deck_eb, "drag-drop", G_CALLBACK(on_deck_drag_drop), NULL);
        }

        GtkWidget *deck_nav_hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        GtkWidget *prev_page_btn = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
        GtkWidget *next_page_btn = gtk_button_new_from_icon_name("go-next-symbolic", GTK_ICON_SIZE_LARGE_TOOLBAR);
        gtk_button_set_relief(GTK_BUTTON(prev_page_btn), GTK_RELIEF_NONE);
        gtk_button_set_relief(GTK_BUTTON(next_page_btn), GTK_RELIEF_NONE);
        gtk_widget_set_valign(prev_page_btn, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(next_page_btn, GTK_ALIGN_CENTER);
        gtk_style_context_add_class(gtk_widget_get_style_context(prev_page_btn), "ultimate-module-btn");
        gtk_style_context_add_class(gtk_widget_get_style_context(next_page_btn), "ultimate-module-btn");
        g_signal_connect(prev_page_btn, "clicked", G_CALLBACK(on_fs_prev_page_clicked), ui);
        g_signal_connect(next_page_btn, "clicked", G_CALLBACK(on_fs_next_page_clicked), ui);

        if (!is_gnome) gtk_box_pack_start(GTK_BOX(deck_nav_hbox), prev_page_btn, FALSE, FALSE, 4);
        if (is_gnome) {   /* the neighbouring pages peek in at the sides (click one to go there) */
            ui->gnome_peek_prev = gnome_make_peek(ui, -1);
            gtk_box_pack_start(GTK_BOX(deck_nav_hbox), ui->gnome_peek_prev, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(deck_nav_hbox), ui->fs_page_stack, TRUE, TRUE, 0);
        if (!is_gnome) gtk_box_pack_end(GTK_BOX(deck_nav_hbox), next_page_btn, FALSE, FALSE, 4);
        else {
            gtk_widget_destroy(prev_page_btn); gtk_widget_destroy(next_page_btn);   /* GNOME: no arrows */
            ui->gnome_peek_next = gnome_make_peek(ui, 1);
            gtk_box_pack_end(GTK_BOX(deck_nav_hbox), ui->gnome_peek_next, FALSE, FALSE, 0);
            gnome_update_peeks(ui);
        }
        gtk_container_add(GTK_CONTAINER(deck_eb), deck_nav_hbox);

        GtkWidget *search_scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(search_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        ui->flow_box = gtk_flow_box_new();
        gtk_widget_set_halign(ui->flow_box, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(ui->flow_box, GTK_ALIGN_START);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ui->flow_box), fs_cols);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ui->flow_box), fs_cols);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(ui->flow_box), 14);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(ui->flow_box), 10);
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ui->flow_box), GTK_SELECTION_SINGLE);
        gtk_flow_box_set_filter_func(GTK_FLOW_BOX(ui->flow_box), filter_flow_func, ui, NULL);
        g_signal_connect(ui->flow_box, "child-activated", G_CALLBACK(on_flow_child_activated), ui);
        populate_win11_grid(ui, ui->flow_box, FALSE);
        gtk_container_add(GTK_CONTAINER(search_scroll), ui->flow_box);

        ui->fs_mode_stack = gtk_stack_new();
        gtk_stack_add_named(GTK_STACK(ui->fs_mode_stack), deck_eb, "deck");
        gtk_stack_add_named(GTK_STACK(ui->fs_mode_stack), search_scroll, "search");

        ui->fs_dots_da = gtk_drawing_area_new();
        gtk_widget_set_size_request(ui->fs_dots_da, 260, 26);
        gtk_widget_set_halign(ui->fs_dots_da, GTK_ALIGN_CENTER);
        gtk_widget_add_events(ui->fs_dots_da, GDK_BUTTON_PRESS_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
        g_signal_connect(ui->fs_dots_da, "draw", G_CALLBACK(on_draw_fs_dots), ui);
        g_signal_connect(ui->fs_dots_da, "button-press-event", G_CALLBACK(on_press_fs_dots), ui);
        g_signal_connect(ui->fs_dots_da, "scroll-event", G_CALLBACK(on_fs_deck_scroll), ui);

        GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        if (show_user) {
            gtk_box_pack_start(GTK_BOX(bottom_bar), create_user_avatar_chip(34), FALSE, FALSE, 4);
        }
        gtk_box_set_center_widget(GTK_BOX(bottom_bar), ui->fs_dots_da);
        gtk_box_pack_end(GTK_BOX(bottom_bar), create_action_buttons_box(ui, FALSE), FALSE, FALSE, 0);

        if (search_top) {
            gtk_box_pack_start(GTK_BOX(ui->root_container), search_center_box, FALSE, FALSE, 0);
        }
        if (is_gnome) {
            /* open windows of this workspace, like the Activities overview */
            ui->gnome_overview_scroll = gtk_scrolled_window_new(NULL, NULL);
            gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(ui->gnome_overview_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
            gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(ui->gnome_overview_scroll), TRUE);
            gtk_widget_set_halign(ui->gnome_overview_scroll, GTK_ALIGN_CENTER);
            ui->gnome_overview = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
            gtk_widget_set_halign(ui->gnome_overview, GTK_ALIGN_CENTER);
            gtk_container_add(GTK_CONTAINER(ui->gnome_overview_scroll), ui->gnome_overview);
            gtk_widget_set_size_request(ui->gnome_overview_scroll, -1, gnome_ws_h(TRUE) + 8);   /* fixed: the grid stays put */
            gtk_widget_add_events(ui->gnome_overview_scroll, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);
            g_signal_connect(ui->gnome_overview_scroll, "scroll-event", G_CALLBACK(on_gnome_strip_scroll), ui);
            gtk_box_pack_start(GTK_BOX(ui->root_container), ui->gnome_overview_scroll, FALSE, FALSE, 6);
            gnome_overview_refresh(ui);
        }
        /* GNOME has no category bar: kept (the page logic uses its buttons) but never shown */
        gtk_box_pack_start(GTK_BOX(ui->root_container), cat_outer_bar, FALSE, FALSE, 0);
        if (is_gnome) gtk_widget_set_no_show_all(cat_outer_bar, TRUE);
        ui->rec_widget = is_gnome ? NULL : build_recommended_section(ui, 4);
        if (ui->rec_widget) {
            gtk_widget_set_halign(ui->rec_widget, GTK_ALIGN_CENTER);
            gtk_widget_set_size_request(ui->rec_widget, MIN(fs_rect.width - 200, 900), -1);
            gtk_box_pack_start(GTK_BOX(ui->root_container), ui->rec_widget, FALSE, FALSE, 4);
        }
        gtk_box_pack_start(GTK_BOX(ui->root_container), ui->fs_mode_stack, TRUE, TRUE, 2);
        if (!search_top) {
            gtk_box_pack_start(GTK_BOX(ui->root_container), search_center_box, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(ui->root_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(ui->root_container), bottom_bar, FALSE, FALSE, 0);

        fs_sync_active_category_pill(ui);

    } else if (style == START_STYLE_WIN11) {
        gint init_w = 580, init_h = 560;
        get_saved_or_default_style_size(shell, START_STYLE_WIN11, &init_w, &init_h);
        gtk_window_set_resizable(GTK_WINDOW(ui->popup_win), TRUE);
        gtk_widget_set_size_request(ui->popup_win, init_w, init_h);
        gtk_window_resize(GTK_WINDOW(ui->popup_win), init_w, init_h);
        gtk_container_set_border_width(GTK_CONTAINER(ui->root_container), 0);
        gtk_box_set_spacing(GTK_BOX(ui->root_container), 0);

        gboolean search_top = should_place_search_top(shell, TRUE);

        /* Main Padded Upper Content Area (Search + Header + Pinned Grid / All Apps) */
        GtkWidget *upper_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_start(upper_vbox, 26);
        gtk_widget_set_margin_end(upper_vbox, 26);
        gtk_widget_set_margin_top(upper_vbox, 20);
        gtk_widget_set_margin_bottom(upper_vbox, 12);

        gtk_entry_set_placeholder_text(GTK_ENTRY(ui->search_entry), "Search for apps, settings, and documents");
        gtk_style_context_add_class(gtk_widget_get_style_context(ui->search_entry), "win11-search-pill");
        gtk_widget_set_size_request(ui->search_entry, -1, 38);

        /* Authentic Windows 11 Header: Bold "Pinned" Title Left + "All apps ›" Pill Button Right */
        GtkWidget *tab_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_margin_start(tab_header, 8);
        gtk_widget_set_margin_end(tab_header, 8);
        gtk_widget_set_margin_top(tab_header, 4);

        ui->win11_pinned_btn = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(ui->win11_pinned_btn), "<b>Pinned</b>");
        gtk_label_set_xalign(GTK_LABEL(ui->win11_pinned_btn), 0.0);

        ui->win11_all_btn = gtk_button_new_with_label("All apps  ›");
        gtk_button_set_relief(GTK_BUTTON(ui->win11_all_btn), GTK_RELIEF_NONE);
        gtk_style_context_add_class(gtk_widget_get_style_context(ui->win11_all_btn), "win11-all-apps-pill");
        g_signal_connect(ui->win11_all_btn, "clicked", G_CALLBACK(on_win11_toggle_view_clicked), ui);

        gtk_box_pack_start(GTK_BOX(tab_header), ui->win11_pinned_btn, FALSE, FALSE, 0);
        gtk_box_pack_end(GTK_BOX(tab_header), ui->win11_all_btn, FALSE, FALSE, 0);

        ui->win11_stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(ui->win11_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

        GtkWidget *scroll_grid = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_grid), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        ui->flow_box = gtk_flow_box_new();
        gtk_widget_set_valign(ui->flow_box, GTK_ALIGN_START);
        gtk_widget_set_halign(ui->flow_box, GTK_ALIGN_CENTER);
        gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(ui->flow_box), TRUE);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ui->flow_box), win11_cols);
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ui->flow_box), win11_cols);
        gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(ui->flow_box), 0);
        gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(ui->flow_box), 0);
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ui->flow_box), GTK_SELECTION_SINGLE);
        gtk_flow_box_set_filter_func(GTK_FLOW_BOX(ui->flow_box), filter_flow_func, ui, NULL);
        g_signal_connect(ui->flow_box, "child-activated", G_CALLBACK(on_flow_child_activated), ui);
        populate_win11_grid(ui, ui->flow_box, TRUE);
        g_object_set_data(G_OBJECT(ui->popup_win), "win11-pinned-flow", ui->flow_box);
        g_object_set_data(G_OBJECT(ui->flow_box), "fav-order", GINT_TO_POINTER(1));
        ensure_search_sort(ui, ui->flow_box);
        fav_area_setup(ui, ui->flow_box);
        gtk_container_add(GTK_CONTAINER(scroll_grid), ui->flow_box);

        GtkWidget *scroll_list = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_list), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        ui->list_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list_box), GTK_SELECTION_NONE);
        gtk_list_box_set_filter_func(GTK_LIST_BOX(ui->list_box), filter_list_func, ui, NULL);
        g_signal_connect(ui->list_box, "row-activated", G_CALLBACK(on_app_row_activated), ui);
        populate_app_list(ui, ui->list_box, TRUE);
        gtk_container_add(GTK_CONTAINER(scroll_list), ui->list_box);

        GtkWidget *pinned_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        gtk_box_pack_start(GTK_BOX(pinned_page), scroll_grid, TRUE, TRUE, 0);
        GtkWidget *rec11 = build_recommended_section(ui, 2);
        if (rec11) gtk_box_pack_start(GTK_BOX(pinned_page), rec11, FALSE, FALSE, 0);
        gtk_stack_add_named(GTK_STACK(ui->win11_stack), pinned_page, "pinned");
        gtk_stack_add_named(GTK_STACK(ui->win11_stack), scroll_list, "all");

        if (search_top) {
            gtk_box_pack_start(GTK_BOX(upper_vbox), ui->search_entry, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(upper_vbox), tab_header, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(upper_vbox), ui->win11_stack, TRUE, TRUE, 0);
        if (!search_top) {
            gtk_box_pack_start(GTK_BOX(upper_vbox), ui->search_entry, FALSE, FALSE, 0);
        }

        /* Authentic Windows 11 Full-Width Dark Tinted Footer Strip */
        GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_style_context_add_class(gtk_widget_get_style_context(bottom_bar), "win11-footer-bar");
        if (show_user) {
            gtk_box_pack_start(GTK_BOX(bottom_bar), create_user_avatar_chip(32), FALSE, FALSE, 8);
        }
        gtk_box_pack_end(GTK_BOX(bottom_bar), create_action_buttons_box(ui, TRUE), FALSE, FALSE, 8);

        gtk_box_pack_start(GTK_BOX(ui->root_container), upper_vbox, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(ui->root_container), bottom_bar, FALSE, FALSE, 0);

    } else if (style == START_STYLE_WIN7) {
        gint init_w = 480, init_h = 490;
        get_saved_or_default_style_size(shell, START_STYLE_WIN7, &init_w, &init_h);
        gtk_window_set_resizable(GTK_WINDOW(ui->popup_win), TRUE);
        gtk_widget_set_size_request(ui->popup_win, init_w, init_h);
        gtk_window_resize(GTK_WINDOW(ui->popup_win), init_w, init_h);

        gboolean search_top = should_place_search_top(shell, FALSE);

        GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);

        /* Item 2: Left Column VBox with Categories Default Stack + Bottom "All Programs ▸" Button */
        GtkWidget *left_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        ui->win7_left_stack = gtk_stack_new();
        gtk_stack_set_transition_type(GTK_STACK(ui->win7_left_stack), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

        GtkWidget *scroll_cats = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_cats), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        GtkWidget *win7_cats_lb = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(win7_cats_lb), GTK_SELECTION_NONE);
        g_signal_connect(win7_cats_lb, "row-activated", G_CALLBACK(on_app_row_activated), ui);
        populate_win7_categories_default_list(ui, win7_cats_lb);
        gtk_container_add(GTK_CONTAINER(scroll_cats), win7_cats_lb);

        GtkWidget *scroll_apps = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll_apps), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        ui->list_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list_box), GTK_SELECTION_NONE);
        gtk_list_box_set_filter_func(GTK_LIST_BOX(ui->list_box), filter_list_func, ui, NULL);
        g_signal_connect(ui->list_box, "row-activated", G_CALLBACK(on_app_row_activated), ui);
        populate_app_list(ui, ui->list_box, FALSE);
        gtk_container_add(GTK_CONTAINER(scroll_apps), ui->list_box);

        gtk_stack_add_named(GTK_STACK(ui->win7_left_stack), scroll_cats, "categories");
        gtk_stack_add_named(GTK_STACK(ui->win7_left_stack), scroll_apps, "apps");

        ui->win7_toggle_btn = gtk_button_new_with_label("All Programs ▸");
        gtk_style_context_add_class(gtk_widget_get_style_context(ui->win7_toggle_btn), "ultimate-module-btn");
        g_signal_connect(ui->win7_toggle_btn, "clicked", G_CALLBACK(on_win7_all_programs_toggled), ui);

        gtk_box_pack_start(GTK_BOX(left_vbox), ui->win7_left_stack, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(left_vbox), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(left_vbox), ui->win7_toggle_btn, FALSE, FALSE, 0);

        GtkWidget *right_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_size_request(right_pane, (cat_mode == START_CAT_DISPLAY_ICON_ONLY) ? (cat_icon_px + 24) : 165, -1);

        if (show_user) {
            GtkWidget *user_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
            gtk_widget_set_margin_top(user_vbox, 4);
            gtk_widget_set_margin_bottom(user_vbox, 6);
            GtkWidget *circle_avatar = create_circular_avatar_widget(54);
            gtk_box_pack_start(GTK_BOX(user_vbox), circle_avatar, FALSE, FALSE, 0);
            if (cat_mode != START_CAT_DISPLAY_ICON_ONLY) {
                gchar *u_mk = g_strdup_printf("<b>%s</b>", g_get_user_name());
                GtkWidget *u_lbl = gtk_label_new(NULL);
                gtk_label_set_markup(GTK_LABEL(u_lbl), u_mk);
                gtk_label_set_justify(GTK_LABEL(u_lbl), GTK_JUSTIFY_CENTER);
                g_free(u_mk);
                gtk_box_pack_start(GTK_BOX(user_vbox), u_lbl, FALSE, FALSE, 0);
            }
            gtk_box_pack_start(GTK_BOX(right_pane), user_vbox, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(right_pane), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
        }

        /* Item 3: Core + Optional Extra Shortcuts in Windows 7 Right Pane */
        const struct { const char *icon; const char *label; const char *place_id; gboolean enabled; } places[] = {
            { "user-home",            "Home",          "home",     TRUE },
            { "folder-documents",     "Documents",     "docs",     TRUE },
            { "folder-download",      "Downloads",     "dl",       TRUE },
            { "folder-pictures",      "Pictures",      "pic",      shell->win7_show_pictures },
            { "folder-music",         "Music",         "mus",      shell->win7_show_music },
            { "folder-videos",        "Videos",        "vid",      shell->win7_show_videos },
            { "user-trash",           "Trash",         "trash",    shell->win7_show_trash },
            { "utilities-terminal",   "Terminal",      "term",     shell->win7_show_terminal },
            { "preferences-system",   "Control Panel", "settings", TRUE },
            { "system-run",           "Run...",        "run",      TRUE }
        };
        gint w7_cat_sp = (shell->start_style_cat_spacing[st] >= 0)
                         ? CLAMP(shell->start_style_cat_spacing[st], 0, 16) : 5;
        gtk_box_set_spacing(GTK_BOX(right_pane), w7_cat_sp);

        for (guint i = 0; i < G_N_ELEMENTS(places); i++) {
            if (!places[i].enabled) continue;
            GtkWidget *pbtn = gtk_button_new();
            gtk_button_set_relief(GTK_BUTTON(pbtn), GTK_RELIEF_NONE);
            gtk_style_context_add_class(gtk_widget_get_style_context(pbtn), "start-category-btn");
            gtk_widget_set_tooltip_text(pbtn, places[i].label);

            GtkWidget *phbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top(phbox, 3 + (w7_cat_sp / 3));
            gtk_widget_set_margin_bottom(phbox, 3 + (w7_cat_sp / 3));
            gtk_widget_set_margin_start(phbox, 4);
            gtk_widget_set_margin_end(phbox, 4);
            if (cat_mode != START_CAT_DISPLAY_LABEL_ONLY) {
                GtkWidget *pimg = gtk_image_new_from_icon_name(places[i].icon, GTK_ICON_SIZE_MENU);
                gtk_image_set_pixel_size(GTK_IMAGE(pimg), cat_icon_px);
                gtk_box_pack_start(GTK_BOX(phbox), pimg, (cat_mode == START_CAT_DISPLAY_ICON_ONLY), FALSE, 2);
            }
            if (cat_mode != START_CAT_DISPLAY_ICON_ONLY) {
                gint cat_pt = (shell->start_style_cat_pt[st] > 0) ? shell->start_style_cat_pt[st] : 11;
                gint place_wt = (shell->win7_places_weight > 0) ? shell->win7_places_weight : 600;
                GtkWidget *plbl = gtk_label_new(places[i].label);
                apply_label_font_size_and_weight(plbl, cat_pt, place_wt);
                gtk_label_set_xalign(GTK_LABEL(plbl), 0.0);
                gtk_widget_set_halign(plbl, GTK_ALIGN_START);
                gtk_box_pack_start(GTK_BOX(phbox), plbl, TRUE, TRUE, 2);
            }
            gtk_container_add(GTK_CONTAINER(pbtn), phbox);

            g_signal_connect(pbtn, "clicked", G_CALLBACK(on_place_shortcut_clicked), (gpointer)places[i].place_id);
            gtk_box_pack_start(GTK_BOX(right_pane), pbtn, FALSE, FALSE, 0);
        }

        gtk_box_pack_start(GTK_BOX(columns), left_vbox, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(columns), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(columns), right_pane, FALSE, FALSE, 0);

        GtkWidget *bottom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        if (!search_top) {
            gtk_box_pack_start(GTK_BOX(bottom_row), ui->search_entry, TRUE, TRUE, 0);
        }
        gtk_box_pack_end(GTK_BOX(bottom_row), classic_power_button(ui), FALSE, FALSE, 0);   /* like Windows 7 */

        if (search_top) {
            gtk_box_pack_start(GTK_BOX(ui->root_container), ui->search_entry, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(ui->root_container), columns, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(ui->root_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(ui->root_container), bottom_row, FALSE, FALSE, 0);

    } else if (style == START_STYLE_WHISKER) {
        gint init_w = 540, init_h = 500;
        get_saved_or_default_style_size(shell, START_STYLE_WHISKER, &init_w, &init_h);
        gtk_window_set_resizable(GTK_WINDOW(ui->popup_win), TRUE);
        gtk_widget_set_size_request(ui->popup_win, init_w, init_h);
        gtk_window_resize(GTK_WINDOW(ui->popup_win), init_w, init_h);

        gboolean search_top = should_place_search_top(shell, FALSE);

        if (show_user) {
            GtkWidget *top_banner = create_user_avatar_chip(38);
            gtk_box_pack_start(GTK_BOX(ui->root_container), top_banner, FALSE, FALSE, 0);
            gtk_box_pack_start(GTK_BOX(ui->root_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
        }

        if (search_top) {
            gtk_box_pack_start(GTK_BOX(ui->root_container), ui->search_entry, FALSE, FALSE, 0);
        }

        GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *cat_scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(cat_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_size_request(cat_scroll, (cat_mode == START_CAT_DISPLAY_ICON_ONLY) ? (cat_icon_px + 28) : 170, -1);

        gint whisk_cat_sp = (shell->start_style_cat_spacing[st] >= 0)
                            ? CLAMP(shell->start_style_cat_spacing[st], 0, 16) : 4;
        GtkWidget *cat_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, whisk_cat_sp);
        gtk_container_add(GTK_CONTAINER(cat_scroll), cat_box);

        GList *whisk_apps = get_sorted_apps_cached(ui);
        for (guint i = 0; i < G_N_ELEMENTS(g_category_specs); i++) {
            const CategorySpec *spec = &g_category_specs[i];
            /* Prune empty categories in Whisker Menu (always keep Favorites=0 and All=1) */
            if (i > 1) {
                gboolean has_any = FALSE;
                for (GList *l = whisk_apps; l != NULL; l = l->next) {
                    GAppInfo *app = G_APP_INFO(l->data);
                    if (g_app_info_should_show(app) && app_matches_category_filter(shell, app, spec->filter)) {
                        has_any = TRUE;
                        break;
                    }
                }
                if (!has_any) continue;
            }

            GtkWidget *cbtn = gtk_button_new();
            gtk_button_set_relief(GTK_BUTTON(cbtn), GTK_RELIEF_NONE);
            gtk_style_context_add_class(gtk_widget_get_style_context(cbtn), "start-category-btn");
            gtk_widget_set_tooltip_text(cbtn, spec->title);

            GtkWidget *chbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_margin_top(chbox, 4 + (whisk_cat_sp / 3));
            gtk_widget_set_margin_bottom(chbox, 4 + (whisk_cat_sp / 3));
            gtk_widget_set_margin_start(chbox, 4);
            gtk_widget_set_margin_end(chbox, 4);
            if (cat_mode != START_CAT_DISPLAY_LABEL_ONLY) {
                GtkWidget *cimg = gtk_image_new_from_icon_name(g_category_specs[i].icon, GTK_ICON_SIZE_MENU);
                gtk_image_set_pixel_size(GTK_IMAGE(cimg), cat_icon_px);
                gtk_box_pack_start(GTK_BOX(chbox), cimg, (cat_mode == START_CAT_DISPLAY_ICON_ONLY), FALSE, 2);
            }
            if (cat_mode != START_CAT_DISPLAY_ICON_ONLY) {
                gint cat_pt = (shell->start_style_cat_pt[st] > 0) ? shell->start_style_cat_pt[st] : 11;
                gint cat_wt = shell->start_style_cat_weight[st];
                GtkWidget *clbl = gtk_label_new(g_category_specs[i].title);
                apply_label_font_size_and_weight(clbl, cat_pt, cat_wt);
                gtk_widget_set_halign(clbl, GTK_ALIGN_START);
                gtk_label_set_xalign(GTK_LABEL(clbl), 0.0);
                gtk_label_set_ellipsize(GTK_LABEL(clbl), PANGO_ELLIPSIZE_END);
                gtk_box_pack_start(GTK_BOX(chbox), clbl, TRUE, TRUE, 2);
            }
            gtk_container_add(GTK_CONTAINER(cbtn), chbox);

            g_object_set_data(G_OBJECT(cbtn), "cat-filter", (gpointer)g_category_specs[i].filter);
            gtk_widget_add_events(cbtn, GDK_ENTER_NOTIFY_MASK);
            g_signal_connect(cbtn, "clicked", G_CALLBACK(on_category_clicked), ui);
            g_signal_connect(cbtn, "enter-notify-event", G_CALLBACK(on_category_hover_enter), ui);
            gtk_box_pack_start(GTK_BOX(cat_box), cbtn, FALSE, FALSE, 0);
        }

        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        GtkWidget *right_pane = scroll;
        if (shell->start_whisker_grid) {
            /* "Show applications as icons" (like Whisker Menu's own option) */
            gint wcols = (shell->start_style_grid_cols[START_STYLE_WHISKER] > 0)
                         ? CLAMP(shell->start_style_grid_cols[START_STYLE_WHISKER], 2, 8) : 4;
            ui->flow_box = gtk_flow_box_new();
            gtk_widget_set_valign(ui->flow_box, GTK_ALIGN_START);
            gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(ui->flow_box), TRUE);
            gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ui->flow_box), 2);
            gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ui->flow_box), wcols);
            gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ui->flow_box), GTK_SELECTION_SINGLE);
            gtk_flow_box_set_filter_func(GTK_FLOW_BOX(ui->flow_box), filter_flow_func, ui, NULL);
            g_signal_connect(ui->flow_box, "child-activated", G_CALLBACK(on_flow_child_activated), ui);
            populate_win11_grid(ui, ui->flow_box, FALSE);
            gtk_container_add(GTK_CONTAINER(scroll), ui->flow_box);
            g_object_set_data(G_OBJECT(ui->flow_box), "fav-order-cat", GINT_TO_POINTER(1));
            ensure_search_sort(ui, ui->flow_box);
            fav_area_setup(ui, ui->flow_box);
            {
                GList *kids = gtk_container_get_children(GTK_CONTAINER(ui->flow_box));
                for (GList *l = kids; l; l = l->next) {
                    GAppInfo *ka = g_object_get_data(G_OBJECT(l->data), "app-info");
                    if (ka && is_app_in_favorites(shell, ka))
                        fav_source_setup(gtk_bin_get_child(GTK_BIN(l->data)), ka, ui->flow_box);
                }
                g_list_free(kids);
            }
            GtkWidget *rec = build_recommended_section(ui, 2);
            if (rec) {
                right_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
                ui->rec_widget = rec;
                gtk_box_pack_start(GTK_BOX(right_pane), rec, FALSE, FALSE, 0);
                gtk_box_pack_start(GTK_BOX(right_pane), scroll, TRUE, TRUE, 0);
            }
        } else {
            ui->list_box = gtk_list_box_new();
            gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list_box), GTK_SELECTION_NONE);
            gtk_list_box_set_filter_func(GTK_LIST_BOX(ui->list_box), filter_list_func, ui, NULL);
            g_signal_connect(ui->list_box, "row-activated", G_CALLBACK(on_app_row_activated), ui);
            populate_app_list(ui, ui->list_box, FALSE);
            gtk_container_add(GTK_CONTAINER(scroll), ui->list_box);
            /* Favourites: your order, drag a row to move it (with a live preview) */
            g_object_set_data(G_OBJECT(ui->list_box), "fav-order-cat", GINT_TO_POINTER(1));
            ensure_search_sort(ui, ui->list_box);
            fav_area_setup(ui, ui->list_box);
            {
                GList *rows = gtk_container_get_children(GTK_CONTAINER(ui->list_box));
                for (GList *l = rows; l; l = l->next) {
                    GAppInfo *ra = g_object_get_data(G_OBJECT(l->data), "app-info");
                    if (ra && is_app_in_favorites(shell, ra))
                        fav_source_setup(gtk_bin_get_child(GTK_BIN(l->data)), ra, ui->list_box);
                }
                g_list_free(rows);
            }
        }

        gtk_box_pack_start(GTK_BOX(columns), cat_scroll, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(columns), gtk_separator_new(GTK_ORIENTATION_VERTICAL), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(columns), right_pane, TRUE, TRUE, 0);

        gtk_box_pack_start(GTK_BOX(ui->root_container), columns, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(ui->root_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 4);

        GtkWidget *bottom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_box_pack_start(GTK_BOX(bottom_row), create_action_buttons_box(ui, FALSE), FALSE, FALSE, 0);
        if (!search_top) {
            gtk_box_pack_end(GTK_BOX(bottom_row), ui->search_entry, TRUE, TRUE, 0);
        }
        gtk_box_pack_start(GTK_BOX(ui->root_container), bottom_row, FALSE, FALSE, 0);

    } else {
        gint init_w = 350, init_h = 470;
        get_saved_or_default_style_size(shell, START_STYLE_DEFAULT, &init_w, &init_h);
        gtk_window_set_resizable(GTK_WINDOW(ui->popup_win), TRUE);
        gtk_widget_set_size_request(ui->popup_win, init_w, init_h);
        gtk_window_resize(GTK_WINDOW(ui->popup_win), init_w, init_h);

        gboolean search_top = should_place_search_top(shell, TRUE);

        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        ui->list_box = gtk_list_box_new();
        gtk_list_box_set_selection_mode(GTK_LIST_BOX(ui->list_box), GTK_SELECTION_NONE);
        gtk_list_box_set_filter_func(GTK_LIST_BOX(ui->list_box), filter_list_func, ui, NULL);
        g_signal_connect(ui->list_box, "row-activated", G_CALLBACK(on_app_row_activated), ui);
        populate_app_list(ui, ui->list_box, FALSE);
        gtk_container_add(GTK_CONTAINER(scroll), ui->list_box);

        GtkWidget *bottom_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        if (show_user) {
            gtk_box_pack_start(GTK_BOX(bottom_bar), create_user_avatar_chip(30), FALSE, FALSE, 2);
        }
        gtk_box_pack_end(GTK_BOX(bottom_bar), create_action_buttons_box(ui, TRUE), FALSE, FALSE, 0);

        if (search_top) {
            gtk_box_pack_start(GTK_BOX(ui->root_container), ui->search_entry, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(ui->root_container), scroll, TRUE, TRUE, 0);
        if (!search_top) {
            gtk_box_pack_start(GTK_BOX(ui->root_container), ui->search_entry, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(ui->root_container), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
        gtk_box_pack_start(GTK_BOX(ui->root_container), bottom_bar, FALSE, FALSE, 0);
    }

    if (sm_style_fullscreen(shell)) {
        gtk_container_add(GTK_CONTAINER(ui->popup_win), ui->root_container);   /* fills the screen: no edges */
    } else {
        gtk_container_add(GTK_CONTAINER(ui->popup_win), wrap_with_edge_handles(ui, ui->root_container));
    }
    {
        static GtkCssProvider *smcss = NULL;
        if (!smcss) {
            smcss = gtk_css_provider_new();
            gtk_css_provider_load_from_data(smcss,
                ".start-alpha-header { color: @theme_selected_bg_color; }"
                ".start-rec-item { padding: 6px 8px; border-radius: 8px; }"
                ".start-rec-sub { font-size: smaller; }"
                ".start-search-extras { padding: 2px 6px; }"
                ".win10-tile { background-color: alpha(@theme_fg_color, 0.10); border: 1px solid transparent; }"
                ".win10-tile:hover { background-color: alpha(@theme_fg_color, 0.20); border-color: alpha(@theme_fg_color, 0.60); }"
                ".win10-tile:active { background-color: alpha(@theme_fg_color, 0.08); }"
                ".win10-tile-label { color: @theme_fg_color; font-size: smaller; }"
                ".win10-rail { background-color: alpha(@theme_fg_color, 0.05); border-radius: 6px; }"
                ".win10-rail-btn { min-width: 0; padding: 4px; border-radius: 4px; }"
                ".win10-letter { font-size: large; font-weight: bold; border-radius: 4px; }"
                ".win10-group-hdr { padding: 2px 4px; border-radius: 4px; }"
                /* keyboard focus: the selected app is clearly marked in every style */
                ".ultimate-popup-win list row:focus, .ultimate-popup-win list row.app-grid-tile:focus {"
                "  background-color: alpha(@theme_selected_bg_color, 0.30); border-color: @theme_selected_bg_color; }"
                ".ultimate-popup-win flowboxchild:focus .app-grid-tile {"
                "  background-color: alpha(@theme_selected_bg_color, 0.30); border-color: @theme_selected_bg_color; }"
                /* GNOME App Grid opening: icons start a bit small and faint, then settle in */
                ".fullscreen-launchpad .app-grid-tile image { transition: -gtk-icon-transform 300ms ease-out, opacity 300ms ease-out; }"
                ".fullscreen-launchpad .app-grid-tile label { transition: opacity 300ms ease-out; }"
                ".fullscreen-launchpad.fs-grid-intro .app-grid-tile image { -gtk-icon-transform: scale(0.80); opacity: 0; }"
                ".fullscreen-launchpad.fs-grid-intro .app-grid-tile label { opacity: 0; }"
                /* closing: the reverse, while the screen fades out */
                ".fullscreen-launchpad.fs-grid-outro .app-grid-tile image { -gtk-icon-transform: scale(0.88); opacity: 0;"
                "  transition: -gtk-icon-transform 160ms ease-in, opacity 160ms ease-in; }"
                ".fullscreen-launchpad.fs-grid-outro .app-grid-tile label { opacity: 0; transition: opacity 160ms ease-in; }"

                ".start-new-badge { font-size: smaller; font-weight: bold; padding: 1px 7px; border-radius: 99px;"
                "  color: @theme_selected_fg_color; background-color: @theme_selected_bg_color; }", -1, NULL);
            gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(smcss),
                                                      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }
    }
    {   /* GNOME App Grid: square full-screen sheet (above the shell's corner-radius setting) */
        static GtkCssProvider *gcss = NULL;
        if (!gcss) {
            gcss = gtk_css_provider_new();
            gtk_css_provider_load_from_data(gcss,
                ".ultimate-popup-win.gnome-app-grid, window.ultimate-popup-win.gnome-app-grid.background,"
                " .gnome-app-grid decoration { border-radius: 0; border: none; box-shadow: none; }", -1, NULL);
            gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(gcss),
                                                      GTK_STYLE_PROVIDER_PRIORITY_USER + 130);
        }
    }
    gtk_widget_show_all(gtk_bin_get_child(GTK_BIN(ui->popup_win)));
    attach_search_extras(ui);
}

static gboolean on_draw_apple_logo(GtkWidget *widget, cairo_t *cr, gpointer data) {
    GtkStyleContext *ctx = gtk_widget_get_style_context(widget);
    GdkRGBA fg;
    gtk_style_context_get_color(ctx, gtk_widget_get_state_flags(widget), &fg);

    gint w = gtk_widget_get_allocated_width(widget);
    gint h = gtk_widget_get_allocated_height(widget);
    gdouble scale = MIN(w, h) / 18.0;

    cairo_save(cr);
    cairo_translate(cr, w / 2.0, h / 2.0);
    cairo_scale(cr, scale, scale);
    gdouble cx = 0.0;
    gdouble cy = 1.0;
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);

    /* Classic Apple silhouette: left & right lobes */
    cairo_arc(cr, cx - 2.6, cy + 0.2, 4.4, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_arc(cr, cx + 2.6, cy + 0.2, 4.4, 0, 2 * G_PI);
    cairo_fill(cr);

    /* Bottom taper */
    cairo_arc(cr, cx - 1.8, cy + 3.0, 3.3, 0, 2 * G_PI);
    cairo_fill(cr);
    cairo_arc(cr, cx + 1.8, cy + 3.0, 3.3, 0, 2 * G_PI);
    cairo_fill(cr);

    /* Right bite cutout */
    cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
    cairo_arc(cr, cx + 6.7, cy - 0.3, 2.3, 0, 2 * G_PI);
    cairo_fill(cr);

    /* Top leaf */
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
    cairo_set_source_rgba(cr, fg.red, fg.green, fg.blue, fg.alpha);
    cairo_translate(cr, cx + 0.8, cy - 5.8);
    cairo_rotate(cr, -0.55);
    cairo_scale(cr, 1.8, 0.9);
    cairo_arc(cr, 0, 0, 1.4, 0, 2 * G_PI);
    cairo_fill(cr);

    cairo_restore(cr);
    return FALSE;
}

static void on_apple_menu_cmd_activate(GtkMenuItem *item, const gchar *cmd) {
    if (cmd && cmd[0] != '\0') {
        spawn_command_with_live_parent(cmd);
    }
}

static void on_apple_menu_launchpad_activate(GtkMenuItem *item, UltimateShell *shell) {
    start_menu_open_from_dock(shell, NULL);
}

static void on_apple_menu_power_activate(GtkMenuItem *item, const gchar *action_type) {
    on_confirm_power_action(NULL, action_type);
}

/* =========================================================================
 * System menu helpers (macOS layout, top-left logo)
 * ========================================================================= */

/* ---- About This Computer ---- */
static gchar *sysinfo_read_first_line(const gchar *path) {
    gchar *buf = NULL;
    if (!g_file_get_contents(path, &buf, NULL, NULL) || !buf) return NULL;
    gchar *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    g_strstrip(buf);
    return buf;
}

static gchar *sysinfo_os_release(const gchar *key) {
    gchar *buf = NULL;
    if (!g_file_get_contents("/etc/os-release", &buf, NULL, NULL) &&
        !g_file_get_contents("/usr/lib/os-release", &buf, NULL, NULL)) return NULL;
    gchar *val = NULL;
    gchar **lines = g_strsplit(buf, "\n", -1);
    gsize klen = strlen(key);
    for (gint i = 0; lines[i]; i++) {
        if (strncmp(lines[i], key, klen) == 0 && lines[i][klen] == '=') {
            gchar *v = g_strstrip(g_strdup(lines[i] + klen + 1));
            gsize l = strlen(v);
            if (l >= 2 && (v[0] == '"' || v[0] == '\'') && v[l - 1] == v[0]) {
                val = g_strndup(v + 1, l - 2);
                g_free(v);
            } else {
                val = v;
            }
            break;
        }
    }
    g_strfreev(lines);
    g_free(buf);
    return val;
}

static gchar *sysinfo_cpu_model(void) {
    gchar *buf = NULL;
    gchar *model = NULL;
    gint cores = 0;
    if (g_file_get_contents("/proc/cpuinfo", &buf, NULL, NULL)) {
        gchar **lines = g_strsplit(buf, "\n", -1);
        for (gint i = 0; lines[i]; i++) {
            if (g_str_has_prefix(lines[i], "model name") && !model) {
                gchar *c = strchr(lines[i], ':');
                if (c) model = g_strstrip(g_strdup(c + 1));
            }
            if (g_str_has_prefix(lines[i], "processor")) cores++;
        }
        g_strfreev(lines);
        g_free(buf);
    }
    if (!model) model = g_strdup("Unknown processor");
    if (cores > 0) {
        gchar *s = g_strdup_printf("%s (%d threads)", model, cores);
        g_free(model);
        return s;
    }
    return model;
}

static gchar *sysinfo_memory(void) {
    gchar *buf = NULL;
    guint64 kb = 0;
    if (g_file_get_contents("/proc/meminfo", &buf, NULL, NULL)) {
        const gchar *p = strstr(buf, "MemTotal:");
        if (p) kb = g_ascii_strtoull(p + 9, NULL, 10);
        g_free(buf);
    }
    return kb ? g_format_size(kb * 1024) : g_strdup("Unknown");
}

static gchar *sysinfo_graphics(void) {
    /* Prefer the GL renderer name if glxinfo exists, otherwise the PCI display controller */
    gchar *out = NULL;
    if (g_spawn_command_line_sync("sh -c \"glxinfo -B 2>/dev/null | sed -n 's/^OpenGL renderer string: //p' | head -n1\"",
                                  &out, NULL, NULL, NULL) && out && g_strstrip(out)[0]) return out;
    g_free(out); out = NULL;
    if (g_spawn_command_line_sync("sh -c \"lspci 2>/dev/null | grep -iE 'vga|3d|display' | head -n1 | sed 's/^[^:]*: //'\"",
                                  &out, NULL, NULL, NULL) && out && g_strstrip(out)[0]) return out;
    g_free(out);
    return g_strdup("Unknown");
}

static gchar *sysinfo_disk(void) {
    GFile *root = g_file_new_for_path("/");
    GFileInfo *fi = g_file_query_filesystem_info(root,
        G_FILE_ATTRIBUTE_FILESYSTEM_FREE "," G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, NULL, NULL);
    g_object_unref(root);
    if (!fi) return g_strdup("Unknown");
    guint64 size = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
    guint64 fre  = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    g_object_unref(fi);
    gchar *a = g_format_size(fre), *b = g_format_size(size);
    gchar *s = g_strdup_printf("%s available of %s", a, b);
    g_free(a); g_free(b);
    return s;
}

static gchar *sysinfo_uptime(void) {
    gchar *u = sysinfo_read_first_line("/proc/uptime");
    gint64 secs = u ? (gint64)g_ascii_strtod(u, NULL) : 0;
    g_free(u);
    gint64 d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;
    if (d > 0) return g_strdup_printf("%" G_GINT64_FORMAT " d %" G_GINT64_FORMAT " h", d, h);
    return g_strdup_printf("%" G_GINT64_FORMAT " h %02" G_GINT64_FORMAT " min", h, m);
}

/* Logo for the menu button & About window: user's icon, else the distro's, else a generic one */
static const gchar *system_logo_icon_name(void) {
    static gchar *cached = NULL;
    if (cached) return cached;
    GtkIconTheme *th = gtk_icon_theme_get_default();
    gchar *logo = sysinfo_os_release("LOGO");
    if (logo && gtk_icon_theme_has_icon(th, logo)) { cached = logo; return cached; }
    g_free(logo);
    gchar *id = sysinfo_os_release("ID");
    if (id) {
        gchar *cand = g_strdup_printf("distributor-logo-%s", id);
        if (gtk_icon_theme_has_icon(th, cand)) { g_free(id); cached = cand; return cached; }
        g_free(cand);
        if (gtk_icon_theme_has_icon(th, id)) { cached = id; return cached; }
        g_free(id);
    }
    const gchar *generic[] = { "distributor-logo", "start-here-symbolic", "start-here", "computer-symbolic", NULL };
    for (gint i = 0; generic[i]; i++) {
        if (gtk_icon_theme_has_icon(th, generic[i])) { cached = g_strdup(generic[i]); return cached; }
    }
    cached = g_strdup("computer");
    return cached;
}

static void on_about_more_info(GtkButton *btn, gpointer data) {
    /* Full system details: first tool that exists */
    gchar *argv[] = { "sh", "-c",
        "for t in hardinfo2 hardinfo gnome-system-monitor xfce4-taskmanager xfce4-about; do "
        "  command -v \"$t\" >/dev/null 2>&1 && exec \"$t\"; done", NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
}

static void about_add_row(GtkGrid *grid, gint row, const gchar *key, const gchar *val) {
    GtkWidget *k = gtk_label_new(key);
    gtk_label_set_xalign(GTK_LABEL(k), 1.0);
    gtk_widget_set_opacity(k, 0.7);
    GtkWidget *v = gtk_label_new(val ? val : "");
    gtk_label_set_xalign(GTK_LABEL(v), 0.0);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(v), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 42);
    gtk_grid_attach(grid, k, 0, row, 1, 1);
    gtk_grid_attach(grid, v, 1, row, 1, 1);
}

static GtkWidget *g_about_win = NULL;

static void show_about_this_computer(void) {
    if (g_about_win) { gtk_window_present(GTK_WINDOW(g_about_win)); return; }

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "About This Computer");
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    gtk_window_set_position(GTK_WINDOW(win), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name(GTK_WINDOW(win), system_logo_icon_name());
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_about_win);
    g_about_win = win;

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);

    GtkWidget *logo = gtk_image_new_from_icon_name(system_logo_icon_name(), GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(logo), 112);
    gtk_widget_set_valign(logo, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), logo, FALSE, FALSE, 0);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gchar *os_name = sysinfo_os_release("PRETTY_NAME");
    if (!os_name) os_name = sysinfo_os_release("NAME");
    GtkWidget *title = gtk_label_new(NULL);
    gchar *tm = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", os_name ? os_name : "Linux");
    gtk_label_set_markup(GTK_LABEL(title), tm);
    g_free(tm);
    gtk_label_set_xalign(GTK_LABEL(title), 0.0);
    gtk_box_pack_start(GTK_BOX(right), title, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);

    gchar *host  = g_strdup(g_get_host_name());
    gchar *kern  = sysinfo_read_first_line("/proc/sys/kernel/osrelease");
    gchar *cpu   = sysinfo_cpu_model();
    gchar *mem   = sysinfo_memory();
    gchar *gpu   = sysinfo_graphics();
    gchar *disk  = sysinfo_disk();
    gchar *up    = sysinfo_uptime();
    gchar *model = sysinfo_read_first_line("/sys/class/dmi/id/product_name");
    gchar *desk  = g_strdup(g_getenv("XDG_CURRENT_DESKTOP") ? g_getenv("XDG_CURRENT_DESKTOP") : "XFCE");

    gint r = 0;
    about_add_row(GTK_GRID(grid), r++, "Computer name", host);
    if (model && model[0]) about_add_row(GTK_GRID(grid), r++, "Model", model);
    about_add_row(GTK_GRID(grid), r++, "Processor", cpu);
    about_add_row(GTK_GRID(grid), r++, "Memory", mem);
    about_add_row(GTK_GRID(grid), r++, "Graphics", gpu);
    about_add_row(GTK_GRID(grid), r++, "Startup disk", disk);
    about_add_row(GTK_GRID(grid), r++, "Kernel", kern ? kern : "");
    about_add_row(GTK_GRID(grid), r++, "Desktop", desk);
    about_add_row(GTK_GRID(grid), r++, "Uptime", up);
    gtk_box_pack_start(GTK_BOX(right), grid, FALSE, FALSE, 0);

    GtkWidget *more = gtk_button_new_with_label("More Info…");
    gtk_widget_set_halign(more, GTK_ALIGN_START);
    g_signal_connect(more, "clicked", G_CALLBACK(on_about_more_info), NULL);
    gtk_box_pack_start(GTK_BOX(right), more, FALSE, FALSE, 6);

    gtk_box_pack_start(GTK_BOX(box), right, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(win), box);
    gtk_widget_show_all(win);
    gtk_window_present(GTK_WINDOW(win));

    g_free(os_name); g_free(host); g_free(kern); g_free(cpu); g_free(mem);
    g_free(gpu); g_free(disk); g_free(up); g_free(model); g_free(desk);
}

static void on_sysmenu_about_activate(GtkMenuItem *item, gpointer data) {
    show_about_this_computer();
}

/* ---- Force Quit (macOS-style list of running apps) ---- */
enum { FQ_COL_ICON, FQ_COL_NAME, FQ_COL_PID, FQ_N_COLS };
static GtkWidget *g_fq_win = NULL;

static void fq_fill(GtkListStore *store, UltimateShell *shell) {
    gtk_list_store_clear(store);
    if (!shell || !shell->tracker || !shell->tracker->screen) return;
    GHashTable *seen = g_hash_table_new(g_direct_hash, g_direct_equal);
    pid_t me = getpid();
    for (GList *l = wnck_screen_get_windows(shell->tracker->screen); l; l = l->next) {
        WnckWindow *w = WNCK_WINDOW(l->data);
        WnckWindowType t = wnck_window_get_window_type(w);
        if (t != WNCK_WINDOW_NORMAL && t != WNCK_WINDOW_DIALOG) continue;
        if (wnck_window_is_skip_tasklist(w) && t == WNCK_WINDOW_NORMAL) continue;
        gint pid = wnck_window_get_pid(w);
        if (pid <= 0 || pid == me || g_hash_table_contains(seen, GINT_TO_POINTER(pid))) continue;
        g_hash_table_add(seen, GINT_TO_POINTER(pid));
        WnckApplication *app = wnck_window_get_application(w);
        const gchar *name = app ? wnck_application_get_name(app) : NULL;
        if (!name || !name[0]) name = wnck_window_get_name(w);
        GdkPixbuf *ic = wnck_window_get_mini_icon(w);
        GtkTreeIter it;
        gtk_list_store_append(store, &it);
        gtk_list_store_set(store, &it, FQ_COL_ICON, ic, FQ_COL_NAME, name ? name : "Application",
                           FQ_COL_PID, pid, -1);
    }
    g_hash_table_destroy(seen);
}

static void on_fq_response(GtkDialog *dlg, gint response, UltimateShell *shell) {
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dlg)); return; }
    GtkTreeView *tv = g_object_get_data(G_OBJECT(dlg), "fq-tree");
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreeModel *model = NULL;
    GtkTreeIter it;
    if (!gtk_tree_selection_get_selected(sel, &model, &it)) return;
    gchar *name = NULL;
    gint pid = 0;
    gtk_tree_model_get(model, &it, FQ_COL_NAME, &name, FQ_COL_PID, &pid, -1);

    GtkWidget *confirm = gtk_message_dialog_new(GTK_WINDOW(dlg), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE, "Do you want to force “%s” to quit?", name ? name : "this app");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(confirm),
        "You will lose any unsaved changes.");
    gtk_dialog_add_buttons(GTK_DIALOG(confirm), "_Cancel", GTK_RESPONSE_CANCEL, "_Force Quit", GTK_RESPONSE_ACCEPT, NULL);
    gint r = gtk_dialog_run(GTK_DIALOG(confirm));
    gtk_widget_destroy(confirm);
    if (r == GTK_RESPONSE_ACCEPT && pid > 1 && pid != getpid()) {
        kill((pid_t)pid, SIGKILL);
        GtkListStore *store = g_object_get_data(G_OBJECT(dlg), "fq-store");
        gtk_list_store_remove(store, &it);
    }
    g_free(name);
}

static void show_force_quit(UltimateShell *shell) {
    if (g_fq_win) { gtk_window_present(GTK_WINDOW(g_fq_win)); return; }
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Force Quit Applications", NULL, 0,
        "_Close", GTK_RESPONSE_CLOSE, "_Force Quit", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 380, 360);
    gtk_window_set_position(GTK_WINDOW(dlg), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(dlg), TRUE);
    g_signal_connect(dlg, "destroy", G_CALLBACK(gtk_widget_destroyed), &g_fq_win);
    g_fq_win = dlg;

    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_container_set_border_width(GTK_CONTAINER(area), 10);
    GtkWidget *hint = gtk_label_new("If an app doesn't respond for a while, select it and click Force Quit.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_pack_start(GTK_BOX(area), hint, FALSE, FALSE, 6);

    GtkListStore *store = gtk_list_store_new(FQ_N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_INT);
    fq_fill(store, shell);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tv), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *rp = gtk_cell_renderer_pixbuf_new();
    GtkCellRenderer *rt = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, rp, FALSE);
    gtk_tree_view_column_pack_start(col, rt, TRUE);
    gtk_tree_view_column_add_attribute(col, rp, "pixbuf", FQ_COL_ICON);
    gtk_tree_view_column_add_attribute(col, rt, "text", FQ_COL_NAME);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv), col);

    GtkWidget *sc = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sc), tv);
    gtk_box_pack_start(GTK_BOX(area), sc, TRUE, TRUE, 0);

    g_object_set_data(G_OBJECT(dlg), "fq-tree", tv);
    g_object_set_data_full(G_OBJECT(dlg), "fq-store", store, g_object_unref);
    g_signal_connect(dlg, "response", G_CALLBACK(on_fq_response), shell);
    gtk_widget_show_all(dlg);
}

static void on_sysmenu_force_quit_activate(GtkMenuItem *item, UltimateShell *shell) {
    show_force_quit(shell);
}

/* ---- Recent Items ---- */
static void on_recent_item_activate(GtkMenuItem *item, gpointer data) {
    const gchar *uri = g_object_get_data(G_OBJECT(item), "recent-uri");
    if (uri) g_app_info_launch_default_for_uri(uri, NULL, NULL);
}

static void on_recent_clear_activate(GtkMenuItem *item, gpointer data) {
    gtk_recent_manager_purge_items(gtk_recent_manager_get_default(), NULL);
}

static gint compare_recent_desc(gconstpointer a, gconstpointer b) {
    GtkRecentInfo *ia = (GtkRecentInfo *)a, *ib = (GtkRecentInfo *)b;
    time_t ta = gtk_recent_info_get_modified(ia), tb = gtk_recent_info_get_modified(ib);
    return (ta < tb) ? 1 : (ta > tb) ? -1 : 0;
}

static GtkWidget *build_recent_items_menu(void) {
    GtkWidget *sub = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "ultimate-themed-menu");
    GList *items = gtk_recent_manager_get_items(gtk_recent_manager_get_default());
    items = g_list_sort(items, compare_recent_desc);
    gint n = 0;
    for (GList *l = items; l && n < 12; l = l->next) {
        GtkRecentInfo *ri = (GtkRecentInfo *)l->data;
        if (!gtk_recent_info_exists(ri)) continue;
        const gchar *label = gtk_recent_info_get_display_name(ri);
        GtkWidget *mi = gtk_menu_item_new_with_label(label ? label : gtk_recent_info_get_uri(ri));
        gchar *tip = gtk_recent_info_get_uri_display(ri);
        if (tip) { gtk_widget_set_tooltip_text(mi, tip); g_free(tip); }
        g_object_set_data_full(G_OBJECT(mi), "recent-uri", g_strdup(gtk_recent_info_get_uri(ri)), g_free);
        g_signal_connect(mi, "activate", G_CALLBACK(on_recent_item_activate), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), mi);
        n++;
    }
    g_list_free_full(items, (GDestroyNotify)gtk_recent_info_unref);
    if (n == 0) {
        GtkWidget *none = gtk_menu_item_new_with_label("No recent items");
        gtk_widget_set_sensitive(none, FALSE);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), none);
    } else {
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), gtk_separator_menu_item_new());
        GtkWidget *clr = gtk_menu_item_new_with_label("Clear Menu");
        g_signal_connect(clr, "activate", G_CALLBACK(on_recent_clear_activate), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(sub), clr);
    }
    return sub;
}

/* ---- App store: whichever one is installed ---- */
static const gchar *find_app_store_command(void) {
    static const gchar *const stores[] = {
        "gnome-software", "plasma-discover", "mintinstall", "pamac-manager",
        "io.elementary.appcenter", "synaptic-pkexec", "synaptic", NULL
    };
    for (gint i = 0; stores[i]; i++) {
        gchar *p = g_find_program_in_path(stores[i]);
        if (p) { g_free(p); return stores[i]; }
    }
    return NULL;
}

static void popup_macos_apple_menu(StartMenuUI *ui) {
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");

    GtkWidget *it_about = gtk_menu_item_new_with_label("About This Computer");
    g_signal_connect(it_about, "activate", G_CALLBACK(on_sysmenu_about_activate), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_about);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *it_settings = gtk_menu_item_new_with_label("System Settings...");
    g_signal_connect(it_settings, "activate", G_CALLBACK(on_apple_menu_cmd_activate), "xfce4-settings-manager");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_settings);

    const gchar *store_cmd = find_app_store_command();
    if (store_cmd) {   /* only shown when a store / package manager is installed */
        GtkWidget *it_store = gtk_menu_item_new_with_label("App Store / Package Manager...");
        g_signal_connect(it_store, "activate", G_CALLBACK(on_apple_menu_cmd_activate), (gpointer)store_cmd);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_store);
    }

    GtkWidget *it_apps = gtk_menu_item_new_with_label("Applications (Launchpad)...");
    g_signal_connect(it_apps, "activate", G_CALLBACK(on_apple_menu_launchpad_activate), ui->shell);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_apps);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *it_recent = gtk_menu_item_new_with_label("Recent Items");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(it_recent), build_recent_items_menu());
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_recent);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *it_fquit = gtk_menu_item_new_with_label("Force Quit...");
    g_signal_connect(it_fquit, "activate", G_CALLBACK(on_sysmenu_force_quit_activate), ui->shell);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_fquit);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *it_sleep = gtk_menu_item_new_with_label("Sleep");
    g_signal_connect(it_sleep, "activate", G_CALLBACK(on_apple_menu_cmd_activate), "systemctl suspend");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_sleep);

    GtkWidget *it_restart = gtk_menu_item_new_with_label("Restart...");
    g_signal_connect(it_restart, "activate", G_CALLBACK(on_apple_menu_power_activate), "restart");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_restart);

    GtkWidget *it_shutdown = gtk_menu_item_new_with_label("Shut Down...");
    g_signal_connect(it_shutdown, "activate", G_CALLBACK(on_apple_menu_power_activate), "shutdown");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_shutdown);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());

    GtkWidget *it_lock = gtk_menu_item_new_with_label("Lock Screen");
    g_signal_connect(it_lock, "activate", G_CALLBACK(on_apple_menu_cmd_activate), "xflock4");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_lock);

    gchar *logout_lbl = g_strdup_printf("Log Out %s...", g_get_user_name());
    GtkWidget *it_logout = gtk_menu_item_new_with_label(logout_lbl);
    g_free(logout_lbl);
    g_signal_connect(it_logout, "activate", G_CALLBACK(on_apple_menu_power_activate), "logout");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), it_logout);

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_widget(GTK_MENU(menu), ui->btn,
                             GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, NULL);
}

static void on_start_btn_toggled(GtkToggleButton *btn, StartMenuUI *ui) {
    if (!ui || !ui->shell) return;
    if (ui->shell->mode == LAYOUT_MACOS) {
        if (gtk_toggle_button_get_active(btn)) {
            g_signal_handlers_block_by_func(btn, G_CALLBACK(on_start_btn_toggled), ui);
            gtk_toggle_button_set_active(btn, FALSE);
            g_signal_handlers_unblock_by_func(btn, G_CALLBACK(on_start_btn_toggled), ui);
            popup_macos_apple_menu(ui);
        }
        return;
    }
    if (gtk_toggle_button_get_active(btn) && (ui->rebuild_pending || !ui->cached_apps)) {
        ui->rebuild_pending = FALSE;
        start_menu_rebuild_style(ui->shell);       /* apps installed or pins changed while it was open */
    }
    if (ui->popup_win) {
        g_object_set_data(G_OBJECT(ui->popup_win), "dock-anchor-btn", NULL);
        if (sm_style_fullscreen(ui->shell) && gtk_toggle_button_get_active(btn)) {
            GdkRectangle fs_rect;
            compute_fullscreen_menu_workarea(ui, &fs_rect);
            gtk_widget_set_size_request(ui->popup_win, fs_rect.width, fs_rect.height);
            gtk_window_resize(GTK_WINDOW(ui->popup_win), fs_rect.width, fs_rect.height);
            gtk_window_move(GTK_WINDOW(ui->popup_win), fs_rect.x, fs_rect.y);
        }
    }
    if (gtk_toggle_button_get_active(btn)) win11_reset_to_pinned(ui);
    shell_toggle_popup_window(btn, ui->popup_win);
    if (gtk_toggle_button_get_active(btn) && ui->search_entry && gtk_widget_get_visible(ui->search_entry)) {
        gtk_entry_set_text(GTK_ENTRY(ui->search_entry), "");
        gtk_widget_grab_focus(ui->search_entry);
    }
}

static void position_popup_above_dock_anchor(GtkWidget *popup_win, GtkWidget *anchor_btn) {
    if (!popup_win || !anchor_btn || !gtk_widget_get_window(anchor_btn)) return;

    gint st = (g_start_ui && g_start_ui->shell) ? CLAMP((gint)g_start_ui->shell->start_style, 0, START_STYLE_COUNT - 1) : 0;
    gint pw = (g_start_ui && g_start_ui->shell) ? CLAMP(g_start_ui->shell->start_style_w[st], 320, 1400) : 420;
    gint ph = (g_start_ui && g_start_ui->shell) ? CLAMP(g_start_ui->shell->start_style_h[st], 360, 1000) : 520;
    /* place by the size the menu was really built at (the saved size can be outside these
       limits, e.g. Classic at 350 x 470): otherwise its edge misses the dock / screen edge */
    {
        gint rw = -1, rh = -1;
        gtk_widget_get_size_request(popup_win, &rw, &rh);
        if (rw > 100) pw = rw;
        if (rh > 100) ph = rh;
    }

    if (g_start_ui && g_start_ui->shell && sm_style_fullscreen(g_start_ui->shell)) {
        GdkRectangle fs_rect;
        compute_fullscreen_menu_workarea(g_start_ui, &fs_rect);
        gtk_widget_set_size_request(popup_win, fs_rect.width, fs_rect.height);
        gtk_window_resize(GTK_WINDOW(popup_win), fs_rect.width, fs_rect.height);
        gtk_window_move(GTK_WINDOW(popup_win), fs_rect.x, fs_rect.y);
        return;
    }
    gtk_widget_set_size_request(popup_win, pw, ph);
    gtk_window_resize(GTK_WINDOW(popup_win), pw, ph);

    gint wx = 0, wy = 0;
    gdk_window_get_origin(gtk_widget_get_window(anchor_btn), &wx, &wy);
    GtkAllocation ba;
    gtk_widget_get_allocation(anchor_btn, &ba);

    gint btn_center_x = wx + ba.x + (ba.width / 2);
    gint btn_top_y    = wy + ba.y;

    GdkDisplay *display = gdk_display_get_default();
    GdkMonitor *mon = gdk_display_get_monitor_at_point(display, btn_center_x, btn_top_y);
    if (!mon) mon = gdk_display_get_primary_monitor(display);
    GdkRectangle geom = { 0, 0, 1920, 1080 };
    if (mon) gdk_monitor_get_geometry(mon, &geom);

    gint target_x = btn_center_x - (pw / 2);
    target_x = CLAMP(target_x, geom.x + 8, geom.x + geom.width - pw - 8);

    gint target_y = btn_top_y - ph - 12;
    if (target_y < geom.y + 36) target_y = geom.y + 36;

    /* same rule as every other dock popup (handles side docks too) */
    gint px = 0, py = 0;
    if (shell_place_popup_at_button(anchor_btn, pw, ph, TRUE, &px, &py)) {
        target_x = px;
        target_y = MAX(py, geom.y + 36);   /* stay clear of the top bar */
    }

    gtk_window_move(GTK_WINDOW(popup_win), target_x, target_y);
}

static void on_dock_popup_size_allocate(GtkWidget *popup_win, GdkRectangle *alloc, gpointer data) {
    if (g_start_ui && g_start_ui->resizing) return;
    GtkWidget *dock_anchor = g_object_get_data(G_OBJECT(popup_win), "dock-anchor-btn");
    gint pending = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(popup_win), "dock-needs-center"));
    if (dock_anchor && pending && gtk_widget_get_visible(popup_win) && alloc->width > 120) {
        g_object_set_data(G_OBJECT(popup_win), "dock-needs-center", GINT_TO_POINTER(0));
        position_popup_above_dock_anchor(popup_win, dock_anchor);
    }
}

/* Super key (xfce4-panel --plugin-event=ultimateshell:popup:bool:true): the same start menu the
   button opens - from the Start button in Windows mode, from the dock's Show Apps / Launchpad
   button in macOS and GNOME mode (the top-bar button there is the Apple menu / absent) */
void start_menu_toggle_from_key(UltimateShell *shell) {
    if (!shell || !g_start_ui || !g_start_ui->popup_win) return;
    /* Pressing Super makes Keyboard Shortcuts grab the keyboard: the open menu loses focus and
       hides itself just before this command arrives (it goes through a separate xfce4-panel
       process). That hide was the "close" - don't open it again. */
    if (!gtk_widget_get_visible(g_start_ui->popup_win)) {
        gint64 *last = g_object_get_data(G_OBJECT(g_start_ui->popup_win), "last-focus-hide-us");
        if (last && g_get_monotonic_time() - *last < 1200000) {
            g_object_set_data(G_OBJECT(g_start_ui->popup_win), "last-focus-hide-us", NULL);
            return;
        }
    }
    if (shell->mode == LAYOUT_MACOS) {
        start_menu_open_from_dock(shell, dock_window_launchpad_btn());
        return;
    }
    if (GTK_IS_TOGGLE_BUTTON(g_start_ui->btn)) {
        gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_start_ui->btn));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_start_ui->btn), !on);
    }
}

void start_menu_open_from_dock(UltimateShell *shell, GtkWidget *anchor_btn) {
    if (!g_start_ui || !g_start_ui->popup_win) return;
    if (!g_object_get_data(G_OBJECT(g_start_ui->popup_win), "dock-alloc-connected")) {
        g_signal_connect_after(g_start_ui->popup_win, "size-allocate",
                               G_CALLBACK(on_dock_popup_size_allocate), NULL);
        g_object_set_data(G_OBJECT(g_start_ui->popup_win), "dock-alloc-connected", GINT_TO_POINTER(1));
    }

    if (gtk_widget_get_visible(g_start_ui->popup_win)) {
        shell_popup_hide(g_start_ui->popup_win);   /* before forgetting the dock anchor: Slide uses it */
        g_object_set_data(G_OBJECT(g_start_ui->popup_win), "dock-anchor-btn", NULL);
        if (GTK_IS_TOGGLE_BUTTON(g_start_ui->btn)) {
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_start_ui->btn), FALSE);
        }
    } else {
        if (g_start_ui->rebuild_pending || !g_start_ui->cached_apps) {
            g_start_ui->rebuild_pending = FALSE;
            start_menu_rebuild_style(shell);
        }
        g_object_set_data(G_OBJECT(g_start_ui->popup_win), "dock-anchor-btn", anchor_btn);
        g_object_set_data(G_OBJECT(g_start_ui->popup_win), "dock-needs-center", GINT_TO_POINTER(1));
        if (GTK_IS_ENTRY(g_start_ui->search_entry)) {
            gtk_entry_set_text(GTK_ENTRY(g_start_ui->search_entry), "");
        }
        win11_reset_to_pinned(g_start_ui);
        shell_popup_cancel_close(g_start_ui->popup_win);
        gtk_widget_show_all(g_start_ui->popup_win);
        if (anchor_btn) position_popup_above_dock_anchor(g_start_ui->popup_win, anchor_btn);
        gtk_window_present(GTK_WINDOW(g_start_ui->popup_win));
        if (GTK_IS_WIDGET(g_start_ui->search_entry)) {
            gtk_widget_grab_focus(g_start_ui->search_entry);
        }
    }
}

void start_menu_update_button_appearance(UltimateShell *shell) {
    if (!g_start_ui || !g_start_ui->btn) return;
    StartMenuUI *ui = g_start_ui;

    if (ui->btn_box) {
        gtk_widget_destroy(ui->btn_box);
        ui->btn_box = NULL;
    }

    ui->btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_valign(ui->btn_box, GTK_ALIGN_CENTER);

    if (shell->mode == LAYOUT_MACOS) {
        gint mac_px = (shell->mac_apple_icon_size > 0) ? CLAMP(shell->mac_apple_icon_size, 12, 36) : 18;
        const gchar *icon_str = shell->mac_apple_icon ? shell->mac_apple_icon : "";
        if (icon_str[0] != '\0' && g_file_test(icon_str, G_FILE_TEST_EXISTS)) {
            GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(icon_str, mac_px, mac_px, TRUE, NULL);
            ui->btn_icon_widget = pb ? gtk_image_new_from_pixbuf(pb) : gtk_image_new_from_file(icon_str);
            if (pb) g_object_unref(pb);
        } else if (icon_str[0] != '\0' && g_ascii_strcasecmp(icon_str, "apple") != 0) {
            ui->btn_icon_widget = gtk_image_new_from_icon_name(icon_str, GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(ui->btn_icon_widget), mac_px);
        } else if (icon_str[0] == '\0') {
            /* Default: the distro's logo from the icon theme (no Apple logo on a non-Mac) */
            ui->btn_icon_widget = gtk_image_new_from_icon_name(system_logo_icon_name(), GTK_ICON_SIZE_MENU);
            gtk_image_set_pixel_size(GTK_IMAGE(ui->btn_icon_widget), mac_px);
        } else {
            /* Only when the user explicitly typed "apple": draw the vector logo */
            ui->btn_icon_widget = gtk_drawing_area_new();
            gtk_widget_set_size_request(ui->btn_icon_widget, mac_px, mac_px);
            gtk_widget_set_valign(ui->btn_icon_widget, GTK_ALIGN_CENTER);
            g_signal_connect(ui->btn_icon_widget, "draw", G_CALLBACK(on_draw_apple_logo), NULL);
        }
        gtk_box_pack_start(GTK_BOX(ui->btn_box), ui->btn_icon_widget, FALSE, FALSE, 2);
        gtk_container_add(GTK_CONTAINER(ui->btn), ui->btn_box);
        gtk_widget_show_all(ui->btn);
        return;
    }

    gboolean show_icon = (shell->start_btn_mode == START_BTN_MODE_ICON_LABEL || shell->start_btn_mode == START_BTN_MODE_ICON_ONLY);
    gboolean show_label = (shell->start_btn_mode == START_BTN_MODE_ICON_LABEL || shell->start_btn_mode == START_BTN_MODE_LABEL_ONLY);

    if (show_icon) {
        gint win_px = (shell->start_btn_icon_size > 0) ? CLAMP(shell->start_btn_icon_size, 14, 48) : 24;
        const gchar *icon_str = (shell->start_btn_icon && shell->start_btn_icon[0] != '\0')
                              ? shell->start_btn_icon : "open-menu-symbolic";
        if (g_file_test(icon_str, G_FILE_TEST_EXISTS)) {
            GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(icon_str, win_px, win_px, TRUE, NULL);
            ui->btn_icon_widget = pb ? gtk_image_new_from_pixbuf(pb) : gtk_image_new_from_file(icon_str);
            if (pb) g_object_unref(pb);
        } else {
            ui->btn_icon_widget = gtk_image_new_from_icon_name(icon_str, GTK_ICON_SIZE_LARGE_TOOLBAR);
            gtk_image_set_pixel_size(GTK_IMAGE(ui->btn_icon_widget), win_px);
        }
        gtk_box_pack_start(GTK_BOX(ui->btn_box), ui->btn_icon_widget, FALSE, FALSE, 0);
    }

    if (show_label) {
        const gchar *lbl_str = shell->start_btn_label ? shell->start_btn_label : "Start";
        ui->btn_label_widget = gtk_label_new(lbl_str);
        gtk_box_pack_start(GTK_BOX(ui->btn_box), ui->btn_label_widget, FALSE, FALSE, 0);
    }

    gtk_container_add(GTK_CONTAINER(ui->btn), ui->btn_box);
    gtk_widget_show_all(ui->btn);
}


/* =========================================================================
 * Right-click on the Start button (Win+X style quick menu)
 * ========================================================================= */
/* run through sh so "||" fallbacks and $DISPLAY work; panel stays the live parent (pkexec) */
static void on_sbm_cmd(GtkMenuItem *mi, const gchar *cmd) {
    gchar *argv[] = { "/bin/sh", "-c", (gchar *)cmd, NULL };
    GPid pid = 0;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, NULL, NULL, &pid, NULL))
        g_child_watch_add(pid, on_spawned_child_exited, NULL);
}

static void on_sbm_shell_settings(GtkMenuItem *mi, StartMenuUI *ui) {
    if (ui && ui->shell && ui->shell->plugin)
        g_signal_emit_by_name(ui->shell->plugin, "configure-plugin");
}

static gboolean sbm_focus_search(gpointer data) {
    StartMenuUI *ui = data;
    if (ui && GTK_IS_WIDGET(ui->search_entry) && gtk_widget_get_visible(ui->search_entry))
        gtk_widget_grab_focus(ui->search_entry);
    return G_SOURCE_REMOVE;
}

static void on_sbm_search(GtkMenuItem *mi, StartMenuUI *ui) {
    GtkWidget *anchor = g_object_get_data(G_OBJECT(mi), "sbm-anchor");
    if (ui->shell && ui->shell->mode == LAYOUT_MACOS) start_menu_open_from_dock(ui->shell, anchor);
    else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->btn), TRUE);
    g_timeout_add(120, sbm_focus_search, ui);
}

static void on_sbm_power(GtkMenuItem *mi, const gchar *action) {
    on_confirm_power_action(NULL, action);
}

static void on_sbm_show_desktop(GtkMenuItem *mi, gpointer d) {
    WnckScreen *sc = (g_start_ui && g_start_ui->shell && g_start_ui->shell->tracker) ? g_start_ui->shell->tracker->screen : NULL;
    if (!sc) return;
    wnck_screen_force_update(sc);
    wnck_screen_toggle_showing_desktop(sc, !wnck_screen_get_showing_desktop(sc));
}

static GtkWidget *sbm_item(GtkWidget *menu, const gchar *label, GCallback cb, gpointer data) {
    GtkWidget *mi = gtk_menu_item_new_with_mnemonic(label);
    g_signal_connect(mi, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    return mi;
}

static void sbm_sep(GtkWidget *menu) {
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* Win+X style menu. Windows layout: right-click the Start button.
   macOS layout: right-click Launchpad in the dock (the Apple menu is not the Start menu). */
void start_menu_popup_quick_menu(GtkWidget *anchor, GdkEventButton *ev) {
    StartMenuUI *ui = g_start_ui;
    if (!ui) return;
    if (GTK_IS_TOGGLE_BUTTON(ui->btn)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->btn), FALSE);

    GtkWidget *menu = gtk_menu_new();
    gtk_menu_set_reserve_toggle_size(GTK_MENU(menu), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(menu), "ultimate-themed-menu");

    gchar *tm = g_find_program_in_path("xfce4-taskmanager");
    if (tm)
        sbm_item(menu, "_Task Manager", G_CALLBACK(on_sbm_cmd), "xfce4-taskmanager");
    g_free(tm);
    sbm_sep(menu);
    sbm_item(menu, "System _Settings", G_CALLBACK(on_sbm_cmd), "xfce4-settings-manager");
    sbm_item(menu, "_Ultimate Shell Settings", G_CALLBACK(on_sbm_shell_settings), ui);
    sbm_sep(menu);
    sbm_item(menu, "T_erminal", G_CALLBACK(on_sbm_cmd), "exo-open --launch TerminalEmulator");
    sbm_item(menu, "Terminal (_Admin)", G_CALLBACK(on_sbm_cmd),
             "pkexec env DISPLAY=\"$DISPLAY\" XAUTHORITY=\"$XAUTHORITY\" xfce4-terminal");
    sbm_sep(menu);
    sbm_item(menu, "File _Manager", G_CALLBACK(on_sbm_cmd), "exo-open --launch FileManager");
    GtkWidget *search_mi = sbm_item(menu, "Searc_h", G_CALLBACK(on_sbm_search), ui);
    g_object_set_data(G_OBJECT(search_mi), "sbm-anchor", anchor);
    sbm_item(menu, "_Run…", G_CALLBACK(on_sbm_cmd), "xfce4-appfinder --collapsed");
    sbm_sep(menu);

    GtkWidget *pw = gtk_menu_item_new_with_mnemonic("Sh_ut down or sign out");
    GtkWidget *sub = gtk_menu_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "ultimate-themed-menu");
    sbm_item(sub, "_Lock", G_CALLBACK(on_sbm_cmd), "xflock4");
    sbm_item(sub, "Log _Out", G_CALLBACK(on_sbm_power), "logout");
    sbm_item(sub, "_Suspend", G_CALLBACK(on_sbm_cmd), "xfce4-session-logout --suspend || systemctl suspend");
    sbm_item(sub, "_Restart", G_CALLBACK(on_sbm_power), "restart");
    sbm_item(sub, "Shut _Down", G_CALLBACK(on_sbm_power), "shutdown");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(pw), sub);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), pw);
    sbm_item(menu, "_Desktop", G_CALLBACK(on_sbm_show_desktop), NULL);

    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done", G_CALLBACK(gtk_widget_destroy), NULL);
    shell_popup_menu_at_button(GTK_MENU(menu), anchor, (GdkEvent *)ev);
}

static gboolean on_start_btn_button_press(GtkWidget *btn, GdkEventButton *ev, StartMenuUI *ui) {
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3) return FALSE;
    if (ui->shell && ui->shell->mode == LAYOUT_MACOS) return FALSE;   /* Apple menu keeps the panel's menu */
    start_menu_popup_quick_menu(btn, ev);
    return TRUE;   /* replaces the panel's default item menu */
}

GtkWidget *start_menu_button_new(UltimateShell *shell) {
    GtkWidget *btn = gtk_toggle_button_new();
    gtk_widget_set_name(btn, "ultimate-start-btn");
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);

    StartMenuUI *ui = g_new0(StartMenuUI, 1);
    g_start_ui = ui;
    ui->shell = shell;
    ui->btn = btn;
    ui->popup_win = shell_create_popup_window(btn, 360, 470, TRUE);
    g_object_set_data(G_OBJECT(ui->popup_win), "is-start-popup", GINT_TO_POINTER(1));

    gtk_widget_add_events(ui->popup_win, GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK);
    g_signal_connect(ui->popup_win, "scroll-event", G_CALLBACK(on_fs_deck_scroll), ui);
    g_signal_connect(ui->popup_win, "key-press-event", G_CALLBACK(on_start_popup_key_press), ui);
    g_signal_connect(ui->popup_win, "key-release-event", G_CALLBACK(on_start_popup_key_release), ui);

    start_menu_rebuild_style(shell);
    start_menu_update_button_appearance(shell);
    g_signal_connect(btn, "toggled", G_CALLBACK(on_start_btn_toggled), ui);
    g_signal_connect(btn, "button-press-event", G_CALLBACK(on_start_btn_button_press), ui);

    return btn;
}
