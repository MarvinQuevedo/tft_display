/*
 * Reloj + clima ESP32 + ST7789V (240x320, landscape).
 *
 * - 3 ciudades fijas: San Miguel, Meanguera, San Alejo (El Salvador)
 * - Cambia de ciudad cada 15 segundos (redrawAll)
 * - Open-Meteo: clima + utc_offset + sunrise/sunset por ciudad
 * - NTP sincroniza la hora con GMT-6 (El Salvador)
 *
 * Refresco de pantalla:
 *   - Reloj: cada 250ms (solo dígitos que cambian)
 *   - Clima: en cada cambio de ciudad, y tras fetch exitoso (cada 10 min)
 */

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "secrets.h"

#define TFT_CS  16
#define TFT_DC  17
#define TFT_RST 21

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

const uint32_t WEATHER_TTL    = 10UL * 60UL * 1000UL;
const uint32_t CITY_SWITCH_MS = 15000UL;

// ---- Ciudades fijas ----
struct CityDef { const char* name; float lat, lon; };
static const CityDef CITIES[3] = {
    { "San Miguel", 13.4833f, -88.1833f },
    { "Meanguera",  13.7333f, -88.1167f },
    { "San Alejo",  13.4000f, -87.9833f },
};
static uint8_t cityIdx      = 0;
static int32_t utcOffsetSec = -21600;  // GMT-6 El Salvador

struct Weather {
    bool     valid;
    uint32_t lastFetchMs;
    float    tempNow, tempMin, tempMax;
    int16_t  codeNow, precipMaxPct, windDir;
    float    windSpeed;
    bool     isDay;
    char     sunriseHHMM[6], sunsetHHMM[6];
    float    hourlyTemp[24];
    int8_t   hourlyPrecip[24];
    uint8_t  hourlyCount;
    int32_t  utcOffsetSec;
};

static Weather   cityWeather[3];
static uint32_t  lastCitySwitch = 0;
static bool      wifiOk = false;
static bool      timeOk = false;

// ---- Layout 320x240 ----
const int16_t SCR_W       = 320;
const int16_t SCR_H       = 240;
const int16_t HEAD_H      = 22;
const int16_t CLOCK_Y     = 32;
const int16_t CLOCK_H     = 48;
const int16_t WX1_Y       = CLOCK_Y + CLOCK_H + 14;  // 94
const int16_t WX2_Y       = WX1_Y + 38;              // 132
const int16_t CHART_Y     = WX2_Y + 26;              // 158
const int16_t CHART_H     = 50;
const int16_t CHART_LBL_Y = CHART_Y + CHART_H + 1;
const int16_t SUN_Y       = SCR_H - 8;
const int16_t CLOCK_LEFT  = (SCR_W - 8 * 36) / 2;   // 16

// ---- Colores ----
uint16_t COL_TEXT, COL_DIM, COL_TIME, COL_DATE, COL_HOT, COL_COLD,
         COL_RAIN, COL_SUN, COL_CLOUD, COL_ACCENT;

const char* DAYS_ES[7]    = { "Domingo","Lunes","Martes","Miercoles","Jueves","Viernes","Sabado" };
const char* MONTHS_ES[12] = { "ene","feb","mar","abr","may","jun","jul","ago","sep","oct","nov","dic" };

// ---------- WiFi ----------
static bool connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);
    wifiOk = (WiFi.status() == WL_CONNECTED);
    return wifiOk;
}

// ---------- HTTPS GET ----------
static bool httpsGet(const String& url, String& out) {
    WiFiClientSecure c;
    c.setInsecure();
    HTTPClient http;
    if (!http.begin(c, url)) return false;
    http.setTimeout(10000);
    int code = http.GET();
    bool ok = (code == HTTP_CODE_OK);
    if (ok) out = http.getString();
    else Serial.printf("HTTP %d -> %s\n", code, url.c_str());
    http.end();
    return ok;
}

// ---------- Open-Meteo ----------
static bool fetchWeather(uint8_t idx) {
    char url[320];
    snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast"
        "?latitude=%.4f&longitude=%.4f"
        "&current_weather=true"
        "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
        "&hourly=temperature_2m,precipitation_probability"
        "&forecast_days=1&timezone=auto",
        CITIES[idx].lat, CITIES[idx].lon);

    String body;
    if (!httpsGet(url, body)) return false;

    JsonDocument doc;
    if (deserializeJson(doc, body)) return false;

    Weather& w = cityWeather[idx];
    w.utcOffsetSec = doc["utc_offset_seconds"] | -21600;

    JsonObject cw = doc["current_weather"].as<JsonObject>();
    if (cw.isNull()) return false;
    w.tempNow   = cw["temperature"]   | 0.0f;
    w.windSpeed = cw["windspeed"]     | 0.0f;
    w.windDir   = cw["winddirection"] | 0;
    w.codeNow   = cw["weathercode"]   | 0;
    w.isDay     = (cw["is_day"]       | 1) != 0;

    JsonObject d = doc["daily"].as<JsonObject>();
    w.tempMax      = d["temperature_2m_max"][0]            | 0.0f;
    w.tempMin      = d["temperature_2m_min"][0]            | 0.0f;
    w.precipMaxPct = d["precipitation_probability_max"][0] | 0;

    auto extractHHMM = [](const char* iso, char* out6) {
        if (strlen(iso) >= 16) memcpy(out6, iso + 11, 5);
        out6[5] = '\0';
    };
    extractHHMM(d["sunrise"][0] | "00:00:00:00:00", w.sunriseHHMM);
    extractHHMM(d["sunset"][0]  | "00:00:00:00:00", w.sunsetHHMM);

    uint8_t n = 0;
    for (JsonVariant v : doc["hourly"]["temperature_2m"].as<JsonArray>()) {
        if (n >= 24) break;
        w.hourlyTemp[n++] = v.as<float>();
    }
    w.hourlyCount = n;
    n = 0;
    for (JsonVariant v : doc["hourly"]["precipitation_probability"].as<JsonArray>()) {
        if (n >= 24) break;
        w.hourlyPrecip[n++] = v.as<int>();
    }

    w.valid = true;
    w.lastFetchMs = millis();
    Serial.printf("[%s] %.1fC code=%d %.0f-%.0fC sol=%s/%s\n",
        CITIES[idx].name, w.tempNow, w.codeNow,
        w.tempMin, w.tempMax, w.sunriseHHMM, w.sunsetHHMM);
    return true;
}

// ---------- NTP ----------
static bool syncTime() {
    configTime(utcOffsetSec, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    struct tm t;
    for (int i = 0; i < 20 && !getLocalTime(&t, 500); i++) Serial.print(".");
    timeOk = getLocalTime(&t, 100);
    if (timeOk) Serial.printf("NTP OK %02d:%02d:%02d\n", t.tm_hour, t.tm_min, t.tm_sec);
    else         Serial.println("NTP FAIL");
    return timeOk;
}

// ---------- Weather code → texto ----------
static const char* weatherDesc(int code) {
    if (code == 0)  return "Despejado";
    if (code <= 2)  return "Pocas nubes";
    if (code == 3)  return "Nublado";
    if (code <= 48) return "Niebla";
    if (code <= 57) return "Llovizna";
    if (code <= 65) return "Lluvia";
    if (code <= 67) return "Lluvia helada";
    if (code <= 77) return "Nieve";
    if (code <= 82) return "Aguacero";
    if (code <= 86) return "Nieve fuerte";
    if (code <= 99) return "Tormenta";
    return "?";
}

// ---------- Iconos ----------
static void iconSun(int cx, int cy) {
    tft.fillCircle(cx, cy, 6, COL_SUN);
    for (int a = 0; a < 8; a++) {
        float ang = a * (PI / 4);
        tft.drawLine(cx + (int)(cos(ang)*8),  cy + (int)(sin(ang)*8),
                     cx + (int)(cos(ang)*11), cy + (int)(sin(ang)*11), COL_SUN);
    }
}
static void iconCloud(int cx, int cy, uint16_t col) {
    tft.fillCircle(cx-5, cy+1, 5, col);
    tft.fillCircle(cx+2, cy-1, 6, col);
    tft.fillCircle(cx+8, cy+2, 4, col);
    tft.fillRect(cx-5, cy+1, 13, 5, col);
}
static void iconRain(int cx, int cy) {
    iconCloud(cx, cy-4, COL_CLOUD);
    for (int i = -6; i <= 8; i += 4)
        tft.drawLine(cx+i, cy+6, cx+i-1, cy+10, COL_RAIN);
}
static void iconStorm(int cx, int cy) {
    iconCloud(cx, cy-4, tft.color565(80,80,100));
    tft.fillTriangle(cx-2, cy+4, cx+1, cy+4, cx,   cy+8, COL_SUN);
    tft.fillTriangle(cx,   cy+8, cx+3, cy+8, cx,   cy+13, COL_SUN);
}
static void drawWxIcon(int cx, int cy, int code) {
    if (code == 0)        iconSun(cx, cy);
    else if (code <= 2) { iconSun(cx-6, cy-4); iconCloud(cx+4, cy+4, COL_CLOUD); }
    else if (code <= 48)  iconCloud(cx, cy, COL_CLOUD);
    else if (code <= 82)  iconRain(cx, cy);
    else if (code <= 99)  iconStorm(cx, cy);
    else                  iconCloud(cx, cy, COL_CLOUD);
}

// ---------- Render ----------
static int lastH = -1, lastM = -1, lastS = -1, lastDay = -1;

static void drawHeader() {
    tft.fillRect(0, 0, SCR_W, HEAD_H, ST77XX_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(COL_TEXT);
    tft.setCursor(6, 4);
    tft.print(CITIES[cityIdx].name);

    struct tm t;
    if (timeOk && getLocalTime(&t, 50)) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%s %d %s", DAYS_ES[t.tm_wday], t.tm_mday, MONTHS_ES[t.tm_mon]);
        tft.setTextSize(1);
        tft.setTextColor(COL_DATE);
        tft.setCursor(SCR_W - 6*(int)strlen(buf) - 6, 8);
        tft.print(buf);
        lastDay = t.tm_mday;
    }
    tft.drawFastHLine(0, HEAD_H-1, SCR_W, COL_DIM);
}

static void drawDigitPair(int pos0, int val) {
    int16_t x = CLOCK_LEFT + pos0 * 36;
    tft.fillRect(x, CLOCK_Y, 72, CLOCK_H, ST77XX_BLACK);
    tft.setTextSize(6);
    tft.setTextColor(COL_TIME);
    tft.setCursor(x, CLOCK_Y);
    tft.printf("%02d", val);
}

static void drawColons() {
    tft.setTextSize(6);
    tft.setTextColor(COL_TIME);
    tft.setCursor(CLOCK_LEFT + 2*36, CLOCK_Y); tft.print(":");
    tft.setCursor(CLOCK_LEFT + 5*36, CLOCK_Y); tft.print(":");
}

static void renderClock() {
    if (!timeOk) return;
    struct tm t;
    if (!getLocalTime(&t, 50)) return;

    if (t.tm_hour != lastH) { drawDigitPair(0, t.tm_hour); lastH = t.tm_hour; }
    if (t.tm_min  != lastM) { drawDigitPair(3, t.tm_min);  lastM = t.tm_min;  }
    if (t.tm_sec  != lastS) { drawDigitPair(6, t.tm_sec);  lastS = t.tm_sec;  }
    drawColons();

    if (t.tm_mday != lastDay) {
        drawHeader();
        for (uint8_t i = 0; i < 3; i++)
            cityWeather[i].lastFetchMs = millis() - WEATHER_TTL - 1;
    }
}

// Dibuja panel de clima completo (stats + chart) — siempre juntos
static void drawWxPanel() {
    tft.fillRect(0, WX1_Y-4, SCR_W, SCR_H-(WX1_Y-4), ST77XX_BLACK);
    tft.drawFastHLine(0, WX1_Y-4, SCR_W, COL_DIM);

    Weather& w = cityWeather[cityIdx];
    if (!w.valid) {
        tft.setTextColor(COL_DIM); tft.setTextSize(1);
        tft.setCursor(8, WX1_Y+4);
        tft.print("Cargando clima...");
        return;
    }

    // Temp grande + descripción
    drawWxIcon(20, WX1_Y+14, w.codeNow);
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f", w.tempNow);
    tft.setTextSize(4); tft.setTextColor(COL_TEXT);
    tft.setCursor(46, WX1_Y); tft.print(buf);
    int tw = strlen(buf) * 24;
    tft.setTextSize(2); tft.setCursor(46+tw,   WX1_Y+4); tft.print("o");
    tft.setTextSize(3); tft.setCursor(46+tw+12, WX1_Y+6); tft.print("C");
    tft.setTextSize(2); tft.setTextColor(COL_ACCENT);
    tft.setCursor(140, WX1_Y+8); tft.print(weatherDesc(w.codeNow));

    // Stats grid
    tft.setTextSize(1);
    tft.setTextColor(COL_HOT);   tft.setCursor(8,   WX2_Y);    tft.printf("Max  %.0f C", w.tempMax);
    tft.setTextColor(COL_COLD);  tft.setCursor(8,   WX2_Y+12); tft.printf("Min  %.0f C", w.tempMin);
    tft.setTextColor(COL_RAIN);  tft.setCursor(120, WX2_Y);    tft.printf("Lluvia %d%%", w.precipMaxPct);
    tft.setTextColor(COL_TEXT);  tft.setCursor(120, WX2_Y+12); tft.printf("Viento %.0f km/h", w.windSpeed);

    // Flecha de viento
    float ang = (w.windDir - 90) * PI / 180.0f;
    int ax = 234, ay = WX2_Y+16;
    tft.drawLine(ax-(int)(cos(ang)*8), ay-(int)(sin(ang)*8),
                 ax+(int)(cos(ang)*8), ay+(int)(sin(ang)*8), COL_TEXT);
    tft.fillCircle(ax+(int)(cos(ang)*8), ay+(int)(sin(ang)*8), 2, COL_TEXT);

    // Sunrise/sunset
    tft.setTextColor(COL_SUN); tft.setCursor(8, SUN_Y);
    tft.printf("Sol  amanece %s   anochece %s", w.sunriseHHMM, w.sunsetHHMM);

    // Chart de temperatura horaria
    if (w.hourlyCount == 0) return;

    float mn = w.hourlyTemp[0], mx = w.hourlyTemp[0];
    for (uint8_t k = 1; k < w.hourlyCount; k++) {
        mn = min(mn, w.hourlyTemp[k]);
        mx = max(mx, w.hourlyTemp[k]);
    }
    float range = max(mx - mn, 1.0f);

    const int16_t left  = 24, right = SCR_W-4;
    const int16_t top   = CHART_Y, bot = CHART_Y + CHART_H - 6;
    float xStep = (float)(right - left) / (w.hourlyCount - 1);

    // Labels min/max
    tft.setTextSize(1);
    tft.setTextColor(COL_HOT); tft.setCursor(2, top+1);  tft.printf("%.0f", mx);
    tft.setTextColor(COL_COLD); tft.setCursor(2, bot-7); tft.printf("%.0f", mn);

    // Barras de lluvia
    int16_t pMaxH = (CHART_H-6) / 3;
    for (uint8_t k = 0; k < w.hourlyCount; k++) {
        if (w.hourlyPrecip[k] <= 0) continue;
        int16_t h = max((int16_t)1, (int16_t)(w.hourlyPrecip[k] * pMaxH / 100));
        tft.fillRect(left+(int16_t)(k*xStep)-1, bot-h, 3, h, COL_RAIN);
    }

    // Línea de temperatura
    int16_t prevX = -1, prevY = -1;
    for (uint8_t k = 0; k < w.hourlyCount; k++) {
        int16_t x = left + (int16_t)(k * xStep);
        int16_t y = top + (int16_t)((mx - w.hourlyTemp[k]) / range * (bot - top));
        if (prevX >= 0) {
            tft.drawLine(prevX, prevY,   x, y,   COL_HOT);
            tft.drawLine(prevX, prevY+1, x, y+1, COL_HOT);
        }
        prevX = x; prevY = y;
    }

    // Marcador hora actual
    float hourFrac = 0;
    struct tm tNow;
    if (timeOk && getLocalTime(&tNow, 50))
        hourFrac = tNow.tm_hour + tNow.tm_min/60.0f + tNow.tm_sec/3600.0f;
    if (hourFrac >= 0 && hourFrac < w.hourlyCount) {
        int hi  = (int)hourFrac;
        int hi2 = (hi+1 < w.hourlyCount) ? hi+1 : hi;
        float tInterp = w.hourlyTemp[hi] * (1.0f-(hourFrac-hi)) + w.hourlyTemp[hi2]*(hourFrac-hi);
        int16_t mxLine = left + (int16_t)(hourFrac * xStep);
        int16_t my = top + (int16_t)((mx-tInterp)/range*(bot-top));
        tft.drawFastVLine(mxLine, top, bot-top, tft.color565(120,120,60));
        tft.fillCircle(mxLine, my, 3, COL_SUN);
    }

    // Labels horas
    tft.setTextColor(COL_DIM);
    for (int h : {0, 6, 12, 18}) {
        int16_t x = left + (int16_t)(h * xStep);
        tft.setCursor(max((int16_t)0, (int16_t)(x-8)), CHART_LBL_Y);
        tft.printf("%02dh", h);
    }
}

static void redrawAll() {
    tft.fillScreen(ST77XX_BLACK);
    drawHeader();
    drawColons();
    lastH = lastM = lastS = lastDay = -1;
    renderClock();
    drawWxPanel();
}

// ---------- Mini-consola boot ----------
static int16_t conY = 0;
static const int16_t CON_LEFT   = 6;
static const int16_t CON_LINE_H = 9;
static uint16_t COL_PROMPT, COL_CMD, COL_LABEL, COL_VAL, COL_OK, COL_ERR, COL_TITLE;

static void conPrint(uint16_t col, int16_t indent, const char* text) {
    tft.setTextSize(1); tft.setTextColor(col);
    tft.setCursor(CON_LEFT + indent, conY);
    tft.print(text);
    conY += CON_LINE_H;
}
static void conPrompt(const char* cmd) {
    tft.setTextSize(1);
    tft.setTextColor(COL_PROMPT); tft.setCursor(CON_LEFT, conY); tft.print("> ");
    tft.setTextColor(COL_CMD);    tft.print(cmd);
    conY += CON_LINE_H;
    delay(120);
}
static void conKV(const char* label, const char* value, uint16_t valCol = 0) {
    tft.setTextSize(1); tft.setTextColor(COL_LABEL);
    tft.setCursor(CON_LEFT+12, conY);
    char buf[16]; snprintf(buf, sizeof(buf), "%-9s: ", label);
    tft.print(buf);
    tft.setTextColor(valCol ? valCol : COL_VAL); tft.print(value);
    conY += CON_LINE_H;
    delay(40);
}
static void conKVf(const char* label, const char* fmt, ...) {
    char vbuf[48]; va_list ap; va_start(ap, fmt); vsnprintf(vbuf, sizeof(vbuf), fmt, ap); va_end(ap);
    conKV(label, vbuf);
}
static void conStatus(bool ok) {
    tft.setTextSize(1); tft.setTextColor(COL_LABEL);
    tft.setCursor(CON_LEFT+12, conY); tft.print("status   : ");
    tft.setTextColor(ok ? COL_OK : COL_ERR); tft.print(ok ? "OK" : "FAIL");
    conY += CON_LINE_H + 4;
    delay(180);
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
    COL_DIM    = tft.color565(120,120,130);
    COL_TIME   = tft.color565(140,230,255);
    COL_DATE   = tft.color565(180,180,200);
    COL_HOT    = tft.color565(255,120, 90);
    COL_COLD   = tft.color565(120,200,255);
    COL_RAIN   = tft.color565( 80,160,255);
    COL_SUN    = tft.color565(255,220, 60);
    COL_CLOUD  = tft.color565(200,200,220);
    COL_ACCENT = tft.color565(180,230,180);
    COL_PROMPT = tft.color565( 60,230, 90);
    COL_CMD    = tft.color565(220,220,255);
    COL_LABEL  = tft.color565(110,110,130);
    COL_VAL    = tft.color565(220,220,200);
    COL_OK     = tft.color565( 60,230, 90);
    COL_ERR    = tft.color565(255, 90, 90);
    COL_TITLE  = tft.color565(120,220,255);

    // Banner boot
    tft.setTextSize(1); tft.setTextColor(COL_TITLE);
    tft.setCursor(CON_LEFT, 4); tft.print("[ esp32 // weather + clock ]");
    tft.drawFastHLine(0, 14, SCR_W, COL_DIM);
    conY = 20;

    // ---- WiFi ----
    conPrompt("wifi.connect()");
    conKV("ssid", WIFI_SSID);
    bool wOk = connectWiFi();
    if (wOk) { conKV("ip", WiFi.localIP().toString().c_str()); conKVf("rssi", "%d dBm", WiFi.RSSI()); }
    else        conKV("error", "no link", COL_ERR);
    conStatus(wOk);
    if (!wOk) { delay(2500); return; }

    // ---- Clima 3 ciudades ----
    for (uint8_t i = 0; i < 3; i++) {
        char label[20];
        snprintf(label, sizeof(label), "meteo(%s)", CITIES[i].name);
        conPrompt(label);
        bool wxOk = fetchWeather(i);
        if (wxOk) {
            conKVf("temp",  "%.1f C", cityWeather[i].tempNow);
            conKVf("state", "%s",     weatherDesc(cityWeather[i].codeNow));
            if (i == 0) { utcOffsetSec = cityWeather[i].utcOffsetSec; conKVf("offset","GMT%+d",(int)(utcOffsetSec/3600)); }
        } else conKV("error", "no data", COL_ERR);
        conStatus(wxOk);
    }

    // ---- NTP ----
    conPrompt("ntp.sync()");
    bool ntpOk = syncTime();
    if (ntpOk) {
        struct tm t; if (getLocalTime(&t, 100))
            conKVf("time", "%02d:%02d:%02d %s %d %s", t.tm_hour, t.tm_min, t.tm_sec,
                   DAYS_ES[t.tm_wday], t.tm_mday, MONTHS_ES[t.tm_mon]);
    } else conKV("error", "no sync", COL_ERR);
    conStatus(ntpOk);

    delay(700);
    lastCitySwitch = millis();
    redrawAll();
}

void loop() {
    if (!wifiOk) return;
    uint32_t now = millis();

    // Cambio de ciudad
    if (now - lastCitySwitch >= CITY_SWITCH_MS) {
        lastCitySwitch = now;
        cityIdx = (cityIdx + 1) % 3;
        lastH = lastM = lastS = lastDay = -1;
        redrawAll();
        Serial.printf("Ciudad: %s\n", CITIES[cityIdx].name);
    }

    // Reloj cada 250ms
    static uint32_t lastTick = 0;
    if (now - lastTick >= 250) { lastTick = now; renderClock(); }

    // Fetch Open-Meteo si TTL expiró — redibuja solo si fue exitoso
    Weather& w = cityWeather[cityIdx];
    if (w.valid && (now - w.lastFetchMs) > WEATHER_TTL) {
        Serial.printf("Refresh %s...\n", CITIES[cityIdx].name);
        if (fetchWeather(cityIdx)) drawWxPanel();
    }
}
