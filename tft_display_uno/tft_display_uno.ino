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
 * El LED integrado (pin 13) no sirve para blink con el TFT: 13 es SCK y el SPI
 * lo domina. Ejemplo de LED externo: pin 6 (u otro libre) para bytecode high/low.
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
#include <avr/pgmspace.h>
#include <EEPROM.h>
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
#define MAX_LINE_CHARS 32

static char curLine[MAX_LINE_CHARS + 1];
static uint8_t curLen = 0;
/** Filas ya pintadas en verde (0 .. rowCount-1). La edición va en la fila rowCount. */
static uint8_t rowCount = 0;

// Numeric variables:
// - A..Z are aliases for V0..V25
// - V0..V31 are supported for more variables.
#define NUM_VARS 8
static long vars[NUM_VARS];
// bit i == variable i ya definida
static uint32_t varsMask = 0;
static bool parseLastUndefinedVar = false;

// String variables: S0$..S7$
#define STR_VARS 2
#define STR_MAX_LEN 8
static char strVars[STR_VARS][STR_MAX_LEN + 1];
// bit i == string var i ya definida
static uint16_t strVarsMask = 0;
static bool parseLastUndefinedStrVar = false;

/** Últimas 3 líneas fijadas (verde); al reset: filas 0..2 = hist[1], hist[2], última. */
static char hist[3][MAX_LINE_CHARS + 1];

// Tiny bytecode VM (EEPROM, no SD)
static const uint8_t BC_MAX_INS = 48;
static uint16_t progBC[BC_MAX_INS];
static uint8_t progBCCount = 0;
static bool progCompiled = false;

static bool vmRunning = false;
static uint8_t vmPC = 0;
static uint32_t vmWaitUntilMs = 0;
/** true si el bytecode en RAM cambió y no se ha guardado con bcsave desde entonces. */
static bool progNeedsSave = false;
static char vmPrintBuf[MAX_LINE_CHARS + 1];
static uint8_t vmPrintLen = 0;
static const uint8_t BC_STR_MAX = 16;
static const uint16_t BC_STR_POOL_MAX = 192;
static char bcStrPool[BC_STR_POOL_MAX + 1];
static uint8_t bcStrCount = 0;
static uint8_t bcStrOff[BC_STR_MAX];
static uint8_t bcStrLen[BC_STR_MAX];
static uint16_t bcStrPoolUsed = 0;

static const int EEPROM_ADDR_BASE = 0;
static const uint8_t EEPROM_MAGIC0 = 'B';
static const uint8_t EEPROM_MAGIC1 = 'C';
static const uint8_t EEPROM_VER = 2;
/** Cabecera: v1 = 4 B (sin autorun); v2 = 6 B (+ autorun on/off + índice programa). */
static const uint8_t EEPROM_VER_MIN = 1;
static const uint8_t EEPROM_REC_START = 0xA5;
static const uint8_t EEPROM_REC_END = 0x5A;
static const uint8_t ARG_TYPE_STRING = 0x20; // bit5 = string
static const uint8_t ARG_LEN_CODE_1B = 0x00; // bits7..6

static uint8_t eepromLayoutVer = 0;
static uint8_t eepromAutorunEnable = 0;
static uint8_t eepromAutorunIndex = 0;

static int eepromHeaderBytes(uint8_t fileVer) {
  return (fileVer == 2) ? 6 : 4;
}

#define CMD_DECL(name, text) \
  static const char name[] PROGMEM = text; \
  static const uint8_t name##_LEN = (uint8_t)(sizeof(text) - 1)
#define MSG_DECL(name, text) static const char name[] PROGMEM = text

CMD_DECL(CMD_HELP, "help");
CMD_DECL(CMD_PRINT, "print");
CMD_DECL(CMD_SUM, "sum");
CMD_DECL(CMD_MUL, "mul");
CMD_DECL(CMD_SUB, "sub");
CMD_DECL(CMD_DIV, "div");
CMD_DECL(CMD_CONCAT, "concat");
CMD_DECL(CMD_OUT, "out");
CMD_DECL(CMD_IN, "in");
CMD_DECL(CMD_HIGH, "high");
CMD_DECL(CMD_LOW, "low");
CMD_DECL(CMD_SLEEP, "sleep");
CMD_DECL(CMD_SLEEP100, "sleep100");
CMD_DECL(CMD_SLEEPS, "sleeps");
CMD_DECL(CMD_GOTO, "goto");
CMD_DECL(CMD_NOP, "nop");
CMD_DECL(CMD_END, "end");
CMD_DECL(CMD_PUT, "put");
CMD_DECL(CMD_RESTART, "restart");
CMD_DECL(CMD_CLEAR, "clear");
CMD_DECL(CMD_RUN, "run");
CMD_DECL(CMD_STOP, "stop");
CMD_DECL(CMD_LOAD, "load");
CMD_DECL(CMD_BC, "bc");
CMD_DECL(CMD_BCX, "bcx");
CMD_DECL(CMD_BCSTR, "bcstr");
CMD_DECL(CMD_BCCLR, "bcclr");
CMD_DECL(CMD_BCSAVE, "bcsave");
CMD_DECL(CMD_BCLIST, "bclist");
CMD_DECL(CMD_BCDUMP, "bcdump");
CMD_DECL(CMD_PLIST, "plist");
CMD_DECL(CMD_EPERASE, "eperase");
CMD_DECL(CMD_AUTORUN, "autorun");
CMD_DECL(CMD_OFF, "off");
CMD_DECL(CMD_ON, "on");
CMD_DECL(CMD_LET, "let");

MSG_DECL(MSG_VM_BAD_PARAMS, "vm bad params");
MSG_DECL(MSG_VM_GOTO_OOR, "vm goto out range");
MSG_DECL(MSG_PRINT_OVERFLOW, "print overflow");
MSG_DECL(MSG_STR_ID_OOR, "str id out range");
MSG_DECL(MSG_VM_BAD_OPCODE, "vm bad opcode");
MSG_DECL(MSG_SYNTAX_ASSIGNMENT, "syntax: assignment");
MSG_DECL(MSG_UNDEFINED_VAR, "undefined variable");
MSG_DECL(MSG_UNDEFINED_STR_VAR, "undefined string variable");
MSG_DECL(MSG_SYNTAX_CONCAT, "syntax: concat");
MSG_DECL(MSG_SYNTAX_PRINT, "syntax: print");
MSG_DECL(MSG_DIV_ZERO, "division by zero");
MSG_DECL(MSG_PIN_OOR, "pin out of range");
MSG_DECL(MSG_PIN_RESERVED, "pin reserved");
MSG_DECL(MSG_UNKNOWN_COMMAND, "unknown command");
MSG_DECL(MSG_SYNTAX_OUT, "syntax: out");
MSG_DECL(MSG_SYNTAX_IN, "syntax: in");
MSG_DECL(MSG_SYNTAX_HIGH, "syntax: high");
MSG_DECL(MSG_SYNTAX_LOW, "syntax: low");
MSG_DECL(MSG_SYNTAX_HELP, "syntax: help");
MSG_DECL(MSG_SYNTAX_RUN, "syntax: run");
MSG_DECL(MSG_SYNTAX_STOP, "syntax: stop");
MSG_DECL(MSG_SYNTAX_LOAD, "syntax: load");
MSG_DECL(MSG_SYNTAX_RESTART, "syntax: restart");
MSG_DECL(MSG_SYNTAX_CLEAR, "syntax: clear");
MSG_DECL(MSG_BC_FULL, "bc full");
MSG_DECL(MSG_SYNTAX_BCX, "syntax: bcx(op,t,a)");
MSG_DECL(MSG_STRING_VAR_OOR, "string variable out of range");
MSG_DECL(MSG_VAR_OOR, "variable out of range");
MSG_DECL(MSG_SYNTAX_STR_ASSIGN, "syntax: string assignment");
MSG_DECL(MSG_SYNTAX_BCCLR, "syntax: bcclr");
MSG_DECL(MSG_SYNTAX_EPERASE, "syntax: eperase");
MSG_DECL(MSG_SYNTAX_BCSAVE, "syntax: bcsave");
MSG_DECL(MSG_NO_BYTECODE, "no bytecode");
MSG_DECL(MSG_SAVE_FAILED_FULL, "save failed/full");
MSG_DECL(MSG_SAVE_VERIFY_FAILED, "save verify failed");
MSG_DECL(MSG_SYNTAX_BCLIST, "syntax: bclist");
MSG_DECL(MSG_SYNTAX_BCDUMP, "syntax: bcdump");
MSG_DECL(MSG_SYNTAX_PLIST, "syntax: plist");
MSG_DECL(MSG_SYNTAX_BC_OP_ARG, "syntax: bc(op,arg)");
MSG_DECL(MSG_STRING_POOL_FULL, "string pool full");
MSG_DECL(MSG_BCX_RANGE, "bcx range");
MSG_DECL(MSG_LOAD_FAILED, "load failed");
MSG_DECL(MSG_RUN_LOAD_FAILED, "run load failed");
MSG_DECL(MSG_NO_BYTECODE_LOADED, "no bytecode loaded");
MSG_DECL(MSG_SYNTAX_BCSTR, "syntax: bcstr(\"...\")");
MSG_DECL(MSG_UNO_TERMINAL, "UNO Terminal");
MSG_DECL(MSG_115200_BAUD, "115200 baud");
MSG_DECL(MSG_VM_DONE, "vm done");
MSG_DECL(MSG_VM_END, "vm end");
MSG_DECL(MSG_BC_CLEARED, "bc cleared");
MSG_DECL(MSG_EEPROM_ERASED, "eeprom erased");
MSG_DECL(MSG_EMPTY, "(empty)");
MSG_DECL(MSG_PROGRAMS_0, "programs:0");
MSG_DECL(MSG_VM_RUNNING, "vm running");
MSG_DECL(MSG_VM_STOPPED, "vm stopped");
MSG_DECL(MSG_OK, "ok");
MSG_DECL(MSG_HIGH_TXT, "HIGH");
MSG_DECL(MSG_LOW_TXT, "LOW");
MSG_DECL(MSG_BYTECODE_READY, "bytecode ready");
MSG_DECL(MSG_AUTORUN_SET, "autorun saved");
MSG_DECL(MSG_AUTORUN_BAD_INDEX, "autorun: bad program index");
MSG_DECL(MSG_AUTORUN_FAIL, "autorun save failed");
MSG_DECL(MSG_SYNTAX_AUTORUN, "syntax: autorun | autorun off | autorun on [n]");
MSG_DECL(MSG_CMDS_HELP, "cmds: bcsave bclist bcdump run load autorun");
MSG_DECL(MSG_SYNTAX_SUM, "syntax: sum");
MSG_DECL(MSG_SYNTAX_MUL, "syntax: mul");
MSG_DECL(MSG_SYNTAX_SUB, "syntax: sub");
MSG_DECL(MSG_SYNTAX_DIV, "syntax: div");

static char flashMsgBuf[MAX_LINE_CHARS + 1];

/** Append decimal u32 to buf (buf must be a valid C string). Avoids snprintf/printf on AVR. */
static void catU32(char* buf, uint32_t v) {
  char t[12];
  ultoa((unsigned long)v, t, 10);
  strcat(buf, t);
}

static void commitErrorText_P(PGM_P msg);
static void commitGreenText_P(PGM_P msg);
static void commitWarnText_P(PGM_P msg);
static void commitTwoGreenTextSecond_P(const char* first, PGM_P second);

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

static void serialOutLine(const char* s) {
  Serial.println(s);
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
  serialOutLine(chunk);

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
    if (idx >= NUM_VARS) {
      return false;
    }
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

static bool strPrefCi_P(const char* s, PGM_P pref) {
  char k = (char)pgm_read_byte(pref);
  while (k != '\0') {
    if (tolower((unsigned char)*s) != tolower((unsigned char)k)) {
      return false;
    }
    s++;
    pref++;
    k = (char)pgm_read_byte(pref);
  }
  return true;
}

/** Palabra clave seguida de fin, espacio o '(' (evita que "printx" sea print). */
static bool keywordAtP(const char* p, PGM_P kw, uint8_t kwLen) {
  if (!strPrefCi_P(p, kw)) {
    return false;
  }
  char c = p[kwLen];
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
  if (!keywordAtP(p, CMD_CONCAT, CMD_CONCAT_LEN)) {
    return false;
  }
  p += CMD_CONCAT_LEN;
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
      ltoa(v, numBuf, 10);
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
      ltoa(v, numBuf, 10);
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
  if (keywordAtP(p, CMD_HELP, CMD_HELP_LEN) || keywordAtP(p, CMD_PRINT, CMD_PRINT_LEN) || keywordAtP(p, CMD_SUM, CMD_SUM_LEN)
      || keywordAtP(p, CMD_MUL, CMD_MUL_LEN) || keywordAtP(p, CMD_SUB, CMD_SUB_LEN) || keywordAtP(p, CMD_DIV, CMD_DIV_LEN)
      || keywordAtP(p, CMD_CONCAT, CMD_CONCAT_LEN) || keywordAtP(p, CMD_OUT, CMD_OUT_LEN) || keywordAtP(p, CMD_IN, CMD_IN_LEN)
      || keywordAtP(p, CMD_HIGH, CMD_HIGH_LEN) || keywordAtP(p, CMD_LOW, CMD_LOW_LEN)       || keywordAtP(p, CMD_RESTART, CMD_RESTART_LEN)
      || keywordAtP(p, CMD_CLEAR, CMD_CLEAR_LEN) || keywordAtP(p, CMD_RUN, CMD_RUN_LEN) || keywordAtP(p, CMD_STOP, CMD_STOP_LEN)
      || keywordAtP(p, CMD_LOAD, CMD_LOAD_LEN) || keywordAtP(p, CMD_BC, CMD_BC_LEN) || keywordAtP(p, CMD_BCX, CMD_BCX_LEN)
      || keywordAtP(p, CMD_BCSTR, CMD_BCSTR_LEN) || keywordAtP(p, CMD_BCCLR, CMD_BCCLR_LEN)
      || keywordAtP(p, CMD_BCSAVE, CMD_BCSAVE_LEN) || keywordAtP(p, CMD_BCLIST, CMD_BCLIST_LEN)
      || keywordAtP(p, CMD_BCDUMP, CMD_BCDUMP_LEN) || keywordAtP(p, CMD_PLIST, CMD_PLIST_LEN)
      || keywordAtP(p, CMD_EPERASE, CMD_EPERASE_LEN)
      || keywordAtP(p, CMD_AUTORUN, CMD_AUTORUN_LEN)) {
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
  return strcmp_P(w, CMD_HELP) == 0 || strcmp_P(w, CMD_PRINT) == 0 || strcmp_P(w, CMD_SUM) == 0
         || strcmp_P(w, CMD_MUL) == 0 || strcmp_P(w, CMD_SUB) == 0 || strcmp_P(w, CMD_DIV) == 0
         || strcmp_P(w, CMD_CONCAT) == 0 || strcmp_P(w, CMD_OUT) == 0 || strcmp_P(w, CMD_IN) == 0
         || strcmp_P(w, CMD_HIGH) == 0 || strcmp_P(w, CMD_LOW) == 0 || strcmp_P(w, CMD_RESTART) == 0
         || strcmp_P(w, CMD_CLEAR) == 0 || strcmp_P(w, CMD_RUN) == 0 || strcmp_P(w, CMD_STOP) == 0
         || strcmp_P(w, CMD_LOAD) == 0 || strcmp_P(w, CMD_BC) == 0 || strcmp_P(w, CMD_BCX) == 0
         || strcmp_P(w, CMD_BCSTR) == 0 || strcmp_P(w, CMD_BCCLR) == 0
         || strcmp_P(w, CMD_BCSAVE) == 0 || strcmp_P(w, CMD_BCLIST) == 0 || strcmp_P(w, CMD_BCDUMP) == 0
         || strcmp_P(w, CMD_PLIST) == 0
         || strcmp_P(w, CMD_EPERASE) == 0
         || strcmp_P(w, CMD_AUTORUN) == 0;
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

static void clear() {
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

  strncpy_P(flashMsgBuf, MSG_UNO_TERMINAL, MAX_LINE_CHARS);
  flashMsgBuf[MAX_LINE_CHARS] = '\0';
  paintRowAt(0, flashMsgBuf, ILI9341_GREEN);
  serialOutLine(flashMsgBuf);
  histPush(flashMsgBuf);
  strncpy_P(flashMsgBuf, MSG_115200_BAUD, MAX_LINE_CHARS);
  flashMsgBuf[MAX_LINE_CHARS] = '\0';
  paintRowAt(1, flashMsgBuf, ILI9341_GREEN);
  serialOutLine(flashMsgBuf);
  histPush(flashMsgBuf);
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
    serialOutLine(info);
    histPush(info);
  }
  rowCount = 3;
  paintEditingRow();
}

static void restartTerminal() {
  clear();
}

/** Mensaje en TFT + Serial (color configurable). */
static void commitStatusText(const char* msg, uint16_t fg) {
  serialOutLine(msg);
  uint8_t maxR = termMaxLines();
  if (rowCount >= maxR) {
    tft.fillScreen(ILI9341_BLACK);
    paintRowAt(0, msg, fg);
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
    paintRowAt(2, msg, fg);
    rowCount = 3;
    histPush(msg);
    paintEditingRow();
    return;
  }
  paintRowAt(rowCount, msg, fg);
  histPush(msg);
  rowCount++;
  paintEditingRow();
}

static void commitErrorText(const char* msg) {
  commitStatusText(msg, ILI9341_RED);
}

static void commitErrorText_P(PGM_P msg) {
  strncpy_P(flashMsgBuf, msg, MAX_LINE_CHARS);
  flashMsgBuf[MAX_LINE_CHARS] = '\0';
  commitErrorText(flashMsgBuf);
}

static void commitWarnText_P(PGM_P msg) {
  strncpy_P(flashMsgBuf, msg, MAX_LINE_CHARS);
  flashMsgBuf[MAX_LINE_CHARS] = '\0';
  commitStatusText(flashMsgBuf, ILI9341_YELLOW);
}

static void commitGreenText_P(PGM_P msg) {
  strncpy_P(flashMsgBuf, msg, MAX_LINE_CHARS);
  flashMsgBuf[MAX_LINE_CHARS] = '\0';
  commitGreenText(flashMsgBuf);
}

static void commitTwoGreenTextSecond_P(const char* first, PGM_P second) {
  strncpy_P(flashMsgBuf, second, MAX_LINE_CHARS);
  flashMsgBuf[MAX_LINE_CHARS] = '\0';
  commitTwoGreenText(first, flashMsgBuf);
}

/** Una línea de salida en verde (misma lógica que una línea confirmada en el terminal). */
static void commitGreenText(const char* tmp) {
  serialOutLine(tmp);
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
  serialOutLine(first);
  serialOutLine(second);
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

/** Como parseTwoLongs(), pero:
 *  - no requiere que termine la línea
 *  - devuelve el puntero donde queda tras ')'
 *  - permite que el llamador valide el resto (por ejemplo, que sea '\0')
 */
static bool parseTwoLongsAndAdvance(const char* p, const char** outEnd, long* a, long* b) {
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
  *outEnd = q;
  return true;
}

enum : uint8_t {
  OP_NOP = 0,
  OP_HIGH = 1,
  OP_LOW = 2,
  // OP_SLEEP + PT_NUM6: ms 0–511 | + PT_STRID: arg×100 ms (0–6300) | + PT_NONE: arg s (0–63)
  OP_SLEEP = 3,
  OP_GOTO = 4,  // arg is instruction index
  OP_PRINT = 5, // print line (flush)
  OP_END = 6,
  OP_PUT = 7    // append to print buffer (no newline)
};

enum : uint8_t {
  PT_NONE = 0,
  PT_NUM6 = 1,
  PT_STRID = 2
};

// Bit layout (16-bit instruction):
// [15..13] OP_SLEEP high bits of ms (else 0)
// [12..11] p0 type
// [10..9 ] param count
// [8 ..6 ] opcode
// [5 ..0 ] arg0 low 6 bits (OP_SLEEP: low 6 bits of ms)
static uint16_t bcPack(uint8_t op, uint8_t pCount, uint8_t p0Type, uint8_t arg) {
  return (uint16_t)(((uint16_t)(p0Type & 0x03) << 11)
                    | ((uint16_t)(pCount & 0x03) << 9)
                    | ((uint16_t)(op & 0x07) << 6)
                    | (uint16_t)(arg & 0x3F));
}

/** OP_SLEEP: ms 0–511; upper 3 bits in [15..13], lower 6 in [5..0]. */
static uint16_t bcPackSleepMs(uint16_t ms) {
  if (ms > 511) {
    ms = 511;
  }
  uint8_t low = (uint8_t)(ms & 0x3F);
  uint8_t hi = (uint8_t)((ms >> 6) & 0x07);
  uint16_t w = bcPack(OP_SLEEP, 1, PT_NUM6, low);
  return (uint16_t)(w | ((uint16_t)hi << 13));
}

static uint16_t bcSleepMsFromWord(uint16_t w) {
  return (uint16_t)((((uint16_t)(w >> 13) & 0x07) << 6) | ((uint16_t)(w & 0x3F)));
}

static uint8_t bcOp(uint16_t w) {
  return (uint8_t)((w >> 6) & 0x07);
}

static uint8_t bcParamCount(uint16_t w) {
  return (uint8_t)((w >> 9) & 0x03);
}

static uint8_t bcParam0Type(uint16_t w) {
  return (uint8_t)((w >> 11) & 0x03);
}

static uint8_t bcArg(uint16_t w) {
  return (uint8_t)(w & 0x3F);
}

static void eepromInitContainer() {
  int addr = EEPROM_ADDR_BASE;
  EEPROM.update(addr++, EEPROM_MAGIC0);
  EEPROM.update(addr++, EEPROM_MAGIC1);
  EEPROM.update(addr++, EEPROM_VER);
  EEPROM.update(addr++, 0); // number of programs
  EEPROM.update(addr++, 0); // autorun off
  EEPROM.update(addr++, 0); // autorun program index
}

static void bcStringReset() {
  bcStrCount = 0;
  bcStrPoolUsed = 0;
  bcStrPool[0] = '\0';
}

static bool bcStringAdd(const char* s, uint8_t* outId) {
  size_t n = strlen(s);
  if (n > 255) {
    return false;
  }
  if (bcStrCount >= BC_STR_MAX) {
    return false;
  }
  if ((uint16_t)(bcStrPoolUsed + n + 1) > BC_STR_POOL_MAX) {
    return false;
  }
  uint8_t id = bcStrCount;
  bcStrOff[id] = (uint8_t)bcStrPoolUsed;
  bcStrLen[id] = (uint8_t)n;
  memcpy(bcStrPool + bcStrPoolUsed, s, n);
  bcStrPoolUsed += (uint16_t)n;
  bcStrPool[bcStrPoolUsed++] = '\0';
  bcStrCount++;
  *outId = id;
  return true;
}

static const char* bcStringById(uint8_t id) {
  if (id >= bcStrCount) {
    return nullptr;
  }
  return bcStrPool + bcStrOff[id];
}

static bool eepromScanMeta(uint8_t* outCount, int* outEndAddr) {
  int addr = EEPROM_ADDR_BASE;
  if (EEPROM.read(addr++) != EEPROM_MAGIC0 || EEPROM.read(addr++) != EEPROM_MAGIC1) {
    return false;
  }
  uint8_t fileVer = EEPROM.read(addr++);
  if (fileVer != EEPROM_VER_MIN && fileVer != EEPROM_VER) {
    return false;
  }
  eepromLayoutVer = fileVer;
  uint8_t count = EEPROM.read(addr++);
  if (count > 31) {
    return false;
  }
  if (fileVer == 2) {
    eepromAutorunEnable = EEPROM.read(addr++);
    eepromAutorunIndex = EEPROM.read(addr++);
  } else {
    eepromAutorunEnable = 0;
    eepromAutorunIndex = 0;
  }

  int eelen = EEPROM.length();
  for (uint8_t i = 0; i < count; i++) {
    if (addr + 4 >= eelen) {
      return false;
    }
    if (EEPROM.read(addr++) != EEPROM_REC_START) {
      return false;
    }
    uint8_t nIns = EEPROM.read(addr++);
    uint8_t nStr = EEPROM.read(addr++);
    if (nIns == 0 || nIns > BC_MAX_INS || nStr > BC_STR_MAX) {
      return false;
    }
    int bytes = (int)nIns * 2;
    if (addr + bytes >= eelen) {
      return false;
    }
    addr += bytes;
    for (uint8_t s = 0; s < nStr; s++) {
      if (addr + 2 >= eelen) {
        return false;
      }
      uint8_t meta = EEPROM.read(addr++);
      uint8_t len = 0;
      uint8_t lenCode = (uint8_t)(meta & 0xC0);
      if (lenCode == ARG_LEN_CODE_1B) {
        len = EEPROM.read(addr++);
      } else {
        return false;
      }
      if (addr + len >= eelen) {
        return false;
      }
      addr += len;
    }
    if (EEPROM.read(addr++) != EEPROM_REC_END) {
      return false;
    }
  }
  *outCount = count;
  *outEndAddr = addr;
  return true;
}

/** v1→v2: inserta 2 bytes de autorun tras el contador; desplaza registros. */
static bool eepromMigrateV1ToV2(void) {
  uint8_t count = 0;
  int endAddr = 0;
  if (!eepromScanMeta(&count, &endAddr)) {
    return false;
  }
  if (eepromLayoutVer != 1) {
    return true;
  }
  int usedLen = endAddr - 4;
  if (usedLen < 0 || 6 + usedLen > EEPROM.length()) {
    return false;
  }
  for (int i = usedLen - 1; i >= 0; i--) {
    EEPROM.update(6 + i, EEPROM.read(4 + i));
  }
  EEPROM.update(2, EEPROM_VER);
  EEPROM.update(4, 0);
  EEPROM.update(5, 0);
  eepromLayoutVer = EEPROM_VER;
  eepromAutorunEnable = 0;
  eepromAutorunIndex = 0;
  return true;
}

static bool eepromWriteAutorun(uint8_t enable, uint8_t progIdx) {
  if (!eepromMigrateV1ToV2()) {
    return false;
  }
  uint8_t count = 0;
  int endAddr = 0;
  if (!eepromScanMeta(&count, &endAddr)) {
    return false;
  }
  if (enable) {
    if (count == 0 || progIdx >= count) {
      return false;
    }
  }
  EEPROM.update(EEPROM_ADDR_BASE + 4, enable ? 1 : 0);
  EEPROM.update(EEPROM_ADDR_BASE + 5, progIdx);
  eepromAutorunEnable = enable ? 1 : 0;
  eepromAutorunIndex = progIdx;
  return true;
}

static bool saveBCToEEPROM() {
  if (progBCCount == 0) {
    return false;
  }
  uint8_t count = 0;
  int endAddr = EEPROM_ADDR_BASE;
  if (!eepromScanMeta(&count, &endAddr)) {
    eepromInitContainer();
    if (!eepromScanMeta(&count, &endAddr)) {
      return false;
    }
  }

  int need = 1 + 1 + 1 + ((int)progBCCount * 2) + 1; // start + nIns + nStr + data + end
  for (uint8_t i = 0; i < bcStrCount; i++) {
    need += 2 + (int)bcStrLen[i]; // meta + len + text
  }
  if (endAddr + need >= EEPROM.length()) {
    return false;
  }

  int addr = endAddr;
  EEPROM.update(addr++, EEPROM_REC_START);
  EEPROM.update(addr++, progBCCount);
  EEPROM.update(addr++, bcStrCount);
  for (uint8_t i = 0; i < progBCCount; i++) {
    EEPROM.update(addr++, (uint8_t)(progBC[i] & 0xFF));
    EEPROM.update(addr++, (uint8_t)(progBC[i] >> 8));
  }
  for (uint8_t i = 0; i < bcStrCount; i++) {
    EEPROM.update(addr++, (uint8_t)(ARG_TYPE_STRING | ARG_LEN_CODE_1B));
    EEPROM.update(addr++, bcStrLen[i]);
    const char* s = bcStringById(i);
    for (uint8_t j = 0; j < bcStrLen[i]; j++) {
      EEPROM.update(addr++, (uint8_t)s[j]);
    }
  }
  EEPROM.update(addr++, EEPROM_REC_END);
  EEPROM.update(EEPROM_ADDR_BASE + 3, (uint8_t)(count + 1));
  return true;
}

static void hexFlushLine(char* line, uint8_t* pos) {
  if (*pos == 0) {
    return;
  }
  if (line[*pos - 1] == ' ') {
    (*pos)--;
  }
  line[*pos] = '\0';
  commitGreenText(line);
  *pos = 0;
}

static void hexAppendByte(char* line, uint8_t* pos, uint8_t b) {
  static const char xd[] = "0123456789abcdef";
  if (*pos + 3 > MAX_LINE_CHARS) {
    hexFlushLine(line, pos);
  }
  line[(*pos)++] = xd[b >> 4];
  line[(*pos)++] = xd[b & 0x0F];
  line[(*pos)++] = ' ';
}

/** Mismo orden de bytes que saveBCToEEPROM() (un registro), en hex por líneas. */
static void dumpBcRecordAsStoredHex() {
  char line[MAX_LINE_CHARS + 1];
  uint8_t pos = 0;
  hexAppendByte(line, &pos, EEPROM_REC_START);
  hexAppendByte(line, &pos, progBCCount);
  hexAppendByte(line, &pos, bcStrCount);
  for (uint8_t i = 0; i < progBCCount; i++) {
    uint16_t w = progBC[i];
    hexAppendByte(line, &pos, (uint8_t)(w & 0xFF));
    hexAppendByte(line, &pos, (uint8_t)(w >> 8));
  }
  for (uint8_t i = 0; i < bcStrCount; i++) {
    hexAppendByte(line, &pos, (uint8_t)(ARG_TYPE_STRING | ARG_LEN_CODE_1B));
    hexAppendByte(line, &pos, bcStrLen[i]);
    const char* s = bcStringById(i);
    for (uint8_t j = 0; j < bcStrLen[i]; j++) {
      hexAppendByte(line, &pos, (uint8_t)s[j]);
    }
  }
  hexAppendByte(line, &pos, EEPROM_REC_END);
  hexFlushLine(line, &pos);
}

static bool loadBCFromEEPROMIndex(uint8_t wantedIndex) {
  uint8_t count = 0;
  int addr = EEPROM_ADDR_BASE;
  if (!eepromScanMeta(&count, &addr)) {
    return false;
  }
  if (wantedIndex >= count) {
    return false;
  }

  addr = EEPROM_ADDR_BASE + eepromHeaderBytes(eepromLayoutVer);
  for (uint8_t i = 0; i < count; i++) {
    if (EEPROM.read(addr++) != EEPROM_REC_START) {
      return false;
    }
    uint8_t nIns = EEPROM.read(addr++);
    uint8_t nStr = EEPROM.read(addr++);
    if (nIns == 0 || nIns > BC_MAX_INS || nStr > BC_STR_MAX) {
      return false;
    }
    if (i == wantedIndex) {
      for (uint8_t j = 0; j < nIns; j++) {
        uint8_t lo = EEPROM.read(addr++);
        uint8_t hi = EEPROM.read(addr++);
        progBC[j] = (uint16_t)lo | ((uint16_t)hi << 8);
      }
      bcStringReset();
      for (uint8_t s = 0; s < nStr; s++) {
        uint8_t meta = EEPROM.read(addr++);
        uint8_t lenCode = (uint8_t)(meta & 0xC0);
        bool isStr = (meta & ARG_TYPE_STRING) != 0;
        if (!isStr || lenCode != ARG_LEN_CODE_1B) {
          return false;
        }
        uint8_t len = EEPROM.read(addr++);
        if ((uint16_t)(bcStrPoolUsed + len + 1) > BC_STR_POOL_MAX || bcStrCount >= BC_STR_MAX) {
          return false;
        }
        uint8_t id = bcStrCount;
        bcStrOff[id] = (uint8_t)bcStrPoolUsed;
        bcStrLen[id] = len;
        for (uint8_t k = 0; k < len; k++) {
          bcStrPool[bcStrPoolUsed++] = (char)EEPROM.read(addr++);
        }
        bcStrPool[bcStrPoolUsed++] = '\0';
        bcStrCount++;
      }
      if (EEPROM.read(addr++) != EEPROM_REC_END) {
        return false;
      }
      progBCCount = nIns;
      progCompiled = true;
      return true;
    }
    addr += (int)nIns * 2;
    for (uint8_t s = 0; s < nStr; s++) {
      uint8_t meta = EEPROM.read(addr++);
      uint8_t lenCode = (uint8_t)(meta & 0xC0);
      uint8_t len = 0;
      if (lenCode == ARG_LEN_CODE_1B) {
        len = EEPROM.read(addr++);
      } else {
        return false;
      }
      addr += len;
    }
    if (EEPROM.read(addr++) != EEPROM_REC_END) {
      return false;
    }
  }
  return false;
}

static void vmStop() {
  vmRunning = false;
  vmPC = 0;
  vmWaitUntilMs = 0;
  vmPrintBufClear();
}

static void vmStart() {
  vmRunning = true;
  vmPC = 0;
  vmWaitUntilMs = 0;
  vmPrintBufClear();
}

static void vmStep() {
  if (!vmRunning) {
    return;
  }
  if (vmPC >= progBCCount) {
    vmStop();
    commitGreenText_P(MSG_VM_DONE);
    return;
  }
  uint32_t now = millis();
  if (vmWaitUntilMs != 0 && (int32_t)(now - vmWaitUntilMs) < 0) {
    return;
  }
  vmWaitUntilMs = 0;

  uint16_t ins = progBC[vmPC];
  uint8_t op = bcOp(ins);
  uint8_t pCount = bcParamCount(ins);
  uint8_t pType = bcParam0Type(ins);
  uint8_t arg = bcArg(ins);

  switch (op) {
    case OP_NOP:
      if (pCount != 0 || pType != PT_NONE) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      vmPC++;
      break;
    case OP_HIGH:
      if (pCount != 1 || pType != PT_NUM6) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      pinMode(arg, OUTPUT);
      digitalWrite(arg, HIGH);
      vmPC++;
      break;
    case OP_LOW:
      if (pCount != 1 || pType != PT_NUM6) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      pinMode(arg, OUTPUT);
      digitalWrite(arg, LOW);
      vmPC++;
      break;
    case OP_SLEEP:
      if (pCount != 1) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      vmPC++;
      if (pType == PT_NUM6) {
        vmWaitUntilMs = now + (uint32_t)bcSleepMsFromWord(ins);
      } else if (pType == PT_STRID) {
        vmWaitUntilMs = now + (uint32_t)arg * 100UL;
      } else if (pType == PT_NONE) {
        vmWaitUntilMs = now + (uint32_t)arg * 1000UL;
      } else {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
      }
      break;
    case OP_GOTO:
      if (pCount != 1 || pType != PT_NUM6) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      if (arg >= progBCCount) {
        vmStop();
        commitErrorText_P(MSG_VM_GOTO_OOR);
      } else {
        vmPC = arg;
      }
      break;
    case OP_PRINT: {
      if (pCount != 1) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      if (pType == PT_NUM6) {
        if (!vmPrintBufAppendNum(arg)) {
          vmStop();
          commitErrorText_P(MSG_PRINT_OVERFLOW);
          return;
        }
      } else if (pType == PT_STRID) {
        const char* s = vmStringById(arg);
        if (s == nullptr) {
          vmStop();
          commitErrorText_P(MSG_STR_ID_OOR);
          return;
        }
        if (!vmPrintBufAppendText(s)) {
          vmStop();
          commitErrorText_P(MSG_PRINT_OVERFLOW);
          return;
        }
      } else {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      if (vmPrintLen == 0) {
        commitGreenText("");
      } else {
        commitGreenText(vmPrintBuf);
        vmPrintBufClear();
      }
      vmPC++;
      break;
    }
    case OP_END:
      if (pCount != 0 || pType != PT_NONE) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      if (vmPrintLen > 0) {
        commitGreenText(vmPrintBuf);
        vmPrintBufClear();
      }
      vmStop();
      commitGreenText_P(MSG_VM_END);
      break;
    case OP_PUT:
      if (pCount != 1) {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      if (pType == PT_NUM6) {
        if (!vmPrintBufAppendNum(arg)) {
          vmStop();
          commitErrorText_P(MSG_PRINT_OVERFLOW);
          break;
        }
      } else if (pType == PT_STRID) {
        const char* s = vmStringById(arg);
        if (s == nullptr) {
          vmStop();
          commitErrorText_P(MSG_STR_ID_OOR);
          break;
        }
        if (!vmPrintBufAppendText(s)) {
          vmStop();
          commitErrorText_P(MSG_PRINT_OVERFLOW);
          break;
        }
      } else {
        vmStop();
        commitErrorText_P(MSG_VM_BAD_PARAMS);
        break;
      }
      vmPC++;
      break;
    default:
      vmStop();
      commitErrorText_P(MSG_VM_BAD_OPCODE);
      break;
  }
}

/** 0=sleep(ms), 1=sleep100, 2=sleeps; solo para parseBcOpToken → bc(sleep…) */
static uint8_t bcSleepParseKind = 0;

static bool parseOptionalIndex(const char* p, bool* hasArg, uint8_t* outIdx) {
  const char* q = p;
  skipWs(&q);
  if (*q == '\0') {
    *hasArg = false;
    *outIdx = 0;
    return true;
  }
  if (*q != '(') {
    return false;
  }
  q++;
  skipWs(&q);
  char* end = nullptr;
  long v = strtol(q, &end, 10);
  if (end == q || v < 0 || v > 31) {
    return false;
  }
  q = end;
  skipWs(&q);
  if (*q != ')') {
    return false;
  }
  q++;
  skipWs(&q);
  if (*q != '\0') {
    return false;
  }
  *hasArg = true;
  *outIdx = (uint8_t)v;
  return true;
}

static bool parseBcOpToken(const char** pp, uint8_t* outOp) {
  bcSleepParseKind = 0;
  const char* p = *pp;
  skipWs(&p);
  if (*p == '\0') {
    return false;
  }

  if (isdigit((unsigned char)*p)) {
    char* end = nullptr;
    long opL = strtol(p, &end, 10);
    if (end == p || opL < 0 || opL > 7) {
      return false;
    }
    *outOp = (uint8_t)opL;
    *pp = end;
    return true;
  }

  char w[10];
  size_t i = 0;
  while (isalpha((unsigned char)*p)) {
    if (i < sizeof(w) - 1) {
      w[i++] = (char)tolower((unsigned char)*p);
    }
    p++;
  }
  w[i] = '\0';
  if (i == 0) {
    return false;
  }

  if (strcmp_P(w, CMD_NOP) == 0) {
    *outOp = OP_NOP;
  } else if (strcmp_P(w, CMD_HIGH) == 0) {
    *outOp = OP_HIGH;
  } else if (strcmp_P(w, CMD_LOW) == 0) {
    *outOp = OP_LOW;
  } else if (strcmp_P(w, CMD_SLEEP100) == 0) {
    *outOp = OP_SLEEP;
    bcSleepParseKind = 1;
  } else if (strcmp_P(w, CMD_SLEEPS) == 0) {
    *outOp = OP_SLEEP;
    bcSleepParseKind = 2;
  } else if (strcmp_P(w, CMD_SLEEP) == 0) {
    *outOp = OP_SLEEP;
    bcSleepParseKind = 0;
  } else if (strcmp_P(w, CMD_GOTO) == 0) {
    *outOp = OP_GOTO;
  } else if (strcmp_P(w, CMD_PRINT) == 0 || strcmp_P(w, CMD_END) == 0) {
    // "print" compila como OP_PRINT; "end" como OP_END.
    *outOp = (strcmp_P(w, CMD_END) == 0) ? OP_END : OP_PRINT;
  } else if (strcmp_P(w, CMD_PUT) == 0) {
    *outOp = OP_PUT;
  } else {
    return false;
  }

  *pp = p;
  return true;
}

static const char* vmStringById(uint8_t id) { return bcStringById(id); }

static void vmPrintBufClear() {
  vmPrintLen = 0;
  vmPrintBuf[0] = '\0';
}

static bool vmPrintBufAppendText(const char* s) {
  size_t n = strlen(s);
  if ((size_t)vmPrintLen + n > MAX_LINE_CHARS) {
    return false;
  }
  memcpy(vmPrintBuf + vmPrintLen, s, n);
  vmPrintLen = (uint8_t)(vmPrintLen + n);
  vmPrintBuf[vmPrintLen] = '\0';
  return true;
}

static bool vmPrintBufAppendNum(uint8_t v) {
  char b[12];
  ultoa((unsigned long)(unsigned)v, b, 10);
  return vmPrintBufAppendText(b);
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

  if (keywordAtP(p, CMD_BCCLR, CMD_BCCLR_LEN)) {
    p += CMD_BCCLR_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_BCCLR);
      return true;
    }
    vmStop();
    progBCCount = 0;
    progCompiled = false;
    progNeedsSave = false;
    bcStringReset();
    commitGreenText_P(MSG_BC_CLEARED);
    return true;
  }

  if (keywordAtP(p, CMD_AUTORUN, CMD_AUTORUN_LEN)) {
    p += CMD_AUTORUN_LEN;
    skipWs(&p);
    if (*p == '\0') {
      uint8_t c = 0;
      int e = 0;
      if (!eepromScanMeta(&c, &e)) {
        commitGreenText_P(MSG_AUTORUN_FAIL);
        return true;
      }
      strcpy(out, "autorun ");
      if (eepromAutorunEnable) {
        strcat(out, "on p#");
        catU32(out, (uint32_t)eepromAutorunIndex);
      } else {
        strcat(out, "off");
      }
      commitGreenText(out);
      return true;
    }
    if (keywordAtP(p, CMD_OFF, CMD_OFF_LEN)) {
      p += CMD_OFF_LEN;
      skipWs(&p);
      if (*p != '\0') {
        commitErrorText_P(MSG_SYNTAX_AUTORUN);
        return true;
      }
      if (!eepromWriteAutorun(0, 0)) {
        commitErrorText_P(MSG_AUTORUN_FAIL);
        return true;
      }
      commitGreenText_P(MSG_AUTORUN_SET);
      return true;
    }
    if (keywordAtP(p, CMD_ON, CMD_ON_LEN)) {
      p += CMD_ON_LEN;
      skipWs(&p);
      uint8_t idx = 0;
      if (*p != '\0') {
        char* end = nullptr;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 31) {
          commitErrorText_P(MSG_SYNTAX_AUTORUN);
          return true;
        }
        p = end;
        skipWs(&p);
        if (*p != '\0') {
          commitErrorText_P(MSG_SYNTAX_AUTORUN);
          return true;
        }
        idx = (uint8_t)v;
      }
      uint8_t c = 0;
      int e = 0;
      if (!eepromScanMeta(&c, &e)) {
        commitErrorText_P(MSG_AUTORUN_FAIL);
        return true;
      }
      if (c == 0 || idx >= c) {
        commitErrorText_P(MSG_AUTORUN_BAD_INDEX);
        return true;
      }
      if (!eepromWriteAutorun(1, idx)) {
        commitErrorText_P(MSG_AUTORUN_FAIL);
        return true;
      }
      commitGreenText_P(MSG_AUTORUN_SET);
      return true;
    }
    commitErrorText_P(MSG_SYNTAX_AUTORUN);
    return true;
  }

  if (keywordAtP(p, CMD_EPERASE, CMD_EPERASE_LEN)) {
    p += CMD_EPERASE_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_EPERASE);
      return true;
    }
    vmStop();
    eepromInitContainer();
    progBCCount = 0;
    progCompiled = false;
    progNeedsSave = false;
    bcStringReset();
    commitGreenText_P(MSG_EEPROM_ERASED);
    return true;
  }

  if (keywordAtP(p, CMD_BCSAVE, CMD_BCSAVE_LEN)) {
    p += CMD_BCSAVE_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_BCSAVE);
      return true;
    }
    if (!progCompiled || progBCCount == 0) {
      commitErrorText_P(MSG_NO_BYTECODE);
      return true;
    }
    if (!saveBCToEEPROM()) {
      commitErrorText_P(MSG_SAVE_FAILED_FULL);
      return true;
    }
    uint8_t n = 0;
    int endAddr = 0;
    if (!eepromScanMeta(&n, &endAddr) || n == 0) {
      commitErrorText_P(MSG_SAVE_VERIFY_FAILED);
      return true;
    }
    progNeedsSave = false;
    strcpy(out, "saved as #");
    catU32(out, (uint32_t)(n - 1));
    commitGreenText(out);
    return true;
  }

  if (keywordAtP(p, CMD_BCLIST, CMD_BCLIST_LEN)) {
    p += CMD_BCLIST_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_BCLIST);
      return true;
    }
    if (!progCompiled || progBCCount == 0) {
      commitGreenText_P(MSG_EMPTY);
      return true;
    }
    for (uint8_t i = 0; i < progBCCount; i++) {
      uint8_t op = bcOp(progBC[i]);
      uint8_t arg = bcArg(progBC[i]);
      uint8_t pc = bcParamCount(progBC[i]);
      uint8_t pt = bcParam0Type(progBC[i]);
      out[0] = '\0';
      catU32(out, (uint32_t)i);
      strcat(out, ":op");
      catU32(out, (uint32_t)op);
      strcat(out, " c");
      catU32(out, (uint32_t)pc);
      strcat(out, " t");
      catU32(out, (uint32_t)pt);
      strcat(out, " a");
      if (op == OP_SLEEP) {
        if (pt == PT_NUM6) {
          catU32(out, (uint32_t)bcSleepMsFromWord(progBC[i]));
        } else if (pt == PT_STRID) {
          catU32(out, (uint32_t)arg);
          strcat(out, "*100ms");
        } else if (pt == PT_NONE) {
          catU32(out, (uint32_t)arg);
          strcat(out, "s");
        } else {
          catU32(out, (uint32_t)arg);
        }
      } else {
        catU32(out, (uint32_t)arg);
      }
      commitGreenText(out);
    }
    for (uint8_t i = 0; i < bcStrCount; i++) {
      strcpy(out, "str#");
      catU32(out, (uint32_t)i);
      strcat(out, ":\"");
      strcat(out, bcStringById(i));
      strcat(out, "\"");
      commitGreenText(out);
    }
    return true;
  }

  if (keywordAtP(p, CMD_BCDUMP, CMD_BCDUMP_LEN)) {
    p += CMD_BCDUMP_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_BCDUMP);
      return true;
    }
    if (!progCompiled || progBCCount == 0) {
      commitGreenText_P(MSG_EMPTY);
      return true;
    }
    dumpBcRecordAsStoredHex();
    return true;
  }

  if (keywordAtP(p, CMD_PLIST, CMD_PLIST_LEN)) {
    p += CMD_PLIST_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_PLIST);
      return true;
    }
    uint8_t count = 0;
    int endAddr = 0;
    if (!eepromScanMeta(&count, &endAddr)) {
      commitGreenText_P(MSG_PROGRAMS_0);
      return true;
    }
    strcpy(out, "programs:");
    catU32(out, (uint32_t)count);
    commitGreenText(out);
    return true;
  }

  if (keywordAtP(p, CMD_BC, CMD_BC_LEN)) {
    p += CMD_BC_LEN;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText_P(MSG_SYNTAX_BC_OP_ARG);
      return true;
    }
    p++;
    uint8_t op = 0;
    if (!parseBcOpToken(&p, &op)) {
      commitErrorText_P(MSG_SYNTAX_BC_OP_ARG);
      return true;
    }
    skipWs(&p);
    if (*p != ',') {
      commitErrorText_P(MSG_SYNTAX_BC_OP_ARG);
      return true;
    }
    p++;
    skipWs(&p);
    char* end = nullptr;
    long arg = strtol(p, &end, 10);
    long argMax = 63L;
    if (op == OP_SLEEP) {
      argMax = (bcSleepParseKind == 0) ? 511L : 63L;
    }
    if (end == p || arg < 0 || arg > argMax) {
      commitErrorText_P(MSG_SYNTAX_BC_OP_ARG);
      return true;
    }
    p = end;
    skipWs(&p);
    if (*p != ')') {
      commitErrorText_P(MSG_SYNTAX_BC_OP_ARG);
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_BC_OP_ARG);
      return true;
    }
    if (progBCCount >= BC_MAX_INS) {
      commitErrorText_P(MSG_BC_FULL);
      return true;
    }
    uint8_t pCount = (op == OP_NOP || op == OP_END) ? 0 : 1;
    uint8_t pType = (pCount == 0) ? PT_NONE : PT_NUM6;
    if (op == OP_SLEEP) {
      if (bcSleepParseKind == 1) {
        progBC[progBCCount++] = bcPack(OP_SLEEP, 1, PT_STRID, (uint8_t)arg);
      } else if (bcSleepParseKind == 2) {
        progBC[progBCCount++] = bcPack(OP_SLEEP, 1, PT_NONE, (uint8_t)arg);
      } else {
        progBC[progBCCount++] = bcPackSleepMs((uint16_t)arg);
      }
    } else {
      progBC[progBCCount++] = bcPack(op, pCount, pType, (uint8_t)arg);
    }
    progCompiled = true;
    progNeedsSave = true;
    strcpy(out, "bc ");
    catU32(out, (uint32_t)progBCCount);
    commitGreenText(out);
    return true;
  }

  if (keywordAtP(p, CMD_BCSTR, CMD_BCSTR_LEN)) {
    p += CMD_BCSTR_LEN;
    skipWs(&p);
    char s[BC_STR_POOL_MAX + 1];
    bool hasParen = false;
    if (*p == '(') {
      hasParen = true;
      p++;
      skipWs(&p);
    }
    if (!parseStringLiteral(&p, s, sizeof(s))) {
      commitErrorText_P(MSG_SYNTAX_BCSTR);
      return true;
    }
    if (hasParen) {
      skipWs(&p);
      if (*p != ')') {
        commitErrorText_P(MSG_SYNTAX_BCSTR);
        return true;
      }
      p++;
    }
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_BCSTR);
      return true;
    }
    uint8_t id = 0;
    if (!bcStringAdd(s, &id)) {
      commitErrorText_P(MSG_STRING_POOL_FULL);
      return true;
    }
    progNeedsSave = true;
    strcpy(out, "str#");
    catU32(out, (uint32_t)id);
    commitGreenText(out);
    return true;
  }

  if (keywordAtP(p, CMD_BCX, CMD_BCX_LEN)) {
    p += CMD_BCX_LEN;
    const char* q = p;
    skipWs(&q);
    if (*q != '(') {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    q++;
    skipWs(&q);
    char* end = nullptr;
    long op = strtol(q, &end, 10);
    if (end == q) {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    q = end;
    skipWs(&q);
    if (*q != ',') {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    q++;
    skipWs(&q);
    long t = strtol(q, &end, 10);
    if (end == q) {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    q = end;
    skipWs(&q);
    if (*q != ',') {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    q++;
    skipWs(&q);
    long a = strtol(q, &end, 10);
    if (end == q) {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    q = end;
    skipWs(&q);
    if (*q != ')') {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    q++;
    skipWs(&q);
    if (*q != '\0') {
      commitErrorText_P(MSG_SYNTAX_BCX);
      return true;
    }
    if (op < 0 || op > 7 || t < 0 || t > 2 || a < 0) {
      commitErrorText_P(MSG_BCX_RANGE);
      return true;
    }
    if ((uint8_t)op == OP_SLEEP) {
      if (t == PT_NUM6) {
        if (a > 511) {
          commitErrorText_P(MSG_BCX_RANGE);
          return true;
        }
      } else if (t == PT_STRID || t == PT_NONE) {
        if (a > 63) {
          commitErrorText_P(MSG_BCX_RANGE);
          return true;
        }
      } else {
        commitErrorText_P(MSG_BCX_RANGE);
        return true;
      }
    } else if (a > 63) {
      commitErrorText_P(MSG_BCX_RANGE);
      return true;
    }
    uint8_t pCount = ((uint8_t)t == PT_NONE) ? 0 : 1;
    if ((uint8_t)op == OP_NOP || (uint8_t)op == OP_END) {
      pCount = 0;
    }
    if ((uint8_t)op == OP_SLEEP && (uint8_t)t == PT_NONE) {
      pCount = 1;
    }
    if (progBCCount >= BC_MAX_INS) {
      commitErrorText_P(MSG_BC_FULL);
      return true;
    }
    if ((uint8_t)op == OP_SLEEP) {
      if (t == PT_NUM6) {
        progBC[progBCCount++] = bcPackSleepMs((uint16_t)a);
      } else if (t == PT_STRID) {
        progBC[progBCCount++] = bcPack(OP_SLEEP, 1, PT_STRID, (uint8_t)(a & 0x3F));
      } else {
        progBC[progBCCount++] = bcPack(OP_SLEEP, 1, PT_NONE, (uint8_t)(a & 0x3F));
      }
    } else {
      progBC[progBCCount++] = bcPack((uint8_t)op, pCount, (uint8_t)t, (uint8_t)a);
    }
    progCompiled = true;
    progNeedsSave = true;
    strcpy(out, "bcx ");
    catU32(out, (uint32_t)progBCCount);
    commitGreenText(out);
    return true;
  }

  if (keywordAtP(p, CMD_LOAD, CMD_LOAD_LEN)) {
    p += CMD_LOAD_LEN;
    bool hasArg = false;
    uint8_t idx = 0;
    if (!parseOptionalIndex(p, &hasArg, &idx)) {
      commitErrorText_P(MSG_SYNTAX_LOAD);
      return true;
    }
    if (!hasArg) {
      idx = 0;
    }
    if (!loadBCFromEEPROMIndex(idx)) {
      commitErrorText_P(MSG_LOAD_FAILED);
      return true;
    }
    progNeedsSave = false;
    strcpy(out, "loaded p");
    catU32(out, (uint32_t)idx);
    strcat(out, " ");
    catU32(out, (uint32_t)progBCCount);
    strcat(out, " ins");
    commitGreenText(out);
    return true;
  }

  if (keywordAtP(p, CMD_RUN, CMD_RUN_LEN)) {
    p += CMD_RUN_LEN;
    bool hasArg = false;
    uint8_t idx = 0;
    if (!parseOptionalIndex(p, &hasArg, &idx)) {
      commitErrorText_P(MSG_SYNTAX_RUN);
      return true;
    }
    if (hasArg) {
      if (!loadBCFromEEPROMIndex(idx)) {
        commitErrorText_P(MSG_RUN_LOAD_FAILED);
        return true;
      }
      progNeedsSave = false;
    }
    if (!progCompiled || progBCCount == 0) {
      commitErrorText_P(MSG_NO_BYTECODE_LOADED);
      return true;
    }
    vmStart();
    commitGreenText_P(MSG_VM_RUNNING);
    return true;
  }

  if (keywordAtP(p, CMD_STOP, CMD_STOP_LEN)) {
    p += CMD_STOP_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_STOP);
      return true;
    }
    vmStop();
    commitGreenText_P(MSG_VM_STOPPED);
    return true;
  }

  // restart: clears TFT and re-initializes terminal state
  if (keywordAtP(p, CMD_RESTART, CMD_RESTART_LEN)) {
    p += CMD_RESTART_LEN;
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
      commitErrorText_P(MSG_SYNTAX_RESTART);
      return true;
    }
    restartTerminal();
    return true;
  }

  // clear: clears TFT and re-initializes terminal state
  if (keywordAtP(p, CMD_CLEAR, CMD_CLEAR_LEN)) {
    p += CMD_CLEAR_LEN;
    skipWs(&p);
    // allow optional parentheses: clear()
    if (*p == '(') {
      p++;
      skipWs(&p);
      if (*p == ')') {
        p++;
      }
      skipWs(&p);
    }
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_CLEAR);
      return true;
    }
    clear();
    return true;
  }

  // Digital pin control:
  //   out(pin)  -> pinMode(pin, OUTPUT)
  //   in(pin)   -> pinMode(pin, INPUT)
  //   high(pin) -> digitalWrite(pin, HIGH)
  //   low(pin)  -> digitalWrite(pin, LOW)
  if (keywordAtP(p, CMD_OUT, CMD_OUT_LEN)) {
    p += CMD_OUT_LEN;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText_P(MSG_SYNTAX_OUT);
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText_P(MSG_SYNTAX_OUT);
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText_P(MSG_SYNTAX_OUT);
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_OUT);
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText_P(MSG_PIN_OOR);
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText_P(MSG_PIN_RESERVED);
      return true;
    }
    pinMode((uint8_t)pin, OUTPUT);
    commitTwoGreenTextSecond_P(curLine, MSG_OK);
    return true;
  }

  if (keywordAtP(p, CMD_IN, CMD_IN_LEN)) {
    p += CMD_IN_LEN;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText_P(MSG_SYNTAX_IN);
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText_P(MSG_SYNTAX_IN);
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText_P(MSG_SYNTAX_IN);
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_IN);
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText_P(MSG_PIN_OOR);
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText_P(MSG_PIN_RESERVED);
      return true;
    }
    pinMode((uint8_t)pin, INPUT);
    commitTwoGreenTextSecond_P(curLine, MSG_OK);
    return true;
  }

  if (keywordAtP(p, CMD_HIGH, CMD_HIGH_LEN)) {
    p += CMD_HIGH_LEN;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText_P(MSG_SYNTAX_HIGH);
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText_P(MSG_SYNTAX_HIGH);
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText_P(MSG_SYNTAX_HIGH);
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_HIGH);
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText_P(MSG_PIN_OOR);
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText_P(MSG_PIN_RESERVED);
      return true;
    }
    digitalWrite((uint8_t)pin, HIGH);
    commitTwoGreenTextSecond_P(curLine, MSG_HIGH_TXT);
    return true;
  }

  if (keywordAtP(p, CMD_LOW, CMD_LOW_LEN)) {
    p += CMD_LOW_LEN;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText_P(MSG_SYNTAX_LOW);
      return true;
    }
    p++;
    long pin = 0;
    if (!parseValueToken(&p, &pin)) {
      commitErrorText_P(MSG_SYNTAX_LOW);
      return true;
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText_P(MSG_SYNTAX_LOW);
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_LOW);
      return true;
    }
    if (pin < 0 || pin > 19) {
      commitErrorText_P(MSG_PIN_OOR);
      return true;
    }
    if (isTftReservedPin((uint8_t)pin)) {
      commitErrorText_P(MSG_PIN_RESERVED);
      return true;
    }
    digitalWrite((uint8_t)pin, LOW);
    commitTwoGreenTextSecond_P(curLine, MSG_LOW_TXT);
    return true;
  }

  // Assignment: A=123, V12=123, A$="hi", S0$="hi"
  {
    const char* q = p;
    skipWs(&q);
    if (keywordAtP(q, CMD_LET, CMD_LET_LEN)) {
      q += CMD_LET_LEN;
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
        commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
        return true;
      }
      q = end;
      if (*q != '$') {
        commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
        return true;
      }
      q++; // consume '$'
      if (idxL < 0 || idxL >= (long)STR_VARS) {
        commitErrorText_P(MSG_STRING_VAR_OOR);
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
        commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
        return true;
      }
      q = end;
      if (idxL < 0 || idxL >= (long)NUM_VARS) {
        commitErrorText_P(MSG_VAR_OOR);
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
            commitErrorText_P(MSG_STRING_VAR_OOR);
            return true;
          }
          idxStr = tmp;
          isStringTarget = true;
          q += 2; // X$
        } else {
          uint8_t tmp = (uint8_t)(name - 'A');
          if (tmp >= NUM_VARS) {
            commitErrorText_P(MSG_VAR_OOR);
            return true;
          }
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
          long a = 0;
          long b = 0;

          skipWs(&q);
          // Permite RHS: sum(a,b), mul(a,b), sub(a,b), div(a,b)
          if (keywordAtP(q, CMD_SUM, CMD_SUM_LEN)) {
            const char* end = nullptr;
            q += CMD_SUM_LEN;
            if (!parseTwoLongsAndAdvance(q, &end, &a, &b)) {
              commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            if (*end != '\0') {
              commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            val = a + b;
            q = end;
          } else if (keywordAtP(q, CMD_MUL, CMD_MUL_LEN)) {
            const char* end = nullptr;
            q += CMD_MUL_LEN;
            if (!parseTwoLongsAndAdvance(q, &end, &a, &b)) {
              commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            if (*end != '\0') {
              commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            val = a * b;
            q = end;
          } else if (keywordAtP(q, CMD_SUB, CMD_SUB_LEN)) {
            const char* end = nullptr;
            q += CMD_SUB_LEN;
            if (!parseTwoLongsAndAdvance(q, &end, &a, &b)) {
              commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            if (*end != '\0') {
              commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            val = a - b;
            q = end;
          } else if (keywordAtP(q, CMD_DIV, CMD_DIV_LEN)) {
            const char* end = nullptr;
            q += CMD_DIV_LEN;
            if (!parseTwoLongsAndAdvance(q, &end, &a, &b)) {
              commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            if (*end != '\0') {
              commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            if (b == 0) {
              commitErrorText_P(MSG_DIV_ZERO);
              return true;
            }
            val = a / b;
            q = end;
          } else {
            // RHS simple: número literal o VAR
            if (!parseValueToken(&q, &val)) {
              commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
            skipWs(&q);
            if (*q != '\0') {
              commitErrorText_P(MSG_SYNTAX_ASSIGNMENT);
              return true;
            }
          }

          varsMask |= (1UL << idxNum);
          vars[idxNum] = val;
          ltoa(val, out, 10);
          commitTwoGreenText(curLine, out);
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
          } else if (keywordAtP(q, CMD_CONCAT, CMD_CONCAT_LEN)) {
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
              commitErrorText_P(MSG_UNDEFINED_STR_VAR);
            } else {
              commitErrorText_P(MSG_SYNTAX_STR_ASSIGN);
            }
            return true;
          }

          skipWs(&q);
          if (*q != '\0') {
            commitErrorText_P(MSG_SYNTAX_STR_ASSIGN);
            return true;
          }

          strncpy(strVars[idxStr], tmpStr, STR_MAX_LEN);
          strVars[idxStr][STR_MAX_LEN] = '\0';
          strVarsMask |= (1U << idxStr);

          commitTwoGreenText(curLine, tmpStr);
          return true;
        }
      }
    }
  }

  if (keywordAtP(p, CMD_HELP, CMD_HELP_LEN)) {
    p += CMD_HELP_LEN;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_HELP);
      return true;
    }
    strncpy_P(out, MSG_CMDS_HELP, MAX_LINE_CHARS);
    out[MAX_LINE_CHARS] = '\0';
    commitTwoGreenText(curLine, out);
    return true;
  }

  if (keywordAtP(p, CMD_CONCAT, CMD_CONCAT_LEN)) {
    char sOut[MAX_LINE_CHARS + 1];
    const char* q = p;
    if (!parseConcatCall(&q, sOut, sizeof(sOut))) {
      if (parseLastUndefinedVar) {
        commitErrorText_P(MSG_UNDEFINED_VAR);
      } else if (parseLastUndefinedStrVar) {
        commitErrorText_P(MSG_UNDEFINED_STR_VAR);
      } else {
        commitErrorText_P(MSG_SYNTAX_CONCAT);
      }
      return true;
    }
    commitTwoGreenText(curLine, sOut);
    return true;
  }

  if (keywordAtP(p, CMD_PRINT, CMD_PRINT_LEN)) {
    p += CMD_PRINT_LEN;
    skipWs(&p);
    if (*p != '(') {
      commitErrorText_P(MSG_SYNTAX_PRINT);
      return true;
    }
    p++;
    skipWs(&p);
    if (*p == '"') {
      const char* q = p;
      if (!parseStringLiteral(&q, out, sizeof(out))) {
        commitErrorText_P(MSG_SYNTAX_PRINT);
        return true;
      }
      p = q;
    } else if (keywordAtP(p, CMD_CONCAT, CMD_CONCAT_LEN)) {
      const char* q = p;
      if (!parseConcatCall(&q, out, sizeof(out))) {
        if (parseLastUndefinedVar) {
          commitErrorText_P(MSG_UNDEFINED_VAR);
        } else if (parseLastUndefinedStrVar) {
          commitErrorText_P(MSG_UNDEFINED_STR_VAR);
        } else {
          commitErrorText_P(MSG_SYNTAX_CONCAT);
        }
        return true;
      }
      p = q;
    } else if (parseStringVarRef(&p, out, sizeof(out))) {
      // parsed as string variable into out
    } else if (parseLastUndefinedStrVar) {
      commitErrorText_P(MSG_UNDEFINED_STR_VAR);
      return true;
    } else {
      long val = 0;
      if (!parseValueToken(&p, &val)) {
        if (parseLastUndefinedVar) {
          commitErrorText_P(MSG_UNDEFINED_VAR);
        } else {
          commitErrorText_P(MSG_SYNTAX_PRINT);
        }
        return true;
      }
      ltoa(val, out, 10);
    }
    skipWs(&p);
    if (*p != ')') {
      commitErrorText_P(MSG_SYNTAX_PRINT);
      return true;
    }
    p++;
    skipWs(&p);
    if (*p != '\0') {
      commitErrorText_P(MSG_SYNTAX_PRINT);
      return true;
    }
    commitTwoGreenText(curLine, out);
    return true;
  }

  long a = 0;
  long b = 0;

  if (keywordAtP(p, CMD_SUM, CMD_SUM_LEN)) {
    p += CMD_SUM_LEN;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_SUM);
      return true;
    }
    ltoa(a + b, out, 10);
    commitTwoGreenText(curLine, out);
    return true;
  }
  if (keywordAtP(p, CMD_MUL, CMD_MUL_LEN)) {
    p += CMD_MUL_LEN;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_MUL);
      return true;
    }
    ltoa(a * b, out, 10);
    commitTwoGreenText(curLine, out);
    return true;
  }
  if (keywordAtP(p, CMD_SUB, CMD_SUB_LEN)) {
    p += CMD_SUB_LEN;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_SUB);
      return true;
    }
    ltoa(a - b, out, 10);
    commitTwoGreenText(curLine, out);
    return true;
  }
  if (keywordAtP(p, CMD_DIV, CMD_DIV_LEN)) {
    p += CMD_DIV_LEN;
    if (!parseTwoLongs(p, &a, &b)) {
      commitErrorText_P(parseLastUndefinedVar ? MSG_UNDEFINED_VAR : MSG_SYNTAX_DIV);
      return true;
    }
    if (b == 0) {
      commitErrorText_P(MSG_DIV_ZERO);
      return true;
    }
    ltoa(a / b, out, 10);
    commitTwoGreenText(curLine, out);
    return true;
  }

  {
    const char* q = curLine;
    char w[16];
    if (parseLeadingWord(&q, w, sizeof(w))) {
      skipWs(&q);
      if (*q == '(' && !isKnownCmdWord(w)) {
        commitErrorText_P(MSG_UNKNOWN_COMMAND);
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
    serialOutLine(curLine);
    rowCount = 3;
    histPush(curLine);
    curLen = 0;
    curLine[0] = '\0';
    paintEditingRow();
    return;
  }

  paintRowAt(rowCount, curLine, ILI9341_GREEN);
  serialOutLine(curLine);
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

  bcStringReset();
  clear();
  uint8_t progCount = 0;
  int eoa = 0;
  if (eepromScanMeta(&progCount, &eoa)) {
    if (eepromAutorunEnable && progCount > 0 && eepromAutorunIndex < progCount) {
      if (loadBCFromEEPROMIndex(eepromAutorunIndex)) {
        progNeedsSave = false;
        vmStart();
        commitGreenText_P(MSG_VM_RUNNING);
      } else if (loadBCFromEEPROMIndex(0)) {
        progNeedsSave = false;
        commitGreenText_P(MSG_BYTECODE_READY);
      }
    } else if (loadBCFromEEPROMIndex(0)) {
      progNeedsSave = false;
      commitGreenText_P(MSG_BYTECODE_READY);
    }
  }
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
      c = '\n';
    }
    if (c == '\n') {
      if (curLen == 0) {
        continue;
      }
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
  vmStep();
}
