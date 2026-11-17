# Changelog

## 0.4.0 — first public beta

### Appearance
- Unified colours: *Follow GTK theme*, *Light*, *Dark* or *Custom* (background, text,
  accent - all with transparency); hover and border colours automatic or custom
- One palette for panel, dock, popups, menus, global menu, search fields, check boxes,
  sliders, tooltips and the settings window
- Panel & dock opacity, popups & menus opacity
- Shell themes: Apply, Save as, Delete, Import, Export; built-in macOS / Windows 11
  Light and Dark (colours only - your shapes stay); "save your look changes to the
  theme?" when leaving the General page, with a *(modified)* marker
- GTK theme, icon theme, title-bar style, shell font, corner radius, popup border and
  shadow on the General page
- Frameless panel (no square edge line) with a shell-drawn panel shadow; dock and dock
  icon shadows drawn by the shell
- Indicators page: taskbar and dock styles, active / inactive / attention colours
- xfce4-panel's dark mode follows the shell colours

### Shell & layouts
- macOS and Windows layouts; four Windows panel layouts with a true centered group
- Workspace indicator (GNOME pills / dots / numbers) with its own settings page
- Animations: Off / Subtle / Full with speed; desktop-compositing switch
- One placement rule for every popup, menu and preview (6 px from the bar, beside it on side docks)

### Dock & taskbar
- App badges and progress bars (Unity LauncherEntry; XApp progress, e.g. Firefox downloads)
- Drop files on an icon to open them with that app
- Count badges redesigned (red for app counts, neutral for open windows)
- Icons update when the icon theme changes
- Maximized windows fill the screen (terminals no longer stop a text row short)
- Live window thumbnails on taskbar hover (Windows layout); name labels on dock hover (macOS)
- Fuller right-click menus (windows, app actions, admin, edit launcher, workspaces, force quit)
- Start / Launchpad quick menu (right-click): Task Manager, settings, terminal, power, …

### Start menu
- Same launcher as the taskbar (terminal apps, working folder, admin apps)
- Rebuilds itself when apps are installed or removed
- Search: keywords, program name, ranking, Enter, ↓, calculator, web search
- "New" badges and *Recently added*; optional *Recent & Recommended*
- Whisker icon view; Windows 11 opens on Pinned; drag to reorder pinned apps
- Exact category matching, accented A–Z headers, edge resizing

### Top bar
- Global menu shortcuts for GTK / GtkApplication apps (own GTK module)
- Window buttons left or right with all title-bar buttons following
- Menus that don't fit move into a » overflow menu; the app name stays readable

### System tray
- New sound popup (devices, now playing with album art and seek, per-app volume)
- Microphone-in-use indicator as its own tray icon
- Overflow menu: 5 columns, drag to rearrange, popups open beside it
- IBus as a movable modern tray icon
- Sound settings are now saved and restored correctly

### Fixes
- Panel heights no longer reset when switching layouts or restarting the panel
- Settings applied by a theme, an import or a page reset are no longer overwritten
  when the settings window closes
- Readable text on light themes (global menu, panel buttons, search boxes)
- Sound popup no longer widens with long media titles
- Win11 Start menu always opens on Pinned, also from the dock

### Clock & calendar
- Windows 7 / 10 / 11 and GNOME-macOS flyouts sized like the real ones
- Notifications in the GNOME flyout; date formats incl. custom; week numbers
- Exact ticking; follows time-zone changes; always opens on today
