#ifndef TASKBAR_H
#define TASKBAR_H

#include "shell-plugin.h"
#include <gio/gio.h>
#include <libwnck/libwnck.h>

typedef struct {
    GtkWidget   *btn;
    GdkPixbuf   *pb_master;
    gboolean     has_windows;
    gboolean     has_active;
    gboolean     is_hovered;
    gboolean     is_pinned;
    gboolean     needs_attention;
    guint        win_count;
    gdouble      bounce_offset;
    const gchar *title;
    gint64       le_count;      /* app-sent badge count (LauncherEntry), 0 = none */
    gdouble      le_progress;   /* app-sent progress 0..1, < 0 = none */
} DockRenderItem;

GtkWidget *taskbar_widget_new(UltimateShell *shell);
void taskbar_update_icon_sizes(UltimateShell *shell);
void taskbar_pin_app_info(GAppInfo *app);
gint taskbar_collect_dock_render_items(DockRenderItem *out_items, gint max_items);

/* Count badge on an icon's top-right corner, shared by the taskbar and the dock.
   alert = TRUE: an app's own count (red). FALSE: open-window count (neutral). */
void taskbar_draw_icon_badge(cairo_t *cr, gdouble icon_x, gdouble icon_y, gdouble icon_sz,
                             gint64 count, gboolean alert);

#endif
