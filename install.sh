#!/bin/sh
# Install Expede on Debian 13 (trixie). Run from the unpacked bundle:
#   ./install.sh            uses ./xp if present (already ripped), else asks for your XP ISO
#   ./install.sh /path/to/winxp.iso
set -e
cd "$(dirname "$0")"

echo "== Installing build tools and the apps Expede uses (needs sudo)"
sudo apt-get update -qq
sudo apt-get install -y build-essential meson ninja-build pkg-config p7zip-full \
	libwlroots-0.18-dev wayland-protocols libwayland-dev libxkbcommon-dev libcairo2-dev \
	libpango1.0-dev libpixman-1-dev libinput-dev libgdk-pixbuf-2.0-dev libdrm-dev \
	foot mousepad lxpolkit network-manager bluez power-profiles-daemon pcmanfm-qt qt6-wayland qtwayland5 qt6-gtk-platformtheme qt5-gtk-platformtheme fonts-dejavu-core alsa-utils brightnessctl

echo "== Windows XP theme files"
mkdir -p "$HOME/.local/share/expede" "$HOME/.local/share/fonts"
if [ -n "$1" ]; then
	python3 -I tools/xprip.py "$1"
elif [ -d xp ]; then
	rm -rf "$HOME/.local/share/expede/xp"
	cp -r xp "$HOME/.local/share/expede/xp"
	python3 -I tools/gtktheme.py "$HOME/.local/share/expede/xp" "$HOME/.local/share/themes"
else
	echo "No ripped files here: run ./install.sh /path/to/your/winxp.iso" >&2
	exit 1
fi
cp "$HOME/.local/share/expede/xp/fonts/"*.ttf "$HOME/.local/share/fonts/"
fc-cache -f

# Command Prompt look for foot, only if you don't have your own foot config
if [ ! -e "$HOME/.config/foot/foot.ini" ]; then
	mkdir -p "$HOME/.config/foot"
	cp config/foot/foot.ini "$HOME/.config/foot/"
fi

echo "== Building"
rm -rf build
meson setup build --prefix=/usr --buildtype=release >/dev/null
ninja -C build
echo "== Installing (needs sudo)"
sudo ninja -C build install >/dev/null

echo
echo "Expede is installed. Log out, pick \"Expede\" on the login screen"
echo "(the session menu, usually a gear or your desktop's name), and log in."
echo "Settings: Start > Control Panel. Quit back to the login screen: Ctrl+Alt+Backspace."
