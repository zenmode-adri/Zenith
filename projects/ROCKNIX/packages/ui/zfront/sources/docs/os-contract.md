# Contrato con el SO (zfront <-> Zenith)

Rama `cpp/os-contract`. Especificación: `/home/epsil/zf-research/02-contrato-so-es.md`. Aquí: qué cubre el código, cómo se
engancha, qué hay que cambiar en el fork (parches en `os-patches/`, **sin aplicar**) y qué falta por verificar en la consola.

## 1. Código

| fichero | contenido |
|---|---|
| `src/httpd.{hpp,cpp}` | servidor HTTP mínimo y estricto: `parse_request` puro (cabeceras <= 8 KB, cuerpo <= 64 KB, sin chunked, sin obs-fold, Content-Length único) + `Server` (1 hilo bloqueado en `poll(-1)` sobre el socket y un eventfd: 0 despertares en reposo, plazo de lectura 2 s por conexión, `Connection: close`, `SO_REUSEADDR`, todo `CLOEXEC`, señales bloqueadas en su hilo). |
| `src/osapi.{hpp,cpp}` | rutas (`route`) sobre `Ops`; `/runningGame`, `/isIdle`, `/emukill` responden desde estado con mutex (el hilo principal está bloqueado dentro del juego justo cuando se preguntan). |
| `src/osservice.cpp` | `ZF_REGISTER_SERVICE("http")` (127.0.0.1:1234, `ZF_HTTP_PORT` para desarrollo) y `("oscontract")` (espera del socket Wayland). Los efectos en la UI van por `cx.sv->jobs.post(nullptr, hecho)`. |
| `src/oscontract.{hpp,cpp}` | arranque/salida, esperas, `.ready`, `bl_power`, `set_kill`, eventos de scripts, modos de salida, temporizador de inactividad, estado de juego/ocupado, lanzamiento externo. |
| `tests/test_httpd.cpp`, `test_osapi.cpp`, `test_oscontract.cpp`, `test_osservice.cpp` | sockets locales (puerto 0), directorio temporal, ejecutor en dry-run. `ctest -R 'test_[ho]'`. |

### API HTTP (127.0.0.1:1234)

| ruta | respuesta | nota |
|---|---|---|
| `POST /notify[?duration=ms]` cuerpo=texto | 200 / 400 sin cuerpo | toast; 200..60000 ms (defecto 5000); una línea, sin controles, <= 200 B |
| `POST /notify/clear` | 200 | quita todos los toasts (sus plazos son CLOCK_MONOTONIC: no avanzan en suspensión, por eso `sleep.sh` lo llama al despertar) |
| `GET /runningGame` | 200 `{"name","path"}` / **201** `{"msg":"NO GAME RUNNING"}` | `rocknix-fake-suspend` decide con 200/201 |
| `GET /isIdle` | 200 `[ true ]` / 201 `[ false ]` | falso con juego o con un `oscontract::Busy` vivo |
| `GET /quit` `/shutdown` `/restart` | 200 | `Mode::Quit`, `Shutdown`, `Reboot` (como ES: `/restart` reinicia el **sistema**); `/quit?confirm=...` -> 501 (no hay menú de confirmación) |
| `GET /emukill` | 200 `{"killed":bool}` | `killall <nombre de /tmp/.process-kill-data>` solo con juego en marcha y nombre seguro distinto de `emulationstation` (ES llamaba a `batocera-es-swissknife`, que **no existe** en el fork) |
| `POST /launch` cuerpo=ruta | 200 / 400 / 409 | 409 si ya hay juego; busca solo en el sistema cuyo `rom_dir` contiene la ruta |
| resto | 404 JSON `{"error":"not found"}` (405 si la ruta existe con otro método) | `/systems`, `/caps`... no implementados |

### Arranque y salida (`oscontract`)

- `startup(sv, dry_run)` (main, una línea): `bl_power=0`, `/tmp/.process-kill-data`=`emulationstation\n`, manejador de SIGHUP, arranca el reloj de inactividad.
- `wait_roms()` (`ES_WAIT_ROMS_READY`, máx. 15 s) en `main` antes de leer sistemas; `wait_display()` (`ES_WAIT_WAYLAND_SOCKET`, máx. 10 s) en el servicio `oscontract`, justo antes del bucle. Son esperas con **inotify** sobre el directorio padre (sin sondeo; si el directorio no existe aún, pasos de 20 ms). Si vencen, se registra y se sigue (como ES).
- `on_first_frame()` (sdlr, primer fotograma): crea `/tmp/emulationstation.ready` y lanza el evento `start`. Se crea justo **después** de presentar el primer fotograma (ES, justo antes): diferencia <= 1 vsync.
- `finish()` (main, tras el bucle, con `App` aún vivo): para el servidor HTTP, `bl_power=0`, borra `.ready`, y ejecuta el modo de salida. Código de salida siempre 0.
- Señales: SIGTERM/SIGINT las convierte SDL en `SDL_QUIT` (no instalamos manejador propio: SDL solo lo hace si la acción es `SIG_DFL`); SIGHUP lo manejamos nosotros y se reenvía como SIGTERM. **Hueco:** durante un juego SDL no existe y SIGTERM mata zfront sin `finish()`; es el mismo comportamiento que ES (la unidad `essway` se lo lleva por cgroup).
- `app_id`: SDL/Wayland usa el nombre del ejecutable. Si el binario no se llama `emulationstation`, `before_video()` pone `SDL_VIDEO_WAYLAND_WMCLASS=emulationstation` antes de cada `SDL_Init` y `game_started()` lo quita para que los emuladores no lo hereden.

### Eventos y modos de salida

`oscontract::fire_event(evento, {args})` reutiliza `launch::fire_event` (mismas reglas que `game-start`/`game-end`: `<dir>/<evento>/*` y `<dir>/*` con el evento como primer argumento, máx. 3 argumentos). En Linux solo `quit` es síncrono (como ES). Para `*-selected` el "nadie escucha" se cachea 5 s. Helpers: `fire_game_selected`, `fire_system_selected`. `game-selected` y `system-selected` los lanzan las pantallas (`screens.cpp`): el carrusel al parar sobre un sistema (y al volver de la lista) y la lista cuando el cursor descansa 0,12 s (una sola vez al mantener una dirección, y una vez al entrar); sin ningún script instalado no se arma ni un temporizador (`oscontract::selected_listening`, cacheado 5 s; sin `startup`, es decir arnés y vista previa, nunca se lanzan scripts).

`oscontract::quit(Mode)` (hilo principal; los menús lo llaman y devuelven `Transition::none()`): `Reboot`/`Shutdown` lanzan `quit <modo>` + `<modo>` al instante (como `quitES`); `finish()` hace el resto tras cerrar la ventana: `Quit` -> evento `quit`; `Restart` -> `restart.please`; `Reboot` -> `reboot.please` + `shutdown -r now`; `Shutdown` -> `shutdown.please` + `shutdown -P -h now`. `Quit` es "salir del frontend": systemd (`Restart=always`) lo relanza, que es lo que hace "RESTART EMULATIONSTATION". `suspend()` = `systemctl suspend` (camino del botón de encendido; `pm-suspend`, que usa ES, no existe en el fork y por eso ES no ofrece suspender ahí).

### Inactividad / salvapantallas

`ScreenSaverTime` (ms, `es_settings.cfg`; 0 = desactivado; defecto ES 300000, la imagen trae 120000). Sin sondeo: `idle_deadline()` entra en el mínimo de plazos del bucle. Al vencer, según `ScreenSaverBehavior` (`src/screensaver.*` + `oscontract::screensaver_start`):

| tipo | qué hace | luz (`bl_power`) |
|---|---|---|
| `black` (defecto; también `slideshow` / `random video`, que no se portan: ES cae a negro igual sin medios) | un solo rectángulo negro opaco (`App::render` ni siquiera construye las pantallas) | `4` (apagada) |
| `dim` | la vista congelada + rectángulo negro alfa `0xA0` (el valor de ES); sin toasts (ES no los dibuja con salvapantallas) | NO se toca (el script `10-backlight-off.sh` de la imagen apaga la luz con cualquier tipo: parche `os-patches/0005`, sin aplicar, para que `dim` sea de verdad «atenuar») |
| `suspend` | solo con `pm-suspend` (igual que ES; sin él, `black`): `systemctl suspend`, NO queda salvapantallas activo y la cuenta de inactividad vuelve a cero | la gestiona `sleep.sh` |

Se disparan `screensaver-start <black\|dim>` y `sleep` (y `screensaver-stop`, `wake` al despertar). La **primera pulsación** solo despierta (se descarta, `before_acts`): `bl_power=0`, `screensaver-stop`, `wake`; también despierta (y retrasa la cuenta) un botón que no corresponde a ninguna acción (`raw_input`). **Con el salvapantallas activo el bucle está congelado**: no se ejecutan `tick` ni animaciones y NO hay más plazos que el reloj de `dim` (un único despertar por minuto, solo con `DrawClock`; con `black` ninguno): se ignoran el reloj/batería de la barra, el parpadeo del cursor, la repetición de teclas y los toasts hasta que alguien despierte la consola. **No arranca** con un juego en marcha, con un `oscontract::Busy` vivo (scraper, descargas: quien lance un trabajo largo debe tomar uno) ni con un `cx.sv->jobs` ocupado; en ese caso lo vuelve a mirar a los 5 s (nunca deja un plazo ya vencido: el bucle giraría a 1 ms). **Vuelta de una suspensión** (botón de encendido → `sleep.sh` enciende el panel): se detecta porque `CLOCK_BOOTTIME - CLOCK_MONOTONIC` crece (dos lecturas de reloj cada vez que el bucle despierta por su tope de 1 s, o cuando `sleep.sh` llama a `/notify/clear`); termina el salvapantallas y reinicia la cuenta sin que haga falta pulsar. Tras un juego (`game_ended`) y tras cualquier acción el contador vuelve a cero; sin plazo mientras el salvapantallas está activo.

No hay «apagado automático por inactividad» en esta imagen: `system.suspend.enable`, `system.suspend.enable_timed_shutdown` y `system.shutdown_delay(_running_game)` los usa solo `rocknix-fake-suspend`, que sale en el acto cuando hay suspensión de hardware (`system.suspendmode != off`, el caso de RK3326) y de zfront solo consulta `/runningGame` y `/shutdown` (ya cubiertos).

## 2. Líneas tocadas en ficheros compartidos

`main.cpp` (4 líneas + `#include`): `oscontract::startup(cx.sv, args.dry_run)` tras `cx.h = ...` (solo con SDL, no en `--preview`/`--dump`); `oscontract::wait_roms()` antes de cargar sistemas; `sdl_run` -> `bool ran = ...; oscontract::finish(); return ran ? 0 : 1;`.

`sdlr.cpp` (11 líneas + `#include`): `before_video()` al empezar `open_video`; plazo de inactividad en el cálculo de `dl` (2 líneas); `before_acts(...)` tras `repeat_due`; `take_external(app)` + el `for` por índice sobre `acts` (la acción externa `POST /launch` se trata como una más; 2 líneas cambiadas); `game_started`/`game_ended` alrededor de `launch::execute`; `oscontract::input(now)` junto a `prep.set_target`; `on_first_frame()` en el bloque `first_logged`.

Ningún cambio en `app.*`, `services.*`, `launch.*`. El `quit` por HTTP/menús entra por `SDL_PushEvent(SDL_QUIT)` (solo con SDL).

## 3. Cambios en el SO (`os-patches/`, aplicar desde la raíz del fork con `patch -p1`)

Esquema elegido: **ES se queda como está** (`/usr/bin/emulationstation`, respaldo) y zfront se instala como `/usr/bin/zfront` (+ `/usr/share/zfront/theme-*`); un ajuste decide cuál arranca. Alternar: `set_setting system.frontend zfront` (o `del_setting system.frontend`), luego `systemctl restart essway` (o `emulationstation` desde el menú actual: sale con 0 y systemd lo relanza).

| parche | qué | riesgo |
|---|---|---|
| `0001-start_es-zfront-selector.diff` | `start_es.sh` lanza zfront si `system.frontend=zfront`, existe el binario y no existe `/run/zfront.failed`; borra `/tmp/emulationstation.ready` antes de lanzar; si zfront sale con error (no 129/143) SIN haber llegado al primer fotograma (no existe `.ready`), crea `/run/zfront.failed` (vale hasta el reinicio) y sigue con ES en la misma pasada; cualquier otra salida sale con el mismo código (0 = systemd relanza, nunca marca fallo). Argumentos por defecto o `zfront.args` de system.cfg. | Bajo: sin el ajuste no cambia nada. Peor caso con zfront roto: hasta ~25 s de esperas acotadas (display 10 s + ROMs 15 s) antes de caer a ES. Un fallo DESPUÉS del primer fotograma es un fallo normal (systemd reintenta a los 2 s, sin red de seguridad); un cuelgue sin salir no lo detecta. |
| `0002-handle-hdmi-hotplug-zfront.diff` | `pgrep -x` y `killall` también por `zfront` | Medio si **no** se aplica: sin él el cambio de salida HDMI no reinicia el frontend (y el script sale en la primera línea). |
| `0003-video_sense-RK3399-zfront.diff` | idem para RK3399 | Nulo en RK3326. |
| `0004-autostart-prefetch-zfront.diff` | lista de precarga propia (`zfront-prefetch.list`), `pidof` del frontend activo (2 sitios) | Bajo: solo rendimiento/diagnóstico; el `-nt` compara con el binario correcto. |

Sin cambio necesario (comprobado en el fork): `essway.service` (sigue ejecutando `start_es.sh`), `sleep.sh` (el aviso usa `app_id` y `/notify`, ambos cumplidos), reglas de Sway (`app_id="emulationstation"`, por `before_video`), `screen_switch` y `mako-notify` (app_id), `010-uimode`/`userconfig-setup` (miran `/usr/bin/emulationstation`, que sigue), `r36-tuner` (`emulationstation.service` no existe en RK3326, ya hoy), `es-scripts` de salvapantallas (zfront dispara los mismos eventos), `rocknix-fake-suspend` (`/runningGame` y `/shutdown`).

Instalación alternativa (si algún comprobante por nombre falla): instalar zfront **como** `/usr/bin/emulationstation` y mover ES a otro directorio (los recursos de ES viven en `/storage/.config`, no junto al binario). Entonces `before_video` no hace falta y `pgrep/pidof/killall` casan sin parchear, pero se pierde el respaldo simple (habría que ajustar `package.mk`).

## 4. Línea de órdenes que `start_es.sh` pasa

`zfront --systems /storage/.config/emulationstation/es_systems.cfg --fullscreen --log-path /var/log` (ver `install.md`). Cerrado:

- `--theme` es opcional: sin él se usa el valor `ThemeSet` de `es_settings.cfg` (`boot::resolve_theme`, `bootargs.cpp`). Un `ThemeSet` Zenith (`es-theme-zenith`, `es-theme-zenith-dark`) se busca en `/storage/.config/emulationstation/themes/` y `/usr/share/themes/`; «dark» en el nombre = oscuro; todo lo demás (sin ajuste, o un tema que no es Zenith: zfront solo tiene Zenith) = claro, que es el ajuste de fábrica de la imagen (`ThemeSet=es-theme-zenith` en el `package.mk` de ES); después `/usr/share/zfront/theme-{dark,light}` y el paquete del SO. Con `--theme` explícito se usa tal cual.
- `--size` ya no hace falta: tras abrir la ventana (`sdlr.cpp`, primera vez) se compara la proporción de la salida real con la del tema cargado (720x720 por defecto) y, si difiere, se recarga el tema para la proporción buena antes del primer fotograma.
- `--log-path DIR` se acepta: stderr va a `DIR/zfront.log` (se añade; se reinicia pasado 1 MB). Sin SDL/arnés (`--preview`, `--dump`) se ignora.
- **No hecho (decisión de diseño):** un tema de ES que no sea Zenith se ignora (no hay forma de dibujarlo); ES por su lado sigue usando el suyo.

## 5. A verificar en la consola (nada de esto se ha podido probar en el PC)

1. `busybox pgrep -x zfront; pidof zfront`; con el esquema de `/usr/bin/emulationstation`: `pgrep -x emulationstation; pidof emulationstation; killall emulationstation` (el nombre de 16 letras se trunca a 15 en `comm`; no se ha asumido cómo casa busybox).
2. `swaymsg -t get_tree | jq '.. | select(.focused?) | .app_id'` debe dar `emulationstation`; reglas `for_window` y `seat attach` aplicadas.
3. `ls /tmp/emulationstation.ready` y el log `ES-READY` de `/var/log/boot.log`; el vigilante de 20 s de `autostart` no debe desactivar el modo temprano.
4. `curl -s -m2 -X POST -d hola 'http://127.0.0.1:1234/notify?duration=1500'` (toast), `.../notify/clear`, `curl -s -o /dev/null -w '%{http_code}' localhost:1234/runningGame` (201 sin juego, 200 con juego), y una suspensión real con el botón (aviso "Suspending..." y sin toast residual al volver).
5. Salvapantallas: tras `ScreenSaverTime` -> `cat /sys/class/backlight/*/bl_power` = 4, la primera tecla no lanza nada y deja 0; tras una sesión larga de juego no salta al volver.
6. `kill -HUP $(pidof zfront)` y `killall zfront` (HDMI): sale con 0 y vuelve a los ~2 s; `bl_power` queda en 0.
7. Que el SDL de la imagen no instale otra respuesta a SIGHUP y que `SDL_PushEvent(SDL_QUIT)` desde `quit()` cierre el bucle (en el PC no hay SDL).

## 6. Huecos y decisiones del usuario

- Elegir el esquema de instalación (zfront aparte + selector, recomendado; o como `/usr/bin/emulationstation`).
- Tema y resolución por defecto en la línea de órdenes: cerrado (apartado 4; pendiente de probar en la consola).
- Eventos `config-changed`/`theme-changed`/`settings-changed`/`controls-changed`/`update-gamelists`: la API existe, falta llamarla desde los menús (`game-selected` y `system-selected` ya salen de carrusel y lista).
- `/quit?confirm=menu`, `/systems...`, `/caps`, `/messagebox`, `/storage/event`, `/addgames`, `/removegames`, `/reloadgames`: no implementados (404/501). Comprobado con grep en todo el fork (25-oct): los únicos scripts que llaman a `:1234` son `sleep.sh` (`/notify`, `/notify/clear`) y `rocknix-fake-suspend` (`/runningGame`, `/shutdown`); no se hace nada más.
- Suspender por inactividad (`ScreenSaverBehavior=suspend`) solo existe si aparece `pm-suspend`; la acción de menú "SUSPEND" usará `oscontract::suspend()` (`systemctl suspend`): confirmar que `systemctl suspend` ejecuta el gancho de `sleep.sh` igual que el botón.
- `launch::default_script_dirs()` usa `<directorio del ejecutable>/scripts` (como ES en ROCKNIX: `Paths.cpp` con `ROCKNIX` define `getExePath()`); el doc 02 y el encargo citan `/usr/share/emulationstation/scripts`, que en ROCKNIX no se usa. Con zfront en `/usr/bin/zfront` el directorio sería `/usr/bin/scripts` (vacío); la carpeta de usuario y `/var/run/emulationstation/scripts` sí valen.
- Sin pantalla de aviso tras `/launch` fallido y sin la comprobación de `/storage/serial_number_check_status` (opcional).
- **Primer arranque: zfront no necesita pantallas propias** (comprobado en el fork). La plantilla de `system.cfg` ya trae `system.language=en_US`; `autostart` ejecuta `firstboot/update.sh` ANTES de lanzar la interfaz (marca `/storage/mod.done`), y `update.sh` pregunta el idioma por framebuffer (A English / B 中文 / X Español, 59 s) y SIEMPRE escribe `system.language` (sin mando o sin pantalla, `en_US`; en `device_change` no pregunta y conserva el valor). Por eso, cuando zfront arranca, el idioma nunca está «sin configurar»; una segunda pregunta sería un duplicado. Lo que ES hace en su primer arranque y zfront no: `GuiDetectDevice` (configurar el mando si no hay `es_input.cfg`), que no aplica porque la imagen trae `es_input.cfg` (`packages/ui/emulationstation/config/common/es_input.cfg`) y los mandos sin entrada se mapean con el mapeo de SDL (`pad.*`); la zona horaria no tiene asistente en ES (solo el menú). Lo único mejorable está en el SO: la pregunta solo ofrece 3 idiomas (el menú de interfaz de zfront ofrece los 35 con catálogo) y no cambia la zona horaria salvo con chino.
