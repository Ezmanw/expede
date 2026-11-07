"""Build a GTK 3/4 theme from the ripped Luna bitmaps, so GTK apps' own
buttons, checkboxes, scroll bars, tabs and progress bars match the desktop.

Every widget state is cut out of the msstyles image strips into its own PNG
(plus a pixel-doubled @2 copy for 200% screens) and drawn with CSS
border-image using the same nine-slice margins the theme file gives.

    python3 -I tools/gtktheme.py [assets dir] [themes dir]
"""
import os
import struct
import sys
import zlib


def read_png(path):
    """Minimal PNG reader for the RGBA PNGs xprip writes (8-bit, colour type 6)."""
    data = open(path, 'rb').read()
    pos, w, h, idat = 8, 0, 0, b''
    while pos < len(data):
        n, tag = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + n]
        if tag == b'IHDR':
            w, h = struct.unpack_from('>II', body)
        elif tag == b'IDAT':
            idat += body
        pos += 12 + n
    raw = zlib.decompress(idat)
    stride = w * 4
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - 4] if x >= 4 else 0
            b = prev[x]
            c = prev[x - 4] if x >= 4 else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(bytes(line))
        prev = line
    return w, h, b''.join(rows)


def write_png(path, w, h, rgba):
    stride = w * 4
    raw = b''.join(b'\x00' + rgba[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(tag, d):
        return struct.pack('>I', len(d)) + tag + d + struct.pack('>I', zlib.crc32(tag + d) & 0xffffffff)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def crop(img, x0, y0, cw, ch):
    w, h, px = img
    out = bytearray()
    for y in range(y0, y0 + ch):
        out += px[(y * w + x0) * 4:(y * w + x0 + cw) * 4]
    return cw, ch, bytes(out)


def frame(img, count, i, vertical=True):
    w, h, _ = img
    if vertical:
        fh = h // count
        return crop(img, 0, i * fh, w, fh)
    fw = w // count
    return crop(img, i * fw, 0, fw, h)


def overlay(base, glyph):
    """Draw glyph centred over base (alpha blend)."""
    bw, bh, bp = base
    gw, gh, gp = glyph
    out = bytearray(bp)
    ox, oy = (bw - gw) // 2, (bh - gh) // 2
    for y in range(gh):
        for x in range(gw):
            g = gp[(y * gw + x) * 4:(y * gw + x) * 4 + 4]
            if g[3] == 0:
                continue
            o = ((oy + y) * bw + ox + x) * 4
            a = g[3] / 255
            for c in range(3):
                out[o + c] = int(g[c] * a + out[o + c] * (1 - a))
            out[o + 3] = max(out[o + 3], g[3])
    return bw, bh, bytes(out)


def double(img):
    w, h, px = img
    out = bytearray()
    for y in range(h * 2):
        row = px[(y // 2) * w * 4:(y // 2 + 1) * w * 4]
        out += b''.join(row[x * 4:x * 4 + 4] * 2 for x in range(w))
    return w * 2, h * 2, bytes(out)


class Theme:
    def __init__(self, luna, assets):
        self.luna, self.assets = luna, assets
        os.makedirs(assets, exist_ok=True)

    def img(self, name):
        return read_png(os.path.join(self.luna, name + '.png'))

    def save(self, name, im):
        write_png(os.path.join(self.assets, name + '.png'), *im)
        write_png(os.path.join(self.assets, name + '@2.png'), *double(im))

    def frames(self, src, count, names, vertical=True):
        im = self.img(src)
        for i, n in enumerate(names):
            if n:
                self.save(n, frame(im, count, i, vertical))


def url(name):
    return f'-gtk-scaled(url("assets/{name}.png"), url("assets/{name}@2.png"))'


def nine(name, t, r, b, l):
    """Nine-slice border-image. GTK 3 draws the slices at the border's width
    (it has no border-image-width), so the border is set to match: small
    slices that still hold XP's rounded corners, and a stretched middle that
    keeps the vertical gradient."""
    return (f'border-style: solid; border-color: transparent; border-width: {t}px {r}px {b}px {l}px; '
            f'border-image-source: {url(name)}; border-image-slice: {t} {r} {b} {l} fill; '
            f'border-image-repeat: stretch;')


def build(xp, themes):
    luna = os.path.join(xp, 'luna')
    out = os.path.join(themes, 'Expede-XP')
    t = Theme(luna, os.path.join(out, 'gtk-3.0', 'assets'))
    states4 = ['normal', 'hover', 'active', 'disabled']
    t.frames('button', 5, [f'button-{s}' for s in states4] + ['button-default'])
    t.frames('checkbox13', 12, [f'check-off-{s}' for s in states4] + [f'check-on-{s}' for s in states4] +
             [f'check-mixed-{s}' for s in states4])
    t.frames('radiobutton13', 8, [f'radio-off-{s}' for s in states4] + [f'radio-on-{s}' for s in states4])
    t.frames('scrollthumbvertical', 4, [f'thumb-v-{s}' for s in states4])
    t.frames('scrollthumbhorizontal', 4, [f'thumb-h-{s}' for s in states4])
    t.frames('scrollshaftvertical', 4, ['trough-v', None, None, None])
    t.frames('scrollshafthorizontal', 4, ['trough-h', None, None, None])
    t.frames('tabitemtop', 5, ['tab-normal', 'tab-hover', 'tab-checked', 'tab-disabled', None])
    t.frames('listviewheader', 5, ['header-normal', 'header-hover', 'header-active', None, None])
    t.save('pane', t.img('tabpaneedge'))
    t.save('progress-chunk', t.img('progresschunk'))
    t.save('progress-track', t.img('progresstrack'))
    t.save('toolbar', t.img('toolbarbackground'))
    # scroll arrows: background with its glyph on top; up, down, left, right x n/h/p/d
    arrows, glyphs = t.img('scrollarrows'), t.img('scrollarrowglyphs')
    for d, dname in enumerate(['up', 'down', 'left', 'right']):
        for s, sname in enumerate(states4):
            i = d * 4 + s
            t.save(f'arrow-{dname}-{sname}', overlay(frame(arrows, 16, i), frame(glyphs, 16, i)))
    combo, cg = t.img('combobutton'), t.img('combobuttonglyph')
    for s, sname in enumerate(states4):
        t.save(f'combo-{sname}', overlay(frame(combo, 4, s), frame(cg, 4, s)))
    for d in ('up', 'down'):
        bg, gl = t.img(f'spinbuttonbackground{d}'), t.img(f'spin{d}glyph')
        for s, sname in enumerate(states4):
            t.save(f'spin-{d}-{sname}', overlay(frame(bg, 4, s), frame(gl, 4, s)))
    exp = t.img('treeexpandcollapse')
    t.save('expander-closed', frame(exp, 2, 0))
    t.save('expander-open', frame(exp, 2, 1))

    css = CSS
    for k, v in {
        '@BUTTON@': nine('button-normal', 4, 4, 4, 4), '@BUTTON_HOVER@': nine('button-hover', 4, 4, 4, 4),
        '@BUTTON_ACTIVE@': nine('button-active', 4, 4, 4, 4), '@BUTTON_DISABLED@': nine('button-disabled', 4, 4, 4, 4),
        '@BUTTON_DEFAULT@': nine('button-default', 4, 4, 4, 4),
        '@THUMB_V@': nine('thumb-v-normal', 3, 3, 3, 3), '@THUMB_V_HOVER@': nine('thumb-v-hover', 3, 3, 3, 3),
        '@THUMB_V_ACTIVE@': nine('thumb-v-active', 3, 3, 3, 3),
        '@THUMB_H@': nine('thumb-h-normal', 3, 3, 3, 3), '@THUMB_H_HOVER@': nine('thumb-h-hover', 3, 3, 3, 3),
        '@THUMB_H_ACTIVE@': nine('thumb-h-active', 3, 3, 3, 3),
        '@TROUGH_V@': nine('trough-v', 0, 1, 0, 1), '@TROUGH_H@': nine('trough-h', 1, 0, 1, 0),
        '@TAB@': nine('tab-normal', 3, 3, 3, 3), '@TAB_HOVER@': nine('tab-hover', 3, 3, 3, 3),
        '@TAB_CHECKED@': nine('tab-checked', 3, 3, 3, 3),
        '@PANE@': nine('pane', 3, 3, 3, 3), '@HEADER@': nine('header-normal', 3, 3, 3, 3),
        '@HEADER_HOVER@': nine('header-hover', 3, 3, 3, 3), '@HEADER_ACTIVE@': nine('header-active', 3, 3, 3, 3),
        '@TRACK@': nine('progress-track', 3, 3, 3, 3), '@TOOLBAR@': nine('toolbar', 0, 0, 0, 0),
    }.items():
        css = css.replace(k, v)
    for s in states4:
        css = css.replace(f'@URL_check-off-{s}@', url(f'check-off-{s}'))
    # any remaining @URL_name@ placeholders
    import re
    css = re.sub(r'@URL_([a-z0-9-]+)@', lambda m: url(m.group(1)), css)

    gtk3 = os.path.join(out, 'gtk-3.0')
    with open(os.path.join(gtk3, 'gtk.css'), 'w') as f:
        f.write(css)
    # GTK 4: same rules; its scroll bars have no stepper buttons
    gtk4 = os.path.join(out, 'gtk-4.0')
    os.makedirs(gtk4, exist_ok=True)
    if os.path.lexists(os.path.join(gtk4, 'assets')):
        os.remove(os.path.join(gtk4, 'assets'))
    os.symlink('../gtk-3.0/assets', os.path.join(gtk4, 'assets'))
    with open(os.path.join(gtk4, 'gtk.css'), 'w') as f:
        f.write(re.sub(r'^\s*-GtkScrollbar[^\n]*\n', '', css, flags=re.M))
    with open(os.path.join(out, 'index.theme'), 'w') as f:
        f.write('[Desktop Entry]\nType=X-GNOME-Metatheme\nName=Expede XP\n'
                'Comment=Windows XP Luna, drawn from your own Windows files\n\n'
                '[X-GNOME-Metatheme]\nGtkTheme=Expede-XP\n')
    return out


CSS = r'''/* Expede XP: Luna for GTK, generated by tools/gtktheme.py from your ripped theme */

@define-color theme_bg_color #ece9d8;
@define-color theme_fg_color #000000;
@define-color theme_base_color #ffffff;
@define-color theme_text_color #000000;
@define-color theme_selected_bg_color #316ac5;
@define-color theme_selected_fg_color #ffffff;
@define-color insensitive_fg_color #aca899;
@define-color borders #7f9db9;
@define-color window_bg_color #ece9d8;
@define-color window_fg_color #000000;
@define-color view_bg_color #ffffff;
@define-color view_fg_color #000000;
@define-color accent_bg_color #316ac5;
@define-color accent_color #316ac5;
@define-color accent_fg_color #ffffff;

* {
	font-family: Tahoma, sans-serif;
	font-size: 8pt;
	outline-color: alpha(#000000, 0.6);
	outline-style: dotted;
	outline-width: 1px;
	outline-offset: -3px;
	-gtk-secondary-caret-color: #000000;
}

window, .background, dialog, popover > contents, popover.background {
	background-color: #ece9d8;
	color: #000000;
}

label:disabled, *:disabled { color: #aca899; }

/* selections */
*:selected, selection, treeview.view:selected, row:selected, list > row:selected,
textview text selection, entry selection, label selection {
	background-color: #316ac5;
	color: #ffffff;
}

/* ---------------- buttons */
button {
	@BUTTON@
	background: none;
	padding: 1px 5px;
	min-height: 15px;
	min-width: 61px;
	color: #000000;
	box-shadow: none;
	text-shadow: none;
}
button:hover { @BUTTON_HOVER@ }
button:active, button:checked { @BUTTON_ACTIVE@ }
button:disabled { @BUTTON_DISABLED@ color: #aca899; }
button.default, button.suggested-action { @BUTTON_DEFAULT@ }
button.image-button, button.circular, toolbar button, headerbar button, .linked button, spinbutton button,
combobox button, scrollbar button, notebook tab button, button.flat, button.titlebutton {
	min-width: 0;
}
button.flat, toolbar button, .toolbar button {
	border-image: none; border: 4px solid transparent; background: transparent;
}
button.flat:hover, toolbar button:hover, .toolbar button:hover {
	@BUTTON_HOVER@
}

/* ---------------- check boxes and radio buttons */
check, radio { min-width: 13px; min-height: 13px; margin: 0 4px 0 0; border: none; background: none; box-shadow: none; }
check { -gtk-icon-source: @URL_check-off-normal@; }
check:hover { -gtk-icon-source: @URL_check-off-hover@; }
check:active { -gtk-icon-source: @URL_check-off-active@; }
check:disabled { -gtk-icon-source: @URL_check-off-disabled@; }
check:checked { -gtk-icon-source: @URL_check-on-normal@; }
check:checked:hover { -gtk-icon-source: @URL_check-on-hover@; }
check:checked:active { -gtk-icon-source: @URL_check-on-active@; }
check:checked:disabled { -gtk-icon-source: @URL_check-on-disabled@; }
check:indeterminate { -gtk-icon-source: @URL_check-mixed-normal@; }
radio { -gtk-icon-source: @URL_radio-off-normal@; }
radio:hover { -gtk-icon-source: @URL_radio-off-hover@; }
radio:active { -gtk-icon-source: @URL_radio-off-active@; }
radio:disabled { -gtk-icon-source: @URL_radio-off-disabled@; }
radio:checked { -gtk-icon-source: @URL_radio-on-normal@; }
radio:checked:hover { -gtk-icon-source: @URL_radio-on-hover@; }
radio:checked:active { -gtk-icon-source: @URL_radio-on-active@; }
radio:checked:disabled { -gtk-icon-source: @URL_radio-on-disabled@; }

/* ---------------- text fields */
entry, spinbutton, textview, .view, treeview.view, list, iconview, placessidebar list {
	background-color: #ffffff;
	color: #000000;
}
entry, spinbutton:not(.vertical) {
	border: 1px solid #7f9db9;
	border-radius: 0;
	padding: 2px 3px;
	min-height: 17px;
	box-shadow: none;
	background-image: none;
}
entry:disabled { background-color: #ebebe4; }
textview text, textview > text, .view text, sourceview text, iconview, treeview.view, list {
	background-color: #ffffff;
	color: #000000;
}
spinbutton entry { border: none; }
spinbutton button { padding: 0 2px; min-width: 16px; }

/* ---------------- combo boxes */
combobox button.combo, dropdown > button {
	border-image: none;
	border: 1px solid #7f9db9;
	background-color: #ffffff;
	background-image: @URL_combo-normal@;
	background-repeat: no-repeat;
	background-position: right center;
	background-size: 15px calc(100% - 2px);
	padding: 1px 20px 1px 3px;
}
combobox button.combo:hover, dropdown > button:hover { background-image: @URL_combo-hover@; }
combobox button.combo arrow, dropdown > button arrow { opacity: 0; }

/* ---------------- scroll bars (17px, with XP's arrow buttons) */
scrollbar {
	-GtkScrollbar-has-backward-stepper: true;
	-GtkScrollbar-has-forward-stepper: true;
	background: none;
	border: none;
}
scrollbar.vertical { @TROUGH_V@ }
scrollbar.horizontal { @TROUGH_H@ }
scrollbar trough { background: none; border: none; min-width: 17px; min-height: 17px; }
scrollbar slider { background: none; min-width: 9px; min-height: 9px; border-radius: 0; margin: 0; }
scrollbar.vertical slider { @THUMB_V@ min-height: 22px; }
scrollbar.vertical slider:hover { @THUMB_V_HOVER@ }
scrollbar.vertical slider:active { @THUMB_V_ACTIVE@ }
scrollbar.horizontal slider { @THUMB_H@ min-width: 22px; }
scrollbar.horizontal slider:hover { @THUMB_H_HOVER@ }
scrollbar.horizontal slider:active { @THUMB_H_ACTIVE@ }
scrollbar button { border-image: none; border: none; padding: 0; min-width: 17px; min-height: 17px; background-size: 17px 17px; }
scrollbar button image, scrollbar button arrow { opacity: 0; }
scrollbar.vertical button.up { background-image: @URL_arrow-up-normal@; }
scrollbar.vertical button.up:hover { background-image: @URL_arrow-up-hover@; }
scrollbar.vertical button.up:active { background-image: @URL_arrow-up-active@; }
scrollbar.vertical button.down { background-image: @URL_arrow-down-normal@; }
scrollbar.vertical button.down:hover { background-image: @URL_arrow-down-hover@; }
scrollbar.vertical button.down:active { background-image: @URL_arrow-down-active@; }
scrollbar.horizontal button.up { background-image: @URL_arrow-left-normal@; }
scrollbar.horizontal button.up:hover { background-image: @URL_arrow-left-hover@; }
scrollbar.horizontal button.down { background-image: @URL_arrow-right-normal@; }
scrollbar.horizontal button.down:hover { background-image: @URL_arrow-right-hover@; }
scrollbar.overlay-indicator:not(.dragging):not(.hovering) slider { min-width: 6px; min-height: 6px; }

/* ---------------- tabs */
notebook > header { background: none; border: none; padding: 0 2px; }
notebook > header tab {
	@TAB@
	background: none; padding: 1px 6px; min-height: 14px; margin: 2px -1px 0 0;
}
notebook > header tab:hover { @TAB_HOVER@ }
notebook > header tab:checked { @TAB_CHECKED@ margin-top: 0; padding-bottom: 3px; }
notebook > stack:not(:only-child) { @PANE@ }

/* ---------------- tree and list headers */
treeview.view header button, columnview > header > button, .view header button {
	@HEADER@
	background: none; padding: 1px 4px; min-width: 0; min-height: 14px;
}
treeview.view header button:hover { @HEADER_HOVER@ }
treeview.view header button:active { @HEADER_ACTIVE@ }
treeview.view { -GtkTreeView-expander-size: 9; }
treeview.view.expander { -gtk-icon-source: @URL_expander-closed@; }
treeview.view.expander:checked { -gtk-icon-source: @URL_expander-open@; }

/* ---------------- progress bars: XP's green blocks */
progressbar trough { @TRACK@ min-height: 9px; background: none; }
progressbar progress {
	background-image: @URL_progress-chunk@;
	background-repeat: repeat-x;
	background-size: 10px 100%;
	border: none; border-radius: 0; min-height: 11px; margin: 2px;
}

/* ---------------- menus: white, blue highlight */
menubar { background-color: #ece9d8; padding: 0; border: none; box-shadow: none; }
menubar > menuitem, menubar > item { padding: 2px 7px; }
menubar > menuitem:hover, menubar > item:hover { background-color: #316ac5; color: #ffffff; }
menu, .menu, .context-menu, popover.menu > contents {
	background-color: #ffffff;
	border: 1px solid #aca899;
	padding: 2px;
	border-radius: 0;
	box-shadow: 2px 2px 3px alpha(#000000, 0.25);
}
menu menuitem, popover.menu modelbutton { padding: 3px 18px 3px 20px; min-height: 16px; border-radius: 0; }
menu menuitem:hover, popover.menu modelbutton:hover { background-color: #316ac5; color: #ffffff; }
menu separator, popover.menu separator { background-color: #aca899; min-height: 1px; margin: 3px 1px; }

/* ---------------- toolbars and status bars */
toolbar, .toolbar, .inline-toolbar, actionbar > revealer > box { @TOOLBAR@ padding: 2px; }
statusbar { background-color: #ece9d8; border-top: 1px solid #aca899; padding: 2px 6px; }
headerbar, .titlebar:not(headerbar) { background: #ece9d8; border-bottom: 1px solid #aca899; min-height: 28px; box-shadow: none; }

/* ---------------- tooltips: XP's pale yellow */
tooltip, tooltip.background { background-color: #ffffe1; color: #000000; border: 1px solid #000000; border-radius: 0; box-shadow: none; }
tooltip * { color: #000000; }

/* ---------------- frames and separators */
frame > border, .frame { border: 1px solid #d0d0bf; border-radius: 0; }
separator { background-color: #aca899; min-width: 1px; min-height: 1px; }
scale trough { background-color: #ffffff; border: 1px solid #7f9db9; min-height: 4px; min-width: 4px; border-radius: 0; }
scale slider { @BUTTON@ min-width: 3px; min-height: 11px; background: none; margin: -8px; }
switch { background-color: #ffffff; border: 1px solid #7f9db9; border-radius: 0; }
switch:checked { background-color: #316ac5; }
switch slider { @BUTTON@ background: none; border-radius: 0; }
'''

if __name__ == '__main__':
    xp = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/.local/share/expede/xp')
    themes = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/.local/share/themes')
    print('gtk theme ->', build(xp, themes))
