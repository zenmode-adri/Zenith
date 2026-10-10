import re, os

T = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes'

ALL = [('iconSystem', 'system'), ('iconUpdates', 'update'), ('iconControllers', 'controller'), ('iconGames', 'games'),
       ('iconUI', 'ui'), ('iconSound', 'sound'), ('iconNetwork', 'network'), ('iconScraper', 'scraper'),
       ('iconAdvanced', 'advanced'), ('iconQuit', 'quit'), ('iconRetroachievements', 'achievements'), ('iconKodi', 'kodi'),
       ('iconRestart', 'restart'), ('iconShutdown', 'shutdown'), ('iconFastShutdown', 'shutdown'), ('iconNetplay', 'netplay'),
       ('iconManual', 'manual'), ('iconPlanemode', 'planemode'), ('searchIcon', 'search')]
MISSING = [x for x in ALL if x[0] in ('iconNetplay', 'iconManual', 'iconPlanemode', 'searchIcon')]
SETS = {'es-theme-art-book-next': MISSING, 'es-theme-iconic': MISSING, 'es-theme-techdweeb': ALL, 'es-theme-xmb-fcamod': ALL}


def rw(path, fn):
    raw = open(path, 'rb').read().decode('utf-8')
    eol = '\r\n' if '\r\n' in raw else '\n'
    new = fn(raw.replace('\r\n', '\n')).replace('\n', eol)
    open(path, 'wb').write(new.encode('utf-8'))


for name, icons in SETS.items():
    ov = f'{T}/{name}/overlay'
    os.makedirs(ov, exist_ok=True)
    body = ''.join(f'         <{k}>./zenith-menu/menu-icon-{v}.svg</{k}>\n' for k, v in icons)
    xml = ('<theme>\n   <formatVersion>7</formatVersion>\n'
           '   <!-- Zenith: line icons for the menu entries this theme does not define (menuIcons can be extended) -->\n'
           '   <view name="menu">\n      <menuIcons name="menuicons">\n' + body + '      </menuIcons>\n   </view>\n</theme>\n')
    open(f'{ov}/zenith-menu-icons.xml', 'w', encoding='utf-8', newline='').write(xml)

    def edit_pkg(raw, name=name):
        if 'zenith-menu' in raw:
            return raw
        copy = ('    # Zenith: menu icons for the entries this theme does not define (same line icons as the Zenith theme)\n'
                '    cp -rf ${PKG_DIR}/../es-theme-zenith/theme/assets/menu ${INSTALL}/usr/share/themes/${PKG_NAME}/zenith-menu\n')
        inc = "    sed -i 's#</theme>#  <include>./zenith-menu-icons.xml</include>\\n</theme>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml\n"
        if './zenith-volume.xml' in raw:
            # techdweeb / xmb: anadir el segundo include en la misma linea sed
            raw = raw.replace('<include>./zenith-volume.xml</include>\\n</theme>',
                              '<include>./zenith-volume.xml</include>\\n  <include>./zenith-menu-icons.xml</include>\\n</theme>', 1)
            anchor = "    cp -f ${PKG_DIR}/NOTICE.md"
            assert anchor in raw
            return raw.replace(anchor, copy + anchor, 1)
        # art book / iconic: bloque nuevo tras la copia del overlay
        m = re.search(r"    fi\n", raw)
        assert m, name
        return raw[:m.end()] + copy + inc + raw[m.end():]

    rw(f'{T}/{name}/package.mk', edit_pkg)
    print('ok', name, len(icons), 'iconos')

# contraste: Iconic (esquema claro) y XMB (texto de ayuda)
def iconic_contrast(raw):
    if '707070' in raw:
        return raw
    return raw


p = f'{T}/es-theme-iconic/package.mk'


def iconic_pkg(raw):
    if 'systemInfoSecondaryColor' in raw:
        return raw
    line = ("    # Zenith: darker secondary text in the light colour scheme (contrast)\n"
            "    sed -i '/ifSubset=\"color-scheme:light\"/,/<\\/variables>/{s#<systemInfoSecondaryColor>a0a0a0#<systemInfoSecondaryColor>707070#;"
            "s#<menuFooterColor>a0a0a0#<menuFooterColor>6e6e6e#}' ${INSTALL}/usr/share/themes/${PKG_NAME}/_inc/ui-components/theme-variables.xml\n")
    anchor = "    cp -f ${PKG_DIR}/NOTICE.md"
    assert anchor in raw
    return raw.replace(anchor, line + anchor, 1)


rw(p, iconic_pkg)


def xmb_help(raw):
    n = raw.count('ffffff66')
    print('xmb: ffffff66 aparece', n, 'veces')
    return raw.replace('<helpTextColor>ffffff66</helpTextColor>', '<helpTextColor>ffffffAA</helpTextColor>') \
              .replace('<helpIconColor>ffffff66</helpIconColor>', '<helpIconColor>ffffffAA</helpIconColor>')


rw(f'{T}/es-theme-xmb-fcamod/overlay/theme.xml', xmb_help)
