#!/bin/bash
# Builds a library of zero-byte files with the REAL names of a ROM library, in tmpfs, to test scanning at scale.
# usage: mkfakelib.sh [SRC] [DEST] [COPIES] [GAMELISTS]
#   SRC        real library (default /mnt/f/RECUPERADO_samsung/Biblioteca_final)
#   DEST       output root (default /dev/shm/zlib)
#   COPIES     replicate every file N times as "name (copyK).ext" to scale up (default 1)
#   GAMELISTS  number of synthetic gamelist.xml (default 25)
# Also writes DEST/es_systems.cfg (one <system> per system folder, with extensions of the files found).
set -eu
SRC=${1:-/mnt/f/RECUPERADO_samsung/Biblioteca_final}
DEST=${2:-/dev/shm/zlib}
COPIES=${3:-1}
GL=${4:-25}
SKIP='^(themes|BGM|bezels|savestates|launchimages|tools|bios)$'
rm -rf "$DEST"; mkdir -p "$DEST/roms"
LIST=$(mktemp)
cd "$SRC"
for s in *; do
  [ -d "$s" ] || continue
  [[ "$s" =~ $SKIP ]] && continue
  find "$s" -type f -print0
done > "$LIST"
cd "$DEST/roms"
# directories first, then files, in bulk
tr '\0' '\n' < "$LIST" | sed 's|/[^/]*$||' | sort -u | tr '\n' '\0' | xargs -0 mkdir -p
for k in $(seq 1 "$COPIES"); do
  if [ "$k" = 1 ]; then tr '\0' '\n' < "$LIST" | tr '\n' '\0' | xargs -0 touch
  else tr '\0' '\n' < "$LIST" | sed -E "s|(\.[^./]*)?\$|.copy$k\1|" | tr '\n' '\0' | xargs -0 touch; fi
done
rm -f "$LIST"
# es_systems.cfg
{
  echo '<?xml version="1.0"?>'; echo '<systemList>'
  for s in *; do
    [ -d "$s" ] || continue
    exts=$(find "$s" -type f -name '*.*' | sed -E 's/.*\.([^.]*)$/\1/' | tr 'A-Z' 'a-z' | sort | uniq -c | sort -rn | awk '$1>=1 && $2!="png" && $2!="xml" && $2!="ini" && $2!="srm" && $2!="cfg"{printf ".%s ",$2}')
    [ -n "$exts" ] || continue
    echo "  <system><name>$s</name><fullname>$s</fullname><path>/storage/roms/$s</path><extension>$exts</extension><command>echo %ROM%</command><platform>$s</platform><theme>$s</theme></system>"
  done
  echo '</systemList>'
} > "$DEST/es_systems.cfg"
# synthetic gamelists: ~10 fields per game, for the biggest systems
n=0
for s in $(for d in *; do [ -d "$d" ] && echo "$(find "$d" -type f | wc -l) $d"; done | sort -rn | awk '{print $2}'); do
  [ "$n" -lt "$GL" ] || break
  grep -q "<name>$s</name>" "$DEST/es_systems.cfg" || continue
  find "$s" -maxdepth 1 -type f ! -name '*.png' ! -name '*.xml' | sed 's|^[^/]*/||' | sed 's/&/\&amp;/g' | awk '
    BEGIN{print "<?xml version=\"1.0\"?>\n<gameList>"}
    {n=$0; sub(/\.[^.]*$/,"",n); p=$0;
     printf "  <game>\n    <path>./%s</path>\n    <name>%s</name>\n    <desc>Synthetic description of %s, long enough to look like a scraped blurb with several words in it.</desc>\n    <image>./media/images/%s.png</image>\n    <video>./media/videos/%s.mp4</video>\n    <rating>0.8</rating>\n    <releasedate>19930101T000000</releasedate>\n    <developer>Dev</developer>\n    <publisher>Pub</publisher>\n    <genre>Action</genre>\n    <players>2</players>\n    <playcount>3</playcount>\n  </game>\n", p,n,n,n,n}
    END{print "</gameList>"}' > "$s/gamelist.xml"
  n=$((n+1))
done
echo "systems: $(grep -c '<system>' "$DEST/es_systems.cfg")  files: $(find "$DEST/roms" -type f | wc -l)  gamelists: $n"
