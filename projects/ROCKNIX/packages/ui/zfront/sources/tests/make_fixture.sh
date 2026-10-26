#!/bin/sh
# Builds a fake ES install for the harness: <out>/es_systems.cfg, <out>/roms/<system>/*, <out>/system.cfg
# Systems (in order): tools, auto-favorites, gbc (14 games), psp, music (2 games). Same carousel as the console capture.
set -e
OUT=${1:?usage: make_fixture.sh <outdir>}
rm -rf "$OUT"; mkdir -p "$OUT/roms"
mk() { # name fullname ext command-core emulator
  mkdir -p "$OUT/roms/$1"
}
mk tools; mk auto-favorites; mk gbc; mk psp; mk music
touch "$OUT/roms/tools/Wifi.sh" "$OUT/roms/tools/Tuner.sh" "$OUT/roms/tools/Backup.sh"
touch "$OUT/roms/auto-favorites/Fav.gb"
i=0
for n in "Donkey Kong Country" "Dragon Warrior Monsters 2" "Harvest Moon 3 GBC" "Zelda: Link's Awakening" "Mario Golf" "Mario Tennis" "Mega Man Xtreme" "Perfect Dark" "Shantae" "Rayman" "Super Mario Bros. Deluxe" "Tetris DX" "The Wario Land 3" "A Bug's Life"; do
  touch "$OUT/roms/gbc/$n.gbc"
done
touch "$OUT/roms/psp/God of War (USA).iso" "$OUT/roms/psp/Tekken [!].iso"
touch "$OUT/roms/music/a.mp3" "$OUT/roms/music/b.mp3"
cat > "$OUT/es_systems.cfg" <<'EOF2'
<?xml version="1.0"?>
<systemList>
  <system>
    <name>tools</name><fullname>Tools</fullname><path>/storage/roms/tools</path><extension>.sh</extension>
    <command>/usr/bin/runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% --controllers="%CONTROLLERSCONFIG%"</command>
    <theme>tools</theme>
  </system>
  <system>
    <name>auto-favorites</name><fullname>Favorites</fullname><path>/storage/roms/auto-favorites</path><extension>.gb</extension>
    <command>/usr/bin/runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR%</command>
    <theme>auto-favorites</theme>
  </system>
  <system>
    <name>gbc</name><fullname>Game Boy Color</fullname><path>/storage/roms/gbc</path><extension>.gbc .GBC .zip</extension>
    <command>/usr/bin/runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% --controllers="%CONTROLLERSCONFIG%"</command>
    <platform>gbc</platform><theme>gbc</theme>
    <emulators>
      <emulator name="retroarch"><cores><core default="true">gambatte</core><core>mgba</core><core>sameboy</core></cores></emulator>
    </emulators>
  </system>
  <system>
    <name>psp</name><fullname>PlayStation Portable</fullname><path>/storage/roms/psp</path><extension>.iso .cso .pbp</extension>
    <command>/usr/bin/runemu.sh %ROM% -P%SYSTEM% --core=%CORE% --emulator=%EMULATOR% --controllers="%CONTROLLERSCONFIG%"</command>
    <theme>psp</theme>
    <emulators>
      <emulator name="ppsspp"><cores><core default="true">ppsspp-sa</core></cores></emulator>
      <emulator name="retroarch"><cores><core>ppsspp</core></cores></emulator>
    </emulators>
  </system>
  <system>
    <name>music</name><fullname>Music</fullname><path>/storage/roms/music</path><extension>.mp3</extension>
    <command>/usr/bin/runemu.sh %ROM% -P%SYSTEM%</command>
    <theme>music</theme>
  </system>
</systemList>
EOF2
printf 'gbc.core=mgba\npsp["Tekken [!].iso"].emulator=retroarch\nsystem.language=es_ES\n# comment\naudio.volume=60\n' > "$OUT/system.cfg"
