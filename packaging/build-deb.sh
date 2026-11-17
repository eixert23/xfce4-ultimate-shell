#!/bin/sh
# Build a .deb of Ultimate Shell for the machine you run this on.
#   sh packaging/build-deb.sh
# Needs the build dependencies from README.md plus:  sudo apt install dpkg-dev
set -e

SRC=$(cd "$(dirname "$0")/.." && pwd)
VER=$(sed -n "s/^ *version: *'\([^']*\)'.*/\1/p" "$SRC/meson.build" | head -n1)
ARCH=$(dpkg --print-architecture)
PKG=xfce4-ultimate-shell
OUT="$SRC/${PKG}_${VER}_${ARCH}.deb"

for tool in meson ninja dpkg-deb dpkg-shlibdeps; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "Missing '$tool'. Install it first:  sudo apt install meson ninja-build dpkg-dev"; exit 1; }
done

# Build outside the source folder (VirtualBox shared folders cause clock-skew errors)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/${PKG}-deb.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
BUILD="$WORK/build"
STAGE="$WORK/stage"

echo "==> Building $PKG $VER ($ARCH)"
meson setup "$BUILD" "$SRC" --prefix=/usr --buildtype=release >/dev/null
ninja -C "$BUILD"
DESTDIR="$STAGE" meson install -C "$BUILD" --no-rebuild >/dev/null

# Strip debug symbols (smaller package)
find "$STAGE" -name '*.so' -exec strip --strip-unneeded {} \;

# Docs + copyright
DOC="$STAGE/usr/share/doc/$PKG"
install -d "$DOC"
install -m644 "$SRC/README.md"    "$DOC/README.md"
install -m644 "$SRC/CHANGELOG.md" "$DOC/CHANGELOG.md"
{
    echo "Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/"
    echo "Upstream-Name: $PKG"
    echo
    echo "Files: *"
    echo "License: GPL-2+"
    echo " On Debian systems the full text is in /usr/share/common-licenses/GPL-2."
} > "$DOC/copyright"

# Library dependencies, worked out from the built files
mkdir -p "$WORK/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" > "$WORK/debian/control"
SHLIBS=$(cd "$WORK" && dpkg-shlibdeps -O $(find "$STAGE" -name '*.so') 2>/dev/null \
         | sed -n 's/^shlibs:Depends=//p')
DEPENDS="xfce4-panel${SHLIBS:+, $SHLIBS}"

SIZE=$(du -sk "$STAGE/usr" | cut -f1)
install -d "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<CTRL
Package: $PKG
Version: $VER
Architecture: $ARCH
Maintainer: Ultimate Shell <noreply@localhost>
Installed-Size: $SIZE
Section: xfce
Priority: optional
Depends: $DEPENDS
Recommends: appmenu-gtk3-module, pavucontrol
Suggests: xfce4-taskmanager, bluez, upower, power-profiles-daemon
Description: macOS- and Windows-style desktop shell for the Xfce panel
 Ultimate Shell turns the Xfce panel into a complete desktop shell: a macOS
 layout (top bar, global menu, dock, Launchpad) or a Windows layout (Start
 menu, taskbar with live thumbnails), with its own system tray (sound,
 Bluetooth, drives, power), clock and calendar, notifications and workspaces.
CTRL

cat > "$STAGE/DEBIAN/postinst" <<'PI'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
    echo "Ultimate Shell installed. Restart the panel (xfce4-panel -r), then add it via"
    echo "Panel > Add New Items > Ultimate Shell."
fi
exit 0
PI
chmod 755 "$STAGE/DEBIAN/postinst"

dpkg-deb --build --root-owner-group "$STAGE" "$OUT" >/dev/null
echo "==> Done: $OUT"
echo "    Install with:  sudo apt install \"$OUT\""
