#!/usr/bin/env python3
"""xprip: pull the Windows XP look out of a user-supplied XP install ISO.

Nothing extracted here is ever redistributed with Expede: the files land in the
user's own data dir and are only read at runtime.

    python3 -I tools/xprip.py /path/to/winxp.iso [outdir]

Needs 7z (p7zip) on PATH. Everything else is plain Python.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

# --- tiny PNG writer ---------------------------------------------------------

def write_png(path, w, h, rgba):
    """rgba: bytes of length w*h*4, top row first."""
    stride = w * 4
    raw = b''.join(b'\x00' + rgba[y * stride:(y + 1) * stride] for y in range(h))

    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(raw, 9)))
        f.write(chunk(b'IEND', b''))

# --- DIB decoding (BMP files and icon images share it) -----------------------

def rle_expand(data, p, w, h, bits):
    """BI_RLE8 / BI_RLE4 -> uncompressed 8bpp rows (bottom-up, DWORD-aligned)."""
    stride = (w + 3) & ~3
    out = bytearray(stride * h)
    x = y = 0
    while y < h and p + 1 < len(data):
        n, v = data[p], data[p + 1]
        p += 2
        if n:  # encoded run
            for i in range(n):
                if x < w:
                    out[y * stride + x] = v if bits == 8 else (v >> 4 if i % 2 == 0 else v & 15)
                x += 1
        elif v == 0:
            x, y = 0, y + 1
        elif v == 1:
            break
        elif v == 2:
            x, y = x + data[p], y + data[p + 1]
            p += 2
        else:  # absolute run of v pixels
            nbytes = v if bits == 8 else (v + 1) // 2
            for i in range(v):
                b = data[p + (i if bits == 8 else i // 2)]
                if x < w:
                    out[y * stride + x] = b if bits == 8 else (b >> 4 if i % 2 == 0 else b & 15)
                x += 1
            p += nbytes + (nbytes & 1)
    return bytes(out)


def decode_dib(data, off=0, icon=False):
    """Decode a BITMAPINFOHEADER DIB at data[off:]. Returns (w, h, rgba, bpp).

    icon=True: height is doubled and an AND mask follows the colour bits.
    """
    hsize, w, h, planes, bpp, comp = struct.unpack_from('<IiiHHI', data, off)
    if comp not in (0, 1, 2, 3):
        raise ValueError(f'DIB compression {comp} not supported')
    ncolors = struct.unpack_from('<I', data, off + 32)[0]
    bottom_up = h > 0
    h = abs(h)
    if icon:
        h //= 2
    p = off + hsize
    if comp == 3 and hsize == 40:
        p += 12  # BI_BITFIELDS masks; XP uses the standard BGRA order anyway
    palette = []
    if bpp <= 8:
        n = ncolors or (1 << bpp)
        for i in range(n):
            b, g, r, _ = data[p + i * 4:p + i * 4 + 4]
            palette.append((r, g, b))
        p += n * 4
    if comp in (1, 2):
        data, p, bpp = rle_expand(data, p, w, h, 8 if comp == 1 else 4), 0, 8
    stride = ((w * bpp + 31) // 32) * 4
    px = bytearray(w * h * 4)
    for row in range(h):
        src = p + row * stride
        y = h - 1 - row if bottom_up else row
        for x in range(w):
            o = (y * w + x) * 4
            if bpp == 32:
                b, g, r, a = data[src + x * 4:src + x * 4 + 4]
                px[o:o + 4] = bytes((r, g, b, a))
            elif bpp == 24:
                b, g, r = data[src + x * 3:src + x * 3 + 3]
                px[o:o + 4] = bytes((r, g, b, 255))
            elif bpp == 16:
                v = struct.unpack_from('<H', data, src + x * 2)[0]
                r, g, b = (v >> 10) & 31, (v >> 5) & 31, v & 31
                px[o:o + 4] = bytes((r * 255 // 31, g * 255 // 31, b * 255 // 31, 255))
            else:
                per = 8 // bpp
                byte = data[src + x // per]
                shift = (per - 1 - x % per) * bpp
                idx = (byte >> shift) & ((1 << bpp) - 1)
                r, g, b = palette[idx] if idx < len(palette) else (0, 0, 0)
                px[o:o + 4] = bytes((r, g, b, 255))
    if bpp == 32 and not any(px[3::4]):
        px[3::4] = b'\xff' * (w * h)  # 32bpp with an empty alpha channel means opaque
    if icon:
        mstride = ((w + 31) // 32) * 4
        mp = p + stride * h
        has_alpha = bpp == 32 and any(px[3::4])
        if not has_alpha and mp + mstride * h <= len(data):
            for row in range(h):
                y = h - 1 - row if bottom_up else row
                for x in range(w):
                    if (data[mp + row * mstride + x // 8] >> (7 - x % 8)) & 1:
                        o = (y * w + x) * 4
                        # AND=1 with a non-black XOR pixel inverts the screen: mark it for
                        # the cursor code (alpha 1, invisible if anything else sees it)
                        px[o:o + 4] = b'\xff\x00\xff\x01' if any(px[o:o + 3]) else b'\x00\x00\x00\x00'
    return w, h, bytes(px), bpp


def bmp_to_png(src, dst, key=(255, 0, 255)):
    data = open(src, 'rb').read()
    if data[:2] == b'BM':
        data = data[14:]
    w, h, px, bpp = decode_dib(data)
    if key == 'corner':  # flat background: whatever colour the top-left pixel is
        key = tuple(px[0:3])
    if bpp < 32 and key:
        px = bytearray(px)
        for i in range(0, len(px), 4):
            if tuple(px[i:i + 3]) == key:
                px[i:i + 4] = b'\x00\x00\x00\x00'
        px = bytes(px)
    write_png(dst, w, h, px)

# --- PE icon groups ----------------------------------------------------------

def icon_from_group(resdir, group_id, size):
    """Best image of the wanted size from a GROUP_ICON in a 7z-extracted .rsrc."""
    grp = open(os.path.join(resdir, 'GROUP_ICON', str(group_id)), 'rb').read()
    _, _, count = struct.unpack_from('<HHH', grp, 0)
    best = None
    for i in range(count):
        w, h, _, _, _, bpp, _, rid = struct.unpack_from('<BBBBHHIH', grp, 6 + i * 14)
        w = w or 256
        score = (w == size, bpp, -abs(w - size))
        if best is None or score > best[0]:
            best = (score, rid)
    rid = best[1]
    for name in (str(rid), str(rid) + '.ico'):
        p = os.path.join(resdir, 'ICON', name)
        if os.path.exists(p):
            data = open(p, 'rb').read()
            if data[:4] == b'\x00\x00\x01\x00':  # 7z wraps the image in a one-entry .ico
                data = data[struct.unpack_from('<I', data, 18)[0]:]
            return decode_dib(data, icon=True)
    raise FileNotFoundError(f'icon {rid} of group {group_id}')

# --- cursors: user32.dll CURSOR resources -> an Xcursor theme ------------------

def scale_nearest(w, h, rgba, f):
    """Pixel-scale an image by f with nearest neighbour (keeps XP's crisp pixels)."""
    nw, nh = round(w * f), round(h * f)
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        sy = min(h - 1, int(y / f))
        for x in range(nw):
            sx = min(w - 1, int(x / f))
            o, i = (y * nw + x) * 4, (sy * w + sx) * 4
            out[o:o + 4] = rgba[i:i + 4]
    return nw, nh, bytes(out)


def write_xcursor(path, images):
    """Xcursor file holding one image per nominal size; images = [(w, h, hx, hy, rgba)].
    Pixels are premultiplied ARGB, little endian."""
    chunks = []
    for w, h, hx, hy, rgba in images:
        pix = bytearray()
        for i in range(0, len(rgba), 4):
            r, g, b, a = rgba[i:i + 4]
            pix += struct.pack('<I', a << 24 | (r * a // 255) << 16 | (g * a // 255) << 8 | (b * a // 255))
        chunks.append((w, struct.pack('<IIIIIIIII', 36, 0xfffd0002, w, 1, w, h, hx, hy, 0) + pix))
    n = len(chunks)
    pos = 16 + 12 * n
    toc = b''
    for size, data in chunks:
        toc += struct.pack('<III', 0xfffd0002, size, pos)
        pos += len(data)
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sIII', b'Xcur', 16, 0x10000, n) + toc + b''.join(d for _, d in chunks))


# user32 cursor group -> the Xcursor names apps and wlroots ask for
CURSORS = {
    100: ['default', 'left_ptr', 'arrow', 'top_left_arrow'],
    101: ['text', 'xterm', 'ibeam'],
    102: ['wait', 'watch'],
    103: ['crosshair', 'cross', 'tcross'],
    104: ['up-arrow', 'center_ptr'],
    105: ['nwse-resize', 'se-resize', 'nw-resize', 'top_left_corner', 'bottom_right_corner', 'size_fdiag'],
    106: ['nesw-resize', 'ne-resize', 'sw-resize', 'top_right_corner', 'bottom_left_corner', 'size_bdiag'],
    107: ['ew-resize', 'e-resize', 'w-resize', 'col-resize', 'left_side', 'right_side', 'sb_h_double_arrow', 'size_hor'],
    108: ['ns-resize', 'n-resize', 's-resize', 'row-resize', 'top_side', 'bottom_side', 'sb_v_double_arrow', 'size_ver'],
    109: ['move', 'all-scroll', 'fleur', 'size_all', 'grabbing'],
    110: ['not-allowed', 'no-drop', 'crossed_circle', 'forbidden'],
    111: ['progress', 'left_ptr_watch', 'half-busy'],
    112: ['help', 'question_arrow', 'whats_this'],
    114: ['pointer', 'hand', 'hand1', 'hand2', 'pointing_hand'],
}


def outline_inverted(w, h, px):
    """XP's I-beam and crosshair invert what's under them; Wayland can't, so draw
    those pixels white with a black outline, which shows on any background."""
    px = bytearray(px)
    marked = {(x, y) for y in range(h) for x in range(w) if px[(y * w + x) * 4 + 3] == 1}
    for x, y in marked:
        for dy in (-1, 0, 1):
            for dx in (-1, 0, 1):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and (nx, ny) not in marked:
                    o = (ny * w + nx) * 4
                    if px[o + 3] == 0:
                        px[o:o + 4] = b'\x00\x00\x00\xff'
    for x, y in marked:
        o = (y * w + x) * 4
        px[o:o + 4] = b'\xff\xff\xff\xff'
    return bytes(px)


def rip_cursors(user32, out, tmp):
    rd = os.path.join(tmp, 'res-user32')
    run7z('x', '-tPE', '-y', '-o' + rd, user32, '.rsrc/CURSOR/*', '.rsrc/GROUP_CURSOR/*')
    res = os.path.join(rd, '.rsrc')
    cdir = os.path.join(out, 'cursors', 'expede-xp', 'cursors')
    os.makedirs(cdir, exist_ok=True)
    with open(os.path.join(out, 'cursors', 'expede-xp', 'index.theme'), 'w') as f:
        f.write('[Icon Theme]\nName=Expede XP\nComment=Windows XP pointers from your own user32.dll\n')
    for gid, names in CURSORS.items():
        grp = open(os.path.join(res, 'GROUP_CURSOR', str(gid)), 'rb').read()
        rid = struct.unpack_from('<H', grp, 6 + 12)[0]  # first entry's resource id
        p = os.path.join(res, 'CURSOR', str(rid))
        data = open(p if os.path.exists(p) else p + '.cur', 'rb').read()
        hx, hy = struct.unpack_from('<HH', data, 0)
        w, h, px, _ = decode_dib(data, 4, icon=True)
        px = outline_inverted(w, h, px)
        # 100%, 150% and 200% screens each get a pointer of the right size
        images = [(w, h, hx, hy, px)]
        for f in (1.5, 2):
            sw, sh, spx = scale_nearest(w, h, px, f)
            images.append((sw, sh, round(hx * f), round(hy * f), spx))
        for name in names:
            write_xcursor(os.path.join(cdir, name), images)

# --- what we take ------------------------------------------------------------

ISO_FILES = {
    'LUNA.MS_': 'luna.msstyles', 'BLISS.JP_': 'bliss.jpg',
    'TAHOMA.TT_': 'tahoma.ttf', 'TAHOMABD.TT_': 'tahomabd.ttf',
    'TREBUC.TT_': 'trebuc.ttf', 'TREBUCBD.TT_': 'trebucbd.ttf',
    'FRAMD.TT_': 'framd.ttf', 'FRAMDIT.TT_': 'framdit.ttf',
    'CHESS.BM_': 'userpic.bmp',
    'ARIAL.TT_': 'arial.ttf', 'ARIALBD.TT_': 'arialbd.ttf', 'ARIALBI.TT_': 'arialbi.ttf',
    'LOGONUI.EX_': 'logonui.exe', 'MSGINA.DL_': 'msgina.dll', 'STOBJECT.DL_': 'stobject.dll',
    'SHELL32.DL_': 'shell32.dll', 'EXPLORER.EX_': 'explorer.exe', 'USER32.DL_': 'user32.dll',
    'NOTEPAD.EX_': 'notepad.exe', 'CMD.EX_': 'cmd.exe', 'MYDOCS.DL_': 'mydocs.dll',
    'XPSTARTU.WA_': 'startup.wav', 'XPSHUTDN.WA_': 'shutdown.wav',
    'XPLOGON.WA_': 'logon.wav', 'XPLOGOFF.WA_': 'logoff.wav',
    'XPBALLN.WA_': 'balloon.wav', 'XPDING.WA_': 'ding.wav', 'XPMIN.WA_': 'minimize.wav',
    'XPRESTOR.WA_': 'restore.wav', 'XPSTART.WA_': 'start.wav', 'XPERROR.WA_': 'error.wav',
}

# (file the group lives in, group id, our name)
ICONS = [
    ('shell32.dll', 16, 'computer'), ('shell32.dll', 18, 'network'),
    ('shell32.dll', 32, 'recycle-empty'), ('shell32.dll', 33, 'recycle-full'),
    ('shell32.dll', 4, 'folder'), ('shell32.dll', 1, 'file'), ('shell32.dll', 3, 'app'),
    ('shell32.dll', 137, 'control-panel'), ('shell32.dll', 23, 'search'),
    ('shell32.dll', 24, 'help'), ('shell32.dll', 25, 'run'),
    ('shell32.dll', 236, 'my-pictures'), ('shell32.dll', 237, 'my-music'),
    ('shell32.dll', 46, 'explorer'), ('shell32.dll', 28, 'turn-off'), ('shell32.dll', 45, 'log-off'),
    ('mydocs.dll', 100, 'my-documents'),
    ('stobject.dll', 230, 'volume'), ('stobject.dll', 231, 'volume-muted'),
    ('notepad.exe', 2, 'notepad'), ('cmd.exe', 'IDI_APPICON', 'cmd'),
]

# Control Panel category and applet icons
ICONS += [
    ('shell32.dll', 270, 'cp-appearance'), ('shell32.dll', 277, 'cp-sounds'),
    ('shell32.dll', 272, 'cp-hardware'), ('shell32.dll', 276, 'cp-datetime'),
    ('shell32.dll', 269, 'cp-users'), ('shell32.dll', 40, 'cp-taskbar'),
    ('shell32.dll', 180, 'cp-display'), ('shell32.dll', 273, 'cp-network'),
    ('shell32.dll', 271, 'cp-programs'), ('shell32.dll', 275, 'cp-performance'),
    ('shell32.dll', 268, 'cp-accessibility'), ('shell32.dll', 17, 'cp-printer'),
    ('stobject.dll', 210, 'cp-power'), ('shell32.dll', 274, 'cp-system'),
]

# Explorer: file types and drives
ICONS += [
    ('shell32.dll', 5, 'folder-open'), ('shell32.dll', 152, 'file-text'), ('shell32.dll', 226, 'file-image'),
    ('shell32.dll', 225, 'file-audio'), ('shell32.dll', 224, 'file-video'), ('shell32.dll', 3, 'file-program'),
    ('shell32.dll', 151, 'file-config'), ('shell32.dll', 155, 'file-font'), ('shell32.dll', 223, 'file-document'),
    ('shell32.dll', 9, 'drive-disk'), ('shell32.dll', 7, 'drive-removable'), ('shell32.dll', 12, 'drive-cd'),
    ('shell32.dll', 10, 'drive-network'), ('shell32.dll', 35, 'desktop'), ('shell32.dll', 22, 'properties'),
]

# Explorer's toolbar: 47 icons of 24px with alpha, normal and hot
SHELL32_BITMAPS = {214: 'toolbar', 215: 'toolbar-hot'}

# XP's wallpapers: file stem on the ISO -> the name Display Properties shows
WALLPAPERS = {
    'ASCENT': 'Ascent', 'AUTUMN': 'Autumn', 'AZUL': 'Azul', 'BLISS': 'Bliss', 'CRYSTAL': 'Crystal',
    'FOLLOW': 'Follow', 'FRIEND': 'Friend', 'HOME': 'Home', 'MOON': 'Moon flower', 'PEACE': 'Peace',
    'POWER': 'Power', 'PURPLEF': 'Purple flower', 'RADIANCE': 'Radiance', 'REDMOON': 'Red moon desert',
    'RIPPLE': 'Ripple', 'STONEH': 'Stonehenge', 'TULIPS': 'Tulips', 'VORTEC': 'Vortec space',
    'WIND': 'Wind', 'WINXP': 'Windows XP',
}
# the older tiled ones are BMPs
TILED_WALLPAPERS = {
    'BUBBLES': 'Soap bubbles', 'COFFEEBN': 'Coffee Bean', 'FEATHER': 'FeatherTexture',
    'GONEFISH': 'Gone Fishing', 'GRNSTONE': 'Greenstone', 'PRAIRIE': 'Prairie Wind',
    'RHODY': 'Rhododendron', 'RVRSUMID': 'River Sumida', 'SANTAFE': 'Santa Fe Stucco',
}
# account pictures (48x48 BMPs)
PICTURES = ['AIRPLANE', 'ASTRO', 'BEACH', 'BUTTERFL', 'CAR', 'CAT', 'CHESS', 'DIRTBIKE', 'DOG', 'DRIP',
            'DUCK', 'FISH', 'FROG', 'GUITAR', 'HORSES', 'KICK', 'LIFTOFF', 'PALMTREE', 'REDFLOWR',
            'SKATER', 'SNWFLAKE', 'SOCCER']

# Taskbar "start" flag (32bpp alpha) lives in explorer.exe's bitmaps.
EXPLORER_BITMAPS = {143: 'start-flag'}

# Welcome screen (logonui.exe) and Turn Off dialog (msgina.dll) bitmaps -> logon/<name>.png
LOGON_BITMAPS = {
    'logonui.exe': {100: 'glow', 112: 'account-selected', 113: 'frame', 119: 'frame-hot',
                    107: 'turnoff', 121: 'turnoff-hot', 124: 'divider-v', 125: 'divider-top',
                    126: 'divider-bottom', 127: 'logo'},
    'msgina.dll': {20142: 'dialog', 20143: 'dialog-flag', 20150: 'dialog-buttons'},
}

# Luna colour scheme files we need (Blue scheme; Homestead/Metallic can follow).
LUNA_PREFIX = 'BLUE_'
LUNA_KEY_OVERRIDES = {'TASKBARTRAY': (255, 0, 0)}


def run7z(*args):
    subprocess.run(['7z', *args], check=True, stdout=subprocess.DEVNULL)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    iso = sys.argv[1]
    if os.path.isdir(iso):
        isos = [f for f in os.listdir(iso) if f.lower().endswith('.iso')]
        if not isos:
            sys.exit(f'no .iso in {iso}')
        iso = os.path.join(iso, isos[0])
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/.local/share/expede/xp')
    for sub in ('luna', 'icons', 'fonts', 'sounds'):
        os.makedirs(os.path.join(out, sub), exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='xprip-') as tmp:
        cab, raw = os.path.join(tmp, 'cab'), os.path.join(tmp, 'raw')
        run7z('e', '-y', '-o' + cab, iso, *['I386/' + n for n in ISO_FILES])
        os.makedirs(raw)
        for packed, name in ISO_FILES.items():
            src = os.path.join(cab, packed)
            if not os.path.exists(src):
                print(f'warning: {packed} not on this ISO', file=sys.stderr)
                continue
            d = os.path.join(tmp, 'x-' + packed)
            run7z('e', '-y', '-o' + d, src)
            inner = os.listdir(d)[0]
            shutil.move(os.path.join(d, inner), os.path.join(raw, name))

        # fonts, wallpaper, sounds: copied as they are
        for name in os.listdir(raw):
            src = os.path.join(raw, name)
            if name.endswith('.ttf'):
                shutil.copy(src, os.path.join(out, 'fonts', name))
            elif name.endswith('.wav'):
                shutil.copy(src, os.path.join(out, 'sounds', name))
            elif name == 'bliss.jpg':
                shutil.copy(src, os.path.join(out, 'bliss.jpg'))
            elif name == 'userpic.bmp':  # an XP account picture for the Start menu
                bmp_to_png(src, os.path.join(out, 'userpic.png'), key=None)

        # Luna: every Blue bitmap as PNG, plus the scheme's ini as UTF-8
        lres = os.path.join(tmp, 'luna')
        run7z('x', '-tPE', '-y', '-o' + lres, os.path.join(raw, 'luna.msstyles'), '.rsrc/BITMAP/*', '.rsrc/TEXTFILE/*')
        bdir = os.path.join(lres, '.rsrc', 'BITMAP')
        n = 0
        for f in os.listdir(bdir):
            if not f.startswith(LUNA_PREFIX):
                continue
            stem = f[len(LUNA_PREFIX):].rsplit('_BMP', 1)[0]
            key = LUNA_KEY_OVERRIDES.get(stem, (255, 0, 255))
            bmp_to_png(os.path.join(bdir, f), os.path.join(out, 'luna', stem.lower() + '.png'), key)
            n += 1
        ini = open(os.path.join(lres, '.rsrc', 'TEXTFILE', 'NORMALBLUE_INI'), 'rb').read()
        with open(os.path.join(out, 'luna', 'normalblue.ini'), 'w') as f:
            f.write(ini.decode('utf-16-le').lstrip('﻿').replace('\r', ''))

        # icons, 32 and 16 px
        extracted = {}
        for file, gid, name in ICONS:
            if file not in extracted:
                rd = os.path.join(tmp, 'res-' + file)
                if os.path.exists(os.path.join(raw, file)):
                    run7z('x', '-tPE', '-y', '-o' + rd, os.path.join(raw, file), '.rsrc/GROUP_ICON/*', '.rsrc/ICON/*', '.rsrc/BITMAP/*')
                extracted[file] = os.path.join(rd, '.rsrc')
            for size in (48, 32, 16):
                try:
                    w, h, px, _ = icon_from_group(extracted[file], gid, size)
                except (FileNotFoundError, struct.error) as e:
                    print(f'warning: icon {name}: {e}', file=sys.stderr)
                    break
                write_png(os.path.join(out, 'icons', f'{name}-{size}.png'), w, h, px)
        eres = os.path.join(tmp, 'res-explorer.exe')
        run7z('x', '-tPE', '-y', '-o' + eres, os.path.join(raw, 'explorer.exe'), '.rsrc/BITMAP/*')
        for bid, name in EXPLORER_BITMAPS.items():
            src = os.path.join(eres, '.rsrc', 'BITMAP', f'{bid}.bmp')
            bmp_to_png(src, os.path.join(out, 'icons', name + '.png'), key=None)
        rip_cursors(os.path.join(raw, 'user32.dll'), out, tmp)
        sres = os.path.join(tmp, 'bmp-shell32')
        run7z('x', '-tPE', '-y', '-o' + sres, os.path.join(raw, 'shell32.dll'), '.rsrc/BITMAP/*')
        for bid, name in SHELL32_BITMAPS.items():
            bmp_to_png(os.path.join(sres, '.rsrc', 'BITMAP', f'{bid}.bmp'), os.path.join(out, 'icons', name + '.png'), key=None)

        # wallpapers and account pictures: one 7z pass for all of them
        os.makedirs(os.path.join(out, 'wallpapers'), exist_ok=True)
        os.makedirs(os.path.join(out, 'pictures'), exist_ok=True)
        wanted = [f'I386/{k}.JP_' for k in WALLPAPERS] + [f'I386/{k}.BM_' for k in TILED_WALLPAPERS] + \
                 [f'I386/{k}.BM_' for k in PICTURES]
        wcab = os.path.join(tmp, 'wcab')
        run7z('e', '-y', '-o' + wcab, iso, *wanted)
        for f in os.listdir(wcab):
            stem = f.split('.')[0].upper()
            d = os.path.join(tmp, 'w-' + stem)
            run7z('e', '-y', '-o' + d, os.path.join(wcab, f))
            src = os.path.join(d, os.listdir(d)[0])
            if stem in WALLPAPERS:
                shutil.copy(src, os.path.join(out, 'wallpapers', WALLPAPERS[stem] + '.jpg'))
            elif stem in TILED_WALLPAPERS:
                bmp_to_png(src, os.path.join(out, 'wallpapers', TILED_WALLPAPERS[stem] + '.tile.png'), key=None)
            elif stem in PICTURES:
                bmp_to_png(src, os.path.join(out, 'pictures', stem.lower() + '.png'), key=None)

        os.makedirs(os.path.join(out, 'logon'), exist_ok=True)
        for file, bitmaps in LOGON_BITMAPS.items():
            rd = os.path.join(tmp, 'logon-' + file)
            run7z('x', '-tPE', '-y', '-o' + rd, os.path.join(raw, file), '.rsrc/BITMAP/*')
            for bid, name in bitmaps.items():
                bmp_to_png(os.path.join(rd, '.rsrc', 'BITMAP', f'{bid}.bmp'), os.path.join(out, 'logon', name + '.png'),
                           key='corner' if name == 'logo' else (255, 0, 255))

    # a GTK theme from the same bitmaps, so GTK apps' widgets match
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import gtktheme
    # <data>/expede/xp -> <data>/themes
    gtk = gtktheme.build(out, os.path.join(os.path.dirname(os.path.dirname(out)), 'themes'))
    print(f'xprip: {n} Luna bitmaps, {len(ICONS)} icons -> {out}; GTK theme -> {gtk}')


if __name__ == '__main__':
    main()
