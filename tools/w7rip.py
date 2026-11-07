#!/usr/bin/env python3
"""w7rip: pull the Windows 7 look out of a user-supplied Windows 7 install ISO.

Nothing extracted here is ever redistributed with Expede: the files land in the
user's own data dir and are only read at runtime.

    python3 -I tools/w7rip.py /path/to/win7.iso [outdir]

The output mirrors xprip's layout (luna/normalblue.ini plus PNGs, icons,
sounds, fonts, wallpapers, cursors), so the same theme engine reads either.
Windows 7 keeps its theme as a compiled binary in aero.msstyles; this turns
that back into the XP-style ini text the engine already understands.

Needs 7z (p7zip) on PATH. Everything else is plain Python.
"""
import os
import re
import shutil
import struct
import sys
import tempfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xprip import bmp_to_png, decode_dib, outline_inverted, run7z, scale_nearest, write_png  # noqa: E402

# --- PNG in any common form -> RGBA -------------------------------------------

def read_png(data):
    """8-bit (and 1/2/4-bit palette) PNGs of colour type 0, 2, 3, 4 or 6 -> (w, h, rgba)."""
    pos, idat, plte, trns = 8, b'', b'', b''
    while pos < len(data):
        n, tag = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + n]
        if tag == b'IHDR':
            w, h, depth, ctype, _, _, interlace = struct.unpack('>IIBBBBB', body)
        elif tag == b'IDAT':
            idat += body
        elif tag == b'PLTE':
            plte = body
        elif tag == b'tRNS':
            trns = body
        pos += 12 + n
    if interlace:
        raise ValueError('interlaced PNG')
    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bits = depth * chans
    stride = (w * bits + 7) // 8
    bpp = max(1, bits // 8)
    raw = zlib.decompress(idat)
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    for y in range(h):
        f = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = line
        o = y * w * 4
        if ctype == 6 and depth == 8:
            out[o:o + w * 4] = line
            continue
        for x in range(w):
            if ctype == 3:
                i = (line[x * depth // 8] >> (8 - depth - (x * depth) % 8)) & ((1 << depth) - 1)
                r, g, b_ = plte[i * 3:i * 3 + 3]
                al = trns[i] if i < len(trns) else 255
            elif ctype == 2:
                r, g, b_ = line[x * 3:x * 3 + 3]
                al = 255
            elif ctype == 0:
                r = g = b_ = line[x]
                al = 255
            elif ctype == 4:
                r = g = b_ = line[x * 2]
                al = line[x * 2 + 1]
            out[o + x * 4:o + x * 4 + 4] = bytes((r, g, b_, al))
    return w, h, bytes(out)


def crop(w, h, px, x0, y0, cw, ch):
    rows = [px[((y0 + y) * w + x0) * 4:((y0 + y) * w + x0 + cw) * 4] for y in range(ch)]
    return cw, ch, b''.join(rows)


def downscale(w, h, px, nw, nh):
    """Box-filter an RGBA image down (alpha-weighted, so edges don't go dark)."""
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        y0, y1 = y * h // nh, max(y * h // nh + 1, (y + 1) * h // nh)
        for x in range(nw):
            x0, x1 = x * w // nw, max(x * w // nw + 1, (x + 1) * w // nw)
            r = g = b = a = n = 0
            for sy in range(y0, y1):
                for sx in range(x0, x1):
                    i = (sy * w + sx) * 4
                    al = px[i + 3]
                    r += px[i] * al; g += px[i + 1] * al; b += px[i + 2] * al; a += al; n += 1
            o = (y * nw + x) * 4
            if a:
                out[o:o + 4] = bytes((r // a, g // a, b // a, a // n))
    return nw, nh, bytes(out)

# --- icons (Vista+ groups mix DIBs and 256px PNGs) ------------------------------

def icon_from_group(resdir, group_id, size):
    grp = open(os.path.join(resdir, 'GROUP_ICON', str(group_id)), 'rb').read()
    count = struct.unpack_from('<H', grp, 4)[0]
    entries = []
    for i in range(count):
        w, h, _, _, _, bpp, _, rid = struct.unpack_from('<BBBBHHIH', grp, 6 + i * 14)
        entries.append((w or 256, bpp, rid))
    exact = [e for e in entries if e[0] == size]
    w, bpp, rid = max(exact, key=lambda e: e[1]) if exact else \
        min((e for e in entries if e[0] > size), key=lambda e: (e[0], -e[1]), default=max(entries))
    p = os.path.join(resdir, 'ICON', str(rid))
    data = open(p if os.path.exists(p) else p + '.ico', 'rb').read()
    if data[:4] == b'\x00\x00\x01\x00':
        data = data[struct.unpack_from('<I', data, 18)[0]:]
    if data[:4] == b'\x89PNG':
        iw, ih, px = read_png(data)
    else:
        iw, ih, px, _ = decode_dib(data, icon=True)
    if iw != size:
        iw, ih, px = downscale(iw, ih, px, size, size)
    return iw, ih, px

# --- cursors: .cur and animated .ani -> Xcursor ---------------------------------

def cur_images(data):
    """Every image in a .cur file: [(w, h, hx, hy, rgba)]."""
    count = struct.unpack_from('<H', data, 4)[0]
    out = []
    for i in range(count):
        _, _, _, _, hx, hy, size, off = struct.unpack_from('<BBBBHHII', data, 6 + i * 16)
        img = data[off:off + size]
        if img[:4] == b'\x89PNG':
            w, h, px = read_png(img)
        else:
            w, h, px, _ = decode_dib(img, icon=True)
            px = outline_inverted(w, h, px)
        out.append((w, h, hx, hy, px))
    return out


def ani_frames(data):
    """RIFF ACON -> ([frame .cur bytes], [delay ms per step], [frame index per step])."""
    frames, rates, seq, jif = [], None, None, 6
    pos = 12
    while pos + 8 <= len(data):
        tag, n = struct.unpack_from('<4sI', data, pos)
        body = data[pos + 8:pos + 8 + n]
        if tag == b'anih':
            jif = struct.unpack_from('<I', body, 28)[0] or 6
        elif tag == b'rate':
            rates = list(struct.unpack('<%dI' % (n // 4), body))
        elif tag == b'seq ':
            seq = list(struct.unpack('<%dI' % (n // 4), body))
        elif tag == b'LIST' and body[:4] == b'fram':
            p = 4
            while p + 8 <= len(body):
                t2, n2 = struct.unpack_from('<4sI', body, p)
                if t2 == b'icon':
                    frames.append(body[p + 8:p + 8 + n2])
                p += 8 + n2 + (n2 & 1)
        pos += 8 + n + (n & 1)
    seq = seq or list(range(len(frames)))
    rates = rates or [jif] * len(seq)
    return frames, [r * 1000 // 60 for r in rates], seq


def write_xcursor_anim(path, steps):
    """steps = [(w, h, hx, hy, rgba, delay_ms)], several sizes and frames mixed."""
    chunks = []
    for w, h, hx, hy, rgba, delay in steps:
        pix = bytearray()
        for i in range(0, len(rgba), 4):
            r, g, b, a = rgba[i:i + 4]
            pix += struct.pack('<I', a << 24 | (r * a // 255) << 16 | (g * a // 255) << 8 | (b * a // 255))
        chunks.append((w, struct.pack('<9I', 36, 0xfffd0002, w, 1, w, h, hx, hy, delay) + pix))
    pos = 16 + 12 * len(chunks)
    toc = b''
    for size, d in chunks:
        toc += struct.pack('<III', 0xfffd0002, size, pos)
        pos += len(d)
    with open(path, 'wb') as f:
        f.write(struct.pack('<4sIII', b'Xcur', 16, 0x10000, len(chunks)) + toc + b''.join(d for _, d in chunks))


def sized(images):
    """Xcursor wants one image per nominal size: keep 32, add 48 and 64 by scaling."""
    base = min(images, key=lambda i: abs(i[0] - 32))
    out = [base]
    for want in (48, 64):
        have = [i for i in images if i[0] == want]
        if have:
            out.append(have[0])
        else:
            w, h, hx, hy, px = base
            f = want / w
            sw, sh, spx = scale_nearest(w, h, px, f)
            out.append((sw, sh, round(hx * f), round(hy * f), spx))
    return out


# Windows/Cursors file -> Xcursor names
CURSORS = {
    'aero_arrow.cur': ['default', 'left_ptr', 'arrow', 'top_left_arrow'],
    'beam_r.cur': ['text', 'xterm', 'ibeam'],
    'aero_busy.ani': ['wait', 'watch'],
    'cross_r.cur': ['crosshair', 'cross', 'tcross'],
    'aero_up.cur': ['up-arrow', 'center_ptr'],
    'aero_nwse.cur': ['nwse-resize', 'se-resize', 'nw-resize', 'top_left_corner', 'bottom_right_corner', 'size_fdiag'],
    'aero_nesw.cur': ['nesw-resize', 'ne-resize', 'sw-resize', 'top_right_corner', 'bottom_left_corner', 'size_bdiag'],
    'aero_ew.cur': ['ew-resize', 'e-resize', 'w-resize', 'col-resize', 'left_side', 'right_side', 'sb_h_double_arrow',
                    'size_hor'],
    'aero_ns.cur': ['ns-resize', 'n-resize', 's-resize', 'row-resize', 'top_side', 'bottom_side', 'sb_v_double_arrow',
                    'size_ver'],
    'aero_move.cur': ['move', 'all-scroll', 'fleur', 'size_all', 'grabbing'],
    'aero_unavail.cur': ['not-allowed', 'no-drop', 'crossed_circle', 'forbidden'],
    'aero_working.ani': ['progress', 'left_ptr_watch', 'half-busy'],
    'aero_helpsel.cur': ['help', 'question_arrow', 'whats_this'],
    'aero_link.cur': ['pointer', 'hand', 'hand1', 'hand2', 'pointing_hand'],
    'aero_pen.cur': ['pencil'],
}


def rip_cursors(cdir_in, out):
    cdir = os.path.join(out, 'cursors', 'expede-7', 'cursors')
    os.makedirs(cdir, exist_ok=True)
    with open(os.path.join(out, 'cursors', 'expede-7', 'index.theme'), 'w') as f:
        f.write('[Icon Theme]\nName=Expede 7\nComment=Windows 7 pointers from your own Windows 7 disc\n')
    for file, names in CURSORS.items():
        p = os.path.join(cdir_in, file)
        if not os.path.exists(p):
            print(f'warning: no {file}', file=sys.stderr)
            continue
        data = open(p, 'rb').read()
        if file.endswith('.ani'):
            frames, delays, seq = ani_frames(data)
            imgs = [sized(cur_images(fr)) for fr in frames]
            steps = []
            for k in range(len(imgs[0])):
                for step, fi in enumerate(seq):
                    w, h, hx, hy, px = imgs[fi][k]
                    steps.append((w, h, hx, hy, px, delays[step] if step < len(delays) else 50))
        else:
            steps = [(*i, 0) for i in sized(cur_images(data))]
        for name in names:
            write_xcursor_anim(os.path.join(cdir, name), steps)

# --- aero.msstyles: compiled properties -> XP-style ini ---------------------------

PROPS = {
    2201: 'Transparent', 2202: 'AutoSize', 2203: 'BorderOnly', 2204: 'Composited', 2205: 'BgFill',
    2206: 'GlyphTransparent', 2207: 'GlyphOnly', 2208: 'AlwaysShowSizingBar', 2209: 'MirrorImage',
    2210: 'UniformSizing', 2211: 'IntegralSizing', 2212: 'SourceGrow', 2213: 'SourceShrink',
    2214: 'DrawBorders', 2215: 'NoEtchedEffect', 2216: 'TextApplyOverlay', 2217: 'TextGlow', 2218: 'TextItalic',
    2219: 'CompositedOpaque', 2220: 'LocalizedMirrorImage',
    2401: 'ImageCount', 2402: 'AlphaLevel', 2403: 'BorderSize', 2404: 'RoundCornerWidth',
    2405: 'RoundCornerHeight', 2411: 'ProgressChunkSize', 2412: 'ProgressSpaceSize', 2413: 'Saturation',
    2414: 'TextBorderSize', 2415: 'AlphaThreshold', 2416: 'Width', 2417: 'Height', 2418: 'GlyphIndex',
    2419: 'TrueSizeStretchMark', 2420: 'MinDpi1', 2421: 'MinDpi2', 2422: 'MinDpi3', 2423: 'MinDpi4',
    2424: 'MinDpi5', 2425: 'TextGlowSize', 2426: 'FramesPerSecond', 2427: 'PixelsPerFrame',
    2428: 'AnimationDelay', 2429: 'GlowIntensity', 2430: 'Opacity', 2431: 'ColorizationColor',
    2432: 'ColorizationOpacity',
    3001: 'ImageFile', 3002: 'ImageFile1', 3003: 'ImageFile2', 3004: 'ImageFile3', 3005: 'ImageFile4',
    3006: 'ImageFile5', 3008: 'GlyphImageFile',
    3401: 'Offset', 3402: 'TextShadowOffset', 3403: 'MinSize', 3404: 'MinSize1', 3405: 'MinSize2',
    3406: 'MinSize3', 3407: 'MinSize4', 3408: 'MinSize5',
    3601: 'SizingMargins', 3602: 'ContentMargins', 3603: 'CaptionMargins',
    3801: 'BorderColor', 3802: 'FillColor', 3803: 'TextColor', 3804: 'EdgeLightColor',
    3805: 'EdgeHighlightColor', 3806: 'EdgeShadowColor', 3807: 'EdgeDkShadowColor', 3808: 'EdgeFillColor',
    3809: 'TransparentColor', 3810: 'GradientColor1', 3811: 'GradientColor2', 3812: 'GradientColor3',
    3813: 'GradientColor4', 3814: 'GradientColor5', 3815: 'ShadowColor', 3816: 'GlowColor',
    3817: 'TextBorderColor', 3818: 'TextShadowColor', 3819: 'GlyphTextColor', 3820: 'GlyphTransparentColor',
    3821: 'FillColorHint', 3822: 'BorderColorHint', 3823: 'AccentColorHint', 3824: 'TextColorHint',
    3825: 'Heading1TextColor', 3826: 'Heading2TextColor', 3827: 'BodyTextColor',
    4001: 'BgType', 4002: 'BorderType', 4003: 'FillType', 4004: 'SizingType', 4005: 'HAlign',
    4006: 'ContentAlignment', 4007: 'VAlign', 4008: 'OffsetType', 4009: 'IconEffect', 4010: 'TextShadowType',
    4011: 'ImageLayout', 4012: 'GlyphType', 4013: 'ImageSelectType', 4014: 'GlyphFontSizingType',
    4015: 'TrueSizeScalingType',
    8002: 'AtlasRect',
}
ENUMS = {
    'BgType': ['ImageFile', 'BorderFill', 'None'], 'SizingType': ['TrueSize', 'Stretch', 'Tile'],
    'ImageLayout': ['Vertical', 'Horizontal'], 'GlyphType': ['None', 'ImageGlyph', 'FontGlyph'],
    'ImageSelectType': ['None', 'Size', 'Dpi'], 'TrueSizeScalingType': ['None', 'Size', 'Dpi'],
    'BorderType': ['Rect', 'RoundRect', 'Ellipse'], 'FillType': ['Solid', 'VertGradient', 'HorzGradient',
                                                                 'RadialGradient', 'TileImage'],
    'HAlign': ['Left', 'Center', 'Right'], 'VAlign': ['Top', 'Center', 'Bottom'],
}

# Part names as uxtheme's schema has them, for the classes Expede draws.
PARTS = {
    'Window': ['Caption', 'SmallCaption', 'MinCaption', 'SmallMinCaption', 'MaxCaption', 'SmallMaxCaption',
               'FrameLeft', 'FrameRight', 'FrameBottom', 'SmallFrameLeft', 'SmallFrameRight', 'SmallFrameBottom',
               'SysButton', 'MdiSysButton', 'MinButton', 'MdiMinButton', 'MaxButton', 'CloseButton',
               'SmallCloseButton', 'MdiCloseButton', 'RestoreButton', 'MdiRestoreButton', 'HelpButton',
               'MdiHelpButton', 'HorzScroll', 'HorzThumb', 'VertScroll', 'VertThumb', 'Dialog'],
    'Button': ['Pushbutton', 'RadioButton', 'CheckBox', 'GroupBox', 'UserButton', 'CommandLink',
               'CommandLinkGlyph'],
    'Tab': ['TabItem', 'TabItemLeftEdge', 'TabItemRightEdge', 'TabItemBothEdge', 'TopTabItem',
            'TopTabItemLeftEdge', 'TopTabItemRightEdge', 'TopTabItemBothEdge', 'Pane', 'Body', 'AeroWizardBody'],
    'TrackBar': ['Track', 'TrackVert', 'Thumb', 'ThumbBottom', 'ThumbTop', 'ThumbVert', 'ThumbLeft',
                 'ThumbRight', 'Tics', 'TicsVert'],
    'ScrollBar': ['ArrowBtn', 'ThumbBtnHorz', 'ThumbBtnVert', 'LowerTrackHorz', 'UpperTrackHorz',
                  'LowerTrackVert', 'UpperTrackVert', 'GripperHorz', 'GripperVert', 'SizeBox'],
    'Progress': ['Bar', 'BarVert', 'Chunk', 'ChunkVert', 'Fill', 'FillVert', 'PulseOverlay', 'MoveOverlay',
                 'PulseOverlayVert', 'MoveOverlayVert', 'TransparentBar', 'TransparentBarVert'],
    'Header': ['HeaderItem', 'HeaderItemLeft', 'HeaderItemRight', 'HeaderSortArrow', 'HeaderDropDown',
               'HeaderDropDownFilter', 'HeaderOverflow'],
    'Combobox': ['DropDownButton', 'Background', 'TransparentBackground', 'BorderNoText', 'Readonly',
                 'DropDownButtonRight', 'DropDownButtonLeft', 'CueBanner'],
    'Edit': ['EditText', 'Caret', 'Background', 'Password', 'BackgroundWithBorder', 'EditBorderNoScroll',
             'EditBorderHScroll', 'EditBorderVScroll', 'EditBorderHVScroll'],
    'Spin': ['Up', 'Down', 'UpHorz', 'DownHorz'],
    'Toolbar': ['Button', 'DropDownButton', 'SplitButton', 'SplitButtonDropDown', 'Separator', 'SeparatorVert'],
    'Tooltip': ['Standard', 'StandardTitle', 'Balloon', 'BalloonTitle', 'Close', 'BalloonStem', 'Wrench'],
    'Menu': ['MenuItem', 'MenuDropDown', 'MenuBarItem', 'MenuBarDropDown', 'Chevron', 'Separator',
             'BarBackground', 'BarItem', 'PopupBackground', 'PopupBorders', 'PopupCheck', 'PopupCheckBackground',
             'PopupGutter', 'PopupItem', 'PopupSeparator', 'PopupSubmenu', 'SystemClose', 'SystemMaximize',
             'SystemMinimize', 'SystemRestore'],
    'TreeView': ['TreeItem', 'Glyph', 'Branch', 'HotGlyph'],
    'ListView': ['ListItem', 'ListGroup', 'ListDetail', 'ListSortedDetail', 'EmptyText', 'GroupHeader',
                 'GroupHeaderLine', 'ExpandButton', 'CollapseButton', 'ColumnDetail'],
    'TaskBar': ['BackgroundBottom', 'BackgroundRight', 'BackgroundTop', 'BackgroundLeft', 'SizingBarBottom',
                'SizingBarRight', 'SizingBarTop', 'SizingBarLeft'],
    'StartPanel': ['UserPane', 'MorePrograms', 'MoreProgramsArrow', 'ProgList', 'ProgListSeparator',
                   'PlacesList', 'PlacesListSeparator', 'Logoff', 'LogoffButtons', 'UserPicture', 'Preview',
                   'MoreProgramsTab', 'NSCHost', 'SoftwareExplorer', 'OpenBox', 'SearchView',
                   'MoreProgramsArrowBack', 'TopMatch', 'LogoffSplitButtonDropDown'],
    'Status': ['Pane', 'GripperPane', 'Gripper'],
    'Rebar': ['Gripper', 'GripperVert', 'Band', 'Chevron', 'ChevronVert', 'Background', 'Splitter',
              'SplitterVert'],
}


def base_class(name):
    return name.split('::', 1)[1] if '::' in name else name


def part_name(cls, part):
    names = PARTS.get(base_class(cls))
    if names and 0 < part <= len(names):
        return names[part - 1]
    return str(part)


def parse_msstyles(resdir):
    """[(class, part, state, prop_id, type, value)] from VARIANT/NORMAL and CMAP."""
    cmap = open(os.path.join(resdir, 'CMAP', 'CMAP'), 'rb').read().decode('utf-16-le')
    classes = [c for c in cmap.split('\0') if c]
    data = open(os.path.join(resdir, 'VARIANT', 'NORMAL'), 'rb').read()
    recs, o = [], 0
    while o + 32 <= len(data):
        name, typ, cls, part, state, ref, _, size = struct.unpack_from('<8i', data, o)
        body = b'' if ref else data[o + 32:o + 32 + size]
        o = (o + 32 + len(body) + 7) & ~7
        if not 0 <= cls < len(classes):
            continue
        if ref:
            val = ref
        elif typ in (200, 202, 203, 204, 206, 213) and len(body) >= 4:
            val = struct.unpack_from('<i', body)[0]
        elif typ in (205, 209) and len(body) >= 16:
            val = struct.unpack_from('<4i', body)
        elif typ in (207, 208) and len(body) >= 8:
            val = struct.unpack_from('<2i', body)
        elif typ == 201:
            val = body.decode('utf-16-le', 'replace').rstrip('\0')
        elif typ == 211:
            val = struct.unpack('<%di' % (len(body) // 4), body[:len(body) // 4 * 4])
        else:
            continue
        recs.append((classes[cls], part, state, name, typ, val))
    return recs


def fmt(key, typ, val):
    if typ == 204:
        return '%d %d %d' % (val & 255, (val >> 8) & 255, (val >> 16) & 255)
    if typ in (206, 213):
        return 'Aero\\%d.png' % val
    if typ == 203:
        return 'true' if val else 'false'
    if typ == 200 and key in ENUMS:
        e = ENUMS[key]
        return e[val] if 0 <= val < len(e) else str(val)
    if isinstance(val, tuple):
        return ', '.join(map(str, val))
    return str(val)


def write_ini(recs, path, dwm_rects):
    sections = {}
    for cls, part, state, pid, typ, val in recs:
        if cls == 'DWMWindow' and pid == 8002:
            continue
        key = PROPS.get(pid)
        if not key:
            continue
        sec = cls if not part else f'{cls}.{part_name(cls, part)}'
        if state:
            sec += f'({state})'
        sections.setdefault(sec, {})[key] = fmt(key, typ, val)
    # DWM parts live in one atlas; each was cut out to its own PNG
    for part, name in dwm_rects.items():
        sections.setdefault(f'DWMWindow.{part}', {})['ImageFile'] = f'Aero\\{name}.png'
    for sec in sections.values():
        # the engine reads ImageFile; Win7 often only has the per-DPI ImageFile1..
        if 'ImageFile' not in sec and 'ImageFile1' in sec:
            sec['ImageFile'] = sec['ImageFile1']
    with open(path, 'w') as f:
        f.write('; Windows 7 Aero, decompiled from your own aero.msstyles by w7rip\n')
        for sec, kv in sections.items():
            f.write(f'\n[{sec}]\n')
            for k, v in kv.items():
                f.write(f'{k} = {v}\n')
    return len(sections)

# --- what we take ------------------------------------------------------------

WIM_FILES = [
    'Windows/Resources/Themes/Aero/aero.msstyles', 'Windows/explorer.exe',
    'Windows/System32/imageres.dll', 'Windows/System32/shell32.dll', 'Windows/System32/authui.dll',
    'Windows/System32/SndVolSSO.dll', 'Windows/System32/notepad.exe', 'Windows/System32/cmd.exe',
    'Windows/Branding/Basebrd/basebrd.dll',
    'Windows/Fonts/segoeui.ttf', 'Windows/Fonts/segoeuib.ttf', 'Windows/Fonts/segoeuii.ttf',
    'Windows/Fonts/segoeuiz.ttf', 'Windows/Fonts/segoeuil.ttf', 'Windows/Fonts/seguisb.ttf',
    'Windows/Media/*.wav', 'Windows/Cursors/aero_*', 'Windows/Cursors/beam_r.cur', 'Windows/Cursors/cross_r.cur',
    'Windows/Web/Wallpaper/*', 'ProgramData/Microsoft/User Account Pictures/Default Pictures/*',
]

# Windows/Media name -> the name Expede plays
SOUNDS = {
    'Windows Logon Sound.wav': 'logon', 'Windows Logoff Sound.wav': 'logoff', 'Windows Shutdown.wav': 'shutdown',
    'Windows Ding.wav': 'ding', 'Windows Minimize.wav': 'minimize', 'Windows Restore.wav': 'restore',
    'Windows Error.wav': 'error', 'Windows Balloon.wav': 'balloon', 'Windows Navigation Start.wav': 'start',
    'Windows Notify.wav': 'notify', 'Windows Exclamation.wav': 'exclamation',
    'Windows User Account Control.wav': 'uac', 'Windows Hardware Insert.wav': 'device-added',
    'Windows Hardware Remove.wav': 'device-removed', 'Windows Recycle.wav': 'recycle',
}

# (file, group id, our name): the same names xprip uses, so the shell finds them
ICONS = [
    ('imageres.dll', 109, 'computer'), ('imageres.dll', 25, 'network'),
    ('imageres.dll', 55, 'recycle-empty'), ('imageres.dll', 54, 'recycle-full'),
    ('imageres.dll', 3, 'folder'), ('imageres.dll', 2, 'file'), ('imageres.dll', 15, 'app'),
    ('imageres.dll', 27, 'control-panel'), ('imageres.dll', 8, 'search'), ('imageres.dll', 99, 'help'),
    ('imageres.dll', 100, 'run'), ('imageres.dll', 113, 'my-pictures'), ('imageres.dll', 108, 'my-music'),
    ('imageres.dll', 1023, 'explorer'), ('imageres.dll', 112, 'my-documents'),
    ('imageres.dll', 5, 'folder-open'), ('imageres.dll', 102, 'file-text'), ('imageres.dll', 72, 'file-image'),
    ('imageres.dll', 131, 'file-audio'), ('imageres.dll', 23, 'file-video'), ('imageres.dll', 15, 'file-program'),
    ('imageres.dll', 69, 'file-config'), ('imageres.dll', 124, 'file-font'), ('imageres.dll', 90, 'file-document'),
    ('imageres.dll', 32, 'drive-disk'), ('imageres.dll', 30, 'drive-removable'), ('imageres.dll', 56, 'drive-cd'),
    ('imageres.dll', 33, 'drive-network'), ('imageres.dll', 183, 'desktop'), ('imageres.dll', 81, 'properties'),
    ('imageres.dll', 1029, 'log-off'), ('imageres.dll', 1013, 'users'), ('imageres.dll', 1024, 'favorites'),
    ('imageres.dll', 184, 'downloads'), ('imageres.dll', 1005, 'videos'), ('imageres.dll', 1020, 'games'),
    ('imageres.dll', 1022, 'homegroup'),
    # Control Panel
    ('imageres.dll', 197, 'cp-appearance'), ('imageres.dll', 1004, 'cp-sounds'),
    ('imageres.dll', 1021, 'cp-hardware'), ('imageres.dll', 144, 'cp-datetime'),
    ('imageres.dll', 74, 'cp-users'), ('imageres.dll', 77, 'cp-taskbar'), ('imageres.dll', 110, 'cp-display'),
    ('imageres.dll', 120, 'cp-network'), ('imageres.dll', 87, 'cp-programs'), ('imageres.dll', 150, 'cp-performance'),
    ('imageres.dll', 86, 'cp-accessibility'), ('imageres.dll', 51, 'cp-printer'), ('imageres.dll', 1001, 'cp-power'),
    ('imageres.dll', 1029, 'cp-system'), ('imageres.dll', 78, 'cp-security'),
    ('SndVolSSO.dll', 124, 'volume'), ('SndVolSSO.dll', 120, 'volume-muted'),
    ('SndVolSSO.dll', 121, 'volume-0'), ('SndVolSSO.dll', 122, 'volume-1'),
    ('SndVolSSO.dll', 123, 'volume-2'), ('SndVolSSO.dll', 100, 'sndvol'),
    ('notepad.exe', 2, 'notepad'), ('cmd.exe', 'IDI_APPICON', 'cmd'),
]

# explorer.exe: the start orb at 96/120/144/192 dpi (3 frames: normal, hot, pressed), user picture frame
EXPLORER_BITMAPS = {6801: 'orb', 6802: 'orb-120', 6803: 'orb-144', 6804: 'orb-192', 7013: 'userframe',
                    7014: 'userframe-80'}

# authui.dll (logon screen) -> logon/<name>.png
AUTHUI_BITMAPS = {
    12223: 'tile-frame', 12238: 'tile-frame-large', 12218: 'tile-small-hot', 12219: 'tile-small',
    12220: 'tile-small-selected', 12289: 'go', 12290: 'go-hot', 12291: 'go-pressed',
    12303: 'shutdown', 12304: 'shutdown-hot', 12306: 'shutdown-pressed', 12292: 'shutdown-arrow',
    12293: 'shutdown-arrow-hot', 12295: 'shutdown-arrow-pressed', 12213: 'ease-of-access',
    12259: 'button', 12260: 'button-hot', 12261: 'button-pressed',
}
# imageres.dll logon backgrounds, one per aspect ratio (IMAGE 5031-5043)
LOGON_BACKGROUNDS = range(5031, 5044)


def find(root, rel):
    """Case-insensitive path lookup inside the extracted image."""
    cur = root
    for part in rel.split('/'):
        if not os.path.isdir(cur):
            return None
        hit = [e for e in os.listdir(cur) if e.lower() == part.lower()]
        if not hit:
            return None
        cur = os.path.join(cur, hit[0])
    return cur


def pick_image(wim, tmp):
    """Index of the fullest edition in install.wim (Ultimate/Professional have every wallpaper)."""
    run7z('e', '-y', '-o' + tmp, wim, '[1].xml')
    xml = open(os.path.join(tmp, '[1].xml'), 'rb').read().decode('utf-16', 'replace')
    images = re.findall(r'<IMAGE INDEX="(\d+)">.*?<NAME>([^<]*)</NAME>', xml, re.S)
    for want in ('ULTIMATE', 'PROFESSIONAL', 'ENTERPRISE', 'HOMEPREMIUM'):
        for idx, name in images:
            if want in name.upper().replace(' ', ''):
                return idx, name
    return images[-1] if images else ('1', 'Windows 7')


def rip(iso, out):
    for sub in ('luna', 'icons', 'fonts', 'sounds', 'wallpapers', 'pictures', 'logon'):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    work = os.path.dirname(os.path.abspath(out))
    with tempfile.TemporaryDirectory(dir=work, prefix='.w7rip-') as tmp:
        print('w7rip: unpacking install.wim (about 2 GB, this takes a minute)')
        run7z('e', '-y', '-o' + tmp, iso, 'sources/install.wim')
        wim = os.path.join(tmp, 'install.wim')
        idx, name = pick_image(wim, tmp)
        print(f'w7rip: using {name}')
        root = os.path.join(tmp, 'img')
        run7z('x', '-y', '-o' + root, wim, *[f'{idx}/{p}' for p in WIM_FILES])
        os.remove(wim)
        img = os.path.join(root, idx)
        rip_from(img, out, tmp)


def rip_from(img, out, tmp):
    # aero.msstyles -> ini + PNGs
    res = os.path.join(tmp, 'aero')
    run7z('x', '-tPE', '-y', '-o' + res, find(img, 'Windows/Resources/Themes/Aero/aero.msstyles'))
    rd = os.path.join(res, '.rsrc')
    luna = os.path.join(out, 'luna')
    n = 0
    for f in os.listdir(os.path.join(rd, 'IMAGE')):
        data = open(os.path.join(rd, 'IMAGE', f), 'rb').read()
        if data[:4] == b'\x89PNG':
            write_png(os.path.join(luna, f.split('.')[0] + '.png'), *read_png(data))
            n += 1
    recs = parse_msstyles(rd)
    atlas_id = next(v for c, p, s, pid, t, v in recs if c == 'DWMWindow' and pid == 213)
    stream = os.path.join(rd, 'STREAM')
    aw, ah, apx = read_png(open(os.path.join(stream, next(f for f in os.listdir(stream)
                                                          if f.split('.')[0] == str(atlas_id))), 'rb').read())
    dwm = {}
    for c, p, s, pid, t, v in recs:
        if c == 'DWMWindow' and pid == 8002:
            x0, y0, x1, y1 = v
            write_png(os.path.join(luna, f'dwm-{p}.png'), *crop(aw, ah, apx, x0, y0, x1 - x0, y1 - y0))
            dwm[p] = f'dwm-{p}'
    nsec = write_ini(recs, os.path.join(luna, 'normalblue.ini'), dwm)
    # the engine loads these two by XP's file names
    ini = open(os.path.join(luna, 'normalblue.ini')).read()
    for sec, alias in (('Button.CheckBox', 'checkbox13'), ('Button.RadioButton', 'radiobutton13')):
        m = re.search(r'\[' + re.escape(sec) + r'\]\n(?:[^\[]*?)ImageFile1 = Aero\\(\d+)\.png', ini)
        if m:
            shutil.copy(os.path.join(luna, m.group(1) + '.png'), os.path.join(luna, alias + '.png'))

    # fonts, sounds, wallpapers, account pictures
    for f in os.listdir(find(img, 'Windows/Fonts') or tmp):
        if f.lower().endswith('.ttf'):
            shutil.copy(os.path.join(find(img, 'Windows/Fonts'), f), os.path.join(out, 'fonts', f.lower()))
    media = find(img, 'Windows/Media')
    for f, name in SOUNDS.items():
        if media and os.path.exists(os.path.join(media, f)):
            shutil.copy(os.path.join(media, f), os.path.join(out, 'sounds', name + '.wav'))
    wall = find(img, 'Windows/Web/Wallpaper')
    for cat in sorted(os.listdir(wall)) if wall else []:
        jpgs = sorted((f for f in os.listdir(os.path.join(wall, cat)) if f.lower().endswith('.jpg')),
                      key=lambda f: int(re.sub(r'\D', '', f) or 0))
        for k, f in enumerate(jpgs, 1):
            dst = 'Windows 7.jpg' if cat.lower() == 'windows' else f'{cat} {k}.jpg'
            shutil.copy(os.path.join(wall, cat, f), os.path.join(out, 'wallpapers', dst))
    pics = find(img, 'ProgramData/Microsoft/User Account Pictures/Default Pictures')
    for f in sorted(os.listdir(pics)) if pics else []:
        if f.lower().endswith('.bmp'):
            bmp_to_png(os.path.join(pics, f), os.path.join(out, 'pictures', f[:-4].lower() + '.png'), key=None)
    first = sorted(os.listdir(os.path.join(out, 'pictures')))
    if first:
        shutil.copy(os.path.join(out, 'pictures', first[0]), os.path.join(out, 'userpic.png'))

    # icons, start orb, logon screen
    extracted = {}
    for file, gid, name in ICONS:
        if file not in extracted:
            src = find(img, 'Windows/System32/' + file) or find(img, 'Windows/' + file)
            d = os.path.join(tmp, 'res-' + file)
            if src:
                run7z('x', '-tPE', '-y', '-o' + d, src, '.rsrc/GROUP_ICON/*', '.rsrc/ICON/*', '.rsrc/BITMAP/*',
                      '.rsrc/IMAGE/*', '.rsrc/WAVE/*')
            extracted[file] = os.path.join(d, '.rsrc')
        for size in (48, 32, 16):
            try:
                write_png(os.path.join(out, 'icons', f'{name}-{size}.png'), *icon_from_group(extracted[file], gid, size))
            except (OSError, ValueError, struct.error) as e:
                print(f'warning: icon {name}: {e}', file=sys.stderr)
                break
    ires = extracted['imageres.dll']
    wave = os.path.join(ires, 'WAVE')
    if os.path.isdir(wave) and os.listdir(wave):  # the real startup sound lives in imageres
        shutil.copy(os.path.join(wave, os.listdir(wave)[0]), os.path.join(out, 'sounds', 'startup.wav'))
    for i in LOGON_BACKGROUNDS:
        p = os.path.join(ires, 'IMAGE', str(i))
        if os.path.exists(p):
            sz = jpeg_size(open(p, 'rb').read())
            if sz:
                shutil.copy(p, os.path.join(out, 'logon', 'background-%dx%d.jpg' % sz))
    spin = os.path.join(ires, 'BITMAP', '5004.bmp')
    if os.path.exists(spin):
        bmp_to_png(spin, os.path.join(out, 'logon', 'spinner.png'), key=None)
    eres = os.path.join(tmp, 'res-explorer')
    run7z('x', '-tPE', '-y', '-o' + eres, find(img, 'Windows/explorer.exe'), '.rsrc/BITMAP/*')
    for bid, name in EXPLORER_BITMAPS.items():
        bmp_to_png(os.path.join(eres, '.rsrc', 'BITMAP', f'{bid}.bmp'), os.path.join(out, 'icons', name + '.png'),
                   key=None)
    ares = os.path.join(tmp, 'res-authui')
    run7z('x', '-tPE', '-y', '-o' + ares, find(img, 'Windows/System32/authui.dll'), '.rsrc/BITMAP/*')
    for bid, name in AUTHUI_BITMAPS.items():
        bmp_to_png(os.path.join(ares, '.rsrc', 'BITMAP', f'{bid}.bmp'), os.path.join(out, 'logon', name + '.png'),
                   key=None)
    bres = os.path.join(tmp, 'res-basebrd')
    brd = find(img, 'Windows/Branding/Basebrd/basebrd.dll')
    if brd:
        run7z('x', '-tPE', '-y', '-o' + bres, brd, '.rsrc/BITMAP/*')
        for bid, name in ((120, 'branding'), (121, 'branding-opaque'), (1120, 'branding-120')):
            p = os.path.join(bres, '.rsrc', 'BITMAP', f'{bid}.bmp')
            if os.path.exists(p):
                bmp_to_png(p, os.path.join(out, 'logon', name + '.png'), key=None)
    rip_cursors(find(img, 'Windows/Cursors'), out)
    return n, nsec


def jpeg_size(d):
    i = 2
    while i + 9 < len(d):
        if d[i] != 0xff:
            return None
        m, n = d[i + 1], struct.unpack_from('>H', d, i + 2)[0]
        if m in (0xc0, 0xc1, 0xc2):
            h, w = struct.unpack_from('>HH', d, i + 5)
            return w, h
        i += 2 + n
    return None


def main(iso=None, out=None):
    if iso is None:
        if len(sys.argv) < 2:
            sys.exit(__doc__)
        iso = sys.argv[1]
        out = sys.argv[2] if len(sys.argv) > 2 else None
    out = out or os.path.expanduser('~/.local/share/expede/7')
    rip(iso, out)
    import gtktheme
    gtk = gtktheme.build(out, os.path.join(os.path.dirname(os.path.dirname(out)), 'themes'))
    print(f'w7rip: Windows 7 theme -> {out}; GTK theme -> {gtk}')


if __name__ == '__main__':
    main()
