"""Art Book Next: la caratula tapaba el reloj/wifi/bateria en 4:3, 3:2 y 16:9 (la caratula arrancaba en y~30 px y la barra
llega a ~40 px). Se bajan y se recortan un poco; en 16:9 la fecha pasa a %Y-%m. Genera overlay/aspect-ratio-*.xml del paquete."""
T = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-art-book-next/overlay'
ORIG = '/mnt/c/tmp/zt/themes/es-theme-art-book-next'


def rw(name, pairs):
    raw = open(f'{ORIG}/{name}', 'rb').read().decode('utf-8')
    eol = '\r\n' if '\r\n' in raw else '\n'
    t = raw.replace('\r\n', '\n')
    for old, new in pairs:
        assert t.count(old) == 1, (name, old[:60], t.count(old))
        t = t.replace(old, new, 1)
    open(f'{T}/{name}', 'wb').write(t.replace('\n', eol).encode('utf-8'))
    print('ok', name)


# 4:3 (640x480): caratula de y 60 a 260 (antes 30 a 250); la barra termina en ~40
rw('aspect-ratio-4-3.xml', [(
    '<pos>0.75 0.2916666666666667</pos><!-- 480 140 -->\n         <maxSize>0.40625 0.4583333333333333</maxSize><!-- 260 220 -->',
    '<pos>0.75 0.3333333333333333</pos><!-- 480 160 -->\n         <maxSize>0.40625 0.4166666666666667</maxSize><!-- 260 200 -->')])
# 3:2 (480x320): caratula de y 30 a 170 (antes 20 a 167)
rw('aspect-ratio-3-2.xml', [(
    '<pos>0.75 0.2916666666666667</pos><!-- 480 140 -->\n         <maxSize>0.40625 0.4583333333333333</maxSize><!-- 260 220 -->',
    '<pos>0.75 0.3125</pos><!-- 360 100 -->\n         <maxSize>0.40625 0.4375</maxSize><!-- 195 140 -->')])
# 16:9 (854x480): con y sin barra de ayuda; fecha %Y-%m
rw('aspect-ratio-16-9.xml', [
    ('<pos>0.68757327080891 0.322916666666667</pos><!-- 586.5 155 -->\n         <maxSize>0.555099648300117 0.520833333333333</maxSize><!-- 473.5 250 -->',
     '<pos>0.68757327080891 0.3333333333333333</pos><!-- 586.5 160 -->\n         <maxSize>0.555099648300117 0.4583333333333333</maxSize><!-- 473.5 220 -->'),
    ('<pos>0.68757327080891 0.34375</pos><!-- 586.5 165 -->\n         <maxSize>0.555099648300117 0.5625</maxSize><!-- 473.5 270 -->',
     '<pos>0.68757327080891 0.3645833333333333</pos><!-- 586.5 175 -->\n         <maxSize>0.555099648300117 0.5</maxSize><!-- 473.5 240 -->'),
    ('<format ifSubset="font-size:small|default">%Y-%m-%d</format>', '<format ifSubset="font-size:small|default">%Y-%m</format>'),
])
