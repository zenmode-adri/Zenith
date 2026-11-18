T = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes'

# Iconic: 0.0275 -> 0.026 (en 4:3 "Kirby: Nightmare in Dream L" seguia cortado)
p = f'{T}/es-theme-iconic/package.mk'
raw = open(p, 'rb').read().decode('utf-8')
if '<fontSize>0.0275</fontSize>' in raw:
    raw = raw.replace('<fontSize>0.0275</fontSize>', '<fontSize>0.026</fontSize>')
    open(p, 'wb').write(raw.encode('utf-8'))
    print('iconic 0.026 ok')

# Art Book Next: letra de la lista de juegos (por defecto) 0.0375 -> 0.0333 para que los nombres largos no se corten
p = f'{T}/es-theme-art-book-next/package.mk'
raw = open(p, 'rb').read().decode('utf-8')
eol = '\r\n' if '\r\n' in raw else '\n'
if 'game list font' not in raw:
    line = ('    # Zenith: game list font a little smaller (default size) so long game names are not cut' + eol +
            "    sed -i '/<textlist name=\"gamelist\">/,/<\\/textlist>/s#<fontSize ifSubset=\"font-size:default\">0.0375</fontSize>#"
            "<fontSize ifSubset=\"font-size:default\">0.0333</fontSize>#' ${INSTALL}/usr/share/themes/${PKG_NAME}/theme.xml" + eol)
    # despues del bloque de copia del overlay
    anchor = '      cp -rf ${PKG_DIR}/overlay/* ${INSTALL}/usr/share/themes/${PKG_NAME}/' + eol + '    fi' + eol
    assert raw.count(anchor) == 1, raw.count(anchor)
    raw = raw.replace(anchor, anchor + line, 1)
    open(p, 'wb').write(raw.encode('utf-8'))
    print('art book ok')
