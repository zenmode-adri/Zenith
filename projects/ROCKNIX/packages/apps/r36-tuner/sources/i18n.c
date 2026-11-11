/* SPDX-License-Identifier: MIT
 * _("English") looks the text up here. A text without an entry shows in English, so nothing breaks; tests/i18n_check.sh
 * lists the ones missing.
 * Same term for the same thing everywhere: frequency, voltage, governor, restore factory settings. */
#include "tuner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const char *en, *es; } Entry;

static Entry T[] = {
    {"None so far.", "Ninguno hasta ahora."},
    {"This configuration is not stable.", "Esta configuración no es estable."},
    {"min", "mín."},
    {"avg", "media"},
    {"now", "ahora"},
    {"Recent results", "Resultados recientes"},
    {"What it checks", "Qué comprueba"},
    {"Last result", "Último resultado"},
    {"CPU: all cores repeat calculations with a known answer.", "CPU: todos los núcleos repiten cálculos de resultado conocido."},
    {"Memory: writes patterns and reads them back.", "Memoria: escribe patrones y los vuelve a leer."},
    {"GPU: runs a graphics load at the same time.", "GPU: ejecuta una carga gráfica a la vez."},
    {"Passed", "Superada"},
    {"Not validated", "Sin validar"},
    {"Cooling down", "Enfriando"},
    {"If it gets too hot it waits for the chip to cool down, so it can take longer than the time chosen.", "Si se calienta demasiado espera a que se enfríe el chip, así que puede tardar más que el tiempo elegido."},
    {"No errors, but these frequencies were not tested: %s. Your changes remain unvalidated.", "Sin errores, pero estas frecuencias no se han probado: %s. Tus cambios siguen sin validar."},
    {"Failed:", "Fallida:"},
    {"errors in", "errores en"},
    {"Stopped: too hot after", "Detenida por temperatura tras"},
    {"Copy the file ending in .dtb.bak over the file with the same name without .bak.", "Copia el fichero que termina en .dtb.bak encima del fichero del mismo nombre sin .bak."},
    {"The automatic revert only works if the boot reaches the operating system. If the console hangs earlier, restore it from a PC with the SD card (see Recovery).", "La reversión automática solo actúa si el arranque llega al sistema operativo. Si la consola se cuelga antes, restáurala desde un PC con la tarjeta SD (ver Recuperación)."},
    {"Saved. Restart to apply it, then run the stability test. If the console does not boot, R36-TUNER-RECOVERY.txt on the SD card has the steps.", "Guardado. Reinicia para aplicarlo y después ejecuta la prueba de estabilidad. Si la consola no arranca, R36-TUNER-RECOVERY.txt en la tarjeta SD tiene los pasos."},
    {"If the console hangs or restarts during this test, your changes are reverted on the next startup, as long as that boot reaches the operating system.", "Si la consola se cuelga o se reinicia durante la prueba, tus cambios se revierten en el siguiente inicio, siempre que ese arranque llegue al sistema operativo."},
    {"Returns to 1296 MHz if a boot that reaches the operating system does not finish. Otherwise restore boot.ini from a PC.", "Vuelve a 1296 MHz si un arranque que llega al sistema operativo no termina. Si no, restaura boot.ini desde un PC."},
    {"The last change prevented a complete boot and was reverted.", "El último cambio impedía completar el arranque y se ha revertido."},
    {"How to restore from a PC with the SD card if the console does not boot.", "Cómo restaurar desde un PC con la tarjeta SD si la consola no arranca."},
    {"Discards every voltage and frequency change stored on the SD card.", "Descarta todos los cambios de voltaje y frecuencia guardados en la tarjeta SD."},
    {"Insert it in a PC and open the FAT partition. R36-TUNER-RECOVERY.txt there has these same steps.", "Insértala en un PC y abre la partición FAT. R36-TUNER-RECOVERY.txt contiene estos mismos pasos."},
    {"Copy tuner/extlinux.conf.factory over extlinux/extlinux.conf.", "Copia tuner/extlinux.conf.factory encima de extlinux/extlinux.conf."},
    {"Optional, to also turn off Turbo and frequency limits: create an empty file named r36-tuner-reset in that partition.", "Opcional, para desactivar también el Turbo y los límites de frecuencia: crea en esa partición un fichero vacío llamado r36-tuner-reset."},
    {"Insert it in a PC and open the FAT partition (/boot). R36-TUNER-RECOVERY.txt there has these same steps.", "Insértala en un PC y abre la partición FAT (/boot). R36-TUNER-RECOVERY.txt contiene estos mismos pasos."},
    {"If you used the CPU overclock, also copy boot.ini.oc_bak over boot.ini.", "Si usaste el overclock de CPU, copia también boot.ini.oc_bak encima de boot.ini."},
    {"Home", "Inicio"},
    {"Settings", "Ajustes"},
    {"Advanced", "Avanzado"},
    {"Tests", "Pruebas"},
    {"Back", "Atrás"},
    {"Select", "Seleccionar"},
    {"Apply", "Aplicar"},
    {"Cancel", "Cancelar"},
    {"Change", "Cambiar"},
    {"Exit", "Salir"},
    {"Tab", "Pestaña"},
    {"Restart now", "Reiniciar ahora"},
    {"Later", "Más tarde"},
    {"On", "Activado"},
    {"Off", "Desactivado"},
    {"Auto", "Automático"},
    {"Start", "Iniciar"},
    {"Run again", "Repetir"},
    {"Stop", "Detener"},
    {"Clear", "Borrar"},
    {"Add", "Añadir"},
    {"Remove", "Quitar"},
    {"Review", "Revisar"},
    {"Factory", "Fábrica"},
    {"Voltage", "Voltaje"},
    {"Frequency", "Frecuencia"},
    {"Current", "Actual"},
    {"New", "Nuevo"},
    {"Saved", "Guardado"},
    {"Active", "Activo"},
    {"Removed", "Eliminada"},
    {"Not added", "No añadida"},
    {"All frequencies", "Todas las frecuencias"},
    {"Chip", "Chip"},
    {"Discard your changes?", "¿Descartar los cambios?"},
    {"Discard", "Descartar"},
    {"Keep editing", "Seguir editando"},
    {"No changes to apply.", "No hay cambios que aplicar."},
    {"Add %d MHz first.", "Añade antes %d MHz."},
    {"Temperature", "Temperatura"},
    {"Battery", "Batería"},
    {"charging", "cargando"},
    {"Memory", "Memoria"},
    {"max", "máx."},
    {"Profile", "Perfil"},
    {"Power saver", "Ahorro"},
    {"Balanced", "Equilibrado"},
    {"Performance", "Rendimiento"},
    {"Reduces maximum frequencies to extend battery life. Demanding games will run slower.", "Reduce las frecuencias máximas para alargar la batería. Los juegos exigentes irán más lentos."},
    {"Frequencies adapt automatically to the load. Recommended for most games.", "Las frecuencias se adaptan solas a la carga. Recomendado para la mayoría de juegos."},
    {"Maximum frequencies while playing. Best for N64, PSP and Dreamcast. Higher temperature and battery use.", "Frecuencias máximas al jugar. Ideal para N64, PSP y Dreamcast. Más temperatura y consumo."},
    {"Custom settings active.", "Ajustes personalizados activos."},
    {"Restart to apply your changes.", "Reinicia para aplicar los cambios."},
    {"The last change prevented the console from booting and was reverted.", "El último cambio impedía arrancar la consola y se ha revertido."},
    {"Changes not validated. Run the stability test.", "Cambios sin validar. Ejecuta la prueba de estabilidad."},
    {"Critical temperature: the system is limiting frequencies.", "Temperatura crítica: el sistema está limitando las frecuencias."},
    {"High temperature.", "Temperatura alta."},
    {"A system update restored the factory configuration. Apply your changes again.", "Una actualización ha restaurado la configuración de fábrica. Vuelve a aplicar tus cambios."},
    {"CPU governor", "Gobernador de la CPU"},
    {"CPU and memory governor", "Gobernador de CPU y memoria"},
    {"CPU maximum frequency", "Frecuencia máxima de la CPU"},
    {"GPU governor", "Gobernador de la GPU"},
    {"GPU maximum frequency", "Frecuencia máxima de la GPU"},
    {"Memory governor", "Gobernador de la memoria"},
    {"Memory maximum frequency", "Frecuencia máxima de la memoria"},
    {"Turbo", "Turbo"},
    {"Performance mode in games", "Modo rendimiento en juegos"},
    {"Language", "Idioma"},
    {"On demand", "Bajo demanda"},
    {"Power saving", "Ahorro de energía"},
    {"Conservative", "Conservador"},
    {"Manual", "Manual"},
    {"Interactive", "Interactivo"},
    {"How the CPU frequency follows the load. Performance always keeps the maximum frequency.", "Cómo sigue la frecuencia de la CPU a la carga. Rendimiento mantiene siempre la máxima."},
    {"Upper frequency limit. Persists after restart.", "Límite superior de frecuencia. Se conserva al reiniciar."},
    {"How the GPU frequency follows the load. On demand may stay too low for emulators.", "Cómo sigue la frecuencia de la GPU a la carga. Bajo demanda puede quedarse corto con emuladores."},
    {"How the memory frequency follows the load. On demand may stay too low for emulators.", "Cómo sigue la frecuencia de la memoria a la carga. Bajo demanda puede quedarse corto con emuladores."},
    {"Enables 1416 and 1512 MHz at 1350 mV. Raises temperature and may cause instability. Persists after restart.", "Activa 1416 y 1512 MHz a 1350 mV. Sube la temperatura y puede causar inestabilidad. Se conserva al reiniciar."},
    {"Keeps GPU and memory at maximum frequency while a game runs. Largest gain in demanding games.", "Mantiene GPU y memoria a su frecuencia máxima durante el juego. La mayor mejora en juegos exigentes."},
    {"Interface language.", "Idioma de la interfaz."},
    {"Enable Turbo?", "¿Activar Turbo?"},
    {"The CPU will run at up to 1512 MHz at 1350 mV.", "La CPU funcionará hasta a 1512 MHz a 1350 mV."},
    {"1512 MHz on all four cores has caused resets on some units. Turbo stays enabled after restart.", "1512 MHz en los cuatro núcleos ha provocado reinicios en algunas unidades. El Turbo sigue activo al reiniciar."},
    {"Enable", "Activar"},
    {"This kernel may not really reach this speed (measured: no gain).", "Este kernel podría no alcanzar esta frecuencia (medido: sin mejora)."},
    {"Checking whether this frequency is real… (8 s)", "Comprobando si esta frecuencia es real… (8 s)"},
    {"CPU voltage curve", "Curva de voltaje de la CPU"},
    {"GPU voltage curve", "Curva de voltaje de la GPU"},
    {"Memory frequencies and voltages", "Frecuencias y voltajes de la memoria"},
    {"Voltage for each CPU frequency, individually or all at once.", "Voltaje de cada frecuencia de la CPU, por separado o todas a la vez."},
    {"Voltage for each GPU frequency, individually or all at once.", "Voltaje de cada frecuencia de la GPU, por separado o todas a la vez."},
    {"Voltage for each frequency, and optional higher frequencies.", "Voltaje de cada frecuencia y frecuencias superiores opcionales."},
    {"GPU and memory share a power rail (vdd_logic): the higher voltage of the two applies.", "GPU y memoria comparten alimentación (vdd_logic): se aplica el mayor de los dos voltajes."},
    {"Experimental frequency: stability varies between units.", "Frecuencia experimental: la estabilidad varía según la unidad."},
    {"CPU overclock (teacupx)", "Overclock de CPU (teacupx)"},
    {"Requires the teacupx kernel. Up to 1512 MHz.", "Requiere el kernel teacupx. Hasta 1512 MHz."},
    {"Voltage summary", "Resumen de voltajes"},
    {"Saved values compared with those active in the kernel.", "Valores guardados frente a los activos en el kernel."},
    {"Restore factory settings", "Restaurar valores de fábrica"},
    {"Discards every voltage and frequency change.", "Descarta todos los cambios de voltaje y frecuencia."},
    {"Recovery", "Recuperación"},
    {"How to restore from a PC if the console does not boot.", "Cómo restaurar desde un PC si la consola no arranca."},
    {"Already on factory settings.", "Ya estás en los valores de fábrica."},
    {"Restore", "Restaurar"},
    {"This console is using a modified device tree. Choose your console again in Zenith DTB Selector, then try again. No changes were made.", "Esta consola usa un device tree modificado. Elige tu consola otra vez en Zenith DTB Selector y vuelve a intentarlo. No se ha cambiado nada."},
    {"Could not read the device tree. No changes were made.", "No se pudo leer el device tree. No se ha cambiado nada."},
    {"Chip bin not detected, so voltages cannot be edited safely. Restart and try again.", "No se ha detectado el bin del chip, así que no se pueden editar los voltajes con seguridad. Reinicia e inténtalo de nuevo."},
    {"This table was not found in the device tree.", "Esta tabla no está en el device tree."},
    {"The change could not be saved. No changes were made.", "No se pudo guardar. No se ha cambiado nada."},
    {"Saved. Restart to apply it, then run the stability test.", "Guardado. Reinicia para aplicarlo y después ejecuta la prueba de estabilidad."},
    {"If the console fails to boot, this change is reverted automatically on the next startup.", "Si la consola no arranca, el cambio se revierte automáticamente en el siguiente inicio."},
    {"Crashes during gameplay are not reverted automatically. Run the stability test to validate the change.", "Los cuelgues durante el juego no se revierten solos. Ejecuta la prueba de estabilidad para validar el cambio."},
    {"Restore factory settings? All voltage and frequency changes will be lost.", "¿Restaurar los valores de fábrica? Se perderán todos los cambios de voltaje y frecuencia."},
    {"Factory settings restored. Restart to apply.", "Valores de fábrica restaurados. Reinicia para aplicarlos."},
    {"Could not restore factory settings.", "No se pudieron restaurar los valores de fábrica."},
    {"active", "activa"},
    {"Maximum frequency", "Frecuencia máxima"},
    {"Boot at this frequency", "Arrancar a esta frecuencia"},
    {"CPU overclock", "Overclock de CPU"},
    {"Safe boot (1296 MHz)", "Arranque seguro (1296 MHz)"},
    {"Recommended. Raise the limit after booting.", "Recomendado. Sube el límite después de arrancar."},
    {"If booting fails, it returns to 1296 MHz automatically.", "Si falla el arranque, vuelve solo a 1296 MHz."},
    {"boot.ini not found. This requires the teacupx kernel.", "No se encuentra boot.ini. Hace falta el kernel teacupx."},
    {"Boot mode", "Modo de arranque"},
    {"CPU benchmark completed at this frequency.", "Benchmark de CPU completado a esta frecuencia."},
    {"Stability test", "Prueba de estabilidad"},
    {"Combined CPU, memory and GPU load with error detection.", "Carga combinada de CPU, memoria y GPU con detección de errores."},
    {"CPU benchmark", "Benchmark de CPU"},
    {"30 s score to compare configurations.", "Puntuación en 30 s para comparar configuraciones."},
    {"Memory benchmark", "Benchmark de memoria"},
    {"Write and copy throughput at maximum frequency. 30 s.", "Velocidad de escritura y copia a la frecuencia máxima. 30 s."},
    {"GPU benchmark", "Benchmark de GPU"},
    {"glmark2, four scenes. About 1 minute.", "glmark2, cuatro escenas. Aprox. 1 minuto."},
    {"History", "Historial"},
    {"Previous results, newest first.", "Resultados anteriores, los más recientes primero."},
    {"Running…", "En curso…"},
    {"Peak", "Máxima"},
    {"Result", "Resultado"},
    {"Higher than the previous result", "Superior al resultado anterior"},
    {"Lower than the previous result", "Inferior al resultado anterior"},
    {"Same as the previous result", "Igual que el resultado anterior"},
    {"Write", "Escritura"},
    {"Copy", "Copia"},
    {"points", "puntos"},
    {"Benchmark program not found. Reinstall R36 Tuner.", "No se encuentra el programa de benchmark. Reinstala R36 Tuner."},
    {"The GPU benchmark failed.", "El benchmark de GPU ha fallado."},
    {"No results yet.", "Aún no hay resultados."},
    {"Clear all results?", "¿Borrar todos los resultados?"},
    {"Duration", "Duración"},
    {"Quick: 5 minutes", "Rápida: 5 minutos"},
    {"Normal: 20 minutes", "Normal: 20 minutos"},
    {"Extended: 60 minutes", "Extendida: 60 minutos"},
    {"After minor changes.", "Tras cambios menores."},
    {"Recommended.", "Recomendada."},
    {"To validate a final configuration.", "Para validar una configuración definitiva."},
    {"Test passed", "Prueba superada"},
    {"Test failed", "Prueba fallida"},
    {"No errors detected. Your changes are validated.", "No se han detectado errores. Tus cambios quedan validados."},
    {"No errors detected.", "No se han detectado errores."},
    {"Errors detected: this configuration is not stable. Raise the voltage or restore factory settings.", "Se han detectado errores: esta configuración no es estable. Sube el voltaje o restaura los valores de fábrica."},
    {"Test stopped: temperature too high.", "Prueba detenida: temperatura demasiado alta."},
    {"Test stopped: temperature too high. Your changes remain unvalidated.", "Prueba detenida: temperatura demasiado alta. Tus cambios siguen sin validar."},
    {"Your changes apply after restarting. Restart the console, then run this test.", "Tus cambios se aplican al reiniciar. Reinicia la consola y luego haz esta prueba."},
    {"The safety net could not be turned on. The test was not started.", "No se pudo activar la red de seguridad. La prueba no ha empezado."},
    {"No errors detected, but the result could not be saved. Your changes remain unvalidated.", "No se han detectado errores, pero no se pudo guardar el resultado. Tus cambios siguen sin validar."},
    {"Test stopped. Your changes remain unvalidated.", "Prueba detenida. Tus cambios siguen sin validar."},
    {"Test stopped.", "Prueba detenida."},
    {"No errors, but these frequencies were not tested: %s. Your changes stay validated.", "Sin errores, pero estas frecuencias no se han probado: %s. Tus cambios siguen validados."},
    {"If the console hangs or restarts during this test, your changes are reverted on the next startup.", "Si la consola se cuelga o se reinicia durante la prueba, tus cambios se revierten en el siguiente inicio."},
    {"Errors", "Errores"},
    {"Time remaining", "Tiempo restante"},
    {"Power off the console and remove the SD card.", "Apaga la consola y extrae la tarjeta SD."},
    {"Insert it in a PC and open the FAT partition.", "Insértala en un PC y abre la partición FAT."},
    {"Open extlinux/extlinux.conf.", "Abre extlinux/extlinux.conf."},
    {"Set the FDT line to the factory device tree. Its name is in tuner/factory.", "En la línea FDT, indica el device tree de fábrica. Su nombre está en tuner/factory."},
    {"Alternatively, select your console again in Zenith DTB Selector.", "También puedes volver a seleccionar tu consola en Zenith DTB Selector."},
    {"Reinsert the SD card and power on the console.", "Vuelve a insertar la tarjeta SD y enciende la consola."},
    {"Insert it in a PC and open the FAT partition (/boot).", "Insértala en un PC y abre la partición FAT (/boot)."},
    {"Locate the device tree file ending in .dtb.bak.", "Localiza el fichero de device tree que termina en .dtb.bak."},
    {"Copy it over the file with the same name without .bak.", "Cópialo sobre el fichero del mismo nombre sin .bak."},
};

static const int N = sizeof(T) / sizeof(T[0]);
static char lang[8] = "en";

static int cmp(const void *a, const void *b) { return strcmp(((const Entry *)a)->en, ((const Entry *)b)->en); }

static void lang_path(char *p, int n) { snprintf(p, n, "%s/lang", plat->data_dir); }

void i18n_init(void) {
    qsort(T, (size_t)N, sizeof(Entry), cmp);
    const char *sys = plat->language ? plat->language() : NULL;
    if (sys) { i18n_set(sys); return; }
    char p[300], b[16];
    lang_path(p, sizeof(p));
    if (sys_read(p, b, sizeof(b)) == 0) {
        /* the old tuner saved 0 / 1 */
        if (!strcmp(b, "1")) snprintf(lang, sizeof(lang), "es");
        else if (!strcmp(b, "0")) snprintf(lang, sizeof(lang), "en");
        else snprintf(lang, sizeof(lang), "%.7s", b);
    }
}

void i18n_set(const char *l) {
    snprintf(lang, sizeof(lang), "%s", strcmp(l, "es") ? "en" : "es");
    if (plat->has_lang_menu) {
        char p[300], v[16];
        lang_path(p, sizeof(p));
        snprintf(v, sizeof(v), "%s\n", lang);
        sys_write(p, v);
    }
}

const char *i18n_get(void) { return lang; }

const char *_(const char *en) {
    if (strcmp(lang, "es")) return en;
    Entry key = {en, NULL};
    const Entry *e = bsearch(&key, T, (size_t)N, sizeof(Entry), cmp);
    return e ? e->es : en;
}
