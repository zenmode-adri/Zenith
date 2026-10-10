#!/bin/bash
# Sube theme.xml (y lo demas que cambie) y reinicia ES. Uso: redeploy.sh [fichero-relativo ...]
HK=SHA256:84k3Gu6Slf8Xh/VmU35IDd3xtDA+/TwSSBReAHMiqC4
SRC=/c/Users/epsil/Documents/r36-kernel-mainline/SESION-2026-10-04-OS-DEFINITIVO/temas-zenith/zenith-switch-claro
FILES=${@:-theme.xml}
for f in $FILES; do
  timeout 60 "/c/Program Files/PuTTY/pscp.exe" -batch -hostkey $HK -pw rocknix "$SRC/$f" root@192.168.1.118:/storage/zenith-test/es-theme-zenith-switch/$f < /dev/null | tail -c 50; echo
done
timeout 90 "/c/Program Files/PuTTY/plink.exe" -batch -hostkey $HK -pw rocknix root@192.168.1.118 'systemctl restart essway; sleep 14; systemctl is-active essway; grep -iE "theme|zenith" /var/log/es_log.txt | grep -v "has no games\|does not exist" | tail -8' < /dev/null
