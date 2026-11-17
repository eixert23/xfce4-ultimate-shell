#include "window-tracker.h"
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

static void apply_hide_titlebar_atom(WnckWindow *window, gboolean hide) {
    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!GDK_IS_X11_DISPLAY(gdk_dpy) || !window) return;

    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    Window xid = wnck_window_get_xid(window);
    Atom prop = XInternAtom(dpy, "_GTK_HIDE_TITLEBAR_WHEN_MAXIMIZED", False);
    unsigned long val = hide ? 1UL : 0UL;

    gdk_x11_display_error_trap_push(gdk_dpy);
    XChangeProperty(dpy, xid, prop, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)&val, 1);
    gdk_x11_display_error_trap_pop_ignored(gdk_dpy);
}

static void on_track_window_opened(WnckScreen *screen, WnckWindow *window, UltimateWindowTracker *tracker) {
    if (wnck_window_get_window_type(window) == WNCK_WINDOW_NORMAL) {
        apply_hide_titlebar_atom(window, tracker->hide_titlebar_on_max);
    }
}

void window_tracker_set_hide_max_titlebar(UltimateWindowTracker *tracker, gboolean enable) {
    tracker->hide_titlebar_on_max = enable;
    GList *windows = wnck_screen_get_windows(tracker->screen);
    for (GList *l = windows; l != NULL; l = l->next) {
        WnckWindow *win = WNCK_WINDOW(l->data);
        if (wnck_window_get_window_type(win) == WNCK_WINDOW_NORMAL) {
            apply_hide_titlebar_atom(win, enable);
        }
    }
}

UltimateWindowTracker *window_tracker_new(void) {
    UltimateWindowTracker *tracker = g_new0(UltimateWindowTracker, 1);
    static WnckHandle *s_wnck_handle = NULL;
    if (!s_wnck_handle) s_wnck_handle = wnck_handle_new(WNCK_CLIENT_TYPE_PAGER);
    tracker->screen = wnck_handle_get_default_screen(s_wnck_handle);
    wnck_screen_force_update(tracker->screen);

    g_signal_connect(tracker->screen, "window-opened",
                     G_CALLBACK(on_track_window_opened), tracker);
    return tracker;
}