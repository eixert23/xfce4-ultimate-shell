#include "win-fill.h"
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>

/* Let maximized windows fill the screen.
   Terminals (xfce4-terminal, xterm, ...) ask to be resized only in whole character
   steps (WM_NORMAL_HINTS resize increments). xfwm4 honours those steps even when
   maximizing, so a maximized terminal stops up to one text row short of the panel.
   We remove the steps from app windows - and again whenever the app sets them anew
   (font change, zoom). Terminals handle any pixel size: the rest becomes padding. */

static UltimateShell *g_wf_shell = NULL;
static GHashTable    *g_watched = NULL;     /* xid -> GdkWindow* (foreign) */

static gboolean wf_enabled(void) { return g_wf_shell && g_wf_shell->wm_fill_maximized; }

/* Remove the resize steps. TRUE when there were any. */
static gboolean strip_increments(Display *dpy, Window xid) {
    XSizeHints *h = XAllocSizeHints();
    long supplied = 0;
    gboolean changed = FALSE;
    gdk_x11_display_error_trap_push(gdk_display_get_default());
    if (XGetWMNormalHints(dpy, xid, h, &supplied) && (h->flags & PResizeInc)
        && (h->width_inc > 1 || h->height_inc > 1)) {
        h->flags &= ~PResizeInc;
        h->width_inc = h->height_inc = 1;
        XSetWMNormalHints(dpy, xid, h);
        XFlush(dpy);
        changed = TRUE;
    }
    gdk_x11_display_error_trap_pop_ignored(gdk_display_get_default());
    XFree(h);
    return changed;
}

/* A maximized window that already got its stepped size: let xfwm4 fit it again */
static gboolean refit_later(gpointer data) {
    WnckWindow *w = data;
    if (WNCK_IS_WINDOW(w) && wnck_window_is_maximized(w)) {
        wnck_window_unmaximize(w);
        wnck_window_maximize(w);
    }
    g_object_unref(w);
    return G_SOURCE_REMOVE;
}

static void fix_window(WnckWindow *w) {
    if (!wf_enabled() || !WNCK_IS_WINDOW(w) || wnck_window_get_window_type(w) != WNCK_WINDOW_NORMAL) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    if (strip_increments(dpy, wnck_window_get_xid(w)) && wnck_window_is_maximized(w))
        g_timeout_add(80, refit_later, g_object_ref(w));
}

/* The app wrote new size hints (font change, zoom): take the steps out again */
static GdkFilterReturn on_prop_filter(GdkXEvent *xev, GdkEvent *ev, gpointer data) {
    XEvent *e = (XEvent *)xev;
    if (e->type == PropertyNotify && e->xproperty.atom == XA_WM_NORMAL_HINTS && wf_enabled()) {
        fix_window(WNCK_WINDOW(data));    /* filter is removed when the window closes */
    }
    return GDK_FILTER_CONTINUE;
}

static void watch_window(WnckWindow *w) {
    if (!WNCK_IS_WINDOW(w) || wnck_window_get_window_type(w) != WNCK_WINDOW_NORMAL) return;
    gulong xid = wnck_window_get_xid(w);
    if (g_hash_table_contains(g_watched, GSIZE_TO_POINTER(xid))) return;
    GdkDisplay *gd = gdk_display_get_default();
    Display *dpy = GDK_DISPLAY_XDISPLAY(gd);

    /* add PropertyChangeMask to what this connection already selects (libwnck uses it too) */
    gdk_x11_display_error_trap_push(gd);
    XWindowAttributes at;
    if (XGetWindowAttributes(dpy, xid, &at))
        XSelectInput(dpy, xid, at.your_event_mask | PropertyChangeMask);
    gdk_x11_display_error_trap_pop_ignored(gd);

    GdkWindow *fw = gdk_x11_window_foreign_new_for_display(gd, xid);
    if (fw) {
        gdk_window_add_filter(fw, on_prop_filter, w);
        g_hash_table_insert(g_watched, GSIZE_TO_POINTER(xid), fw);
    }
    fix_window(w);
}

static void on_opened(WnckScreen *s, WnckWindow *w, gpointer d) { watch_window(w); }

static void on_closed(WnckScreen *s, WnckWindow *w, gpointer d) {
    gulong xid = wnck_window_get_xid(w);
    GdkWindow *fw = g_hash_table_lookup(g_watched, GSIZE_TO_POINTER(xid));
    if (fw) {
        gdk_window_remove_filter(fw, on_prop_filter, w);
        g_object_unref(fw);
        g_hash_table_remove(g_watched, GSIZE_TO_POINTER(xid));
    }
}

void win_fill_apply_settings(UltimateShell *shell) {
    g_wf_shell = shell;
    WnckScreen *scr = (shell && shell->tracker) ? shell->tracker->screen : NULL;
    if (!scr) return;
    static gboolean hooked = FALSE;
    if (!hooked) {
        g_watched = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_signal_connect(scr, "window-opened", G_CALLBACK(on_opened), NULL);
        g_signal_connect(scr, "window-closed", G_CALLBACK(on_closed), NULL);
        hooked = TRUE;
    }
    for (GList *l = wnck_screen_get_windows(scr); l; l = l->next) watch_window(WNCK_WINDOW(l->data));
    if (wf_enabled())
        for (GList *l = wnck_screen_get_windows(scr); l; l = l->next) fix_window(WNCK_WINDOW(l->data));
}
