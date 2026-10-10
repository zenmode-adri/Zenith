import re

T = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-xmb-fcamod'
ORIG = '/mnt/c/tmp/zt/themes/es-theme-xmb-fcamod'
FONT = './_inc/fonts/FOT-NewRodin Pro DB.otf'

# W, H, alto del grupo, y, x (margen derecho), separacion, tamano de letra
PARAMS = {
    '1-1':  dict(h=0.0375, y=0.01875, x=-0.0306, sep=0.0125, fs=0.027),
    '4-3':  dict(h=0.0417, y=0.0242, x=-0.025,  sep=0.0156, fs=0.0333),
    '16-9': dict(h=0.0417, y=0.0242, x=-0.0187, sep=0.0156, fs=0.0333),
    '21-9': dict(h=0.0417, y=0.0242, x=-0.0143, sep=0.0156, fs=0.0333),
}

BLOCK = '''      <batteryIndicator name="batteryIndicator">
         <visible>false</visible>
      </batteryIndicator>
      <text name="clock">
         <pos>1 1</pos>
      </text>
      <stackpanel name="xmb-status" extra="true">
         <h>{h}</h>
         <w>1</w>
         <pos>{x} {y}</pos>
         <separator>{sep}</separator>
         <zIndex>100</zIndex>
         <orientation>horizontal</orientation>
         <reverse>true</reverse>
         <clock name="xmb-clock" extra="true">
            <color>ffffff</color>
            <fontSize>{fs}</fontSize>
            <lineSpacing>1</lineSpacing>
            <fontPath>{font}</fontPath>
         </clock>
         <batteryText name="xmb-battery-text" extra="true">
            <color>ffffffcc</color>
            <fontSize>{fs}</fontSize>
            <lineSpacing>1</lineSpacing>
            <fontPath>{font}</fontPath>
         </batteryText>
         <batteryIcon name="xmb-battery-icon" extra="true">
            <maxSize>1 1</maxSize>
            <color>ffffff</color>
            <full>./zenith-status/battery-icon-100.svg</full>
            <at75>./zenith-status/battery-icon-75.svg</at75>
            <at50>./zenith-status/battery-icon-50.svg</at50>
            <at25>./zenith-status/battery-icon-25.svg</at25>
            <empty>./zenith-status/battery-icon-0.svg</empty>
            <incharge>./zenith-status/battery-icon-charging.svg</incharge>
         </batteryIcon>
         <networkIcon name="xmb-network-icon" extra="true">
            <maxSize>1 1</maxSize>
            <color>ffffff</color>
            <networkIcon>./zenith-status/network-icon-on.svg</networkIcon>
            <planemodeIcon>./zenith-status/network-icon-off.svg</planemodeIcon>
         </networkIcon>
      </stackpanel>
'''

for key, p in PARAMS.items():
    raw = open(f'{ORIG}/aspect-ratio-{key}.xml', 'rb').read().decode('utf-8')
    eol = '\r\n' if '\r\n' in raw else '\n'
    txt = raw.replace('\r\n', '\n')
    # quitar reloj y bateria originales del primer <view name="screen">
    a = txt.index('<view name="screen">')
    b = txt.index('</view>', a)
    view = txt[a:b]
    view = re.sub(r'\s*<text name="clock">.*?</text>', '', view, flags=re.S)
    view = re.sub(r'\s*<batteryIndicator name="batteryIndicator">.*?</batteryIndicator>', '', view, flags=re.S)
    view = view.rstrip() + '\n' + BLOCK.format(font=FONT, **p) + '   '
    txt = txt[:a] + view + txt[b:]
    open(f'{T}/overlay/aspect-ratio-{key}.xml', 'wb').write(txt.replace('\n', eol).encode('utf-8'))
    print('xmb', key, 'ok')

# package.mk: copiar los iconos de estado de Zenith
p = f'{T}/package.mk'
raw = open(p, 'rb').read().decode('utf-8')
if 'zenith-status' not in raw:
    eol = '\r\n' if '\r\n' in raw else '\n'
    anchor = '    cp -f ${PKG_DIR}/NOTICE.md'
    line = ('    # Zenith: battery / Wi-Fi icons for the status group (same as the Zenith theme)' + eol +
            '    cp -rf ${PKG_DIR}/../es-theme-zenith/theme/assets/icons ${INSTALL}/usr/share/themes/${PKG_NAME}/zenith-status' + eol)
    assert anchor in raw
    open(p, 'wb').write(raw.replace(anchor, line + anchor, 1).encode('utf-8'))
    print('package.mk ok')
