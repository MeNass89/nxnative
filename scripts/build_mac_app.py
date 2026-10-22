#!/usr/bin/env python3
# Mac app for Dark Souls Remastered (title 01004AB00A260000, update 1.0.3 = v196608) from a finished scripts/build_ipa_from_nsp.py run on the same NSP folder:
#   the host (dm/host, built here for this game) + the rewritten modules (one main per internal resolution 1080p / 1440p / 4K and frame rate 60 / 90 / 120, patch_nso.py
#   with DM_DSR_RES / DM_DSR_FPS) + the Metal shader libraries + the game's RomFS, all as APFS clones of <NSP folder>/01004AB00A260000_build (no extra disk on APFS),
#   and the launcher (dm/mac/Launcher.swift: settings window with graphics, controls, audio, mods and saves) as the app's executable. Saves live in
#   ~/Library/Application Support/nxnative/DarkSoulsRemastered, outside the app, so rebuilding the app keeps them.
# Two ways to run it: open this file in Spyder and press F5 (it uses the values under "Settings" below; an empty NSP folder is asked for at run time);
#   or in a terminal: python3 scripts/build_mac_app.py <NSP folder> [-o <app path>] (command-line values override "Settings"; exit code 0 on success, 1 on error).
# Needs: the Python that ran build_ipa_from_nsp.py (capstone, lz4), Xcode's command-line tools (swiftc, make, codesign). The first run rewrites the nine mains (about 1 minute each);
#   later runs reuse a main whose rewriter (patch_nso.py) has not changed since.
import os, shutil, subprocess, sys
from pathlib import Path

# ---- Settings ----
NSP_DIR = ''                                                   # folder holding the game's NSPs, where build_ipa_from_nsp.py left 01004AB00A260000_build (empty = ask at run time)
OUT_APP = '~/Applications/Dark Souls Remastered.app'           # the app to write (replaced if it exists; saves are not inside it)
GLYPHS = ''                                                    # optional folder of controller button pictures (<ps|xbox>-<position|label> mods); empty = none

TITLE, VERSION, DISPLAY = '01004AB00A260000', 'v196608', '1.0.3'
REPO = Path(__file__).resolve().parent.parent
MODES = [(res, fps) for res in ('', '2k', '4k') for fps in (60, 90, 120)]   # main<res><fps>: res '' = 1080p, 2k = 1440p, 4k = 2160p (the launcher's menus)
RES_ENV = {'': None, '2k': '1440', '4k': '2160'}


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode: raise SystemExit(f'failed ({r.returncode}): {" ".join(map(str, cmd))}')


def clone(src, dst):   # APFS clone (cp -c), a plain copy elsewhere
    run(['cp', '-c', '-R', src, dst])


def build(nsp_dir, out, glyphs):
    b = Path(nsp_dir).expanduser() / f'{TITLE}_build'
    exefs, romfs = b / 'extract' / VERSION / 'ExeFS', b / 'extract' / VERSION / 'RomFS'
    patched, cache, profile = b / f'patched_{VERSION}', b / 'shader' / 'cache', REPO / 'dm' / 'games' / TITLE
    for p, why in ((exefs / 'main', 'the extracted game (run build_ipa_from_nsp.py on this folder first; keep the extracted files)'),
                   (romfs, 'the extracted RomFS'), (patched, 'the rewritten modules from build_ipa_from_nsp.py'), (cache, 'the Metal shader libraries from build_ipa_from_nsp.py'),
                   (profile / 'game.tsv', 'the game profile in this repository')):
        if not p.exists(): raise SystemExit(f'missing {p}: {why}')
    print('[1/5] host')
    r = subprocess.run(['make', '-C', str(REPO / 'dm' / 'host'), '-j8', f'GAME={TITLE}'], capture_output=True, text=True)   # compiler warnings only shown when the build fails
    if r.returncode: raise SystemExit(f'host build failed:\n{(r.stdout + r.stderr)[-3000:]}')
    print('[2/5] rewritten mains (1080p / 1440p / 4K × 60 / 90 / 120)')
    rewriter = REPO / 'dm' / 'tools' / 'patch_nso.py'
    for res, fps in MODES:
        d = b / f'v{res}{fps}'
        if (d / 'main.patched.bin').exists() and (d / 'main.patched.bin').stat().st_mtime > rewriter.stat().st_mtime: continue
        print(f'      main {res or "1080p"} {fps} fps')
        if d.exists(): shutil.rmtree(d)
        clone(patched, d)
        for f in d.glob('main.*'): f.unlink()
        env = dict(os.environ, DM_DSR_FPS=str(fps))
        if RES_ENV[res]: env['DM_DSR_RES'] = RES_ENV[res]
        run([sys.executable, rewriter, exefs / 'main', d, 'main'], env=env, stdout=subprocess.DEVNULL)
    print('[3/5] app files')
    out = Path(out).expanduser()
    if out.exists(): shutil.rmtree(out)
    macos, res_dir = out / 'Contents' / 'MacOS', out / 'Contents' / 'Resources'
    g = res_dir / 'game'
    macos.mkdir(parents=True); g.mkdir(parents=True)
    shutil.copy2(REPO / 'dm' / 'host' / 'dmhost', macos / 'dmhost')
    clone(patched, g / 'patched')
    for res, fps in MODES: clone(b / f'v{res}{fps}', g / f'main{res}{fps}')
    clone(cache, g / 'msl')
    clone(romfs, g / 'romfs')
    (g / 'games' / TITLE).mkdir(parents=True)
    for f in [*profile.glob('*.tsv'), profile / 'dmsh_chain.h']: shutil.copy2(f, g / 'games' / TITLE / f.name)
    if (b / 'build' / 'font').is_dir(): shutil.copytree(b / 'build' / 'font', g / 'font')
    if glyphs: clone(Path(glyphs).expanduser(), g / 'glyphs')
    icon = b / 'extract' / VERSION / 'icon.jpg'
    if icon.exists(): subprocess.run(['sips', '-s', 'format', 'icns', icon, '--out', res_dir / 'AppIcon.icns'], capture_output=True)
    print('[4/5] launcher')
    run(['swiftc', '-O', '-parse-as-library', REPO / 'dm' / 'mac' / 'Launcher.swift', '-o', macos / 'DarkSouls'])
    (out / 'Contents' / 'Info.plist').write_text(f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleName</key><string>Dark Souls Remastered</string>
<key>CFBundleDisplayName</key><string>Dark Souls Remastered</string>
<key>CFBundleIdentifier</key><string>org.nxnative.{TITLE.lower()}</string>
<key>CFBundleExecutable</key><string>DarkSouls</string>
<key>CFBundleIconFile</key><string>AppIcon</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>{DISPLAY}</string>
<key>LSApplicationCategoryType</key><string>public.app-category.games</string>
<key>LSMinimumSystemVersion</key><string>26.0</string>
<key>NSHighResolutionCapable</key><true/>
<key>GCSupportsControllerUserInteraction</key><true/>
</dict></plist>
''')
    print('[5/5] ad hoc signature')
    run(['codesign', '--force', '--deep', '-s', '-', out], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f'built {out}')


def main(argv=None):
    import argparse
    ap = argparse.ArgumentParser(prog='build_mac_app.py', description='Dark Souls Remastered Mac app from a finished build_ipa_from_nsp.py run')
    ap.add_argument('nsp_dir', nargs='?', default=NSP_DIR, help='folder holding the NSPs and 01004AB00A260000_build')
    ap.add_argument('-o', '--out', default=OUT_APP, help=f'app to write (default {OUT_APP})')
    ap.add_argument('--glyphs', default=GLYPHS, help='optional folder of controller button picture mods')
    a = ap.parse_args(argv)
    nsp = a.nsp_dir or (input('NSP folder: ').strip() if sys.stdin.isatty() else '')
    if not nsp: raise SystemExit('no NSP folder given')
    build(nsp, a.out, a.glyphs)


if __name__ == '__main__':
    try: main()
    except SystemExit as e:
        if e.code not in (None, 0): print(e.code if isinstance(e.code, str) else '', file=sys.stderr); sys.exit(1)
