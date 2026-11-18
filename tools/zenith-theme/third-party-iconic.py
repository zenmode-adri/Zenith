import re

p = '/home/epsil/r36s-fork/projects/ROCKNIX/packages/ui/themes/es-theme-iconic/package.mk'
raw = open(p, 'rb').read().decode('utf-8')
eol = '\r\n' if '\r\n' in raw else '\n'
t = raw.replace('\r\n', '\n')
if 'game-info-details.xml' in t:
    print('ya aplicado')
else:
    pat = re.compile(r"    # Zenith: darker secondary text in the light colour scheme \(contrast\)\n    sed -i [^\n]*\n")
    assert len(pat.findall(t)) == 1
    new = (
        "    # Zenith: contrast, help bar and long-text fixes found reviewing screenshots at 1:1, 4:3, 3:2 and 16:9\n"
        "    UI=${INSTALL}/usr/share/themes/${PKG_NAME}/_inc/ui-components\n"
        "    sed -i '/ifSubset=\"color-scheme:light\"/,/<\\/variables>/{s#<systemInfoSecondaryColor>a0a0a0#<systemInfoSecondaryColor>707070#;"
        "s#<menuFooterColor>a0a0a0#<menuFooterColor>6e6e6e#;s#<gamelistPrimaryColor>777777#<gamelistPrimaryColor>555555#;"
        "s#<gamelistSecondaryColor>777777#<gamelistSecondaryColor>555555#}' ${UI}/theme-variables.xml\n"
        "    # smaller help text in the system view of wide screens so it does not run into the clock\n"
        "    sed -i 's#<pos>0.48 0.967</pos>#<pos>0.48 0.967</pos><fontSize>0.019</fontSize>#' ${UI}/help.xml\n"
        "    # the release date was cut with an ellipsis\n"
        "    sed -i '/name=\"md_releasedate\"/,/<\\/datetime>/s#<size>0.08 0.024</size>#<size>0.12 0.024</size>#' ${UI}/game-info-details.xml\n"
        "    # long game names were cut without an ellipsis\n"
        "    sed -i 's#<fontSize>0.030333333333333</fontSize>#<fontSize>0.0275</fontSize>#' ${UI}/gamelist-textlist.xml\n")
    t = pat.sub(lambda m: new, t, count=1)
    open(p, 'wb').write(t.replace('\n', eol).encode('utf-8'))
    print('ok iconic package.mk')
