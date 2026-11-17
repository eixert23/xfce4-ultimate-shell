project('xfce4-ultimate-shell', 'c',
  version: '0.4.0',
  license: 'GPL-2.0-or-later',
  default_options: ['c_std=gnu11', 'warning_level=2']
)
add_project_arguments('-Wno-unused-parameter', language: 'c')
gtk_dep       = dependency('gtk+-3.0')
glib_dep      = dependency('glib-2.0')
gio_unix_dep  = dependency('gio-unix-2.0')
panel_dep     = dependency('libxfce4panel-2.0')
ui_dep        = dependency('libxfce4ui-2')
util_dep      = dependency('libxfce4util-1.0')
xfconf_dep    = dependency('libxfconf-0')
wnck_dep      = dependency('libwnck-3.0')
x11_dep       = dependency('x11')
dbusmenu_dep  = dependency('dbusmenu-gtk3-0.4')
xtst_dep      = dependency('xtst')

panel_libdir  = panel_dep.get_variable(pkgconfig: 'libdir')
plugin_dir    = panel_libdir / 'xfce4' / 'panel' / 'plugins'
desktop_dir   = get_option('prefix') / get_option('datadir') / 'xfce4' / 'panel' / 'plugins'

sources = [
  'src/shell-plugin.c',
  'src/start-menu.c',
  'src/clock-widget.c',
  'src/dock-window.c',
  'src/window-tracker.c',
  'src/taskbar.c',
  'src/windowck.c',
  'src/global-menu.c',
  'src/systray.c',
  'src/workspace-indicator.c',
    
  'src/notifications.c',
  'src/launcher-entry.c',
  'src/win-fill.c'
]

shared_module('ultimateshell',
  sources,
  dependencies: [gtk_dep, glib_dep, gio_unix_dep,
    panel_dep, ui_dep, util_dep, xfconf_dep,
    wnck_dep, x11_dep, xtst_dep, dbusmenu_dep
  ],
  install: true,
  install_dir: plugin_dir
)

install_data('ultimateshell.desktop',
  install_dir: desktop_dir
)

# Small GTK3 module that publishes each app's menu shortcuts for the global menu
gtk_libdir     = gtk_dep.get_variable(pkgconfig: 'libdir')
gtk_module_dir = gtk_libdir / 'gtk-3.0' / 'modules'
shared_module('ultimate-accels-gtk-module',
  'gtkmodule/ultimate-accels.c',
  dependencies: [gtk_dep, x11_dep],
  name_prefix: 'lib',
  install: true,
  install_dir: gtk_module_dir
)
