#!/usr/bin/env python3
"""Genera 0012-es-compact-help.patch sobre el arbol de ES ya parcheado (0001-0011)."""
import glob, os, shutil, subprocess, sys

SRC = glob.glob('/home/epsil/r36s-fork/build.ROCKNIX-RK3326.aarch64/build/emulationstation-cada*/')[0].rstrip('/')
WORK = '/home/epsil/es0012'
shutil.rmtree(WORK, ignore_errors=True)
F = 'es-app/src/views/gamelist/ISimpleGameListView.cpp'
for d in ('a', 'b'):
    os.makedirs(os.path.dirname(f'{WORK}/{d}/{F}'), exist_ok=True)
    shutil.copy(f'{SRC}/{F}', f'{WORK}/{d}/{F}')

OLD = '/mnt/c/tmp/zt/es/0012-es-compact-help.patch'
if os.path.exists(OLD):
    for d in ('a', 'b'):
        subprocess.run(['patch', '-R', '-p1', '-s', '-f', '-i', OLD], cwd=f'{WORK}/{d}', check=False)


def edit(old, new):
    p = f'{WORK}/b/{F}'
    s = open(p, encoding='utf-8', newline='').read()
    if s.count(old) != 1:
        sys.exit(f'{s.count(old)} COINCIDENCIAS: {old[:80]!r}')
    open(p, 'w', encoding='utf-8', newline='').write(s.replace(old, new, 1))


edit('\tbool invertNorthButton = Settings::getInstance()->getBool("GameOptionsAtNorth");\n\n\tprompts.push_back(HelpPrompt(BUTTON_BACK',
     '\tbool invertNorthButton = Settings::getInstance()->getBool("GameOptionsAtNorth");\n\n'
     '\t// small screens (e.g. 480x320): the help bar overflows with the long labels, so use short ones and skip the rare "OPTIONS"\n'
     '\tconst bool compactHelp = Renderer::getScreenWidth() <= 560;\n\n\tprompts.push_back(HelpPrompt(BUTTON_BACK')
edit('\tif (!UIModeController::getInstance()->isUIModeKid())\n\t\tprompts.push_back(HelpPrompt("select", _("OPTIONS")',
     '\tif (!compactHelp && !UIModeController::getInstance()->isUIModeKid())\n\t\tprompts.push_back(HelpPrompt("select", _("OPTIONS")')
edit('HelpPrompt("x", _("GAME OPTIONS") + std::string("/") + _("FAVORITE"), [&]',
     'HelpPrompt("x", compactHelp ? std::string(_("GAME OPTIONS")) : _("GAME OPTIONS") + std::string("/") + _("FAVORITE"), [&]')
edit('HelpPrompt("x", _("SAVE STATES") + std::string("/") + _("FAVORITE"), [&]',
     'HelpPrompt("x", compactHelp ? std::string(_("SAVE STATES")) : _("SAVE STATES") + std::string("/") + _("FAVORITE"), [&]')
edit('HelpPrompt("y", _("SEARCH") + std::string("/") + _("RANDOM"), [&]',
     'HelpPrompt("y", compactHelp ? std::string(_("SEARCH")) : _("SEARCH") + std::string("/") + _("RANDOM"), [&]')

r = subprocess.run(['diff', '-ruN', 'a', 'b'], cwd=WORK, capture_output=True, text=True)
patch = r.stdout.replace('\r\n', '\n')
header = ('Zenith 0012: compact help bar on small screens (width <= 560 px): short labels (SEARCH, GAME OPTIONS/SAVE STATES without\n'
          '"/FAVORITE") and no "OPTIONS" entry, so the bar fits at 480x320 instead of overflowing and dropping entries.\n\n')
open('/mnt/c/tmp/zt/es/0012-es-compact-help.patch', 'w', encoding='utf-8', newline='\n').write(header + patch)
print('patch bytes', len(patch))
