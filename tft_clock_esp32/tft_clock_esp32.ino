/*
 * Reloj + clima ESP32 + ST7789V (240x320, landscape).
 *
 * - Detecta ubicación con http://ip-api.com/json/
 * - Open-Meteo da clima + utc_offset_seconds + sunrise/sunset
 * - NTP (pool.ntp.org / time.google.com) sincroniza la hora con el offset detectado
 * - Pantalla: HH:MM:SS gigante + fecha + clima compacto + sol arriba/abajo
 *
 * Refresh:
 *   - Reloj: cada segundo (redibujado parcial sólo de los dígitos que cambian)
 *   - Clima: cada 10 min (no bloquea si falla)
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

#define TFT_CS   16
#define TFT_DC   17
#define TFT_RST  21

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

const uint32_t WEATHER_TTL = 10UL * 60UL * 1000UL;

struct Location {
    char  city[32];
    char  region[16];
    char  country[4];
    float lat, lon;
    int32_t utcOffsetSec;
    bool   ok;
};

struct Weather {
    bool    valid;
    uint32_t lastFetchMs;
    float   tempNow;
    int16_t codeNow;
    bool    isDay;
    float   tempMin, tempMax;
    int16_t precipMaxPct;
    float   windSpeed;
    int16_t windDir;
    char    sunriseHHMM[6];
    char    sunsetHHMM[6];
    float   hourlyTemp[24];
    int8_t  hourlyPrecip[24];
    uint8_t hourlyCount;
    int8_t  currentHourIdx;
};

Location loc;
Weather  weather;
bool     wifiOk = false;
bool     timeOk = false;

// ---- Layout 320x240 ----
const int16_t SCR_W  = 320;
const int16_t SCR_H  = 240;
const int16_t HEAD_H = 22;
const int16_t CLOCK_Y = 32;
const int16_t CLOCK_H = 48 * 1;  // tamaño 6: 48px de alto
const int16_t WX1_Y    = CLOCK_Y + CLOCK_H + 14;  // ~94
const int16_t WX2_Y    = WX1_Y + 38;              // grid stats (y=132,144)
const int16_t CHART_Y  = WX2_Y + 26;              // ~158
const int16_t CHART_H  = 50;                      // hasta y=208
const int16_t CHART_LBL_Y = CHART_Y + CHART_H + 1;
const int16_t SUN_Y    = SCR_H - 8;

// Posiciones de los 8 dígitos del reloj a tamaño 6 (36px wide cada uno)
const int16_t CLOCK_LEFT = (SCR_W - 8 * 36) / 2;  // = 16

// ---- Colores ----
uint16_t COL_TEXT, COL_DIM, COL_TIME, COL_DATE, COL_HOT, COL_COLD,
         COL_RAIN, COL_SUN, COL_CLOUD, COL_ACCENT;

const char* DAYS_ES[7]   = { "Domingo", "Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado" };
const char* MONTHS_ES[12] = { "ene", "feb", "mar", "abr", "may", "jun",
                              "jul", "ago", "sep", "oct", "nov", "dic" };

// ---------- WiFi ----------
static bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(250);
        tries++;
    }
    wifiOk = (WiFi.status() == WL_CONNECTED);
    return wifiOk;
}

// ---------- HTTP ----------
static bool httpGet(const String& url, String& out, bool secure) {
    HTTPClient http;
    bool begun;
    if (secure) {
        WiFiClientSecure c;
        c.setInsecure();
        begun = http.begin(c, url);
        if (!begun) return false;
        http.setTimeout(10000);
        int code = http.GET();
        bool ok = (code == HTTP_CODE_OK);
        if (ok) out = http.getString();
        else Serial.printf("HTTPS %d -> %s\n", code, url.c_str());
        http.end();
        return ok;
    } else {
        WiFiClient c;
        begun = http.begin(c, url);
        if (!begun) return false;
        http.setTimeout(8000);
        int code = http.GET();
        bool ok = (code == HTTP_CODE_OK);
        if (ok) out = http.getString();
        else Serial.printf("HTTP %d -> %s\n", code, url.c_str());
        http.end();
        return ok;
    }
}

// ---------- Detección IP ----------
static bool detectLocation() {
    String body;
    if (!httpGet("http://ip-api.com/json/", body, false)) return false;
    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;
    if (strcmp(doc["status"] | "", "success") != 0) return false;

    const char* city = doc["city"] | "Local";
    snprintf(loc.city, sizeof(loc.city), "%s", city);
    snprintf(loc.region, sizeof(loc.region), "%s", (const char*)(doc["region"] | ""));
    snprintf(loc.country, sizeof(loc.country), "%s", (const char*)(doc["countryCode"] | ""));
    loc.lat = doc["lat"] | 0.0f;
    loc.lon = doc["lon"] | 0.0f;
    loc.ok = (loc.lat != 0 || loc.lon != 0);
    Serial.printf("Auto: %s (%.4f, %.4f)\n", loc.city, loc.lat, loc.lon);
    return loc.ok;
}

// ---------- Open-Meteo: clima + offset + sol ----------
static bool fetchWeather() {
    char url[320];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current_weather=true"
        "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
        "&hourly=temperature_2m,precipitation_probability"
        "&forecast_days=1&timezone=auto",
        loc.lat, loc.lon);

    String body;
    if (!httpGet(String(url), body, true)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    loc.utcOffsetSec = doc["utc_offset_seconds"] | -21600;  // default GMT-6 SV

    JsonObject cw = doc["current_weather"].as<JsonObject>();
    if (cw.isNull()) return false;
    weather.tempNow   = cw["temperature"]   | 0.0f;
    weather.windSpeed = cw["windspeed"]     | 0.0f;
    weather.windDir   = cw["winddirection"] | 0;
    weather.codeNow   = cw["weathercode"]   | 0;
    weather.isDay     = (cw["is_day"]       | 1) != 0;

    JsonObject d = doc["daily"].as<JsonObject>();
    weather.tempMax      = d["temperature_2m_max"][0]            | 0.0f;
    weather.tempMin      = d["temperature_2m_min"][0]            | 0.0f;
    weather.precipMaxPct = d["precipitation_probability_max"][0] | 0;

    // sunrise/sunset vienen como "2026-04-18T05:48"
    auto extractHHMM = [](const char* iso, char* out) {
        if (strlen(iso) >= 16) memcpy(out, iso + 11, 5);
        out[5] = '\0';
    };
    extractHHMM(d["sunrise"][0] | "00:00:00:00:00", weather.sunriseHHMM);
    extractHHMM(d["sunset"][0]  | "00:00:00:00:00", weather.sunsetHHMM);

    // Hora actual (de current_weather.time -> "2026-04-18T08:45")
    const char* tNow = cw["time"] | "";
    weather.currentHourIdx = (strlen(tNow) >= 13)
        ? (tNow[11] - '0') * 10 + (tNow[12] - '0') : 0;

    JsonArray ht = doc["hourly"]["temperature_2m"].as<JsonArray>();
    JsonArray hp = doc["hourly"]["precipitation_probability"].as<JsonArray>();
    uint8_t n = 0;
    for (JsonVariant v : ht) {
        if (n >= 24) break;
        weather.hourlyTemp[n++] = v.as<float>();
    }
    weather.hourlyCount = n;
    n = 0;
    for (JsonVariant v : hp) {
        if (n >= 24) break;
        weather.hourlyPrecip[n++] = v.as<int>();
    }

    weather.valid = true;
    weather.lastFetchMs = millis();
    Serial.printf("Clima: %.1fC code=%d max=%.0f min=%.0f sol=%s/%s offset=%ld\n",
                  weather.tempNow, weather.codeNow, weather.tempMax, weather.tempMin,
                  weather.sunriseHHMM, weather.sunsetHHMM, loc.utcOffsetSec);
    return true;
}

// ---------- NTP ----------
static bool syncTime() {
    configTime(loc.utcOffsetSec, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    Serial.print("NTP");
    struct tm t;
    int tries = 0;
    while (!getLocalTime(&t, 500) && tries < 20) {
        Serial.print(".");
        tries++;
    }
    timeOk = getLocalTime(&t, 100);
    if (timeOk) Serial.printf("\nHora: %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    else Serial.println(" FAIL");
    return timeOk;
}

// ---------- Weather code → texto ----------
static const char* weatherDesc(int code) {
    if (code == 0) return "Despejado";
    if (code <= 2) return "Pocas nubes";
    if (code == 3) return "Nublado";
    if (code <= 48) return "Niebla";
    if (code <= 57) return "Llovizna";
    if (code <= 65) return "Lluvia";
    if (code <= 67) return "Lluvia helada";
    if (code <= 77) return "Nieve";
    if (code <= 82) return "Aguacero";
    if (code <= 86) return "Nieve";
    if (code <= 99) return "Tormenta";
    return "?";
}

// ---------- Iconos pequeños (24x24 en cx,cy) ----------
static void iconSmallSun(int cx, int cy) {
    tft.fillCircle(cx, cy, 6, COL_SUN);
    for (int a = 0; a < 8; a++) {
        float ang = a * (PI / 4);
        int x1 = cx + (int)(cos(ang) * 8);
        int y1 = cy + (int)(sin(ang) * 8);
        int x2 = cx + (int)(cos(ang) * 11);
        int y2 = cy + (int)(sin(ang) * 11);
        tft.drawLine(x1, y1, x2, y2, COL_SUN);
    }
}
static void iconSmallCloud(int cx, int cy, uint16_t color) {
    tft.fillCircle(cx - 5, cy + 1, 5, color);
    tft.fillCircle(cx + 2, cy - 1, 6, color);
    tft.fillCircle(cx + 8, cy + 2, 4, color);
    tft.fillRect(cx - 5, cy + 1, 13, 5, color);
}
static void iconSmallRain(int cx, int cy) {
    iconSmallCloud(cx, cy - 4, COL_CLOUD);
    for (int i = -6; i <= 8; i += 4) {
        tft.drawLine(cx + i, cy + 6, cx + i - 1, cy + 10, COL_RAIN);
    }
}
static void iconSmallStorm(int cx, int cy) {
    iconSmallCloud(cx, cy - 4, tft.color565(80, 80, 100));
    int bx = cx, by = cy + 4;
    tft.fillTriangle(bx - 2, by, bx + 1, by, bx, by + 4, COL_SUN);
    tft.fillTriangle(bx, by + 4, bx + 3, by + 4, bx, by + 9, COL_SUN);
}
static void drawWxIcon(int cx, int cy, int code, bool day) {
    if (code == 0)        iconSmallSun(cx, cy);
    else if (code <= 2) { iconSmallSun(cx - 6, cy - 4); iconSmallCloud(cx + 4, cy + 4, COL_CLOUD); }
    else if (code <= 48)  iconSmallCloud(cx, cy, COL_CLOUD);
    else if (code <= 67)  iconSmallRain(cx, cy);
    else if (code <= 82)  iconSmallRain(cx, cy);
    else if (code <= 99)  iconSmallStorm(cx, cy);
    else                  iconSmallCloud(cx, cy, COL_CLOUD);
}

// ---------- Render ----------
// Estado para redraw incremental del reloj
static int lastH = -1, lastM = -1, lastS = -1, lastDay = -1;

static void drawHeader() {
    tft.fillRect(0, 0, SCR_W, HEAD_H, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(COL_TEXT);
    tft.setCursor(6, 4);
    tft.print(loc.city);

    if (timeOk) {
        struct tm t;
        if (getLocalTime(&t, 50)) {
            char buf[24];
            snprintf(buf, sizeof(buf), "%s %d %s",
                     DAYS_ES[t.tm_wday], t.tm_mday, MONTHS_ES[t.tm_mon]);
            tft.setTextSize(1);
            tft.setTextColor(COL_DATE);
            int16_t x = SCR_W - 6 * (int)strlen(buf) - 6;
            tft.setCursor(x, 8);
            tft.print(buf);
            lastDay = t.tm_mday;
        }
    }
    tft.drawFastHLine(0, HEAD_H - 1, SCR_W, COL_DIM);
}

// Dibuja un par "HH" en las posiciones [pos0, pos1] (0-7)
static void drawDigitPair(int pos0, int val) {
    char buf[3];
    snprintf(buf, sizeof(buf), "%02d", val);
    int16_t x = CLOCK_LEFT + pos0 * 36;
    // borrar fondo del par (2 chars × 36 = 72 px)
    tft.fillRect(x, CLOCK_Y, 72, 48, ST77XX_BLACK);
    tft.setTextSize(6);
    tft.setTextColor(COL_TIME);
    tft.setCursor(x, CLOCK_Y);
    tft.print(buf);
}

static void drawColons() {
    tft.setTextSize(6);
    tft.setTextColor(COL_TIME);
    tft.setCursor(CLOCK_LEFT + 2 * 36, CLOCK_Y);
    tft.print(":");
    tft.setCursor(CLOCK_LEFT + 5 * 36, CLOCK_Y);
    tft.print(":");
}

static void renderClock() {
    if (!timeOk) return;
    struct tm t;
    if (!getLocalTime(&t, 50)) return;

    if (t.tm_hour != lastH) { drawDigitPair(0, t.tm_hour); lastH = t.tm_hour; }
    if (t.tm_min  != lastM) {
        drawDigitPair(3, t.tm_min);
        lastM = t.tm_min;
        // mover el marcador del chart con el tiempo real
        if (weather.valid) drawTempChart();
    }
    if (t.tm_sec  != lastS) { drawDigitPair(6, t.tm_sec);  lastS = t.tm_sec;  }
    drawColons();

    // Cambio de día → refresca header y fuerza refetch del clima
    // (max/min, sunrise/sunset y horas son de "ayer" hasta el próximo fetch)
    if (t.tm_mday != lastDay) {
        drawHeader();
        Serial.println("[dia cambió] invalidando cache de clima");
        weather.lastFetchMs = millis() - WEATHER_TTL - 1;  // fuerza TTL expirado
    }
}

static void drawWeather() {
    // Limpia desde debajo del reloj hasta abajo
    tft.fillRect(0, WX1_Y - 4, SCR_W, SCR_H - WX1_Y + 4, ST77XX_BLACK);
    tft.drawFastHLine(0, WX1_Y - 4, SCR_W, COL_DIM);

    if (!weather.valid) {
        tft.setTextColor(COL_DIM);
        tft.setTextSize(1);
        tft.setCursor(8, WX1_Y + 4);
        tft.print("Cargando clima...");
        return;
    }

    // Línea 1: icono + temp grande + descripción
    drawWxIcon(20, WX1_Y + 14, weather.codeNow, weather.isDay);

    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", weather.tempNow);
    tft.setTextSize(4);
    tft.setTextColor(COL_TEXT);
    tft.setCursor(46, WX1_Y);
    tft.print(buf);
    int tw = strlen(buf) * 6 * 4;
    tft.setTextSize(2);
    tft.setCursor(46 + tw, WX1_Y + 4);
    tft.print("o");
    tft.setTextSize(3);
    tft.setCursor(46 + tw + 12, WX1_Y + 6);
    tft.print("C");

    tft.setTextSize(2);
    tft.setTextColor(COL_ACCENT);
    tft.setCursor(140, WX1_Y + 8);
    tft.print(weatherDesc(weather.codeNow));

    // Línea 2: stats grid (columna izquierda, columna derecha)
    tft.setTextSize(1);

    tft.setTextColor(COL_HOT);
    tft.setCursor(8, WX2_Y);
    tft.printf("Max  %.0f C", weather.tempMax);
    tft.setTextColor(COL_COLD);
    tft.setCursor(8, WX2_Y + 12);
    tft.printf("Min  %.0f C", weather.tempMin);

    tft.setTextColor(COL_RAIN);
    tft.setCursor(120, WX2_Y);
    tft.printf("Lluvia %d%%", weather.precipMaxPct);
    tft.setTextColor(COL_TEXT);
    tft.setCursor(120, WX2_Y + 12);
    tft.printf("Viento %.0f km/h", weather.windSpeed);

    // pequeña flecha de viento
    int ax = 234, ay = WX2_Y + 16;
    float ang = (weather.windDir - 90) * PI / 180.0f;
    int hx = ax + (int)(cos(ang) * 8);
    int hy = ay + (int)(sin(ang) * 8);
    int tx2 = ax - (int)(cos(ang) * 8);
    int ty2 = ay - (int)(sin(ang) * 8);
    tft.drawLine(tx2, ty2, hx, hy, COL_TEXT);
    tft.fillCircle(hx, hy, 2, COL_TEXT);

    // Línea 3: sol arriba/abajo
    tft.setTextSize(1);
    tft.setTextColor(COL_SUN);
    tft.setCursor(8, SUN_Y);
    tft.printf("Sol  amanece %s   anochece %s",
               weather.sunriseHHMM, weather.sunsetHHMM);
}

static void drawTempChart() {
    tft.fillRect(0, CHART_Y - 1, SCR_W, CHART_H + 14, ST77XX_BLACK);
    if (!weather.valid || weather.hourlyCount == 0) return;

    // min/max para escalar
    float mn = weather.hourlyTemp[0], mx = weather.hourlyTemp[0];
    for (uint8_t k = 1; k < weather.hourlyCount; k++) {
        if (weather.hourlyTemp[k] < mn) mn = weather.hourlyTemp[k];
        if (weather.hourlyTemp[k] > mx) mx = weather.hourlyTemp[k];
    }
    float range = mx - mn;
    if (range < 1.0f) range = 1.0f;

    int16_t left = 24;            // deja espacio para etiqueta de min/max a la izq
    int16_t right = SCR_W - 4;
    int16_t top = CHART_Y;
    int16_t bot = CHART_Y + CHART_H - 6;
    float xStep = (float)(right - left) / (weather.hourlyCount - 1);

    // labels de min/max a la izquierda
    char buf[8];
    tft.setTextSize(1);
    tft.setTextColor(COL_HOT);
    snprintf(buf, sizeof(buf), "%.0f", mx);
    tft.setCursor(2, top + 1);
    tft.print(buf);
    tft.setTextColor(COL_COLD);
    snprintf(buf, sizeof(buf), "%.0f", mn);
    tft.setCursor(2, bot - 7);
    tft.print(buf);

    // barras de probabilidad de lluvia (azul, abajo)
    int16_t pBaseY = bot;
    int16_t pMaxH = (CHART_H - 6) / 3;
    for (uint8_t k = 0; k < weather.hourlyCount; k++) {
        if (weather.hourlyPrecip[k] <= 0) continue;
        int16_t x = left + (int16_t)(k * xStep);
        int16_t h = (int16_t)(weather.hourlyPrecip[k] * pMaxH / 100);
        if (h < 1) h = 1;
        tft.fillRect(x - 1, pBaseY - h, 3, h, COL_RAIN);
    }

    // línea de temperatura (gruesa)
    int16_t prevX = -1, prevY = -1;
    for (uint8_t k = 0; k < weather.hourlyCount; k++) {
        int16_t x = left + (int16_t)(k * xStep);
        float frac = (mx - weather.hourlyTemp[k]) / range;
        int16_t y = top + (int16_t)(frac * (bot - top));
        if (prevX >= 0) {
            tft.drawLine(prevX, prevY, x, y, COL_HOT);
            tft.drawLine(prevX, prevY + 1, x, y + 1, COL_HOT);
        }
        prevX = x; prevY = y;
    }

    // Marcador de hora actual: usa la hora REAL del sistema (con fracción de minuto)
    // para que se mueva suavemente con el tiempo, no sólo cuando refresca el clima.
    float hourFrac = (float)weather.currentHourIdx;  // fallback API
    struct tm tNow;
    if (timeOk && getLocalTime(&tNow, 50)) {
        hourFrac = tNow.tm_hour + tNow.tm_min / 60.0f + tNow.tm_sec / 3600.0f;
    }
    if (hourFrac >= 0 && hourFrac < weather.hourlyCount) {
        int16_t mxLine = left + (int16_t)(hourFrac * xStep);
        // interpolar Y entre las dos velas adyacentes
        int hi = (int)hourFrac;
        int hi2 = hi + 1;
        if (hi2 >= weather.hourlyCount) hi2 = hi;
        float fracBetween = hourFrac - hi;
        float tInterp = weather.hourlyTemp[hi] * (1.0f - fracBetween)
                      + weather.hourlyTemp[hi2] * fracBetween;
        float yFrac = (mx - tInterp) / range;
        int16_t my = top + (int16_t)(yFrac * (bot - top));

        tft.drawFastVLine(mxLine, top, bot - top, tft.color565(120, 120, 60));
        tft.fillCircle(mxLine, my, 3, COL_SUN);
    }

    // labels de hora abajo
    tft.setTextColor(COL_DIM);
    const int hours[] = { 0, 6, 12, 18 };
    for (int h : hours) {
        int16_t x = left + (int16_t)(h * xStep);
        char hbuf[8];
        snprintf(hbuf, sizeof(hbuf), "%02dh", h);
        int16_t lx = x - 8;
        if (lx < 0) lx = 0;
        tft.setCursor(lx, CHART_LBL_Y);
        tft.print(hbuf);
    }
}

static void redrawAll() {
    tft.fillScreen(ST77XX_BLACK);
    drawHeader();
    drawColons();
    lastH = lastM = lastS = lastDay = -1;
    renderClock();
    drawWeather();
    drawTempChart();
}

// ---------- Mini-consola estilo terminal (boot screen) ----------
static int16_t conY = 0;
static const int16_t CON_LEFT = 6;
static const int16_t CON_LINE_H = 9;

static uint16_t COL_PROMPT, COL_CMD, COL_LABEL, COL_VAL, COL_OK, COL_ERR, COL_TITLE;

static void conInit(const char* title) {
    tft.fillScreen(ST77XX_BLACK);
    // banner superior
    tft.setTextSize(1);
    tft.setTextColor(COL_TITLE);
    tft.setCursor(CON_LEFT, 4);
    tft.print(title);
    tft.drawFastHLine(0, 14, SCR_W, COL_DIM);
    conY = 20;
}

static void conPrompt(const char* cmd) {
    tft.setTextSize(1);
    tft.setTextColor(COL_PROMPT);
    tft.setCursor(CON_LEFT, conY);
    tft.print("> ");
    tft.setTextColor(COL_CMD);
    tft.print(cmd);
    conY += CON_LINE_H;
    delay(120);
}

// Imprime "  label   : value" alineado
static void conKV(const char* label, const char* value, uint16_t valColor = 0) {
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL);
    tft.setCursor(CON_LEFT + 12, conY);
    char buf[16];
    snprintf(buf, sizeof(buf), "%-9s: ", label);
    tft.print(buf);
    tft.setTextColor(valColor ? valColor : COL_VAL);
    tft.print(value);
    conY += CON_LINE_H;
    delay(40);
}

static void conKVf(const char* label, const char* fmt, ...) {
    char vbuf[48];
    va_list ap; va_start(ap, fmt);
    vsnprintf(vbuf, sizeof(vbuf), fmt, ap);
    va_end(ap);
    conKV(label, vbuf);
}

static void conStatus(bool ok) {
    tft.setTextSize(1);
    tft.setTextColor(COL_LABEL);
    tft.setCursor(CON_LEFT + 12, conY);
    tft.print("status   : ");
    tft.setTextColor(ok ? COL_OK : COL_ERR);
    tft.print(ok ? "OK" : "FAIL");
    conY += CON_LINE_H + 4;  // espacio extra entre comandos
    delay(180);
}

static void conNote(const char* msg) {
    tft.setTextSize(1);
    tft.setTextColor(COL_DIM);
    tft.setCursor(CON_LEFT + 12, conY);
    tft.print(msg);
    conY += CON_LINE_H;
    delay(60);
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

void setup() {
    Serial.begin(115200);
    delay(50);

    tft.init(240, 320, SPI_MODE3);
    tft.setSPISpeed(20000000UL);
    tft.setRotation(1);
    tft.invertDisplay(false);
    tft.fillScreen(ST77XX_BLACK);

    COL_TEXT   = ST77XX_WHITE;
    COL_DIM    = tft.color565(120, 120, 130);
    COL_TIME   = tft.color565(140, 230, 255);
    COL_DATE   = tft.color565(180, 180, 200);
    COL_HOT    = tft.color565(255, 120, 90);
    COL_COLD   = tft.color565(120, 200, 255);
    COL_RAIN   = tft.color565(80, 160, 255);
    COL_SUN    = tft.color565(255, 220, 60);
    COL_CLOUD  = tft.color565(200, 200, 220);
    COL_ACCENT = tft.color565(180, 230, 180);

    // Colores consola
    COL_PROMPT = tft.color565(60, 230, 90);
    COL_CMD    = tft.color565(220, 220, 255);
    COL_LABEL  = tft.color565(110, 110, 130);
    COL_VAL    = tft.color565(220, 220, 200);
    COL_OK     = tft.color565(60, 230, 90);
    COL_ERR    = tft.color565(255, 90, 90);
    COL_TITLE  = tft.color565(120, 220, 255);

    conInit("[ esp32 // weather + clock ]");

    // ---- 1. WiFi ----
    conPrompt("wifi.connect()");
    conKV("ssid", WIFI_SSID);
    bool wOk = connectWiFi();
    if (wOk) {
        conKV("ip",   WiFi.localIP().toString().c_str());
        conKVf("rssi", "%d dBm", WiFi.RSSI());
    } else {
        conKV("error", "no link", COL_ERR);
    }
    conStatus(wOk);
    if (!wOk) { delay(2500); return; }

    // ---- 2. Geolocalización por IP ----
    conPrompt("ip_lookup()");
    conKV("via", "ip-api.com");
    bool ipOk = detectLocation();
    if (ipOk) {
        conKVf("city",    "%s, %s", loc.city, loc.country);
        conKVf("region",  "%s",     loc.region);
        conKVf("lat,lon", "%.3f, %.3f", loc.lat, loc.lon);
    } else {
        conKV("warn", "fallback SV", COL_ERR);
        snprintf(loc.city, sizeof(loc.city), "San Salvador");
        snprintf(loc.country, sizeof(loc.country), "SV");
        loc.lat = 13.69f; loc.lon = -89.22f;
        loc.utcOffsetSec = -21600;
        loc.ok = true;
    }
    conStatus(ipOk);

    // ---- 3. Open-Meteo ----
    conPrompt("open_meteo()");
    conKV("via", "api.open-meteo.com");
    bool wxOk = fetchWeather();
    if (wxOk) {
        conKVf("temp",   "%.1f C", weather.tempNow);
        conKVf("state",  "%s",     weatherDesc(weather.codeNow));
        conKVf("range",  "%.0f - %.0f C", weather.tempMin, weather.tempMax);
        conKVf("offset", "%+ld (GMT%+d)",
               (long)loc.utcOffsetSec, (int)(loc.utcOffsetSec / 3600));
    } else {
        conKV("error", "no data", COL_ERR);
    }
    conStatus(wxOk);

    // ---- 4. NTP ----
    conPrompt("ntp.sync()");
    conKV("pools", "pool.ntp.org +2");
    bool ntpOk = syncTime();
    if (ntpOk) {
        struct tm t;
        if (getLocalTime(&t, 100)) {
            conKVf("time", "%02d:%02d:%02d", t.tm_hour, t.tm_min, t.tm_sec);
            conKVf("date", "%s %d %s", DAYS_ES[t.tm_wday], t.tm_mday, MONTHS_ES[t.tm_mon]);
        }
    } else {
        conKV("error", "no sync", COL_ERR);
    }
    conStatus(ntpOk);

    // ---- 5. Render ----
    conPrompt("ui.render()");
    conNote("loading dashboard...");
    delay(700);

    redrawAll();
}

void loop() {
    if (!wifiOk) return;

    // Reloj cada segundo
    static uint32_t lastTick = 0;
    uint32_t now = millis();
    if (now - lastTick >= 250) {  // chequea 4x/seg para precisión
        lastTick = now;
        renderClock();
    }

    // Refresh clima cada WEATHER_TTL
    if (weather.valid && (now - weather.lastFetchMs) > WEATHER_TTL) {
        Serial.println("Refresh clima...");
        if (fetchWeather()) {
            drawWeather();
            drawTempChart();
        }
    }
}
