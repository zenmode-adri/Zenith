# Menu framework: visual specification

Source of truth for the look of `menu`, `ui_dialogs`, `ui_osd` and `toast`. Code: `src/ui_style.*` (colours, metrics,
shapes), `src/menu.*`, `src/ui_dialogs.*`, `src/ui_osd.*`, `src/toast.cpp`. Demo: screen `menu.demo`.

Rule: **nothing is a fixed pixel count.** Every length is a fraction of the screen width `W` or height `H`, or a multiple
of `em` (the menu text size). Text sizes are `floor` of a fraction of `H`, never below 1 px. Rectangles are snapped to
whole pixels; rounded corners and icons are tiny anti-aliased images cached in `Assets`. The only absolute values are
hairlines and borders (1 px, 2 px for the focused key outline), minimum sizes (scrollbar 3 px, slider track 3 px, caret 2 px)
and 1 px minimum shadow offsets.

## 1. Variables read from the theme (`layout.xml`, per aspect ratio)

| Variable | Meaning | 4:3 value |
|---|---|---|
| `mx` | side margin (fraction of W) | 0.02813 |
| `avY` | vertical centre of the title / status bar (of H) | 0.06667 |
| `fMenuTitle` | title size, times `kTitleScale` 1.22 | 0.03834 |
| `fMenu` | `em`: rows, dialogs, buttons | 0.0375 |
| `fMenuSmall` | hint line, headers, popup title | 0.03021 |
| `fMenuFoot` | help bar and toasts' reference | 0.02708 |
| `listPosY` / `listH` / `listLines` | list viewport top / height (of H) / rows | 0.1375 / 0.7125 / 9 |
| `selH` | selector pill height (of H) | 0.0625 |
| `helpY` | vertical centre of the help bar (of H) | 0.94792 |
| `volW volH volY fVol` | OSD pill width, height, centre Y, text size | 0.34687 0.07917 0.06667 0.02917 |

Constants (`ui_style.hpp`): `kTitleScale = 1.22`, `kColMaxH = 1.8`, `kPillInset = 0.55`.

Derived (`Metrics::from`):

```
margin   = round(mx * W)
em       = floor(fMenu * H)            title_px = floor(fMenuTitle * H * 1.22)
small_px = floor(fMenuSmall * H)       foot_px  = floor(fMenuFoot * H)
col_w    = round(min(W - 2*margin, 1.8*H));  col_x = round((W - col_w)/2)
list_top = round(listPosY * H)         row_h    = floor(listH * H / listLines)   (integer)
list_h   = row_h * listLines
pill_h   = round(selH * H), capped at row_h, +1 if (row_h - pill_h) is odd  (the pill is centred in the row)
inset    = round(pill_h * 0.55)        (text inset inside the pill; same for every row)
```

Every target screen is narrower than 1.8 H, so the column is the full width minus margins.

Resulting pixels (for review, derived from the real layouts):

| Screen | em | title | small | foot | rows | row_h | pill_h | list_top | help_y |
|---|---|---|---|---|---|---|---|---|---|
| 480x320 (3:2) | 15 | 19 | 12 | 11 | 7 | 32 | 26 | 49 | 300 |
| 640x480 (4:3) | 18 | 22 | 14 | 12 | 9 | 38 | 30 | 66 | 455 |
| 720x405 (16:9) | 15 | 18 | 12 | 10 | 9 | 32 | 26 | 56 | 384 |
| 720x720 (1:1) | 21 | 27 | 17 | 15 | 12 | 43 | 37 | 90 | 684 |

## 2. Colours

Taken from `theme.xml` view `menu` (`${var}` resolved) when present, otherwise derived from the base variables
`bg txt dim dimDark cyan cyanDark cyanSoft`. `mix(a,b,t)` = RGB lerp. Dark/light is `luma(bg) < 0.5`.

| Role | Source / derivation |
|---|---|
| background | `bg` (drawn by the screen's `Scene`) |
| panel (dialog cards, input field) | `menuBackground`, else `mix(bg, white, dark 0.04 / light 0.60)` |
| line (hairlines, borders) | `menuText.separatorColor`, else `mix(panel, txt, 0.09)` |
| text | `txt` |
| dim text (unselected rows, hint, values) | `dim` (light) / `dimDark` (dark) |
| selector pill | `cyanSoft` (both themes); danger entries use `danger_soft` |
| selected text / value | `cyanDark` (both themes; the theme defines it per light/dark) |
| switch on / off | `menuSwitch.colorOn/colorOff`, else `mix(bg, cyan, .55)` / `mix(bg, txt, .35)` |
| slider track / OSD track | `menuVolume.trackColor`, else `mix(bg, txt, .30)` |
| slider fill / OSD bar | `cyan` |
| knob | `txt` (dark) / white (light) |
| group header text / band | `menuGroup.color`, else `mix(bg, txt, .60)` / `txt` alpha .05 |
| OSD pill | `menuVolume.color`, else `mix(bg, white, dark .08 / light 1.0)` |
| key / key alt / button | `mix(bg, txt, .10)` / `mix(bg, txt, .18)` / `mix(panel, txt, .12)` |
| danger text / danger soft | dark `#EE6B74` / light `#C23440`; soft = `mix(bg, danger, dark .28 / light .15)` |
| scrim behind dialogs | black, alpha 165 (dark) / 80 (light) |

## 3. Menu screen (title, list, help bar)

* **Title**: `title_px`, vertical centre `avY*H`, left edge `col_x + inset`; ellipsis at 72% of `col_w`. Right side: the
  shared status bar (wifi, battery, clock) at the position `avY`.
* **Hairline** under the title: 1 px, `col_w` wide, halfway between the title bottom and the first pill top.
* **List viewport**: `list_top .. list_top + row_h*listLines`, clipped. Rows are `row_h`, headers `0.9 row_h`, separators
  `0.4 row_h`. Scrolling is animated (exponential, tau 0.055 s) and centres the cursor; it snaps to entry boundaries.
* **Selector pill**: `col_w` wide minus nothing (full column), `pill_h` tall, radius `pill_h/2`, centred in the row; it
  glides with the same easing. Its colour is the "selector" role above.
* **Row**: icon column (optional) `isz = round(1.3 em)`, text starts `0.7 em` after it; otherwise at `col_x + inset`. Label
  `em`, ellipsis against the right controls. Right controls end at `col_x + col_w - inset`. Gap label/controls `1 em`.
* **Header (group)**: `small_px`, `group_txt`, baseline at 66% of its row, a 1 px line above it.
* **Hint line**: `small_px`, `dim`, at `list_bottom + 0.30 * (help_y - list_bottom)`, left edge `col_x + inset`, one line.
* **Scrollbar**: only if the content is taller than the viewport; width `max(round(0.007 H), 3)`, thumb height
  `max(round(list_h^2 / content_h), 2 em)`, alpha .30 of `txt`, placed `max(round(0.35 inset), 2)` outside the column.
* **Help bar**: same geometry as the carousel (`HelpBar`, centred at `helpY*H`, `fHelp`/`foot_px`), prompts per entry kind:
  action `Choose Select Back`, toggle `Choose Change Back`, slider `Choose Adjust Back`, choice `Choose Change Select Back`
  (shown in sentence case, see 6b).

## 4. Entry widgets (right-hand side of a row)

| Entry | Drawing |
|---|---|
| action | label; submenu: chevron `1.0 em` on the right |
| toggle | switch `sw_h = round(1.25 em)`, `sw_w = round(1.85 sw_h)`, pill track, knob diameter `sw_h - 2*pad` with `pad = max(round(0.14 sw_h), 2)`, on = knob right |
| slider | track `0.30 * inner_w` wide (inner = `col_w - 2 inset`), thickness `max(round(0.28 em), 3)`, fill in cyan, knob `round(0.95 em)`, ring around the knob while selected; value text (with unit) left of the track |
| choice | value text (dim) + chevrons `< >` when it cycles (`no_cycle` shows only `>` and opens the popup) |
| text | value (dim), `*` masked when `secret`; `>` chevron |
| info | label left, value right, never focusable-looking: dim both |
| header / separator | see section 3 |
| disabled (`.disabled()`) | label and controls at alpha .42 (.60 under the cursor); skipped by the cursor |
| submenu whose screen is not registered | not in the list at all (never drawn, never focused) |
| danger | label in the danger colour, selected pill uses `danger_soft` |

## 5. Dialogs (`ui_dialogs`)

All: scrim over the whole screen, card centred, radius `round(0.6 pill_h)`, border 1 px `line`, fill `panel`, no shadow.

* **Confirm / message**: card width `min(max(25 em, 0.5 W), min(0.92 W, col_w))`; pad `round(1.3 em)`; text `em`, wrapped
  (max 8 lines, line spacing 1.25), centred; optional title `1.15 em` in `cyanDark` above it; buttons `pill_h` high,
  `bw = max(round(7 em), text + 2 em)`, gap between text and buttons `round(1.2 em)`. The selected button is cyan with
  `on_color` text, the other `btn`. YES / NO order, cursor starts on NO; B cancels.
* **Choice popup**: width `min(max(widest + 2 pad + 2 inset + 1.6 em, 14 em), min(0.92 W, col_w))`, height
  `min(0.8 H, rows*row_h + title + 2 pad)`, pad `1 em`; rows are `row_h` with the same pill (`cyanSoft`); the current
  value has a check mark on the right; scrollbar `max(round(0.006 H), 3)` inside the card; it opens on the current value.
* **Keyboard**: field `round(1.3 pill_h)` high, radius `0.3 field_h`, `panel` fill, caret `1.25 em` x `max(H/300, 2)`
  px cyan; keys 10 per row, `gap = max(round(0.011 H), 2)`, `key_w = floor((gw - 9 gap)/10)`, `kh = min(free/5,
  0.95 key_w)`, radius `0.28 kh`; bottom row SHIFT / 123 / SPACE / DELETE / OK with widths 1.6/1.6/3.0/1.6/2.2 tenths of
  the row (minus the 4 gaps). The whole block is centred `30%` of the free space from the top. Layers: lower, upper, symbols.
  Pad: A types, B deletes one letter (cancels and closes when the field is empty), X space, Y shift, L reveals a password,
  Start accepts (OK), Select cancels. The caret blinks on the wall clock, 0.53 s per phase, solid while keys are pressed;
  it only exists while the keyboard is the top screen: it sets `wake_in` (the loop sleeps until the next phase) and
  `take_redraw`, never `tick == true`, so nothing else wakes the loop.

## 6. Toast and OSD

* **Toast**: pill, height `round(2.3 em)`, radius `h/2`, padding `round(1.1 em)`, cyan dot `round(0.55 em)`, text `em`
  ellipsised to `0.9 W`/`col_w`. Centred at the TOP, like the OSD. They stack downwards, oldest first, gap `round(0.5 em)`,
  starting at `max(osd_bottom, header_line) + gap`: the slot below the OSD pill (reserved even while the pill is hidden, so a
  toast never jumps) and below the header hairline of the menu under it. They stop before the help bar (`help_y - foot_px`).
  Fill `panel`, border `line`, shadow black alpha 90 (dark) / 45 (light). No fades (overlays get no ticks).
* **OSD level** (`osd_show_level(cx, "volume"|"brightness", 0..1, ms=1600)`): pill `volW*W` x `volH*H` centred on
  `x = W/2`, `y = volY*H`, radius `h/2`, border 1 px `line`. When it would touch the title of the menu under it (same columns
  +- `0.02 W` and same rows +- `0.015 H`) or lie across the header hairline (3:2, 480x320: `volY` 0.175 puts it over the
  title's bottom and the line), it moves to just below the hairline (`line_y + max(0.015 H, 3)`); menus and the keyboard publish
  their header box every frame (`ui::header_box()`), `draw_toasts` reads and clears it. The carousel publishes none: the pill
  stays where the theme says. Padding `round(0.34 h)`, icon `round(0.52 h)` (theme's
  `menu-icon-sound.svg` or a procedural sun), bar thickness `max(round(0.14 h), 3)` ending before the percentage text
  (`fVol`, width reserved for "100%"). Hides itself through a marker toast of the same duration.

## 6b. Text case

Labels are sentence case ("Game settings", "Wi-Fi password", "Switches and sliders"): no style forces capitals, headers
included. The `tr()` KEYS stay the upper-case English text of ES. The conversion lives in one place, `i18n::display_text`
(`i18n_case.cpp`), which `tr()` / `ctr()` / `ntr()` use after the catalog lookup: a translation found in a catalog is shown as
written there; an untranslated key that is entirely upper case becomes sentence case (first letter capital, rest lower case,
accents handled for Latin-1/Latin Extended-A); anything with a lower-case letter is left alone. The result is interned once per
key (stable pointer, no allocation on later calls). Words that keep their spelling: WI-FI->Wi-Fi, IP HDMI SD USB CPU GPU RGB LED
BIOS DNS SSH OTA PSP GBA NES SNES PS1 PS2 N64 NDS 3DS RAM FPS CRT LCD ES OS ID VM OK MAME FBNeo RetroArch Bluetooth, units after
a number (5GHZ->5GHz) and a few more (list in `i18n_case.cpp`); printf conversions (`%d`) are skipped. User data (SSIDs, game
names, hostnames) never goes through it because it does not go through `tr()`. Note: ES's own Spanish (and other) catalogs are in
capitals, so in those languages labels keep the catalog's case until zfront catalogs with sentence-case translations exist.

## 7. Behaviour

* Up/Down move with repeat, skipping separators, headers and disabled entries; wrap at the ends. L/R page by
  `listLines - 1`. Left/Right change toggle / slider / cycling choice (slider step with repeat). A activates, B goes
  back (flushes a pending slider commit). Start closes the WHOLE stack of menus (like ES): `Transition::close_menus()` pops
  every screen with `is_menu()` (menus, dialogs, keyboard) down to the first one that is not (carousel, game list); each popped
  menu gets `on_dismiss` (pending commit + `on_close`). Inside the on-screen keyboard Start is OK.
* Slider: `commit` is debounced 0.5 s, and flushed on B, on activation or when the cursor moves; the live `on_change`
  fires on every step.
* Async menus: `MenuBuilder::refresh` is safe to call from a job; the menu rebuilds at the next frame and keeps the
  cursor by entry id (or label). Calling it after the menu was destroyed is a no-op.
* Idle cost: nothing wakes the loop at rest; `tick` returns true only while the scroll/selector are moving or a commit is
  pending (`wake_in`).

## 8. Registered screens and API

`menu.demo` (every widget) and `menu.main` (provisional). `menu.main` opens, when registered by other modules:
`menu.game menu.controllers menu.ui menu.collections menu.sound menu.network menu.scraper menu.updates menu.system
menu.quit`. A missing one is not listed (`submenu` hides entries whose screen is not registered; there is no dimmed state any more).

```cpp
MenuSpec s; s.title = tr("SOUND SETTINGS");
s.build = [](MenuBuilder& m, Ctx& cx) {
  m.header(tr("OUTPUT"));
  m.slider(tr("VOLUME"), 0, 100, 1, get, set).unit("%").commit(save);
  m.toggle(tr("MUTE"), get_b, set_b);
  m.choice(tr("OUTPUT DEVICE"), {{"spk", tr("SPEAKER")}, {"hp", tr("HEADPHONES")}}, get_s, set_s);
  m.text(tr("HOSTNAME"), get_s, set_s).secret();     // opens the on-screen keyboard
  m.submenu(tr("ADVANCED"), "menu.sound.adv").icon("advanced");
  m.action(tr("RESET"), [] {}).danger().confirm(tr("Reset all sound settings?"));
  m.info(tr("IP ADDRESS"), "192.168.1.42");
};
ZF_REGISTER_SCREEN("menu.sound", [](Ctx&, const std::string&) { return make_menu(std::move(s)); });
```

Dialogs: `ui::confirm(text, on_yes)`, `ui::message(text, on_ok, title)`, `ui::choose(...)`, `ui::keyboard(...)` return a
`Transition`; return it from an action. OSD: `osd_show_level(cx, "volume", 0.62f)`.

## 9. Design decisions of the user (25-oct)

1. Labels: sentence case (see 6b).
2. Toasts: at the top, centred, below the OSD slot and the menu header.
3. Unavailable submenus: hidden, not dimmed.
4. Keyboard: B deletes a letter and, with the text empty, cancels and closes.
5. Start in a menu closes the whole menu stack.
