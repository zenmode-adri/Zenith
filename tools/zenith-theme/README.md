# Zenith theme tooling

Scripts used to generate and test the Zenith ES theme, the volume popup patch and the first-boot screens
(paths inside the scripts point at the original Windows workspace; they are references, not a build step).

- `gen_layout.py`: per-aspect-ratio geometry (`layout.xml`) and exact-size selector pills for 1:1, 4:3, 3:2, 16:9.
- `mkassets.py`, `mkassets2.py`, `mkhelp.py`, `mkstatus.py`: PNG assets (tiles, frame, help icons, utility logos).
- `genframes2.py`: first-boot / boot frames with a layout grid (`--debug` draws margins and gaps).
- `overlay_theme.py`: measurement overlay on real screenshots of the console.
- `stage_pkg.py`: stages `es-theme-zenith` (light + dark overlay) for the package.
- `mkpatch0009.py` + `VolumeInfoComponent.*`: source of `0009-es-volume-osd.patch`.
- `esres.sh`, `runres.sh`, `shot.sh`, `cons.sh`, `kbdstart.sh`, `kbd2.c`, `upl.sh`, `redeploy.sh`: run ES windowed at WxH on the
  console, send virtual key presses and grab screenshots over SSH.
