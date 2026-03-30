/*
 * TFT ILI9341 — terminal por Serial (Uno)
 *
 * curLine[] + rowCount; pequeño hist[3] solo para al llenar la pantalla.
 * Cada tecla: fillRect de la fila de edición (salvo Enter / salto de línea largo).
 * Pantalla llena: fillScreen y se muestran 3 líneas verdes (2 anteriores + la
 * última) gracias a hist; debajo la línea de edición. Texto tamaño 1, alto 8 px.
 * Líneas largas: corte solo por software a termCols() caracteres por fila (sin
 * depender del wrap del TFT).
 *
 * Pines: CS=10, DC=9, RST=8 | SD_CS=4, TOUCH_CS=5 | MOSI=11, SCK=13
 *
 * RST (reset): hardware del ILI9341 en tft.begin(). No limpia píxeles en runtime.
 * Monitor Serie: SERIAL_BAUD (115200 por defecto).
 *
 * Librerías: Adafruit ILI9341 + Adafruit GFX
 *
 * Mini-BASIC (una línea, Enter): se intenta antes de guardar texto normal.
 *   print("hola")   sum(12, 23)   mul(2, 3)   sub(10, 4)   div(20, 4)   help
 * Errores BASIC: mensaje rojo + Serial (inglés: unknown command, syntax: …).
 * Línea que parece BASIC: no se parte en verde al escribir; al Enter solo salida.
 */

#include <SPI.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

#define TFT_CS   10
#define TFT_DC    9
#define TFT_RST   8
#define SD_CS     4
#define TOUCH_CS  5

Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

static const uint32_t TFT_SPI_HZ = 1000000UL;
static const uint32_t SERIAL_BAUD = 115200UL;

static const uint8_t TFT_ROTATION = 1;

static const bool CLEAR_BLACK_ALL_ROTATIONS = false;

static const uint8_t TERM_TEXT_SIZE = 1;
static const uint8_t LINE_HEIGHT = 8;

/* Buffer de edición; más grande para comandos BASIC sin trocear en verde al escribir. */
// En la práctica el TFT muestra ~40-50 cols con textSize=1; bajar esto
// reduce SRAM global y también el pico de stack.
#define MAX_LINE_CHARS 64

static char curLine[MAX_LINE_CHARS + 1];
static uint8_t curLen = 0;
/** Filas ya pintadas en verde (0 .. rowCount-1). La edición va en la fila rowCount. */
static uint8_t rowCount = 0;

// Numeric variables:
// - A..Z are aliases for V0..V25
// - V0..V31 are supported for more variables.
#define NUM_VARS 32
static long vars[NUM_VARS];
// bit i == variable i ya definida
static uint32_t varsMask = 0;
static bool parseLastUndefinedVar = false;

// String variables: S0$..S7$
#define STR_VARS 12
#define STR_MAX_LEN 24
static char strVars[STR_VARS][STR_MAX_LEN + 1];
// bit i == string var i ya definida
static uint16_t strVarsMask = 0;
static bool parseLastUndefinedStrVar = false;

/** Últimas 3 líneas fijadas (verde); al reset: filas 0..2 = hist[1], hist[2], última. */
static char hist[3][MAX_LINE_CHARS + 1];

static void histPush(const char* s) {
  memcpy(hist[0], hist[1], MAX_LINE_CHARS + 1);
  memcpy(hist[1], hist[2], MAX_LINE_CHARS + 1);
  strncpy(hist[2], s, MAX_LINE_CHARS);
  hist[2][MAX_LINE_CHARS] = '\0';
}

/** Caracteres por fila (corte explícito; coincide con ancho en px / 6*textSize). */
static uint8_t termCols() {
  int16_t c = tft.width() / (6 * TERM_TEXT_SIZE) - 1;
  if (c < 8) {
    c = 8;
  }
  if (c > MAX_LINE_CHARS - 1) {
    c = MAX_LINE_CHARS - 1;
  }
  return (uint8_t)c;
}

/** Filas en alto; mínimo 4 (tres verdes tras reset + una de edición). */
static uint8_t termMaxLines() {
  uint8_t m = (uint8_t)(tft.height() / LINE_HEIGHT);
  if (m < 4) {
    m = 4;
  }
  return m;
}

static void paintRowAt(uint8_t row, const char* s, uint16_t fg) {
  int16_t y = (int16_t)row * (int16_t)LINE_HEIGHT;
  if (y >= tft.height()) {
    return;
  }
  uint16_t w = tft.width();
  uint16_t hRow = LINE_HEIGHT;
  if ((int32_t)y + hRow > tft.height()) {
    hRow = (uint16_t)(tft.height() - y);
  }
  tft.fillRect(0, y, w, hRow, ILI9341_BLACK);
  tft.setTextSize(TERM_TEXT_SIZE);
  tft.setTextWrap(false);
  tft.setTextColor(fg);
  tft.setCursor(0, y);
  tft.print(s);
}

static void paintEditingRow() {
  paintRowAt(rowCount, curLine, ILI9341_DARKGREY);
}

/** Pantalla llena: borra todo; 3 líneas verdes = dos anteriores (hist) + última; edición abajo.
 *  No modifica curLine/curLen (el resto de un wrap largo sigue en curLine). */
static void screenFullReset(const char* lastCommitted) {
  tft.fillScreen(ILI9341_BLACK);
  paintRowAt(0, hist[1], ILI9341_GREEN);
  paintRowAt(1, hist[2], ILI9341_GREEN);
  paintRowAt(2, lastCommitted, ILI9341_GREEN);
  rowCount = 3;
  histPush(lastCommitted);
  paintEditingRow();
}

static void clearAllRotationsBlack() {
  for (uint8_t r = 0; r < 4; r++) {
    tft.setRotation(r);
    tft.fillScreen(ILI9341_BLACK);
  }
}

/** Emite exactamente `cols` caracteres como línea verde; el resto queda en curLine. */
static void emitOneChunk(uint8_t cols) {
  if (curLen < cols) {
    return;
  }

  char chunk[MAX_LINE_CHARS + 1];
  memcpy(chunk, curLine, cols);
  chunk[cols] = '\0';
  uint8_t rest = (uint8_t)(curLen - cols);
  memmove(curLine, curLine + cols, rest + 1);
  curLen = rest;

  uint8_t maxR = termMaxLines();

  if (rowCount >= maxR - 1) {
    screenFullReset(chunk);
    return;
  }

  paintRowAt(rowCount, chunk, ILI9341_GREEN);
  histPush(chunk);
  rowCount++;
  paintEditingRow();
}

/** Al llegar al ancho lógico, corta y baja el trozo siguiente (control nuestro). */
static void wrapEmitChunk(uint8_t cols) {
  if (curLen < cols) {
    return;
  }
  emitOneChunk(cols);
}

static void skipWs(const char** pp) {
  const char* p = *pp;
  while (*p == ' ' || *p == '\t') {
    p++;
  }
  *pp = p;
}

/** value := number | VAR
 * VAR es una letra A-Z (case-insensitive). Si VAR no existe se pone
 * parseLastUndefinedVar=true y el parse falla (para poder mostrar el error
 * correcto).
 */
static bool parseValueToken(const char** pp, long* out) {
  parseLastUndefinedVar = false;
  const char* p = *pp;
  skipWs(&p);
  if (*p == '\0') {
    return false;
  }
  // Indexed numeric variable: V0..V31 (V<number>)
  if (toupper((unsigned char)p[0]) == 'V' && isdigit((unsigned char)p[1])) {
    p++; // consume 'V'
    char* end = nullptr;
    long idxL = strtol(p, &end, 10);
    if (end == p) {
      return false;
    }
    if (idxL < 0 || idxL >= (long)NUM_VARS) {
      return false;
    }
    uint8_t idx = (uint8_t)idxL;
    p = end;
    if ((varsMask & (1UL << idx)) == 0) {
      parseLastUndefinedVar = true;
      return false;
    }
    *out = vars[idx];
    *pp = p;
    return true;
  }

  // Aliases A..Z (A->V0, ... Z->V25)
  if (isalpha((unsigned char)*p)) {
    char name = (char)toupper((unsigned char)*p);
    if (name < 'A' || name > 'Z') {
      return false;
    }
    uint8_t idx = (uint8_t)(name - 'A');
    p++;
    if ((varsMask & (1UL << idx)) == 0) {
      parseLastUndefinedVar = true;
      return false;
    }
    *out = vars[idx];
    *pp = p;
    return true;
  }

  // Plain number
  char* end = nullptr;
  long v = strtol(p, &end, 10);
  if (end == p) {
    return false;
  }
  *out = v;
  *pp = end;
  return true;
}

/** String literal or string variable.
 *  string literal: "...."
 *  string var: S<number>$  (ex: S0$)
 *  string var alias: A$..Z$ (mapped to S0..S25 if within STR_VARS)
 */
static bool parseStringVarRef(const char** pp, char* out, size_t outMax) {
  parseLastUndefinedStrVar = false;
  const char* p = *pp;
  skipWs(&p);
  if (*p == '\0') {
    return false;
  }

  uint8_t idx = 0;

  // S<number>$
  if (toupper((unsigned char)*p) == 'S' && isdigit((unsigned char)p[1])) {
    p++; // consume 'S'
    char* end = nullptr;
    long idxL = strtol(p, &end, 10);
    if (end == p) {
      return false;
    }
    p = end;
    if (*p != '$') {
      return false;
    }
    p++; // consume '$'
    if (idxL < 0 || idxL >= (long)STR_VARS) {
      return false;
    }
    idx = (uint8_t)idxL;
  } else if (isalpha((unsigned char)*p) && p[1] == '$') {
    // A$..Z$ alias
    char name = (char)toupper((unsigned char)*p);
    if (name < 'A' || name > 'Z') {
      return false;
    }
    idx = (uint8_t)(name - 'A');
    if (idx >= STR_VARS) {
      // fuera de rango de RAM reservada para strings
      return false;
    }
    p += 2; // consume 'X' + '$'
  } else {
    return false;
  }

  if ((strVarsMask & (1U << idx)) == 0) {
    parseLastUndefinedStrVar = true;
    return false;
  }

  strncpy(out, strVars[idx], outMax - 1);
  out[outMax - 1] = '\0';
  *pp = p;
  return true;
}

static bool parseStringLiteral(const char** pp, char* out, size_t outMax) {
  const char* p = *pp;
  skipWs(&p);
  if (*p != '"') {
    return false;
  }
  p++; // consume opening '"'
  size_t o = 0;
  while (*p != '\0' && *p != '"' && o < outMax - 1) {
    out[o++] = *p++;
  }
  if (*p != '"') {
    return false;
  }
  out[o] = '\0';
  p++; // consume closing '"'
  *pp = p;
  return true;
}

static bool parseStringToken(const char** pp, char* out, size_t outMax) {
  if (parseStringLiteral(pp, out, outMax)) {
    return true;
  }
  if (parseStringVarRef(pp, out, outMax)) {
    return true;
  }
  return false;
}

static bool strPrefCi(const char* s, const char* pref) {
  while (*pref != '\0') {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*pref)) {
      return false;
    }
    s++;
    pref++;
  }
  return true;
}

/** Palabra clave seguida de fin, espacio o '(' (evita que "printx" sea print). */
static bool keywordAt(const char* p, const char* kw) {
  if (!strPrefCi(p, kw)) {
    return false;
  }
  size_t n = strlen(kw);
  char c = p[n];
  return c == '\0' || isspace((unsigned char)c) || c == '(';
}

/** concat(x,y) donde x/y pueden ser:
 *  - string literal "..."
 *  - string var S0$..S7$ (o A$.. si aplica)
 *  - number literal o var num (A o V<number>)
 */
static bool parseConcatCall(const char** pp, char* out, size_t outMax) {
  parseLastUndefinedVar = false;
  parseLastUndefinedStrVar = false;
  const char* p = *pp;
  if (!keywordAt(p, "concat")) {
    return false;
  }
  p += 6; // "concat"
  skipWs(&p);
  if (*p != '(') {
    return false;
  }
  p++;

  size_t o = 0;

  // Arg 1
  {
    char sBuf[STR_MAX_LEN + 1];
    long v = 0;
    if (parseStringToken(&p, sBuf, sizeof(sBuf))) {
      size_t sl = strlen(sBuf);
      if (o + sl >= outMax) {
        return false;
      }
      memcpy(out + o, sBuf, sl);
      o += sl;
      out[o] = '\0';
    } else if (parseValueToken(&p, &v)) {
      char numBuf[16];
      snprintf(numBuf, sizeof(numBuf), "%ld", v);
      size_t nl = strlen(numBuf);
      if (o + nl >= outMax) {
        return false;
      }
      memcpy(out + o, numBuf, nl);
      o += nl;
      out[o] = '\0';
    } else {
      return false;
    }
  }

  skipWs(&p);
  if (*p != ',') {
    return false;
  }
  p++;

  // Arg 2
  {
    char sBuf[STR_MAX_LEN + 1];
    long v = 0;
    skipWs(&p);
    if (parseStringToken(&p, sBuf, sizeof(sBuf))) {
      size_t sl = strlen(sBuf);
      if (o + sl >= outMax) {
        return false;
      }
      memcpy(out + o, sBuf, sl);
      o += sl;
      out[o] = '\0';
    } else if (parseValueToken(&p, &v)) {
      char numBuf[16];
      snprintf(numBuf, sizeof(numBuf), "%ld", v);
      size_t nl = strlen(numBuf);
      if (o + nl >= outMax) {
        return false;
      }
      memcpy(out + o, numBuf, nl);
      o += nl;
      out[o] = '\0';
    } else {
      return false;
    }
  }

  skipWs(&p);
  if (*p != ')') {
    return false;
  }
  p++;

  skipWs(&p);
  *pp = p;
  out[o] = '\0';
  return *p == '\0' || isspace((unsigned char)*p);
}

/** Si parece comando BASIC, no hacemos wrap en verde al escribir (solo resultado al Enter). */
static bool lineMaybeBasicCommand(void) {
  curLine[curLen] = '\0';
  const char* p = curLine;
  skipWs(&p);
  if (keywordAt(p, "help") || keywordAt(p, "print") || keywordAt(p, "sum")
      || keywordAt(p, "mul") || keywordAt(p, "sub") || keywordAt(p, "div")
      || keywordAt(p, "concat") || keywordAt(p, "out") || keywordAt(p, "in")
      || keywordAt(p, "high") || keywordAt(p, "low") || keywordAt(p, "restart")) {
    return true;
  }

  // Assignment detection (A=..., V12=..., S0$=...); buscamos '=' en la línea.
  if (isalpha((unsigned char)*p)) {
    const char* s = p;
    while (*s != '\0') {
      if (*s == '=') {
        return true;
      }
      s++;
    }
  }
  return false;
}

static bool parseLeadingWord(const char** pp, char* buf, size_t maxLen) {
  skipWs(pp);
  const char* p = *pp;
  size_t i = 0;
  while (*p != '\0'
         && (isalnum((unsigned char)*p) || *p == '_')) {
    if (i < maxLen - 1) {
      buf[i++] = (char)tolower((unsigned char)*p);
    }
    p++;
  }
  buf[i] = '\0';
  *pp = p;
  return i > 0;
}

static bool isKnownCmdWord(const char* w) {
  return strcmp(w, "help") == 0 || strcmp(w, "print") == 0 || strcmp(w, "sum") == 0
         || strcmp(w, "mul") == 0 || strcmp(w, "sub") == 0 || strcmp(w, "div") == 0
         || strcmp(w, "concat") == 0 || strcmp(w, "out") == 0 || strcmp(w, "in") == 0
         || strcmp(w, "high") == 0 || strcmp(w, "low") == 0 || strcmp(w, "restart") == 0;
}

static bool isTftReservedPin(uint8_t pin) {
  // Evita tocar pines usados por el TFT/SD/SPI (pueden dejar la pantalla en blanco).
  switch (pin) {
    case TFT_RST: // 8
    case TFT_DC:  // 9
    case TFT_CS:  // 10
    case SD_CS:   // 4
    case TOUCH_CS:// 5
    case 11:      // MOSI (SPI)
    case 12:      // MISO (SPI)
    case 13:      // SCK (SPI)
      return true;
    default:
      return false;
  }
}

static void restartTerminal() {
  // Limpieza global de la pantalla y estado del terminal.
  tft.setRotation(TFT_ROTATION);
  if (CLEAR_BLACK_ALL_ROTATIONS) {
    clearAllRotationsBlack();
    tft.setRotation(TFT_ROTATION);
  }
  tft.fillScreen(ILI9341_BLACK);

  memset(hist, 0, sizeof(hist));
  varsMask = 0;
  memset(vars, 0, sizeof(vars));
  strVarsMask = 0;
  memset(strVars, 0, sizeof(strVars));

  curLen = 0;
  curLine[0] = '\0';
  rowCount = 0;

  paintRowAt(0, "TFT Serial terminal", ILI9341_GREEN);
  histPush("TFT Serial terminal");
  paintRowAt(1, "115200 baud", ILI9341_GREEN);
  histPush("115200 baud");
  {
    char info[16];
    uint8_t c = termCols();
    uint8_t m = termMaxLines();
    info[0] = 'c';
    info[1] = '=';
    info[2] = (char)('0' + (c / 10));
    info[3] = (char)('0' + (c % 10));
    info[4] = ' ';
    info[5] = 'm';
    info[6] = '=';
    info[7] = (char)('0' + (m / 10));
    info[8] = (char)('0' + (m % 10));
    info[9] = '\0';
    paintRowAt(2, info, ILI9341_GREEN);
    histPush(info);
  }
  rowCount = 3;
  paintEditingRow();
}

/** Mensaje de error en rojo; siempre Serial + TFT (antes se perdía TFT si rowCount >= maxR). */
static void commitErrorText(const char* msg) {
  Serial.println(msg);

  uint8_t maxR = termMaxLines();
  if (rowCount >= maxR) {
    tft.fillScreen(ILI9341_BLACK);
    paintRowAt(0, msg, ILI9341_RED);
    rowCount = 1;
    memset(hist, 0, sizeof(hist));
    curLen = 0;
    curLine[0] = '\0';
    paintEditingRow();
    return;
  }
  if (rowCount == maxR - 1) {
    tft.fillScreen(ILI9341_BLACK);
    paintRowAt(0, hist[1], ILI9341_GREEN);
    paintRowAt(1, hist[2], ILI9341_GREEN);
    paintRowAt(2, msg, ILI9341_RED);
    rowCount = 3;
    histPush(msg);
    paintEditingRow();
    return;
  }
  paintRowAt(rowCount, msg, ILI9341_RED);
  histPush(msg);
  rowCount++;
  paintEditingRow();
}

/** Una línea de salida en verde (misma lógica que una línea confirmada en el terminal). */
static void commitGreenText(const char* tmp) {
  uint8_t maxR = termMaxLines();
  if (rowCount >= maxR) {
    return;
  }
  if (rowCount == maxR - 1) {
    screenFullReset(tmp);
    return;
  }
  paintRowAt(rowCount, tmp, ILI9341_GREEN);
  histPush(tmp);
  rowCount++;
  paintEditingRow();
}

/** Commit de 2 líneas verdes seguidas (cmd + resultado).
 *  Esto evita que, si estabas casi al límite, el primer commit se pierda
 *  por un reset entre líneas. */
static void commitTwoGreenText(const char* first, const char* second) {
  uint8_t maxR = termMaxLines();

  if (rowCount <= maxR - 2) {
    paintRowAt(rowCount, first, ILI9341_GREEN);
    histPush(first);
    rowCount++;
    paintRowAt(rowCount, second, ILI9341_GREEN);
    histPush(second);
    rowCount++;
    paintEditingRow();
    return;
  }

  // Poca cabida: borra y deja solo estas dos líneas para que no se pierdan.
  tft.fillScreen(ILI9341_BLACK);
  memset(hist, 0, sizeof(hist));
  rowCount = 0;
  paintRowAt(rowCount, first, ILI9341_GREEN);
  histPush(first);
  rowCount++;
  paintRowAt(rowCount, second, ILI9341_GREEN);
  histPush(second);
  rowCount++;
  paintEditingRow();
}

static bool parseTwoLongs(const char* p, long* a, long* b) {
  const char* q = p;
  skipWs(&q);
  if (*q != '(') {
    return false;
  }
  q++;
  skipWs(&q);
  if (!parseValueToken(&q, a)) {
    return false;
  }
  skipWs(&q);
  if (*q != ',') {
    return false;
  }
  q++;
  skipWs(&q);
  if (!parseValueToken(&q, b)) {
    return false;
  }
  skipWs(&q);
  if (*q != ')') {
    return false;
  }
  q++;
  skipWs(&q);
  return *q == '\0';
}

/** Si la línea actual es un comando BASIC, lo ejecuta y devuelve true (no texto normal). */
static bool tryBasicCommand() {
  curLine[curLen] = '\0';
  const char* p = curLine;
  skipWs(&p);
  if (*p == '\0') {
    return false;
  }

  char out[MAX_LINE_CHARS + 1];

  // restart: clears TFT and re-initializes terminal state
  if (keywordAt(p, "restart")) {
    p += 7;
    skipWs(&p);
    // allow optional parentheses: restart()
    if (*p == '(') {
      p++;
      skipWs(&p);
      if (*p == ')') {
        p++;
      }
      skipWs(&p);
    }
    if (*p != '\0') {
      commitErrorText("syntax: restart");
      return true;
    }
    restartTerminal();
    Serial.println(F("restarted"));
    return true;
  }

  // Digital pin control:
  //   out(pin)  -> pinMode(pin, OUTPUT)
  //   in(pin)   -> pinMode(pin, INPUT)
  //   high(pin) -> digitalWrite(pin, HIGH)
  //   low(pin)  -> digitalWrite(pin, LOW)
  if (keywordAt(p, "out")) {
    p += 3;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText("syntax: out");
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText("syntax: out");
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText("syntax: out");
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText("syntax: out");
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText("pin out of range");
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText("pin reserved");
      return true;
    }
    pinMode((uint8_t)pin, OUTPUT);
    commitTwoGreenText(curLine, "ok");
    Serial.println(F("ok"));
    return true;
  }

  if (keywordAt(p, "in")) {
    p += 2;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText("syntax: in");
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText("syntax: in");
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText("syntax: in");
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText("syntax: in");
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText("pin out of range");
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText("pin reserved");
      return true;
    }
    pinMode((uint8_t)pin, INPUT);
    commitTwoGreenText(curLine, "ok");
    Serial.println(F("ok"));
    return true;
  }

  if (keywordAt(p, "high")) {
    p += 4;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText("syntax: high");
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText("syntax: high");
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText("syntax: high");
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText("syntax: high");
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText("pin out of range");
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText("pin reserved");
      return true;
    }
    digitalWrite((uint8_t)pin, HIGH);
    commitTwoGreenText(curLine, "HIGH");
    Serial.println(F("HIGH"));
    return true;
  }

  if (keywordAt(p, "low")) {
    p += 3;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText("syntax: low");
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText("syntax: low");
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText("syntax: low");
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText("syntax: low");
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText("pin out of range");
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText("pin reserved");
      return true;
    }
    digitalWrite((uint8_t)pin, LOW);
    commitTwoGreenText(curLine, "LOW");
    Serial.println(F("LOW"));
    return true;
  }

  // Assignment: A=123, V12=123, A$="hi", S0$="hi"
  {
    const char* q = p;
    skipWs(&q);
    if (keywordAt(q, "let")) {
      q += 3;
      skipWs(&q);
    }

    // Determine assignment target type
    bool isStringTarget = false;
    bool numericTarget = false;
    uint8_t idxNum = 0;
    uint8_t idxStr = 0;

    // S<number>$ or A$..Z$ alias (if within STR_VARS)
    if (toupper((unsigned char)*q) == 'S' && isdigit((unsigned char)q[1])) {
      q++; // 'S'
      char* end = nullptr;
      long idxL = strtol(q, &end, 10);
      if (end == q) {
        commitErrorText("syntax: assignment");
        return true;
      }
      q = end;
      if (*q != '$') {
        commitErrorText("syntax: assignment");
        return true;
      }
      q++; // consume '$'
      if (idxL < 0 || idxL >= (long)STR_VARS) {
        commitErrorText("string variable out of range");
        return true;
      }
      idxStr = (uint8_t)idxL;
      isStringTarget = true;
    } else if (toupper((unsigned char)*q) == 'V' && isdigit((unsigned char)q[1])) {
      // V<number>
      q++; // 'V'
      char* end = nullptr;
      long idxL = strtol(q, &end, 10);
      if (end == q) {
        commitErrorText("syntax: assignment");
        return true;
      }
      q = end;
      if (idxL < 0 || idxL >= (long)NUM_VARS) {
        commitErrorText("variable out of range");
        return true;
      }
      idxNum = (uint8_t)idxL;
      numericTarget = true;
    } else if (isalpha((unsigned char)*q)) {
      char name = (char)toupper((unsigned char)*q);
      if (name >= 'A' && name <= 'Z') {
        if (q[1] == '$') {
          uint8_t tmp = (uint8_t)(name - 'A');
          if (tmp >= STR_VARS) {
            commitErrorText("string variable out of range");
            return true;
          }
          idxStr = tmp;
          isStringTarget = true;
          q += 2; // X$
        } else {
          uint8_t tmp = (uint8_t)(name - 'A');
          idxNum = tmp;
          numericTarget = true;
          q++; // consume letter
        }
      }
    }

    skipWs(&q);
    if (isStringTarget || numericTarget) {
      if (*q != '=') {
        // Looks like a valid variable ref but not assignment; not BASIC command.
      } else {
        q++; // '='
        if (numericTarget) {
          long val = 0;
          if (!parseValueToken(&q, &val)) {
            commitErrorText(parseLastUndefinedVar ? "undefined variable" : "syntax: assignment");
            return true;
          }
          skipWs(&q);
          if (*q != '\0') {
            commitErrorText("syntax: assignment");
            return true;
          }
          varsMask |= (1UL << idxNum);
          vars[idxNum] = val;
          snprintf(out, sizeof(out), "%ld", val);
          commitTwoGreenText(curLine, out);
          Serial.println(out);
          return true;
        }

        if (isStringTarget) {
          // RHS can be: "literal" | concat(...) | Sx$ / Ax$
          char tmpStr[STR_MAX_LEN + 1];
          skipWs(&q);
          bool ok = false;
          if (*q == '"') {
            const char* qq = q;
            ok = parseStringLiteral(&qq, tmpStr, sizeof(tmpStr));
            if (ok) {
              q = qq;
            }
          } else if (keywordAt(q, "concat")) {
            const char* qq = q;
            ok = parseConcatCall(&qq, tmpStr, sizeof(tmpStr));
            if (ok) {
              q = qq;
            }
          } else if (parseStringVarRef(&q, tmpStr, sizeof(tmpStr))) {
            ok = true;
          }

          if (!ok) {
            if (parseLastUndefinedStrVar) {
              commitErrorText("undefined string variable");
            } else {
              commitErrorText("syntax: string assignment");
            }
            return true;
          }

          skipWs(&q);
          if (*q != '\0') {
            commitErrorText("syntax: string assignment");
            return true;
          }

          strncpy(strVars[idxStr], tmpStr, STR_MAX_LEN);
          strVars[idxStr][STR_MAX_LEN] = '\0';
          strVarsMask |= (1U << idxStr);

          commitTwoGreenText(curLine, tmpStr);
          Serial.println(tmpStr);
          return true;
        }
      }
    }
  }

  if (keywordAt(p, "help")) {
    p += 4;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText("syntax: help");
      return true;
    }
    Serial.println(F("cmds: print(\"...\") concat(x,y) out(pin) in(pin) high(pin) low(pin) sum(a,b) mul(a,b) sub(a,b) div(a,b) restart help"));
    strncpy(out, "cmds: print concat out in high low sum mul sub div restart", MAX_LINE_CHARS);
    out[MAX_LINE_CHARS] = '\0';
    commitTwoGreenText(curLine, out);
    return true;
  }

  if (keywordAt(p, "concat")) {
    char sOut[MAX_LINE_CHARS + 1];
    const char* q = p;
    if (!parseConcatCall(&q, sOut, sizeof(sOut))) {
      if (parseLastUndefinedVar) {
        commitErrorText("undefined variable");
      } else if (parseLastUndefinedStrVar) {
        commitErrorText("undefined string variable");
      } else {
        commitErrorText("syntax: concat");
      }
      return true;
    }
    commitTwoGreenText(curLine, sOut);
    Serial.println(sOut);
    return true;
  }

  if (keywordAt(p, "print")) {
    p += 5;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText("syntax: print");
      return true;
    }
    p++;
    skipWs(&p);
    if (*p == '"') {
      const char* q = p;
      if (!parseStringLiteral(&q, out, sizeof(out))) {
        commitErrorText("syntax: print");
        return true;
      }
      p = q;
    } else if (keywordAt(p, "concat")) {
      const char* q = p;
      if (!parseConcatCall(&q, out, sizeof(out))) {
        if (parseLastUndefinedVar) {
          commitErrorText("undefined variable");
        } else if (parseLastUndefinedStrVar) {
          commitErrorText("undefined string variable");
        } else {
          commitErrorText("syntax: concat");
        }
        return true;
      }
      p = q;
    } else if (parseStringVarRef(&p, out, sizeof(out))) {
      // parsed as string variable into out
    } else if (parseLastUndefinedStrVar) {
      commitErrorText("undefined string variable");
      return true;
    } else {
      long val = 0;
      if (!parseValueToken(&p, &val)) {
        if (parseLastUndefinedVar) {
          commitErrorText("undefined variable");
        } else {
          commitErrorText("syntax: print");
        }
        return true;
      }
      snprintf(out, sizeof(out), "%ld", val);
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText("syntax: print");
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText("syntax: print");
      return true;
    }
    commitTwoGreenText(curLine, out);
    Serial.println(out);
    return true;
  }

  long a = 0;
  long b = 0;

  if (keywordAt(p, "sum")) {
    p += 3;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText(parseLastUndefinedVar ? "undefined variable" : "syntax: sum");
      return true;
    }
    snprintf(out, sizeof(out), "%ld", a + b);
    commitTwoGreenText(curLine, out);
    Serial.println(out);
    return true;
  }
  if (keywordAt(p, "mul")) {
    p += 3;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText(parseLastUndefinedVar ? "undefined variable" : "syntax: mul");
      return true;
    }
    snprintf(out, sizeof(out), "%ld", a * b);
    commitTwoGreenText(curLine, out);
    Serial.println(out);
    return true;
  }
  if (keywordAt(p, "sub")) {
    p += 3;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText(parseLastUndefinedVar ? "undefined variable" : "syntax: sub");
      return true;
    }
    snprintf(out, sizeof(out), "%ld", a - b);
    commitTwoGreenText(curLine, out);
    Serial.println(out);
    return true;
  }
  if (keywordAt(p, "div")) {
    p += 3;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText(parseLastUndefinedVar ? "undefined variable" : "syntax: div");
      return true;
    }
    if (b == 0) {
      commitErrorText("division by zero");
      return true;
    }
    snprintf(out, sizeof(out), "%ld", a / b);
    commitTwoGreenText(curLine, out);
    Serial.println(out);
    return true;
  }

  {
    const char* q = curLine;
    char w[16];
    if (parseLeadingWord(&q, w, sizeof(w))) {
      skipWs(&q);
      if (*q == '(' && !isKnownCmdWord(w)) {
        commitErrorText("unknown command");
        return true;
      }
    }
  }

  return false;
}

static void flushCurrentLine() {
  curLine[curLen] = '\0';
  uint8_t cols = termCols();
  uint8_t maxR = termMaxLines();

  /* Trozos de ancho fijo hasta dejar solo el último tramo (<= cols). */
  while (curLen > cols) {
    emitOneChunk(cols);
    if (rowCount >= maxR) {
      return;
    }
  }

  if (rowCount >= maxR) {
    return;
  }

  if (rowCount == maxR - 1) {
    // Reproduce screenFullReset() pero asegurando que:
    // - la línea confirmada (curLine) se pinta antes de limpiarla
    // - la fila de edición queda en blanco
    tft.fillScreen(ILI9341_BLACK);
    paintRowAt(0, hist[1], ILI9341_GREEN);
    paintRowAt(1, hist[2], ILI9341_GREEN);
    paintRowAt(2, curLine, ILI9341_GREEN);
    rowCount = 3;
    histPush(curLine);
    curLen = 0;
    curLine[0] = '\0';
    paintEditingRow();
    return;
  }

  paintRowAt(rowCount, curLine, ILI9341_GREEN);
  histPush(curLine);
  rowCount++;
  curLen = 0;
  curLine[0] = '\0';
  paintEditingRow();
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(150);

  pinMode(SD_CS, OUTPUT);
  digitalWrite(SD_CS, HIGH);
  pinMode(TOUCH_CS, OUTPUT);
  digitalWrite(TOUCH_CS, HIGH);

  tft.begin(TFT_SPI_HZ);
  delay(50);

  tft.setRotation(TFT_ROTATION);
  if (CLEAR_BLACK_ALL_ROTATIONS) {
    clearAllRotationsBlack();
    tft.setRotation(TFT_ROTATION);
  }
  tft.fillScreen(ILI9341_BLACK);

  memset(hist, 0, sizeof(hist));
  varsMask = 0;
  // vars[] no hace falta, pero lo dejamos limpio.
  memset(vars, 0, sizeof(vars));
  strVarsMask = 0;
  memset(strVars, 0, sizeof(strVars));
  curLen = 0;
  curLine[0] = '\0';
  rowCount = 0;

  paintRowAt(0, "TFT Serial terminal", ILI9341_GREEN);
  histPush("TFT Serial terminal");
  paintRowAt(1, "115200 baud", ILI9341_GREEN);
  histPush("115200 baud");
  {
    char info[16];
    uint8_t c = termCols();
    uint8_t m = termMaxLines();
    info[0] = 'c';
    info[1] = '=';
    info[2] = (char)('0' + (c / 10));
    info[3] = (char)('0' + (c % 10));
    info[4] = ' ';
    info[5] = 'm';
    info[6] = '=';
    info[7] = (char)('0' + (m / 10));
    info[8] = (char)('0' + (m % 10));
    info[9] = '\0';
    paintRowAt(2, info, ILI9341_GREEN);
    histPush(info);
  }
  rowCount = 3;
  paintEditingRow();
}

void loop() {
  bool needEditingPaint = false;

  while (Serial.available() > 0) {
    int ci = Serial.read();
    if (ci < 0) {
      break;
    }
    char c = (char)ci;

    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      curLine[curLen] = '\0';
      if (!tryBasicCommand()) {
        flushCurrentLine();
        needEditingPaint = false;
      } else {
        curLen = 0;
        curLine[0] = '\0';
        needEditingPaint = true;
      }
      continue;
    }
    if (c == '\b' || c == 127) {
      if (curLen > 0) {
        curLen--;
        curLine[curLen] = '\0';
        needEditingPaint = true;
      }
      continue;
    }
    if ((uint8_t)c < 32) {
      continue;
    }

    uint8_t cols = termCols();
    if (curLen >= cols) {
      if (!lineMaybeBasicCommand()) {
        wrapEmitChunk(cols);
        needEditingPaint = false;
      }
    }
    if (curLen < MAX_LINE_CHARS - 1) {
      curLine[curLen++] = c;
      curLine[curLen] = '\0';
      needEditingPaint = true;
    }
  }

  if (needEditingPaint) {
    paintEditingRow();
  }
}
