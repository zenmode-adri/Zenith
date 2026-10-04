# ES: version nueva + parches de rendimiento rebasados (2026-10-04)

Base: ROCKNIX/emulationstation-next @ cada856d86e3115fbbf0bce09dd761b0ee8fa9fd (2026-09-28). Antes: AveyondFly/emulationstation-next master (flotante).
Parche unico: patches/RK3326/0001-es-rk3326-perf-rebased.patch (los antiguos 0001-0004 reaplicados con patch -F3).

Hunks DESCARTADOS (no aplicaban, el codigo cambio; ROCKNIX refactorizo el menu WiFi el 23-sep):
- 0001/ApiSystem.cpp #4: cache de modos de pantalla wlr-randr (solo sirve con Sway).
- 0001/GuiMenu.cpp #3 y #5, 0003/GuiMenu.cpp #2 y #3 (toggles WiFi asincronos con GuiLoading).
Los antiguos estan en /home/epsil/es-patches. Si el menu WiFi vuelve a bloquear la UI, rebasar esos hunks a mano.

## Correcciones tras el primer intento de compilacion (2026-10-04)
- package.mk: ES nuevo exige una sola API grafica; ahora -DGLES3=1 (como ROCKNIX upstream), antes GL+GLES2.
- Parche: el bloque de funciones auxiliares (getSessionShOutput...) cayo dentro de openSystemSettings(); movido a ambito de fichero.
- Parche: openNetworkSettings define "Window* window" una sola vez al principio (la definicion antigua se elimina); la lambda de enable_adhoc captura window.
- Parche: eliminada una linea invalidateSessionShOutput("timeinfo...") colocada por error entre las opciones de ventilacion.
- Compilado y comprobado con el compilador cruzado del propio build (GuiMenu.cpp, ApiSystem.cpp, SystemConf.cpp).
