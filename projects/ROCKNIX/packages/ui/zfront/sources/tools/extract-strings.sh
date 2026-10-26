#!/bin/sh
# Collects every translatable string of zfront into a gettext template.
#
#   cpp/tools/extract-strings.sh [OUT.pot] [ES.pot]
#
# Finds tr("..."), ntr("sing", "plural", n) and ctr("ctx", "text") in cpp/src (the English text is the msgid, as in ES's
# own catalogs). Needs GNU gettext (xgettext, msgcomm). If ES's template is found (second argument, $ZF_ES_POT, or the
# usual place in the ES checkout), the strings that ES's catalogs do NOT have are listed on stderr: those need a
# translation of their own in a `zfront` catalog (zfront.po -> `msgfmt -o zfront.mo`, installed next to
# emulationstation2.mo as <locale>/<lang>/LC_MESSAGES/zfront.mo); the others are already translated in 35 languages.
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$here/zfront.pot}
es_pot=${2:-${ZF_ES_POT:-/home/epsil/es-new-work/src/locale/emulationstation2.pot}}

command -v xgettext >/dev/null 2>&1 || { echo "extract-strings: xgettext not found (install GNU gettext)" >&2; exit 1; }

cd "$here"
# xgettext only looks at calls of the listed functions, so th.str("pill"), trim("x") & co are never picked up
files=$(find src -name '*.cpp' -o -name '*.hpp' | sort)
# shellcheck disable=SC2086
xgettext --language=C++ --from-code=UTF-8 --package-name=zfront \
  --msgid-bugs-address=https://github.com/zenmode-adri/Zenith \
  --keyword=tr:1 --keyword=N_:1 --keyword=NC_:1c,2 --keyword=ntr:1,2 --keyword=ctr:1c,2 --add-comments=TRANSLATORS --sort-by-file \
  -o "$out" $files

n=$(grep -c '^msgid "' "$out" || true)
echo "extract-strings: $((n - 1)) strings -> $out"

if [ -f "$es_pot" ] && command -v msgcomm >/dev/null 2>&1; then
  tmp=$(mktemp -d)
  trap 'rm -rf "$tmp"' EXIT
  # messages that are in only one of the two templates, then the ones of those that are ours
  msgcomm --unique "$es_pot" "$out" -o "$tmp/unique.pot" 2>/dev/null || true
  msgcomm "$tmp/unique.pot" "$out" -o "$tmp/missing.pot" 2>/dev/null || true
  miss=$(grep -c '^msgid "' "$tmp/missing.pot" 2>/dev/null || true)
  echo "extract-strings: $((${miss:-0} - 1)) string(s) not in $es_pot (they need a zfront catalog):" >&2
  grep -E '^(#: |msgid ".+")' "$tmp/missing.pot" >&2 2>/dev/null || true
fi
