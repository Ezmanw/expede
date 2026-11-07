<div align="center">

# Expede

**A Wayland desktop for Linux that looks, sounds and behaves like Windows.**

*"XP" + "DE" backwards.*

Built from scratch on [wlroots](https://gitlab.freedesktop.org/wlroots/wlroots) · C · Debian 13 and up

</div>

---

Expede doesn't imitate the Windows look with hand-drawn copies. It reads the
**real theme files** (bitmaps, metrics, fonts, sounds, pointers) from a Windows
install disc **you own**, and draws the desktop from them, the way Windows'
own theme engine does.

No Microsoft files are in this repository, and none are ever shipped. You point
the ripper at your ISO, and it writes everything to your home folder.

> **No screenshots here, on purpose:** any screenshot would show Microsoft's
> artwork. Build it and see. It takes about a minute.

## What you get

### The desktop
- **Luna window frames:** XP's title bars, borders and min/max/close buttons, with hover and pressed states, and the paler look for inactive windows.
- **The taskbar:** the green **start** button, task buttons that focus or minimise their window, and the notification area with the volume icon and clock.
- **The Start menu:** XP's two-column layout with your account picture, pinned and recent programs, places, and **All Programs** as XP's cascading list of every installed app.
- **Desktop:** XP's wallpapers, desktop icons with two-line labels, and XP's selection highlight.
- **XP's pointers**, including the I-beam, which is outlined so it shows on dark backgrounds.

### The session
- **Welcome screen:** the click-your-name screen, laid out from the layout file inside XP's own logon program, followed by *"welcome"* and "Loading your personal settings...".
- **Log Off, and the Turn Off Computer dialog:** Stand By, Turn Off and Restart.
- **System sounds:** startup, log on, log off, shutdown, minimise, restore, and the "ding" when you set the volume.

### Explorer
Expede's own file manager, drawn with the same theme bitmaps:

- **Navigation:**
  - XP's toolbar: Back, Forward, Up, Search, Folders and Views.
  - An address bar you can type into.
  - The blue task pane: File and Folder Tasks, Other Places, and a Details box that shows what's selected.
  - Or a **Folders** tree in place of the task pane.
- **Views:** Tiles, Icons, List, and Details with sortable columns.
- **Working with files:** XP right-click menus, cut / copy / paste (with Explorer's "Copy of" naming), rename in place, New Folder, and Properties.
- **Recycle Bin:** Restore and Empty. It uses the standard Linux trash, so other file managers see the same items.
- **My Computer:** your drives with their free space, and **Search Companion** for finding files.
- **Shortcuts:** F2, Delete (Shift+Delete deletes permanently), Ctrl+A/C/X/V, Backspace, Alt+arrows, Ctrl+L, Ctrl+Shift+N and type-to-select.

### The Control Panel
XP's Control Panel, laid out the same way:

- **Category view:** pick a category such as *Performance and Maintenance* to see its Control Panel icons. Classic View lists every applet at once.
- **Each applet is an XP property sheet** with tabs and OK / Cancel / Apply. Changes apply straight away, with no logging out.

| Applet | What it does |
|---|---|
| Display | 29 XP wallpapers with a preview, size of text and items, screen resolution and refresh rate for each monitor |
| Taskbar and Start Menu | window snapping |
| Network Connections | Wi-Fi on and off, available networks with signal bars, connect with a key, connection status (NetworkManager) |
| Add or Remove Programs | every installed program, with Remove (apt or flatpak, in a terminal so you can see it) |
| Program Access and Defaults | which program opens web links, e-mail, text, pictures, music, video, folders and PDFs |
| Sounds and Audio Devices | live volume and mute, sound scheme, choice of speaker and microphone, microphone level |
| System | your OS, kernel, CPU, memory and computer name |
| Power Options | turn off the monitor after idling (videos keep it on), power profiles, battery meter |
| Mouse | pointer speed, tap to click, natural scrolling |
| Bluetooth Devices | turn it on and off, add (scan), connect, disconnect and remove devices |
| Date and Time | 12/24-hour clock, time zone, keyboard layout |
| User Accounts | XP's account pictures |

Settings that need your password (like the time zone) ask through an XP-styled prompt (lxpolkit).

### Apps match too
The ripper also builds a **GTK 3/4 theme** from the same Luna bitmaps. It covers buttons, checkboxes and radio buttons, scroll bars with XP's arrows, tabs, green block progress bars, combo boxes, list headers, menus and tooltips. Expede applies it to apps started inside it, so other desktop sessions keep their own look. Qt apps (KDE apps, PCManFM-Qt) follow it too, through Qt's GTK platform theme. GNOME's libadwaita apps keep their own style.

### Works like a modern desktop underneath
- **HiDPI:** the scale is picked from your screen's DPI (100–300%). The bitmaps are pixel-doubled so they stay crisp, and apps render sharp through fractional scaling.
- **Volume and brightness keys**, through PipeWire, PulseAudio or ALSA, whichever is installed.
- **App frames:** apps that draw their own title bars (Chromium, GNOME/GTK4, Electron) are left alone. Everything else gets a Luna frame.
- **Also:** fullscreen video, touchpad tap-to-click and natural scrolling, middle-click paste, apps raising their own windows, and menus that stay on screen.
- **Windows 7-style snapping:** drag a window to the top or sides. It's optional and off by default, because XP didn't have it.

## Getting started

You need **Debian 13** (or anything with wlroots 0.18) and a **Windows XP install ISO**.

```sh
git clone https://github.com/Ezmanw/expede
cd expede
./install.sh /path/to/your/winxp.iso
```

That installs the build tools and a few apps (foot as Command Prompt,
Mousepad as Notepad), rips your ISO into
`~/.local/share/expede/xp`, builds Expede and installs it.

Then log out, choose **Expede** from your login screen's session menu, and log in.

| Key | Does |
|---|---|
| Win | Start menu (Esc closes it) |
| Alt+Tab / Alt+F4 | switch / close windows |
| Win+R / Win+E | Command Prompt / file manager |
| Ctrl+Alt+Backspace | quit Expede |

## How the ripping works

`tools/xprip.py` is plain Python plus `7z`. It reads the compressed files on the
XP disc, then pulls resources out of the Windows binaries inside them:

- `luna.msstyles`: 186 theme bitmaps and the NormalBlue theme file that says how to stretch them
- `shell32.dll`, `explorer.exe`, `stobject.dll`: icons, the start flag, the speaker
- `user32.dll`: pointers, written out as an Xcursor theme at 1×, 1.5× and 2×
- `logonui.exe`, `msgina.dll`: the Welcome screen and Turn Off dialog
- fonts (Tahoma, Trebuchet MS, Franklin Gothic, Arial), wallpapers, account pictures, sounds

The script decodes the image formats itself (BMP, RLE, ICO and CUR) and needs nothing else installed.

## Settings file

The Control Panel writes `~/.config/expede/expede.conf`, which you can also edit by hand:

```ini
scale = auto              # or 100, 125, 150, 175, 200
wallpaper = Tulips.jpg
wallpaper_style = auto    # stretch, center, tile
snap = false
tap_to_click = true
natural_scroll = true
pointer_speed = 0         # -100 … 100
screen_off = 10           # minutes; 0 = never
output.HDMI-A-1.mode = 1920x1080@60
sounds = true
clock = 12                # or 24
keyboard_layout = gb      # any XKB layout, e.g. us(dvorak)
user_picture = duck
```

After editing it by hand, run `pkill -HUP -x expede` to apply.

## Hacking on it

```
src/main.c      window management, input, outputs (tinywl-based)
src/theme.c     the Luna theme engine: msstyles ini, nine-slice drawing, text
src/shell.c     desktop, taskbar, Start menu, volume pop-up
src/session.c   Welcome screen, log off, Turn Off dialog, sounds
src/settings.c  the Control Panel (a separate Wayland program)
src/explorer.c  Explorer
src/ui.c        the toolkit Explorer is built on: windows, widgets, keyboard
src/apps.c      installed apps from .desktop files
src/volume.c    wpctl / pactl / amixer
tools/xprip.py  the ripper
tools/gtktheme.py  builds the GTK theme from the ripped bitmaps
vm/             a QEMU test VM that boots straight into Expede
```

`vm/run.sh` runs a Debian VM headless, and `vm/qmp.py` takes screenshots and clicks in it.

## Roadmap

- **Windows 7**, with the Frutiger Aero glass effect as a toggle and an option to mix XP and 7 parts

## License

MIT for Expede's own code. Windows and its artwork belong to Microsoft. Expede
only ever reads them from your own copy.
