#!/usr/bin/env python3
"""w10rip: pull the Windows 10 look out of a user-supplied Windows 10 install ISO.

Nothing extracted here is ever redistributed with Expede: the files land in the
user's own data dir and are only read at runtime.

    python3 -I tools/w10rip.py /path/to/win10.iso [outdir]

Windows 10 keeps its theme in the same compiled aero.msstyles format as
Windows 7 (w7rip decodes it), but its taskbar, Start menu and title bars are
mostly drawn by the shell itself; Expede draws those with Segoe MDL2 Assets,
the icon font this copies from the ISO.

Needs 7z (p7zip) on PATH. Everything else is plain Python.
"""
import os
import re
import shutil
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from xprip import bmp_to_png, run7z, write_png  # noqa: E402
import w7rip  # noqa: E402
from w7rip import find, icon_from_group, parse_msstyles, read_png, crop, write_ini, rip_cursors  # noqa: E402

WIM_FILES = [
    'Windows/Resources/Themes/aero/aero.msstyles',
    'Windows/SystemResources/imageres.dll.mun', 'Windows/SystemResources/shell32.dll.mun',
    'Windows/SystemResources/SndVolSSO.dll.mun',
    'Windows/Fonts/segoeui*.ttf', 'Windows/Fonts/seguisb.ttf', 'Windows/Fonts/segmdl2.ttf',
    'Windows/Media/Windows*.wav', 'Windows/Cursors/aero_*', 'Windows/Cursors/beam_r.cur',
    'Windows/Cursors/cross_r.cur', 'Windows/Web/*',
    'ProgramData/Microsoft/User Account Pictures/*',
]

SOUNDS = dict(w7rip.SOUNDS)
SOUNDS.update({
    'Windows Logon.wav': 'logon', 'Windows Startup.wav': 'startup', 'Windows Unlock.wav': 'unlock',
    'Windows Background.wav': 'background', 'Windows Foreground.wav': 'foreground',
    'Windows Notify System Generic.wav': 'notify', 'Windows Notify Email.wav': 'notify-email',
    'Windows Notify Messaging.wav': 'notify-message', 'Windows Notify Calendar.wav': 'notify-calendar',
})

# the same names and (it turns out) the same imageres group ids as Windows 7
ICONS = [(f.replace('.dll', '.dll.mun'), gid, name) for f, gid, name in w7rip.ICONS if f == 'imageres.dll']
ICONS += [('SndVolSSO.dll.mun', 120, 'volume'), ('SndVolSSO.dll.mun', 121, 'volume-muted')]

WALL_SETS = {'Theme1': 'Windows', 'Theme2': 'Flowers'}


def pick_image(wim, tmp):
    run7z('e', '-y', '-o' + tmp, wim, '[1].xml')
    xml = open(os.path.join(tmp, '[1].xml'), 'rb').read().decode('utf-16', 'replace')
    images = re.findall(r'<IMAGE INDEX="(\d+)">.*?<NAME>([^<]*)</NAME>', xml, re.S)
    for want in ('Windows 10 Pro', 'Windows 10 Home', 'Windows 10 Education', 'Windows 10 Enterprise'):
        for idx, name in images:
            if name == want:
                return idx, name
    return images[0] if images else ('1', 'Windows 10')


def rip_from(img, out, tmp):
    for sub in ('luna', 'icons', 'fonts', 'sounds', 'wallpapers', 'pictures', 'logon'):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    res = os.path.join(tmp, 'aero10')
    run7z('x', '-tPE', '-y', '-o' + res, find(img, 'Windows/Resources/Themes/aero/aero.msstyles'))
    rd = os.path.join(res, '.rsrc')
    luna = os.path.join(out, 'luna')
    n = 0
    for f in os.listdir(os.path.join(rd, 'IMAGE')):
        data = open(os.path.join(rd, 'IMAGE', f), 'rb').read()
        if data[:4] == b'\x89PNG':
            try:
                write_png(os.path.join(luna, f.split('.')[0] + '.png'), *read_png(data))
                n += 1
            except (ValueError, KeyError) as e:
                print(f'warning: image {f}: {e}', file=sys.stderr)
    recs = parse_msstyles(rd)
    dwm = {}
    atlas = [v for c, p, s, pid, t, v in recs if c == 'DWMWindow' and pid == 213]
    stream = os.path.join(rd, 'STREAM')
    if atlas and os.path.isdir(stream):
        hit = [f for f in os.listdir(stream) if f.split('.')[0] == str(atlas[0])]
        if hit:
            aw, ah, apx = read_png(open(os.path.join(stream, hit[0]), 'rb').read())
            for c, p, s, pid, t, v in recs:
                if c == 'DWMWindow' and pid == 8002 and s == 0:
                    x0, y0, x1, y1 = v
                    if 0 <= x0 < x1 <= aw and 0 <= y0 < y1 <= ah:
                        write_png(os.path.join(luna, f'dwm-{p}.png'), *crop(aw, ah, apx, x0, y0, x1 - x0, y1 - y0))
                        dwm[p] = f'dwm-{p}'
    path = os.path.join(luna, 'normalblue.ini')
    nsec = write_ini(recs, path, dwm)
    text = open(path).read().replace('; Windows 7 Aero, decompiled', '; Windows 10, decompiled', 1)
    open(path, 'w').write(text)
    for sec, alias in (('Button.CheckBox', 'checkbox13'), ('Button.RadioButton', 'radiobutton13')):
        m = re.search(r'\[' + re.escape(sec) + r'\]\n(?:[^\[]*?)ImageFile1 = Aero\\(\d+)\.png', text)
        if m and os.path.exists(os.path.join(luna, m.group(1) + '.png')):
            shutil.copy(os.path.join(luna, m.group(1) + '.png'), os.path.join(luna, alias + '.png'))

    fonts = find(img, 'Windows/Fonts')
    for f in os.listdir(fonts) if fonts else []:
        if f.lower().endswith('.ttf'):
            shutil.copy(os.path.join(fonts, f), os.path.join(out, 'fonts', f.lower()))
    media = find(img, 'Windows/Media')
    for f, name in SOUNDS.items():
        if media and os.path.exists(os.path.join(media, f)):
            shutil.copy(os.path.join(media, f), os.path.join(out, 'sounds', name + '.wav'))

    web = find(img, 'Windows/Web')
    wall = os.path.join(web, 'Wallpaper') if web else None
    if wall and os.path.exists(os.path.join(wall, 'Windows', 'img0.jpg')):
        shutil.copy(os.path.join(wall, 'Windows', 'img0.jpg'), os.path.join(out, 'wallpapers', 'Windows 10.jpg'))
    four_k = os.path.join(web, '4K', 'Wallpaper', 'Windows') if web else None
    if four_k and os.path.isdir(four_k):
        # the hero picture at every screen shape Windows ships; the desktop picks the closest
        for f in os.listdir(four_k):
            m = re.match(r'img0_(\d+)x(\d+)\.jpg', f)
            if m:
                shutil.copy(os.path.join(four_k, f), os.path.join(out, 'wallpapers', f'.hero-{m[1]}x{m[2]}.jpg'))
    for sub, label in WALL_SETS.items():
        d = os.path.join(wall, sub) if wall else None
        jpgs = sorted((f for f in os.listdir(d) if f.endswith('.jpg')), key=lambda f: int(re.sub(r'\D', '', f) or 0)) \
            if d and os.path.isdir(d) else []
        for k, f in enumerate(jpgs, 1):
            shutil.copy(os.path.join(d, f), os.path.join(out, 'wallpapers', f'{label} {k}.jpg'))
    screen = os.path.join(web, 'Screen') if web else None
    for k, f in enumerate(sorted(os.listdir(screen)) if screen and os.path.isdir(screen) else [], 1):
        ext = f.rsplit('.', 1)[-1].lower()
        shutil.copy(os.path.join(screen, f), os.path.join(out, 'logon', f'lock-{k}.{ext}'))

    pics = find(img, 'ProgramData/Microsoft/User Account Pictures')
    for f in os.listdir(pics) if pics else []:
        if f.lower().endswith('.png'):
            shutil.copy(os.path.join(pics, f), os.path.join(out, 'pictures', f.lower()))
    if pics and os.path.exists(os.path.join(pics, 'user-192.png')):
        shutil.copy(os.path.join(pics, 'user-192.png'), os.path.join(out, 'userpic.png'))

    extracted = {}
    for file, gid, name in ICONS:
        if file not in extracted:
            src = find(img, 'Windows/SystemResources/' + file)
            d = os.path.join(tmp, 'res10-' + file)
            if src:
                run7z('x', '-tPE', '-y', '-o' + d, src, '.rsrc/GROUP_ICON/*', '.rsrc/ICON/*')
            extracted[file] = os.path.join(d, '.rsrc')
        for size in (48, 32, 16):
            try:
                write_png(os.path.join(out, 'icons', f'{name}-{size}.png'), *icon_from_group(extracted[file], gid, size))
            except (OSError, ValueError, struct.error) as e:
                print(f'warning: icon {name}: {e}', file=sys.stderr)
                break
    cur = find(img, 'Windows/Cursors')
    if cur:
        rip_cursors(cur, out)
        # same pointers as Windows 7; give them Windows 10's theme name
        src, dst = os.path.join(out, 'cursors', 'expede-7'), os.path.join(out, 'cursors', 'expede-10')
        if os.path.isdir(dst):
            shutil.rmtree(dst)
        os.rename(src, dst)
        idx = os.path.join(dst, 'index.theme')
        open(idx, 'w').write('[Icon Theme]\nName=Expede 10\nComment=Windows 10 pointers from your own Windows 10 disc\n')
    return n, nsec


def rip(iso, out):
    work = os.path.dirname(os.path.abspath(out))
    os.makedirs(work, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work, prefix='.w10rip-') as tmp:
        print('w10rip: unpacking install.wim (about 5 GB, this takes a few minutes)')
        run7z('e', '-y', '-o' + tmp, iso, 'sources/install.wim')
        wim = os.path.join(tmp, 'install.wim')
        idx, name = pick_image(wim, tmp)
        print(f'w10rip: using {name}')
        root = os.path.join(tmp, 'img')
        run7z('x', '-y', '-o' + root, wim, *[f'{idx}/{p}' for p in WIM_FILES])
        os.remove(wim)
        return rip_from(os.path.join(root, idx), out, tmp)


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.expanduser('~/.local/share/expede/10')
    rip(sys.argv[1], out)
    import gtktheme
    gtk = gtktheme.build(out, os.path.join(os.path.dirname(os.path.dirname(out)), 'themes'))
    print(f'w10rip: Windows 10 theme -> {out}; GTK theme -> {gtk}')


if __name__ == '__main__':
    main()
