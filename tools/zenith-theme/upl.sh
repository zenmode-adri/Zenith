#!/bin/bash
# Sube ficheros del tema a la consola sin reiniciar ES. Uso: upl.sh fichero-relativo ...
HK=SHA256:84k3Gu6Slf8Xh/VmU35IDd3xtDA+/TwSSBReAHMiqC4
SRC=${ZENITH_THEMES_SRC:-temas-zenith}/zenith-switch-claro
for f in "$@"; do timeout 60 "/c/Program Files/PuTTY/pscp.exe" -batch -hostkey $HK -pw rocknix "$SRC/$f" root@192.168.1.118:/storage/zenith-test/es-theme-zenith-switch/$f < /dev/null | tail -c 40; echo; done
