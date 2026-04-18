/*
 * Crypto ticker para ESP32 + ST7789V (240x320, landscape).
 * Dos modos:
 *   LIVE      - últimos 30 min en velas de 1 min, refetch cada 5 s
 *   HISTORICO - últimos 7 días en velas de 4 h, refetch cada 5 min
 *
 * Ciclo: para cada crypto se muestra LIVE y luego HISTORICO (5 s c/u).
 * XCH (CoinGecko) sólo soporta HISTORICO.
 *
 * Caché por (crypto, modo). Precio en vivo se sirve desde la última vela.
 * Requiere: secrets.h con WIFI_SSID y WIFI_PASS
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"

#define TFT_CS   16
#define TFT_DC   17
#define TFT_RST  21

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ---------- Modos ----------
enum DisplayMode : uint8_t { MODE_LIVE = 0, MODE_HISTORIC = 1 };
const uint8_t NUM_MODES = 2;

struct ModeConfig {
    const char* label;             // texto del subheader: "30m/1m", "7d/4h"
    const char* binanceInterval;   // "1m", "4h"
    uint8_t     binanceLimit;      // 30, 42
    int8_t      coingeckoDays;     // 1, 7  (-1 si no soportado)
    uint32_t    candleTtlMs;       // refetch velas
    uint32_t    priceTtlMs;        // refetch precio (sólo HISTORICO)
    uint32_t    dwellMs;           // tiempo en pantalla
    int16_t     totalUnits;        // 30 ó 7
    char        unitChar;          // 'm' ó 'd'
    bool        priceFromTicker;   // true => fetch separado /ticker/price
};

const ModeConfig MODES[NUM_MODES] = {
    // LIVE: precio = close de la última vela 1m (siempre fresco), no necesita ticker
    { "30m/1m", "1m", 30, -1,  5UL*1000UL,         5UL*1000UL, 5UL*1000UL, 30, 'm', false },
    // HISTORICO: ticker separado para precio en vivo
    { "7d/4h",  "4h", 42,  7,  5UL*60*1000UL, 15UL*1000UL, 5UL*1000UL,  7, 'd', true  },
};

// ---------- Cryptos ----------
enum Source : uint8_t { SRC_BINANCE, SRC_COINGECKO, SRC_KUCOIN };

struct Crypto {
    Source       source;
    const char*  apiId;        // "BTCUSDT" (binance), "chia" (cg), "XCH-USDT" (kucoin)
    const char*  displayName;
};

const Crypto CRYPTOS[] = {
    { SRC_KUCOIN,  "XCH-USDT", "XCH"  },
    { SRC_BINANCE, "BTCUSDT",  "BTC"  },
    { SRC_BINANCE, "ETHUSDT",  "ETH"  },
    { SRC_BINANCE, "BNBUSDT",  "BNB"  },
    { SRC_BINANCE, "SOLUSDT",  "SOL"  },
    { SRC_BINANCE, "XRPUSDT",  "XRP"  },
    { SRC_BINANCE, "DOGEUSDT", "DOGE" },
};
const uint8_t NUM_CRYPTOS = sizeof(CRYPTOS) / sizeof(CRYPTOS[0]);

const uint8_t MAX_CANDLES = 48;

struct Candle { float open, high, low, close; };

struct CryptoCache {
    Candle   candles[MAX_CANDLES];
    uint8_t  count;
    uint32_t lastCandleMs;
    uint32_t lastPriceMs;
    float    livePrice;
    bool     hasCandles;
    bool     hasPrice;
};

// caché 2D: por crypto y por modo
CryptoCache cache[NUM_CRYPTOS][NUM_MODES];

uint8_t  currentCrypto = 0;
uint8_t  currentMode   = MODE_HISTORIC;
uint32_t lastSwitch    = 0;
bool     wifiOk        = false;

// Layout landscape 320x240
const int16_t SCR_W   = 320;
const int16_t SCR_H   = 240;
const int16_t HEAD_H  = 50;
const int16_t SUB_H   = 14;
const int16_t FOOT_H  = 12;
const int16_t CHART_X = 4;
const int16_t CHART_Y = HEAD_H + SUB_H;
const int16_t CHART_W = SCR_W - 8;
const int16_t CHART_H = SCR_H - HEAD_H - SUB_H - FOOT_H;

uint16_t COL_UP, COL_DOWN, COL_GRID, COL_TEXT, COL_LIVE;

// ---------- Soporte de modo por crypto ----------
static bool modeSupported(uint8_t ci, uint8_t mi) {
    const Crypto& c = CRYPTOS[ci];
    if (c.source == SRC_COINGECKO) return MODES[mi].coingeckoDays > 0;
    return true;  // Binance y KuCoin soportan ambos modos
}

// Mapea el modo a "type" de KuCoin: 1min, 4hour, etc.
static const char* kucoinTypeFor(uint8_t mi) {
    if (mi == MODE_LIVE) return "1min";
    return "4hour";
}

// ---------- WiFi ----------
static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("WiFi");
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(250);
        Serial.print(".");
        tries++;
    }
    wifiOk = (WiFi.status() == WL_CONNECTED);
    if (wifiOk) Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
    else Serial.println(" FAIL");
}

// ---------- HTTP ----------
static bool httpGet(const String& url, String& out) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;
    if (!https.begin(client, url)) return false;
    https.setTimeout(8000);
    https.setUserAgent("ESP32-Crypto/1.0");

    int code = https.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("HTTP %d -> %s\n", code, url.c_str());
        https.end();
        return false;
    }
    out = https.getString();
    https.end();
    return true;
}

// ---------- Velas: Binance ----------
static bool fetchCandlesBinance(const char* symbol, uint8_t modeIdx, CryptoCache& dst) {
    const ModeConfig& m = MODES[modeIdx];
    String url = "https://api.binance.com/api/v3/klines?symbol=";
    url += symbol;
    url += "&interval=";
    url += m.binanceInterval;
    url += "&limit=";
    url += m.binanceLimit;

    String body;
    if (!httpGet(url, body)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    JsonArray arr = doc.as<JsonArray>();
    uint8_t n = 0;
    for (JsonVariant v : arr) {
        if (n >= MAX_CANDLES) break;
        JsonArray k = v.as<JsonArray>();
        dst.candles[n].open  = k[1].as<float>();
        dst.candles[n].high  = k[2].as<float>();
        dst.candles[n].low   = k[3].as<float>();
        dst.candles[n].close = k[4].as<float>();
        n++;
    }
    if (n == 0) return false;
    dst.count = n;
    dst.hasCandles = true;
    return true;
}

// ---------- Velas: KuCoin ----------
// URL: /api/v1/market/candles?symbol=XCH-USDT&type=1min
// Respuesta: { "code":"200000", "data": [[ts, open, CLOSE, HIGH, LOW, vol, turn], ...] }
// Devuelve newest-first → hay que invertir el orden
static bool fetchCandlesKuCoin(const char* symbol, uint8_t modeIdx, CryptoCache& dst) {
    const ModeConfig& m = MODES[modeIdx];
    String url = "https://api.kucoin.com/api/v1/market/candles?symbol=";
    url += symbol;
    url += "&type=";
    url += kucoinTypeFor(modeIdx);

    String body;
    if (!httpGet(url, body)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    if (strcmp(doc["code"] | "", "200000") != 0) {
        Serial.printf("KuCoin err: %s\n", (const char*)(doc["msg"] | "?"));
        return false;
    }

    JsonArray arr = doc["data"].as<JsonArray>();
    uint8_t want = m.binanceLimit;  // mismo límite que Binance (30 ó 42)
    uint8_t avail = arr.size();
    uint8_t n = (avail < want) ? avail : want;
    if (n == 0) return false;

    // Las primeras `n` velas (más recientes) van al final del array invertido.
    // dst.candles[0] = más vieja, dst.candles[n-1] = más reciente
    for (uint8_t i = 0; i < n; i++) {
        JsonArray k = arr[i].as<JsonArray>();   // i=0 es la más reciente
        uint8_t idx = n - 1 - i;
        dst.candles[idx].open  = k[1].as<float>();
        dst.candles[idx].close = k[2].as<float>();  // ¡CLOSE en índice 2!
        dst.candles[idx].high  = k[3].as<float>();
        dst.candles[idx].low   = k[4].as<float>();
    }
    dst.count = n;
    dst.hasCandles = true;
    return true;
}

// ---------- Precio en vivo (sólo HISTORICO; LIVE usa close de última vela) ----------
static bool fetchPriceBinance(const char* symbol, float& out) {
    String url = "https://api.binance.com/api/v3/ticker/price?symbol=";
    url += symbol;
    String body;
    if (!httpGet(url, body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    out = doc["price"].as<float>();
    return out > 0;
}

static bool fetchPriceKuCoin(const char* symbol, float& out) {
    String url = "https://api.kucoin.com/api/v1/market/orderbook/level1?symbol=";
    url += symbol;
    String body;
    if (!httpGet(url, body)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (strcmp(doc["code"] | "", "200000") != 0) return false;
    out = doc["data"]["price"].as<float>();
    return out > 0;
}

// ---------- Refresh con TTL ----------
static bool refreshCandles(uint8_t ci, uint8_t mi) {
    const Crypto& c = CRYPTOS[ci];
    Serial.printf("Velas %s [%s]\n", c.displayName, MODES[mi].label);
    bool ok = false;
    switch (c.source) {
        case SRC_BINANCE:  ok = fetchCandlesBinance(c.apiId, mi, cache[ci][mi]); break;
        case SRC_KUCOIN:   ok = fetchCandlesKuCoin(c.apiId, mi, cache[ci][mi]);  break;
        case SRC_COINGECKO: ok = false; break;  // ya no se usa
    }
    if (ok) {
        cache[ci][mi].lastCandleMs = millis();
        // sembrar livePrice si no había
        if (!cache[ci][mi].hasPrice) {
            cache[ci][mi].livePrice = cache[ci][mi].candles[cache[ci][mi].count - 1].close;
            cache[ci][mi].hasPrice = true;
            cache[ci][mi].lastPriceMs = millis();
        }
        // En LIVE el "precio" siempre lo da la última vela (ya recién refrescada)
        if (!MODES[mi].priceFromTicker) {
            cache[ci][mi].livePrice = cache[ci][mi].candles[cache[ci][mi].count - 1].close;
            cache[ci][mi].lastPriceMs = millis();
        }
    }
    return ok;
}

static bool refreshPrice(uint8_t ci, uint8_t mi) {
    const Crypto& c = CRYPTOS[ci];
    float p = 0;
    bool ok = false;
    switch (c.source) {
        case SRC_BINANCE:  ok = fetchPriceBinance(c.apiId, p); break;
        case SRC_KUCOIN:   ok = fetchPriceKuCoin(c.apiId, p);  break;
        case SRC_COINGECKO: ok = false; break;
    }
    if (ok) {
        cache[ci][mi].livePrice = p;
        cache[ci][mi].hasPrice = true;
        cache[ci][mi].lastPriceMs = millis();
        Serial.printf("  $%.4f\n", p);
    }
    return ok;
}

static bool ensureFresh(uint8_t ci, uint8_t mi) {
    if (!wifiOk) return false;
    uint32_t now = millis();
    CryptoCache& cc = cache[ci][mi];
    const ModeConfig& m = MODES[mi];

    if (!cc.hasCandles || (now - cc.lastCandleMs) > m.candleTtlMs) {
        refreshCandles(ci, mi);
    }
    // En modos donde el precio viene del ticker, refrescar aparte
    if (m.priceFromTicker) {
        if (!cc.hasPrice || (now - cc.lastPriceMs) > m.priceTtlMs) {
            refreshPrice(ci, mi);
        }
    }
    return cc.hasCandles;
}

// ---------- Render ----------
static void formatPrice(char* buf, size_t n, float p) {
    if (p < 1.0f)          snprintf(buf, n, "$%.4f", p);
    else if (p < 100.0f)   snprintf(buf, n, "$%.2f", p);
    else if (p < 10000.0f) snprintf(buf, n, "$%.2f", p);
    else                   snprintf(buf, n, "$%.0f", p);
}

static void drawHeader(uint8_t ci, uint8_t mi) {
    const Crypto& c = CRYPTOS[ci];
    const CryptoCache& cc = cache[ci][mi];

    tft.fillRect(0, 0, SCR_W, HEAD_H, ST77XX_BLACK);

    tft.setTextColor(COL_TEXT);
    tft.setTextSize(4);
    tft.setCursor(6, 10);
    tft.print(c.displayName);

    if (!cc.hasPrice) return;

    float price = cc.livePrice;
    float first = cc.hasCandles ? cc.candles[0].open : price;
    float pct = (first > 0) ? ((price - first) / first) * 100.0f : 0.0f;

    char pbuf[16];
    formatPrice(pbuf, sizeof(pbuf), price);
    tft.setTextSize(3);
    tft.setCursor(120, 6);
    tft.print(pbuf);

    uint16_t cl = (pct >= 0) ? COL_UP : COL_DOWN;
    tft.setTextColor(cl);
    tft.setTextSize(2);
    char cbuf[16];
    snprintf(cbuf, sizeof(cbuf), "%+.2f%%", pct);
    tft.setCursor(120, 30);
    tft.print(cbuf);

    tft.drawFastHLine(0, HEAD_H, SCR_W, COL_GRID);
}

static void drawSubHeader(uint8_t ci, uint8_t mi) {
    const Crypto& c = CRYPTOS[ci];
    const CryptoCache& cc = cache[ci][mi];

    tft.fillRect(0, HEAD_H + 1, SCR_W, SUB_H - 1, ST77XX_BLACK);
    tft.setTextSize(1);

    // badge LIVE en verde brillante para ver que estás en tiempo real
    if (mi == MODE_LIVE) {
        tft.fillRect(4, HEAD_H + 2, 28, 10, COL_LIVE);
        tft.setTextColor(ST77XX_BLACK);
        tft.setCursor(7, HEAD_H + 4);
        tft.print("LIVE");
    } else {
        tft.fillRect(4, HEAD_H + 2, 28, 10, tft.color565(60, 60, 70));
        tft.setTextColor(ST77XX_WHITE);
        tft.setCursor(7, HEAD_H + 4);
        tft.print("HIST");
    }

    tft.setTextColor(COL_GRID);
    tft.setCursor(38, HEAD_H + 4);
    tft.print(MODES[mi].label);

    tft.setCursor(94, HEAD_H + 4);
    const char* srcName = "?";
    if      (c.source == SRC_BINANCE)  srcName = "BINANCE";
    else if (c.source == SRC_KUCOIN)   srcName = "KUCOIN";
    else if (c.source == SRC_COINGECKO) srcName = "COINGECKO";
    tft.print(srcName);

    // edad del precio + número de velas
    uint32_t age = (millis() - cc.lastPriceMs) / 1000UL;
    char buf[24];
    if (cc.hasPrice) snprintf(buf, sizeof(buf), "%us  %u v", (unsigned)age, (unsigned)cc.count);
    else             snprintf(buf, sizeof(buf), "...");
    int16_t x = SCR_W - 6 * (int)strlen(buf) - 4;
    tft.setCursor(x, HEAD_H + 4);
    tft.print(buf);
}

static void drawFooter(uint8_t ci, uint8_t mi) {
    const CryptoCache& cc = cache[ci][mi];
    tft.fillRect(0, SCR_H - FOOT_H, SCR_W, FOOT_H, ST77XX_BLACK);
    if (!cc.hasCandles) return;

    tft.setTextSize(1);
    tft.setTextColor(COL_GRID);

    const ModeConfig& m = MODES[mi];
    // genera marcas según el modo
    struct Mark { float frac; const char* label; };
    char lbuf[5][8];
    Mark marks[5];
    int nMarks = 0;

    if (mi == MODE_LIVE) {
        // -30m, -20m, -10m, AHORA
        const int steps[] = { 30, 20, 10, 0 };
        for (int i = 0; i < 4; i++) {
            marks[nMarks].frac = 1.0f - (float)steps[i] / m.totalUnits;
            if (steps[i] == 0) snprintf(lbuf[nMarks], 8, "AHORA");
            else snprintf(lbuf[nMarks], 8, "-%dm", steps[i]);
            marks[nMarks].label = lbuf[nMarks];
            nMarks++;
        }
    } else {
        // -7d, -5d, -3d, -1d, HOY
        const int steps[] = { 7, 5, 3, 1, 0 };
        for (int i = 0; i < 5; i++) {
            marks[nMarks].frac = 1.0f - (float)steps[i] / m.totalUnits;
            if (steps[i] == 0) snprintf(lbuf[nMarks], 8, "HOY");
            else snprintf(lbuf[nMarks], 8, "-%dd", steps[i]);
            marks[nMarks].label = lbuf[nMarks];
            nMarks++;
        }
    }

    for (int i = 0; i < nMarks; i++) {
        int16_t x = CHART_X + (int16_t)(marks[i].frac * (CHART_W - 1));
        for (int16_t y = CHART_Y + CHART_H - 4; y < CHART_Y + CHART_H; y++) {
            tft.drawPixel(x, y, COL_GRID);
        }
        int16_t lblW = strlen(marks[i].label) * 6;
        int16_t lblX = x - lblW / 2;
        if (lblX < 0) lblX = 0;
        if (lblX + lblW > SCR_W) lblX = SCR_W - lblW;
        tft.setCursor(lblX, SCR_H - FOOT_H + 2);
        tft.print(marks[i].label);
    }
}

static void drawChart(uint8_t ci, uint8_t mi) {
    const CryptoCache& cc = cache[ci][mi];
    tft.fillRect(CHART_X, CHART_Y, CHART_W, CHART_H, ST77XX_BLACK);
    if (!cc.hasCandles) return;

    float minP = cc.candles[0].low, maxP = cc.candles[0].high;
    for (uint8_t i = 1; i < cc.count; i++) {
        if (cc.candles[i].low  < minP) minP = cc.candles[i].low;
        if (cc.candles[i].high > maxP) maxP = cc.candles[i].high;
    }
    float pad = (maxP - minP) * 0.08f;
    if (pad <= 0) pad = maxP * 0.001f + 0.0001f;
    minP -= pad; maxP += pad;
    float range = maxP - minP;

    // líneas verticales sutiles según el modo
    const ModeConfig& m = MODES[mi];
    int divs = (mi == MODE_LIVE) ? 3 : 7;  // cada 10m en live, cada 1d en hist
    for (int d = 1; d < divs; d++) {
        int16_t x = CHART_X + (int16_t)((float)d / divs * (CHART_W - 1));
        for (int16_t y = CHART_Y; y < CHART_Y + CHART_H; y += 6) {
            tft.drawPixel(x, y, tft.color565(40, 40, 50));
        }
    }

    // ref: open de la primera vela
    float ref = cc.candles[0].open;
    if (ref >= minP && ref <= maxP) {
        int16_t y = CHART_Y + (int16_t)((maxP - ref) * (CHART_H - 1) / range);
        for (int16_t x = CHART_X; x < CHART_X + CHART_W; x += 4) {
            tft.drawPixel(x, y, COL_GRID);
        }
    }

    int16_t cw = CHART_W / cc.count;
    int16_t bw = cw - 2;
    if (bw < 1) bw = 1;

    for (uint8_t i = 0; i < cc.count; i++) {
        const Candle& c = cc.candles[i];
        bool up = c.close >= c.open;
        uint16_t color = up ? COL_UP : COL_DOWN;

        int16_t x0 = CHART_X + i * cw;
        int16_t cx = x0 + cw / 2;

        int16_t yH = CHART_Y + (int16_t)((maxP - c.high)  * (CHART_H - 1) / range);
        int16_t yL = CHART_Y + (int16_t)((maxP - c.low)   * (CHART_H - 1) / range);
        int16_t yO = CHART_Y + (int16_t)((maxP - c.open)  * (CHART_H - 1) / range);
        int16_t yC = CHART_Y + (int16_t)((maxP - c.close) * (CHART_H - 1) / range);

        tft.drawFastVLine(cx, yH, yL - yH + 1, color);

        int16_t yTop = up ? yC : yO;
        int16_t yBot = up ? yO : yC;
        int16_t bh = yBot - yTop + 1;
        if (bh < 1) bh = 1;
        tft.fillRect(x0 + 1, yTop, bw, bh, color);
    }

    // etiquetas max/min
    char buf[16];
    tft.setTextSize(1);
    tft.setTextColor(COL_GRID);
    formatPrice(buf, sizeof(buf), maxP);
    tft.setCursor(SCR_W - 6 * (int)strlen(buf) - 4, CHART_Y + 2);
    tft.print(buf);
    formatPrice(buf, sizeof(buf), minP);
    tft.setCursor(SCR_W - 6 * (int)strlen(buf) - 4, CHART_Y + CHART_H - 10);
    tft.print(buf);
}

static void showMessage(const char* msg, uint16_t color) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setTextColor(color);
    tft.setTextSize(2);
    int16_t len = strlen(msg);
    int16_t x = (SCR_W - len * 12) / 2;
    if (x < 4) x = 4;
    tft.setCursor(x, SCR_H / 2 - 8);
    tft.print(msg);
}

static void renderAll(uint8_t ci, uint8_t mi) {
    drawHeader(ci, mi);
    drawSubHeader(ci, mi);
    drawChart(ci, mi);
    drawFooter(ci, mi);
}

// ---------- Schedule: avanza al siguiente (crypto, mode) válido ----------
static void advanceSchedule() {
    for (int safety = 0; safety < NUM_CRYPTOS * NUM_MODES + 1; safety++) {
        currentMode++;
        if (currentMode >= NUM_MODES) {
            currentMode = 0;
            currentCrypto = (currentCrypto + 1) % NUM_CRYPTOS;
        }
        if (modeSupported(currentCrypto, currentMode)) return;
    }
}

void setup() {
    Serial.begin(115200);
    delay(50);

    tft.init(240, 320, SPI_MODE3);
    tft.setSPISpeed(20000000UL);
    tft.setRotation(1);
    tft.invertDisplay(false);
    tft.fillScreen(ST77XX_BLACK);

    COL_UP   = tft.color565(0, 200, 80);
    COL_DOWN = tft.color565(220, 60, 60);
    COL_GRID = tft.color565(80, 80, 90);
    COL_TEXT = ST77XX_WHITE;
    COL_LIVE = tft.color565(0, 230, 100);

    for (uint8_t i = 0; i < NUM_CRYPTOS; i++)
        for (uint8_t j = 0; j < NUM_MODES; j++)
            cache[i][j] = CryptoCache{};

    showMessage("Conectando WiFi...", COL_TEXT);
    connectWiFi();
    if (!wifiOk) { showMessage("WiFi fallo", COL_DOWN); return; }

    // arrancar en XCH HISTORICO
    currentCrypto = 0;
    currentMode = MODE_HISTORIC;
    showMessage("Cargando...", COL_TEXT);
    ensureFresh(currentCrypto, currentMode);
    renderAll(currentCrypto, currentMode);
    lastSwitch = millis();
}

void loop() {
    if (!wifiOk) return;
    uint32_t now = millis();

    // cambio según el dwell del modo actual
    uint32_t dwell = MODES[currentMode].dwellMs;
    if (now - lastSwitch >= dwell) {
        advanceSchedule();
        bool fresh = ensureFresh(currentCrypto, currentMode);
        if (fresh) renderAll(currentCrypto, currentMode);
        else {
            char err[40];
            snprintf(err, sizeof(err), "%s: error", CRYPTOS[currentCrypto].displayName);
            showMessage(err, COL_DOWN);
        }
        lastSwitch = millis();
        return;
    }

    // En LIVE: refrescar velas si vence TTL → redibuja todo (precio = última vela)
    // En HISTORICO: refrescar sólo el ticker → redibuja header/sub
    CryptoCache& cc = cache[currentCrypto][currentMode];
    const ModeConfig& m = MODES[currentMode];

    if (cc.hasCandles && (now - cc.lastCandleMs) > m.candleTtlMs) {
        if (refreshCandles(currentCrypto, currentMode)) {
            renderAll(currentCrypto, currentMode);
        }
    } else if (m.priceFromTicker && cc.hasCandles
               && (now - cc.lastPriceMs) > m.priceTtlMs) {
        if (refreshPrice(currentCrypto, currentMode)) {
            drawHeader(currentCrypto, currentMode);
            drawSubHeader(currentCrypto, currentMode);
        }
    } else if (cc.hasCandles) {
        // refresca sólo el contador "Xs" cada segundo
        static uint32_t lastSubRedraw = 0;
        if (now - lastSubRedraw > 1000) {
            drawSubHeader(currentCrypto, currentMode);
            lastSubRedraw = now;
        }
    }
}
