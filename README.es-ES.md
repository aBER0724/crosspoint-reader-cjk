

# CrossPoint Reader CJK

**[Español](./README.md)** | [中文](./README-ZH.md) | [日本語](./README-JA.md)

Para agentes de codificación IA que trabajen en este repositorio, consulte [AGENTS.md](./AGENTS.md).

> Una versión adaptada para CJK del firmware del lector de tinta electrónica **Xteink X4** basada en [daveallie/crosspoint-reader](https://github.com/daveallie/crosspoint-reader).

Este proyecto adapta el CrossPoint Reader original para admitir CJK, con una interfaz multilingüe y renderizado de fuentes CJK.

![](./docs/images/cover.jpg)

## ✨ Nuevas funciones para CJK

### 🌏 Soporte de interfaz multilingüe (I18n)

- **Localización completa**: Admite idiomas de interfaz en chino, inglés y japonés.
- Cambie el idioma de la interfaz en cualquier momento en la Configuración.
- Todos los menús, avisos y configuraciones están completamente localizados.
- Sistema de traducción dinámica basado en IDs de cadena.

### 📝 Sistema de fuentes CJK

- **Soporte de fuentes externas**:
  - **Fuente de lectura**: Se utiliza para el contenido de los libros (tamaño y familia de fuente seleccionables).
  - **Fuente de UI**: Se utiliza para menús, títulos e interfaz.
  - Opción de compartir fuente: Utilice la fuente de lectura como fuente de UI para ahorrar memoria.
- Optimización de caché LRU para mejorar el rendimiento del renderizado CJK.
- Mecanismo integrado de respaldo para caracteres ASCII para reducir el uso de memoria.
- Fuentes de ejemplo:
  - [Source Han Sans CN](fonts/SourceHanSansCN-Bold_20_20x20.bin) (Fuente de UI)
  - [King Hwa Old Song](fonts/KingHwaOldSong_38_33x39.bin) (Fuente de lectura)

### 🎨 Temas y pantalla

- **Tema Lyra**: Un tema de interfaz moderno con resaltados de selección redondeados, paginación con barra de desplazamiento y métricas de diseño refinadas, completamente adaptado para páginas CJK.
- **Cambio de modo oscuro/claro**: Se aplica tanto al lector como a la interfaz.
- Cambio de tema fluido sin actualización completa.

### 📖 Funciones del proyecto original

- [x] Análisis y renderizado de EPUB (EPUB 2 y EPUB 3)
- [x] Soporte de imágenes dentro de EPUB
- [x] Posición de lectura guardada
- [x] Explorador de archivos con selector de archivos
  - [x] Selector básico de EPUB desde el directorio raíz
  - [x] Soporte para carpetas anidadas
  - [ ] Selector de EPUB con portada
- [x] Pantalla de suspensión personalizable
  - [x] Pantalla de suspensión con portada
- [x] Carga de libros por WiFi
- [x] Actualizaciones OTA por WiFi
- [x] Integración de KOReader Sync para progreso de lectura entre dispositivos
- [x] Opciones de fuente, diseño y visualización configurables
  - [ ] Fuentes proporcionadas por el usuario
  - [ ] Soporte UTF completo
- [x] Rotación de pantalla

Soporte multilingüe: Lea EPUB en varios idiomas, incluidos inglés, español, francés, alemán, italiano, portugués, ruso, ucraniano, polaco, sueco, noruego, [y más](./USER_GUIDE.md#supported-languages).

Consulte [la guía del usuario](./USER_GUIDE.md) para obtener instrucciones sobre el uso de CrossPoint.

Para más detalles sobre el alcance del proyecto, consulte el documento [SCOPE.md](SCOPE.md).

### 📖 Diseño de lectura

- **Sangría de primera línea**: Active/desactive la sangría de párrafos mediante CSS `text-indent` para mejorar la legibilidad.
- Ancho de sangría calculado en función del ancho real de los caracteres CJK.
- **Analizador CSS de streaming**: Maneja hojas de estilo grandes sin agotar la memoria.
- **Corrección del espaciado entre palabras CJK**: Elimina espacios erróneos entre caracteres CJK adyacentes.

## 📦 Lista de funciones

- [x] Análisis y renderizado de EPUB (EPUB 2 y EPUB 3)
- [x] Visualización del texto alternativo de imágenes EPUB
- [x] Soporte de lectura de texto plano TXT
- [x] Guardado del progreso de lectura
- [x] Explorador de archivos (admite carpetas anidadas)
- [x] Pantalla de suspensión personalizable (admite visualización de portada)
- [x] Sincronización del progreso de lectura de KOReader
- [x] Carga de archivos por WiFi
- [x] Actualización de firmware OTA por WiFi
- [x] Gestión de credenciales WiFi (escanear, guardar, eliminar mediante interfaz web)
- [x] Mejoras del modo AP (soporte de portal cautivo)
- [x] Cambio de modo oscuro/claro
- [x] Sangría de primera línea para párrafos
- [x] Soporte de guionado multilingüe
- [x] Personalización de fuente, diseño y estilo de visualización
  - [x] Sistema de fuentes externas (fuentes de lectura y UI)
- [x] Rotación de pantalla (configuración de orientación independiente para UI/lector)
  - [x] Rotación de la interfaz de lectura (vertical, horizontal CW/CCW, invertida)
  - [x] Rotación de la interfaz UI (vertical, invertida)
- [x] Conexión inalámbrica con Calibre e integración de biblioteca web
- [x] Visualización de imágenes de portada
- [x] Tema Lyra (con adaptación completa para páginas CJK)
- [x] Sincronización del progreso de lectura de KOReader
- [x] Analizador CSS de streaming (evita agotamiento de memoria en hojas de estilo EPUB grandes)

Para instrucciones detalladas de operación, consulte la [Guía del usuario](./USER_GUIDE.md).

## 📥 Instalación

### Flasheo web del proyecto original

1. Conecte su Xteink X4 a su computadora mediante USB-C
2. Descargue el archivo `firmware.bin` desde la versión que desee a través de la [página de versiones](https://github.com/crosspoint-reader/crosspoint-reader/releases)
3. Vaya a https://xteink.dve.al/ y escriba el firmware utilizando la sección "OTA fast flash controls"

### Flasheo web (Recomendado)

1. Conecte el Xteink X4 a su computadora mediante USB-C.
2. Visite https://xteink-flasher-cjk.vercel.app/ y haga clic en **"Flash CrossPoint CJK firmware"** para escribir el último firmware CJK directamente.

> **Consejo**: Para restaurar el firmware oficial o el firmware original de CrossPoint, utilice la misma página. Para cambiar la partición de arranque, visite https://xteink-flasher-cjk.vercel.app/debug.

### Compilación manual

#### Requisitos

* **PlatformIO Core** (`pio`)
* Python 3.8+
* Pillow (`python3 -m pip install Pillow`)
* Cable USB-C
* Xteink X4

#### Obtener el código

```bash
git clone --recursive https://github.com/aBER0724/crosspoint-reader-cjk

# O, si ya clonó sin --recursive:
git submodule update --init --recursive
```

#### Compilar y escribir

Ejecute el siguiente comando después de conectar el dispositivo:

```bash
pio run --target upload
```
### Depuración

Después de escribir las nuevas funciones, se recomienda capturar registros detallados desde el puerto serial.

Primero, asegúrese de que todos los paquetes de Python requeridos estén instalados:

```python
python3 -m pip install pyserial colorama matplotlib
```
después de eso, ejecute el script:
```sh
# Para Linux
# Esto se probó en Debian y debería funcionar en la mayoría de los sistemas Linux.
python3 scripts/debugging_monitor.py

# Para macOS
python3 scripts/debugging_monitor.py /dev/cu.usbmodem2101
```
Puede ser necesario realizar ajustes menores para Windows.

## 🔠 Fuentes

### Generación de fuentes

- `scripts/generate_cjk_ui_font.py`

- [DotInk](https://apps.apple.com/us/app/dotink-eink-assistant/id6754073002) (Recomendado: use DotInk para generar fuentes de lectura y obtener una mejor vista previa de cómo se verán en el dispositivo).

El encabezado de la fuente UI integrada de 20px se regenera automáticamente durante las compilaciones de PlatformIO desde `fonts/思源黑体-Bold.otf` y los caracteres de traducción i18n actuales.

### Configuración de fuentes

1. Cree una carpeta `fonts/` en el directorio raíz de la tarjeta SD.
2. Coloque los archivos de fuente en formato `.bin` dentro de la carpeta.
3. Seleccione "Reader/Reader Font" o "Display/UI Font" en la configuración.

**Formato de nombre de archivo de fuente**: `FontName_size_WxH.bin`

Ejemplos:
- `SourceHanSansCN-Medium_20_20x20.bin` (UI: 20pt, 20x20)
- `KingHwaOldSong_38_33x39.bin` (Lectura: 38pt, 33x39)

**Descripción de fuentes**:
- **Fuente de lectura**: Se utiliza para el texto del contenido de los libros.
- **Fuente de UI**: Se utiliza para menús, títulos y elementos de la interfaz.

> Debido a las limitaciones de memoria, la fuente integrada utiliza un conjunto de caracteres UI compacto generado a partir de las traducciones actuales.
> Se recomienda almacenar fuentes UI y de lectura más completas en la tarjeta SD para una mejor experiencia.
> Si generar fuentes es inconveniente, puede descargar las fuentes de ejemplo proporcionadas anteriormente.

## ℹ️ Preguntas frecuentes

1. Para capítulos con muchas páginas, el indexado puede tardar más en la primera apertura. Esto ha mejorado significativamente con el analizador CSS de streaming, pero los capítulos muy grandes aún pueden requerir unos segundos.
    > Si el dispositivo se queda atascado indexando después de un reinicio y no se completa durante mucho tiempo, impidiendo volver a otras páginas, vuelva a escribir el firmware.
2. Si se queda atascado en una interfaz específica, intente reiniciar el dispositivo.
3. El ESP32-C3 tiene una memoria muy limitada. El uso de archivos de fuentes CJK grandes tanto para UI como para lectura simultáneamente puede provocar fallos por agotamiento de memoria. Se recomienda mantener las fuentes UI en 20pt o menos.
4. Al abrir la pantalla de inicio por primera vez después de agregar nuevos libros, el dispositivo generará miniaturas de portada. Puede aparecer una ventana emergente de "Cargando" durante unos segundos; esto es normal, no un congelamiento.
5. Si está en v0.3.3 y las actualizaciones OTA normales no funcionan, utilice la [versión de recuperación SD](https://github.com/aBER0724/crosspoint-reader-cjk/releases/tag/sd-recovery). Descargue su `firmware.bin`, colóquelo en la tarjeta SD junto con el firmware que desea instalar y siga las notas de nomenclatura en la página de versiones.

## 🤝 Colaboradores

Gracias a todos quienes han contribuido a CrossPoint Reader CJK.

- [aBER0724](https://github.com/aBER0724) — mantenedor
- [donutboyy](https://github.com/donutboyy) — soporte para actualización de firmware desde tarjeta SD

## 🤝 Contribuir

Si es nuevo en la base de código, comience con la [documentación de contribución](./docs/contributing/README.md).

Si busca una forma de ayudar, eche un vistazo al [tablero de discusión de ideas](https://github.com/crosspoint-reader/crosspoint-reader/discussions/categories/ideas).
Si hay algo allí en lo que le gustaría trabajar, deje un comentario para que podamos evitar duplicar esfuerzos.

Todos aquí somos voluntarios, por lo que le pedimos ser respetuoso y paciente. Para más detalles sobre nuestra gobernanza y comunidad
principios, consulte [GOVERNANCE.md](GOVERNANCE.md).

### Para enviar una contribución:

1. Cree una rama enfocada desde `master`.
2. Ejecute las verificaciones locales antes de abrir una PR:

```sh
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

Si su cambio afecta la lógica de guionado, también ejecute:

```sh
./test/run_hyphenation_eval.sh english
```

3. Use un título de PR semántico y complete `.github/PULL_REQUEST_TEMPLATE.md`.

Para herramientas de codificación agenticas (agentes de codificación IA), lea también [AGENTS.md](./AGENTS.md).

## 📜 Créditos

- [CrossPoint Reader](https://github.com/daveallie/crosspoint-reader) - Proyecto original
- [open-x4-sdk](https://github.com/daveallie/open-x4-sdk) - SDK de desarrollo Xteink X4

---

**Aviso legal**: Este proyecto no está afiliado con Xteink ni con el fabricante de hardware X4; es puramente un proyecto comunitario.
