# Instalación de zfront en la imagen (y vuelta a ES)

Esquema: **ES no se toca** (`/usr/bin/emulationstation`, respaldo) y zfront se instala **al lado** como `/usr/bin/zfront`. Un ajuste de
`system.cfg` decide cuál arranca `start_es.sh`. Nada de esto está aplicado al fork del SO: los parches están en `os-patches/`.

## 1. Qué instala `cmake --install`

    cmake -S cpp -B build/aarch64 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/aarch64-rocknix.cmake
    ninja -C build/aarch64
    DESTDIR=/ruta/staging cmake --install build/aarch64 --prefix /usr --strip

| destino | contenido |
|---|---|
| `/usr/bin/zfront` | el binario (SDL2, FreeType, libstdc++, libm, libgcc_s y libc: todo está ya en la imagen; `readelf -d` solo debe mostrar esas) |
| `/usr/share/zfront/locale/<idioma>/LC_MESSAGES/zfront.mo` | textos propios de zfront; `msgfmt` los compila al construir (hace falta gettext en la máquina de construcción). Los textos que ES ya traduce salen del catálogo de ES |
| `/usr/share/zfront/theme-dark`, `theme-light` | el tema Zenith ya combinado (oscuro = claro + `overlay-dark`, como `es-theme-zenith/package.mk`). Origen: `-DZF_THEME_SRC=<paquete es-theme-zenith>` (por defecto el del fork). `-DZF_INSTALL_THEME=OFF` si no se quiere duplicar: la imagen ya trae `/usr/share/themes/es-theme-zenith{,-dark}` y zfront los encuentra (apartado 3) |
| `/usr/share/doc/zfront/` | este documento, `os-contract.md` y los parches `os-patches/` |

No hay carpeta de scripts que instalar: los scripts de eventos (`game-start`, `game-selected`...) son los de siempre en
`/storage/.config/emulationstation/scripts/<evento>/`, los mismos que lee ES.

Para empaquetar en el SO basta un paquete nuevo (`projects/ROCKNIX/packages/ui/zfront/package.mk`, toolchain `cmake`, dependencias
`SDL2 freetype`) que ejecute lo de arriba; no se ha creado.

## 2. Elegir zfront o ES

Un solo ajuste, en `/storage/.config/system/configs/system.cfg`:

    set_setting system.frontend zfront     # arranca zfront
    del_setting system.frontend            # vuelve a ES
    systemctl restart essway               # aplica el cambio sin reiniciar (o "RESTART EMULATIONSTATION" desde el menú actual)

`start_es.sh` (parche `0001`) lanza zfront solo si `system.frontend=zfront`, `/usr/bin/zfront` existe y no existe `/run/zfront.failed`.
Si zfront sale con error **antes de dibujar el primer fotograma** se crea `/run/zfront.failed` (vale hasta el reinicio) y esa misma
pasada sigue con ES: un zfront roto nunca deja la consola sin interfaz. Un fallo posterior es una salida normal (systemd reintenta a los 2 s).

Argumentos por defecto: `--systems /storage/.config/emulationstation/es_systems.cfg --fullscreen --log-path /var/log`; se pueden
cambiar con `set_setting zfront.args "..."`. No hacen falta más:

* `--theme` se deduce de `ThemeSet` (el tema que ES tiene elegido): `es-theme-zenith-dark` (o cualquier nombre con «dark») -> oscuro, `es-theme-zenith` -> claro (es el ajuste de fábrica de la imagen);
  zfront solo tiene Zenith, así que sin ajuste o con otro tema de ES (Art Book Next...) usa el claro. Busca la carpeta en
  `/storage/.config/emulationstation/themes/`, `/usr/share/themes/` y `/usr/share/zfront/`. `--theme <carpeta>` fuerza una.
* `--size` se deduce de la salida real: con `--fullscreen` la ventana toma el tamaño de la salida y, si su proporción no es la del
  tema cargado, el tema se recarga antes del primer fotograma (480x320, 640x480, 720x405, 720x720...).
* `--log-path DIR` escribe stderr en `DIR/zfront.log` (se reinicia pasado 1 MB). ES guarda su log en `emulationstation.log`. La salida de los emuladores no va ahí: `runemu.sh` la manda a `/var/log/exec.log`.

Los ajustes (`system.cfg`, `es_settings.cfg`, `es_input.cfg`, gamelists, savestates) son los mismos ficheros que usa ES: se puede
alternar sin perder nada.

## 3. Volver a ES

    del_setting system.frontend; rm -f /run/zfront.failed; systemctl restart essway

Para quitar zfront del todo basta borrar `/usr/bin/zfront` (con él ausente `start_es.sh` ya arranca ES) y `/usr/share/zfront`.

## 4. Parches del SO (`os-patches/`, desde la raíz del fork con `patch -p1`)

| parche | qué hace |
|---|---|
| `0001-start_es-zfront-selector` | `start_es.sh` arranca zfront si `system.frontend=zfront` (argumentos de arriba) y cae a ES si zfront falla antes del primer fotograma. **Imprescindible** |
| `0002-handle-hdmi-hotplug-zfront` | `handle-hdmi-hotplug` reconoce y reinicia también `zfront` al conectar/desconectar HDMI. Sin él, ese cambio de salida no reinicia el frontend |
| `0003-video_sense-RK3399-zfront` | lo mismo en `video_sense` de RK3399 (no afecta a RK3326) |
| `0004-autostart-prefetch-zfront` | `autostart` precarga en la caché de páginas el binario y las bibliotecas de zfront (lista propia `zfront-prefetch.list`) y vigila el frontend correcto. Solo rendimiento/diagnóstico |
| `0005-screensaver-dim-keeps-backlight` | `10-backlight-off.sh` no apaga la luz con el salvapantallas «dim», para que `dim` sea de verdad atenuar. Opcional |

Detalles, riesgos y la lista de comprobaciones en la consola: `os-contract.md` (apartados 3 y 5).

## 5. Instalación alternativa

Si algún comprobante por nombre fallara, instalar zfront **como** `/usr/bin/emulationstation` y mover ES a otro directorio (los
recursos de ES viven en `/storage/.config`): entonces `pgrep`/`pidof`/`killall` casan sin parchear 0002-0004, pero se pierde el
respaldo simple. Es la alternativa descrita en `os-contract.md` (apartado 3).
