# TFT Display Project — CLAUDE.md

Proyecto de aprendizaje con pantallas TFT en dos plataformas: Arduino Uno y ESP32.

---

## Estructura del repositorio

```
tft_display/
├── tft_display_uno/        # Terminal Mini-BASIC en Arduino Uno + ILI9341
├── tft_display_esp32s/     # Sketches para ESP32 + ST7789V
├── tft_diag/               # Herramientas de diagnóstico
├── tft_orientation_probe/  # Probe para detectar rotación correcta
├── serial_tools/           # send_program.py (helper Python para el Uno)
├── semaforo_simple/        # Demo semáforo simple
└── semaforo_matrix_start/  # Demo semáforo con matriz
```

---

## Hardware — dos configuraciones

### Arduino Uno + ILI9341

| Señal   | Pin Arduino |
|---------|-------------|
| TFT CS  | 10          |
| TFT DC  | 9           |
| TFT RST | 8           |
| SD CS   | 4           |
| Touch CS| 5           |
| MOSI    | 11          |
| MISO    | 12          |
| SCK     | 13          |

- **Chip display:** ILI9341 (240×320)
- **Librería:** `Adafruit_ILI9341` + `Adafruit_GFX`
- **SPI:** 1 MHz (`TFT_SPI_HZ`)
- **Rotación por defecto:** landscape (`TFT_ROTATION = 1`)
- **Pin 13 = SCK** — no usar para LED mientras TFT activo

### ESP32 + ST7789V ⚠️

| Señal   | Pin ESP32 |
|---------|-----------|
| TFT CS  | 16        |
| TFT DC  | 17        |
| TFT RST | 21        |
| MOSI    | 23        |
| SCK     | 18        |

- **Chip display:** **ST7789V** (240×320, PCB rojo "2.8'' TFT 240xRGBx320 V1.1")
- **Librería:** `Adafruit ST7735 and ST7789` → `Adafruit_ST7789.h`
- **SPI:** **20 MHz** (cables cortos en protoboard, 3.3V)
- **Voltaje:** 3.3V — NO conectar a 5V

**Inicialización correcta ESP32:**
```cpp
#include <Adafruit_ST7789.h>
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// en setup():
tft.init(240, 320, SPI_MODE3);
tft.setSPISpeed(20000000UL);
tft.setRotation(0);
tft.invertDisplay(false);  // ⚠️ OBLIGATORIO en este panel — ver abajo
```

**Constantes de color:** `ST77XX_BLACK`, `ST77XX_WHITE`, `ST77XX_RED`, etc.

### ⚠️ `invertDisplay(false)` SIEMPRE en este panel ST7789V

La librería `Adafruit_ST7789` envía `INVON` por defecto en `init()`. Este panel ya tiene la inversión aplicada por hardware, así que el `INVON` extra hace que TODOS los colores aparezcan invertidos por bits (ROJO `0xF800` → CYAN, BLANCO → NEGRO, etc.).

**Fix:** llamar `tft.invertDisplay(false)` justo después de `setRotation()` para enviar `INVOFF` y dejar los colores correctos. Sin esto, fotos y primitivos GFX se ven con todos los colores complementarios.

Verificado con sketch `tft_color_cal/`.

---

## Errores críticos conocidos

### ❌ Usar Adafruit_ILI9341 con la pantalla del ESP32
El display del ESP32 es **ST7789V**, no ILI9341. La secuencia de inicialización es incompatible: solo se escribe parcialmente el GRAM y el resto de la pantalla muestra ruido/píxeles de estados anteriores.

**Fix:** usar `Adafruit_ST7789` con `tft.init(240, 320, SPI_MODE3)`.

### ❌ SPI lento en ESP32 setup() → Watchdog reset
Si el SPI es ≤ 1 MHz, un `fillScreen` de 320×240 tarda ~2.5 s. Cuatro `fillScreen` (para limpiar las 4 rotaciones) = ~10 s → el task watchdog del ESP32 (5 s por defecto) resetea el MCU a mitad del borrado, dejando el GRAM parcialmente escrito.

**Fix:** usar mínimo **10 MHz**, idealmente 20 MHz para cables cortos.

### ❌ Pin 13 como LED en el Uno
Pin 13 es SCK del SPI. Úsarlo como LED mientras el TFT está activo corrompe la comunicación.

---

## Compilar y subir

### Arduino Uno
```bash
arduino-cli compile -b arduino:avr:uno tft_display_uno
arduino-cli upload -b arduino:avr:uno -p /dev/cu.usbmodemXXXX tft_display_uno
```

### ESP32
```bash
arduino-cli compile --fqbn esp32:esp32:esp32 tft_display_esp32s
arduino-cli upload --fqbn esp32:esp32:esp32 --port /dev/cu.usbserial-0001 tft_display_esp32s
```

El puerto USB del ESP32 detectado en este equipo: `/dev/cu.usbserial-0001`

---

## Sketch de calibración (ESP32)

`tft_display_esp32s/tft_display_esp32s.ino` actualmente contiene un sketch de calibración que:
- Limpia todo el GRAM en las 4 rotaciones al arrancar
- Cicla ROT0→ROT1→ROT2→ROT3 cada 4 segundos
- Muestra borde doble blanco, marcadores de esquina, bandas RGB, número de rotación y dimensiones
- Imprime por Serial: `ROT=X  WxH`

Útil para verificar que toda la pantalla se escribe correctamente antes de portar el terminal.

---

## Terminal Mini-BASIC (Arduino Uno)

Sketch principal: `tft_display_uno/tft_display_uno.ino`

- **Baud:** 115200
- **Texto tamaño 1**, 8 px por línea
- Historial de 3 líneas al llenar pantalla
- Mini-BASIC: `print()`, `sum()`, `mul()`, `sub()`, `div()`, `concat()`, variables `V0–V7` / `A–H`, strings `S0$–S1$`
- GPIO: `out()`, `in()`, `high()`, `low()`
- VM bytecode: `bc()`, `bcsave`, `bclist`, `run`, `load`, `autorun`
- EEPROM: hasta 31 programas, layout v2 con autorun

**Límites críticos Uno:**
- Flash: ~99% usado (`32184 / 32256 bytes`) — cualquier adición puede romper el link
- RAM global: `1184 / 2048 bytes`
- Añadir features: preferir deduplicación y strings cortos en PROGMEM

---

## Librerías requeridas

| Librería | Para |
|----------|------|
| `Adafruit_ILI9341` | Arduino Uno + ILI9341 |
| `Adafruit_ST7735 and ST7789` | ESP32 + ST7789V |
| `Adafruit_GFX` | Ambas plataformas |
| `Adafruit_BusIO` | Dependencia de Adafruit |

---

## Troubleshooting rápido

| Síntoma | Causa probable |
|---------|----------------|
| Pantalla en blanco | Alimentación, backlight, pines CS/DC/RST incorrectos |
| Solo parte de pantalla (ruido en resto) | Librería incorrecta (ILI9341 vs ST7789V) o SPI muy lento (watchdog ESP32) |
| Texto/imagen espejada o girada | Rotación incorrecta — probar ROT 0–3 |
| Ruido persistente entre rotaciones | SPI < 10 MHz en ESP32 → watchdog reset parcial |
| `save failed/full` en Uno | EEPROM llena; usar `eperase` |
| Link error Uno "text section exceeds" | Flash lleno; reducir features o strings |
