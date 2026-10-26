# menu.main, menu.quit, menu.ui

Files: `menu_main.*`, `menu_quit.cpp`, `menu_ui.*`, `menu_lang.hpp`, `ui_prefs.*`; tests in `tests/test_menu_main.cpp`.

## menu.main
Ten rows in ES order, each opens the screen another module registers and is shown only while `Registry::has(<screen>)`:

| Row (ES key) | Screen | Icon |
|---|---|---|
| GAME SETTINGS | menu.game | games |
| CONTROLLER & BLUETOOTH | menu.controllers | controller |
| USER INTERFACE SETTINGS | menu.ui | ui |
| GAME COLLECTION SETTINGS | menu.collections | advanced |
| SOUND SETTINGS | menu.sound | sound |
| NETWORK SETTINGS | menu.network | network |
| SCRAPER | menu.scraper | scraper |
| UPDATES & DOWNLOADS | menu.updates | update |
| SYSTEM SETTINGS | menu.system | system |
| QUIT | menu.quit | quit |

`WIDGET DEMO` (`menu.demo`) is added only when `cx.sv->cmd.dry_run` (harness, `--preview`, tests), never on the device.
Start on the carousel opens `menu.main`; the old development quit on B in the carousel was removed.

## menu.quit
RESTART EMULATIONSTATION (`Mode::Quit`), RESTART SYSTEM (`Reboot`), SUSPEND SYSTEM (only if `oscontract::suspend_supported()`),
SHUTDOWN SYSTEM (`Shutdown`). Each one asks YES/NO first (`kConfirmQuit`, one constant to follow ES patch 0053 and drop it).
FAST SHUTDOWN is not offered (`kShowFastShutdown`).

## menu.ui
See the table at the top of `menu_ui.cpp`. Visible: LANGUAGE (REGION), SWITCH CONFIRM & CANCEL BUTTONS, SCREEN BRIGHTNESS
(only with a backlight), SHOW CLOCK, SHOW BATTERY STATUS (only with a battery), START SCREENSAVER AFTER, SCREENSAVER TYPE.

- Language: `system.language` via `conf.persist`, then `i18n::set_language`; `LangMenu` rebuilds open menus on the next frame/key.
  Known limit: the title of an already-open menu stays in the old language until reopened (needs a `Menu::set_title`).
- Clock/battery: `ui_prefs` filters the status (`DrawClock`, `ClockMode12`, `ShowBattery`) in `App`; the percentage text only if mode is text.
- InvertButtons: live through `Pad::set_invert_buttons`.
- Brightness: sysfs written live while moving, `brightness set N` (the OS script, stores `system.brightness`) after the pause.

Hidden (zfront cannot do them): theme set/configuration, gamelist view style, game options at north, touchscreen, transitions,
media during fast scroll, on-screen help, music/video/slideshow screensavers, gamelist options.
