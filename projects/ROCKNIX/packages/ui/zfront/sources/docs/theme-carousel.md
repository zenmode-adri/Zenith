# Variables opcionales del tema: estructura del carrusel de inicio

Un tema (`theme.xml`, bloque `<variables>`) puede cambiar la ESTRUCTURA del carrusel de sistemas con estas variables. Todas son
opcionales y se leen con `Theme::f(nombre, defecto)` / `Theme::str(nombre, defecto)`: el defecto de cada una es EXACTAMENTE el valor
que Zenith siempre ha tenido, de modo que un tema que no las dice (Zenith claro y oscuro) sale pixel-identico
(`cpp/tools/zenith-baseline.sh check`). Un valor fuera de rango se recorta al limite, nunca rompe el dibujo.

Se usan en `cpp/src/scene.cpp` (`car_of`, `system_view`, `list_view`); el test es `cpp/tests/test_theme_carousel.cpp`.

| Variable | Defecto (Zenith) | Rango | Que hace |
|---|---|---|---|
| `carScale` | `1.23` | 1 .. 1.5 | Tamano de la tarjeta seleccionada respecto a las demas (1 = todas iguales). |
| `carSpacing` | `1.21` | 0.5 .. 2 | Separacion extra entre la tarjeta seleccionada y sus vecinas (multiplica el hueco que deja crecer a la seleccionada). |
| `carCount` | `5` | 1 .. 9 (entero) | Tarjetas visibles a la vez. Reparte el ancho `carW` entre ellas: menos tarjetas = mas separadas. El paso entre tarjetas nunca baja del ancho de una (`cellW`). |
| `carTileScale` | `1.5` | 1 .. 1.8 | Tamano de la imagen de la tarjeta (`assets/tile.png`, `frame.png`) respecto al contenedor. En Zenith la tarjeta visible ocupa 2/3 de esa imagen (el resto es sombra/brillo), asi que la tarjeta visible mide `carTileScale x 2/3` del contenedor. La caja del logo (0.90 x 0.5 del contenedor) se encoge en proporcion `carTileScale / 1.5` para que el logo siga dentro de la tarjeta; la etiqueta (y = 0.671 del contenedor) no se mueve. |
| `typeScale` | `1` | 0.6 .. 1.4 | Multiplicador del texto del carrusel: nombre del sistema cuando no hay logo (`fLogo`), etiquetas bajo el icono (`fLabel`) y nombre + numero de juegos bajo el carrusel (`fSys`); la linea fina y la fila "seguir jugando" se desplazan con ella. |
| `sysBar` | `1` | 0 / 1 | Barra de color del sistema antes de su nombre bajo el carrusel. Con `0` no se dibuja y el nombre empieza en el margen. |
| `listCase` | `""` | `""`, `upper`, `lower` | Mayusculas/minusculas de los textos de la lista de juegos: `""` deja lo que cada texto lleva hoy (filas en MAYUSCULAS, titulo y nombre de la columna derecha como vienen); `upper` y `lower` lo fuerzan en filas, titulo, nombre de la columna derecha y nombre de la vista detallada. Otro valor se ignora. No afecta a los menus (de eso se ocupa `ui_style`). |

No hecho: `iconStroke` (grosor de los iconos de menu). Los iconos son SVG con el trazo fijo en el propio fichero
(`stroke-width="1.7"`) y se rasterizan por `Assets::svg(ruta, alto)`; cambiarlo exige reescribir el SVG antes de rasterizar y
ampliar la clave de cache de `Assets`, y toca `menu.cpp`/`ui_osd.cpp`. Un tema que quiera otro trazo puede llevar sus propios SVG.

## Ejemplo

```xml
<variables>
  <carScale>1.0</carScale>        <!-- tarjetas todas iguales -->
  <carCount>3</carCount>          <!-- tres a la vez, mas grandes y separadas -->
  <carTileScale>1.35</carTileScale>
  <typeScale>1.15</typeScale>
  <sysBar>0</sysBar>
  <listCase>lower</listCase>
</variables>
```

Estas variables las lee zfront, no EmulationStation: el `<carousel>` de `theme.xml` que usa ES sigue con sus valores literales
(`logoScale`, `scaledLogoSpacing`...). Si un tema se usa tambien con ES, hay que escribir los mismos numeros en ambos sitios.
