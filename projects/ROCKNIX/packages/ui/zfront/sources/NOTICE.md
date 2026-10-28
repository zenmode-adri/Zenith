# Notice

zfront is licensed under the GNU General Public License, version 2 (see `LICENSE`).

Copyright (C) 2026 zenmode-adri.

zfront is an independent frontend written from scratch (C++17, about 28,000 lines of its own code). It is designed to be a
drop-in replacement for EmulationStation on Zenith, so it reads the same files and offers the same functions. This file
lists what it owes to other projects.

## EmulationStation

Zenith ships EmulationStation (ROCKNIX `emulationstation-next`, itself derived from Batocera's EmulationStation, which is
derived from Alec "Aloshi" Lofquist's EmulationStation). Its license is the MIT license:

    Copyright (c) 2014 Alec Lofquist

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
    documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
    persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
    Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
    COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
    OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

zfront does not contain EmulationStation's source files. A comparison of the two code bases finds 0.4 % of zfront's
longer lines (and 0.14 % of its 5-line windows) identical to ES, almost all in the data table of `scraper_ss.cpp`. What was
taken or reproduced from ES is:

* **Small pieces ported from ES source**
  * `src/menu_game_features.cpp`: the logic of `CustomFeatures::loadEsFeaturesFile` and `SystemData::loadFeatures`,
    `isFeatureSupported`, `getCustomFeatures` (so both frontends offer the same options for `es_features.cfg`).
  * `src/launch.hpp`: the shell escaping of `Utils::FileSystem::getEscapedPath`.
  * `src/menu_network_countries.hpp`: the ISO 3166-1 country table of `GuiMenu.cpp` (`getCountryCodes`).
  * `src/scraper_ss.cpp`: the platform name to ScreenScraper system id table, from `PlatformId.cpp` / `ScreenScraper.cpp`.
* **Behaviour and formats re-implemented** (so existing configurations keep working): the files `es_systems.cfg`,
  `gamelist.xml`, `es_settings.cfg`, `es_features.cfg`, `system.cfg` and the theme format; the settings keys; the HTTP
  interface on port 1234 that the system scripts call; the hook events and script folders; the launch command
  placeholders; the savestate naming; the menu options and their English labels (kept identical on purpose, so that ES's
  own translation catalogs, `emulationstation2.mo`, which zfront reads at run time from the image and does not ship, apply).

## Third-party code included in `cpp/third_party`

| Component | License | Copyright |
|---|---|---|
| pugixml | MIT (`cpp/third_party/pugixml/LICENSE.txt`) | (c) 2006-2025 Arseny Kapoulkine |
| nanosvg | zlib (`cpp/third_party/nanosvg/nanosvg_license.txt`) | (c) 2013-14 Mikko Mononen |
| stb_image | MIT or public domain, your choice (text at the end of `stb_image.h`) | (c) 2017 Sean Barrett |
| libwebp (decoder) | BSD 3-clause (`cpp/third_party/libwebp/COPYING`, `AUTHORS`, `PATENTS`) | (c) 2010 Google Inc. |

## Libraries linked at run time

SDL2 (zlib license), FreeType (FreeType License / GPL-2), and the C and C++ runtimes of the image. They come with the image;
zfront does not bundle them.

## Zenith theme

The Zenith theme (`es-theme-zenith` and its dark overlay, installed under `/usr/share/themes/`) is an original work by
zenmode-adri released under GPL-2.0; its own notice is `NOTICE.md` inside the theme folder. The third-party parts it bundles are:

| Part | Origin | License |
|---|---|---|
| Console logos (`assets/logos/*.webp`, light and dark variants), except the artwork the theme's own notice lists as made for Zenith | *Iconic* EmulationStation theme by Delgan (https://github.com/Delgan/iconic-es), based on *iconic-es-de* by Siddy212; some were resized or recolored for Zenith | CC0 1.0 Universal |
| Star, battery and network icons (`assets/icons/*.svg`) | The same Iconic theme, unmodified | CC0 1.0 Universal |
| Font *Rubik* (`assets/fonts/Rubik-Regular.ttf`) | Copyright 2015 The Rubik Project Authors (https://github.com/googlefonts/rubik) | SIL Open Font License 1.1 (text in `cpp/docs/licenses/OFL-Rubik.txt`, installed to `/usr/share/zfront/licenses/`) |

The rest of the theme (menu icons, switches, selector pills, help-bar icons, splash, avatar and the four navigation sounds, which
are synthesized by a script) is stated by the theme's notice to have been made for Zenith and is covered by the GPL-2.0.

The names and logos of consoles and manufacturers shown by the theme are trademarks of their respective owners; their use here
only identifies the systems and implies no endorsement. CC0 applies to the artwork of the Iconic theme, not to those marks.

### Fonts loaded from the EmulationStation package at run time

zfront does not bundle them; it loads, if present, the fallback fonts that ship with EmulationStation: Font Awesome (Dave Gandy),
Droid Sans Fallback (Google, Apache License 2.0), Pyeojin Gothic (Suh Ji-hwan, OFL 1.1) and Vazirmatn (The Vazirmatn Project
Authors, OFL 1.1). Their licenses are those of the EmulationStation package. No color emoji font is among them.
