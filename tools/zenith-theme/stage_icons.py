import os, shutil, sys
B = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-zenith'
SRC = '/mnt/c/tmp/zt/ico/menu'
os.makedirs(f'{B}/theme/assets/menu', exist_ok=True)
for f in sorted(os.listdir(SRC)):
    if f.endswith('.svg'):
        shutil.copy(f'{SRC}/{f}', f'{B}/theme/assets/menu/{f}')
names = [('iconSystem','system'),('iconUpdates','update'),('iconControllers','controller'),('iconGames','games'),('iconUI','ui'),
 ('iconSound','sound'),('iconNetwork','network'),('iconScraper','scraper'),('iconAdvanced','advanced'),('iconQuit','quit'),
 ('iconRetroachievements','achievements'),('iconKodi','kodi'),('iconRestart','restart'),('iconShutdown','shutdown'),
 ('iconFastShutdown','shutdown'),('iconNetplay','netplay'),('iconManual','manual'),('iconPlanemode','planemode'),('searchIcon','search')]
block = ['    <menuIcons name="menuicons">'] + [f'      <{k}>./assets/menu/menu-icon-{v}.svg</{k}>' for k, v in names] + ['    </menuIcons>']
for path in (f'{B}/theme/theme.xml', f'{B}/overlay-dark/theme.xml'):
    raw = open(path, 'rb').read().decode('utf-8')
    crlf = '\r\n' in raw
    if '<menuIcons' in raw:
        print('ya tiene menuIcons:', path); continue
    eol = '\r\n' if crlf else '\n'
    lines = raw.split(eol)
    idx = max(i for i, l in enumerate(lines) if '</menuVolume>' in l)
    lines[idx+1:idx+1] = block
    open(path, 'wb').write(eol.join(lines).encode('utf-8'))
    print('ok', path, 'CRLF' if crlf else 'LF')
