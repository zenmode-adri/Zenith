#!/bin/bash
# Simula es_settings (sin consola): con la locale precompilada NO se ejecuta localedef y LOCPATH apunta a ella;
# sin ella se conserva el respaldo (localedef en el dispositivo). Uso: bash es_settings-locale-test.sh
set -u
SRC="$(cd "$(dirname "$0")" && pwd)/../sources/es_settings"
T=$(mktemp -d /tmp/es_settings-locale-test.XXXXXX)   # queda en /tmp (tmpfs); no se borra
mkdir -p "$T/bin" "$T/storage/.cache" "$T/storage/.config/emulationstation/locale" "$T/etc/profile.d" "$T/pre/es_ES.UTF-8" "$T/log"
echo "export FOO_BAR=1" > "$T/etc/profile.d/999-export"
echo x > "$T/pre/es_ES.UTF-8/LC_NAME"
printf '#!/bin/sh\n[ "$1" = system.language ] && echo "$LANGSET"\n[ "$1" = system.timezone ] && echo UTC\nexit 0\n' > "$T/bin/get_setting"
printf '#!/bin/sh\necho "$@" >> "%s/localedef.calls"\nexit 0\n' "$T" > "$T/bin/localedef"
for c in performance systemctl set_kill; do printf '#!/bin/sh\nexit 0\n' > "$T/bin/$c"; done
chmod +x "$T"/bin/*
sed -e 's#^\. /etc/profile#:#' -e "s#/storage/#$T/storage/#g" -e "s#/etc/profile.d#$T/etc/profile.d#g" \
    -e "s#/var/log/#$T/log/#g" "$SRC" > "$T/es_settings"
fail=0
run() { # $1 idioma  $2 carpeta precompilada -> "LOCPATH|LANG"; deja el numero de llamadas en $T/n
  : > "$T/localedef.calls"
  PATH="$T/bin:$PATH" LANGSET="$1" ZENITH_PRELOCALE="$2" bash -c ". $T/es_settings >/dev/null 2>&1; echo \"\$LOCPATH|\$LANG\"" 2>/dev/null | tail -1
  wc -l < "$T/localedef.calls" > "$T/n"
}
chk() { [ "$1" = "$2" ] && echo "ok    $3" || { echo "FALLO $3: '$1' != '$2'"; fail=1; }; }
r=$(run es_ES "$T/pre"); chk "$r" "$T/pre|es_ES.UTF-8" "con precompilada: LOCPATH a la imagen"; chk "$(tr -d ' ' <"$T/n")" 0 "con precompilada: 0 llamadas a localedef"
r=$(run es_ES "$T/ausente"); chk "$r" "$T/storage/.config/emulationstation/locale|es_ES.UTF-8" "sin precompilada: LOCPATH de /storage"
chk "$(tr -d ' ' <"$T/n")" 1 "sin precompilada: 1 llamada a localedef (respaldo)"
r=$(run xx_YY "$T/pre"); chk "$(tr -d ' ' <"$T/n")" 1 "idioma raro con carpeta presente: respaldo"
exit $fail
