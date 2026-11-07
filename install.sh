#!/bin/sh
# All-in-one Linux installer for Expede (Debian/Ubuntu/Mint/Pop!_OS, Arch/CachyOS/Manjaro,
# Fedora/Nobara, openSUSE, Void, Alpine). Run from the repo or unpacked bundle:
#   ./install.sh                            uses ./xp and/or ./7 (or ~/.local/share/expede/*) if present
#   ./install.sh /path/to/winxp.iso [win7.iso]
set -e
cd "$(dirname "$0")"

echo "== Installing build tools and the apps Expede uses (needs sudo)"
if command -v apt-get >/dev/null 2>&1; then
	sudo apt-get update -qq
	WLR_PKG="libwlroots-0.18-dev"
	apt-cache show libwlroots-0.18-dev >/dev/null 2>&1 || WLR_PKG="libwlroots-dev"
	sudo apt-get install -y build-essential meson ninja-build pkg-config p7zip-full python3 fontconfig \
		"$WLR_PKG" wayland-protocols libwayland-dev libxkbcommon-dev libcairo2-dev \
		libpango1.0-dev libpixman-1-dev libinput-dev libgdk-pixbuf-2.0-dev libdrm-dev \
		foot mousepad network-manager bluez power-profiles-daemon pcmanfm-qt \
		qt6-wayland qtwayland5 fonts-dejavu-core alsa-utils brightnessctl wl-clipboard
	sudo apt-get install -y lxpolkit qt6-gtk-platformtheme qt5-gtk-platformtheme 2>/dev/null || true
elif command -v pacman >/dev/null 2>&1; then
	WLR_PKG="wlroots0.18"
	pacman -Si wlroots0.18 >/dev/null 2>&1 || WLR_PKG="wlroots"
	SEVENZ="7zip"
	pacman -Si 7zip >/dev/null 2>&1 || SEVENZ="p7zip"
	sudo pacman -S --needed --noconfirm base-devel meson ninja pkgconf "$SEVENZ" python fontconfig \
		"$WLR_PKG" wayland-protocols wayland libxkbcommon cairo pango pixman \
		libinput gdk-pixbuf2 libdrm foot mousepad lxsession networkmanager \
		bluez bluez-utils power-profiles-daemon pcmanfm-qt qt6-wayland qt5-wayland \
		ttf-dejavu alsa-utils brightnessctl wl-clipboard
elif command -v dnf >/dev/null 2>&1; then
	WLR_PKG="wlroots-devel"
	dnf -q info wlroots0.18-devel >/dev/null 2>&1 && WLR_PKG="wlroots0.18-devel" || true
	sudo dnf install -y gcc make meson ninja-build pkgconf-pkg-config p7zip p7zip-plugins python3 fontconfig \
		"$WLR_PKG" wayland-protocols-devel wayland-devel libxkbcommon-devel cairo-devel \
		pango-devel pixman-devel libinput-devel gdk-pixbuf2-devel libdrm-devel \
		foot mousepad lxpolkit NetworkManager bluez power-profiles-daemon pcmanfm-qt \
		qt6-qtwayland qt5-qtwayland dejavu-sans-fonts alsa-utils brightnessctl wl-clipboard
elif command -v zypper >/dev/null 2>&1; then
	sudo zypper --non-interactive install gcc make meson ninja pkgconf-pkg-config 7zip python3 fontconfig \
		wlroots-devel wayland-protocols-devel wayland-devel libxkbcommon-devel cairo-devel \
		pango-devel pixman-devel libinput-devel gdk-pixbuf-devel libdrm-devel \
		foot mousepad lxpolkit NetworkManager bluez power-profiles-daemon pcmanfm-qt \
		qt6-wayland libqt5-qtwayland dejavu-fonts alsa-utils brightnessctl wl-clipboard
elif command -v xbps-install >/dev/null 2>&1; then
	sudo xbps-install -Sy base-devel meson ninja pkg-config 7zip python3 fontconfig \
		wlroots0.18-devel wayland-protocols wayland-devel libxkbcommon-devel cairo-devel \
		pango-devel pixman-devel libinput-devel gdk-pixbuf-devel libdrm-devel \
		foot mousepad lxsession NetworkManager bluez power-profiles-daemon pcmanfm-qt \
		qt6-wayland qt5-wayland dejavu-fonts-ttf alsa-utils brightnessctl wl-clipboard
elif command -v apk >/dev/null 2>&1; then
	sudo apk add build-base meson ninja pkgconf 7zip python3 fontconfig \
		wlroots-dev wayland-protocols wayland-dev libxkbcommon-dev cairo-dev \
		pango-dev pixman-dev libinput-dev gdk-pixbuf-dev libdrm-dev \
		foot mousepad lxsession networkmanager bluez power-profiles-daemon pcmanfm-qt \
		qt6-qtwayland qt5-qtwayland font-dejavu alsa-utils brightnessctl wl-clipboard
else
	echo "Unknown package manager: make sure wlroots 0.18, wayland, cairo, pango, gdk-pixbuf2, libinput, libxkbcommon, meson, ninja, and 7z are installed." >&2
fi

SEVENZ_BIN="7z"
for c in 7z 7zz 7za; do
	if command -v "$c" >/dev/null 2>&1; then SEVENZ_BIN="$c"; break; fi
done

echo "== Windows theme files"
mkdir -p "$HOME/.local/share/expede" "$HOME/.local/share/fonts"
if [ -n "$1" ]; then
	for iso in "$@"; do
		if "$SEVENZ_BIN" l "$iso" sources/install.wim 2>/dev/null | grep -q install.wim; then
			python3 -I tools/w7rip.py "$iso"
		else
			python3 -I tools/xprip.py "$iso"
		fi
	done
fi
if [ -d xp ]; then
	rm -rf "$HOME/.local/share/expede/xp"
	cp -r xp "$HOME/.local/share/expede/xp"
fi
if [ -d 7 ]; then
	rm -rf "$HOME/.local/share/expede/7"
	cp -r 7 "$HOME/.local/share/expede/7"
fi
if [ -d "$HOME/.local/share/expede/xp" ]; then
	python3 -I tools/gtktheme.py "$HOME/.local/share/expede/xp" "$HOME/.local/share/themes"
	cp "$HOME/.local/share/expede/xp/fonts/"*.ttf "$HOME/.local/share/fonts/" 2>/dev/null || true
fi
if [ -d "$HOME/.local/share/expede/7" ]; then
	python3 -I tools/gtktheme.py "$HOME/.local/share/expede/7" "$HOME/.local/share/themes"
	cp "$HOME/.local/share/expede/7/fonts/"*.ttf "$HOME/.local/share/fonts/" 2>/dev/null || true
fi
if [ ! -d "$HOME/.local/share/expede/xp" ] && [ ! -d "$HOME/.local/share/expede/7" ]; then
	echo "No ripped files found: run ./install.sh /path/to/your/winxp.iso [win7.iso]" >&2
	exit 1
fi
fc-cache -f

# Command Prompt look for foot, only if you don't have your own foot config
if [ ! -e "$HOME/.config/foot/foot.ini" ]; then
	mkdir -p "$HOME/.config/foot"
	cp config/foot/foot.ini "$HOME/.config/foot/"
fi

echo "== Building"
rm -rf build
PKG_CONFIG_PATH="/usr/lib/wlroots0.18/pkgconfig:/usr/lib64/wlroots0.18/pkgconfig:${PKG_CONFIG_PATH}" \
	meson setup build --prefix=/usr --buildtype=release >/dev/null
ninja -C build
echo "== Installing (needs sudo)"
sudo ninja -C build install >/dev/null

echo
echo "Expede is installed. Log out, pick \"Expede\" on the login screen"
echo "(the session menu, usually a gear or your desktop's name), and log in."
echo "Settings: Start > Control Panel. Quit back to the login screen: Ctrl+Alt+Backspace."
