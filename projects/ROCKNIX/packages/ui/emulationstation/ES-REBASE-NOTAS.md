# ES: version nueva + parches de rendimiento rebasados (2026-10-04)

Base: ROCKNIX/emulationstation-next @ cada856d86e3115fbbf0bce09dd761b0ee8fa9fd (2026-09-28). Antes: AveyondFly/emulationstation-next master (flotante).
Parche unico: patches/RK3326/0001-es-rk3326-perf-rebased.patch (los antiguos 0001-0004 reaplicados con patch -F3).

Hunks DESCARTADOS (no aplicaban, el codigo cambio; ROCKNIX refactorizo el menu WiFi el 23-sep):
- 0001/ApiSystem.cpp #4: cache de modos de pantalla wlr-randr (solo sirve con Sway).
- 0001/GuiMenu.cpp #3 y #5, 0003/GuiMenu.cpp #2 y #3 (toggles WiFi asincronos con GuiLoading).
Los antiguos estan en /home/epsil/es-patches. Si el menu WiFi vuelve a bloquear la UI, rebasar esos hunks a mano.
