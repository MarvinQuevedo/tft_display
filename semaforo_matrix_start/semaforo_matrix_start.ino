/*
  TFT "movie-inspired" animation showreel (10 effects).
  Non-blocking, rotates effect automatically every few seconds.
*/

#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>

const uint8_t TFT_CS = 10;
const uint8_t TFT_DC = 9;
const uint8_t TFT_RST = 8;
const uint32_t TFT_SPI_HZ = 1000000UL;

Adafruit_ILI9341 tft(TFT_CS, TFT_DC, TFT_RST);

static uint16_t panelColor(uint8_t r, uint8_t g, uint8_t b) {
  return tft.color565(b, g, r);
}

enum EffectId : uint8_t {
  FX_MATRIX = 0,
  FX_HYPERSPACE,
  FX_TRON_GRID,
  FX_TERMINATOR_GLITCH,
  FX_BLADE_RUNNER_NEON,
  FX_BAT_SIGNAL,
  FX_JURASSIC_AMBER,
  FX_AVENGERS_PORTAL,
  FX_GHOSTBUSTERS_SLIME,
  FX_PAC_CHASE,
  FX_COUNT
};

static const char* FX_NAMES[FX_COUNT] = {
  "MATRIX",
  "HYPERSPACE",
  "TRON GRID",
  "T-GLITCH",
  "NEON RAIN",
  "BAT SIGNAL",
  "AMBER DNA",
  "PORTAL",
  "ECTO SLIME",
  "PAC CHASE"
};

static const uint32_t FX_DURATION_MS = 8000UL;
static const uint16_t SCREEN_W = 320;
static const uint16_t SCREEN_H = 240;
static const uint8_t RIGHT_GHOST_MARGIN_PX = 10;
static const uint16_t DRAW_W = SCREEN_W - RIGHT_GHOST_MARGIN_PX;
static uint8_t currentFx = FX_MATRIX;
static uint32_t fxStartMs = 0;
static uint32_t fxTickMs = 0;
static uint16_t frameCount = 0;

// Shared particles reused by multiple effects.
struct Particle {
  int16_t x;
  int16_t y;
  int8_t vx;
  int8_t vy;
  uint8_t life;
};

static const uint8_t MAX_PARTICLES = 24;
static Particle particles[MAX_PARTICLES];
static int16_t particlePrevX[MAX_PARTICLES];
static int16_t particlePrevY[MAX_PARTICLES];

static int16_t prevBatX = -1000;
static int16_t prevDna1X = -1000;
static int16_t prevDna1Y = -1000;
static int16_t prevDna2X = -1000;
static int16_t prevDna2Y = -1000;
static int16_t prevPacX = -1000;
static int16_t prevGhostX = -1000;

// Matrix state.
static const uint8_t CHAR_W = 6;
static const uint8_t CHAR_H = 8;
static const uint8_t MATRIX_MAX_COLS = 52;
static const char MATRIX_CHARS[] =
  "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
  "abcdefghijklmnopqrstuvwxyz"
  "!@#$%^&*()[]{}<>+-=*/\\|?:;.,";
static int16_t matrixHeadRow[MATRIX_MAX_COLS];
static uint8_t matrixTrailLen[MATRIX_MAX_COLS];
static uint32_t matrixNextStepMs[MATRIX_MAX_COLS];
static uint16_t matrixBaseColor[MATRIX_MAX_COLS];
static uint8_t matrixCols = 0;
static uint8_t matrixRows = 0;
static uint8_t matrixCursor = 0;

static uint16_t effectTitleBgColor(uint8_t fx) {
  switch (fx) {
    case FX_MATRIX: return panelColor(10, 55, 10);
    case FX_HYPERSPACE: return panelColor(10, 10, 40);
    case FX_TRON_GRID: return panelColor(0, 30, 50);
    case FX_TERMINATOR_GLITCH: return panelColor(60, 0, 0);
    case FX_BLADE_RUNNER_NEON: return panelColor(40, 0, 30);
    case FX_BAT_SIGNAL: return panelColor(30, 25, 0);
    case FX_JURASSIC_AMBER: return panelColor(65, 30, 0);
    case FX_AVENGERS_PORTAL: return panelColor(30, 15, 60);
    case FX_GHOSTBUSTERS_SLIME: return panelColor(10, 35, 10);
    case FX_PAC_CHASE: return panelColor(20, 20, 20);
    default: return ILI9341_BLACK;
  }
}

static void drawTitle(uint8_t fx) {
  tft.fillRect(0, 0, DRAW_W, 16, effectTitleBgColor(fx));
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setCursor(6, 4);
  tft.setTextColor(ILI9341_WHITE, effectTitleBgColor(fx));
  tft.print(FX_NAMES[fx]);
  tft.fillRect(DRAW_W, 0, RIGHT_GHOST_MARGIN_PX, 16, ILI9341_BLACK);
}

static void matrixResetColumn(uint8_t c, uint32_t now) {
  matrixHeadRow[c] = -(int16_t)random(0, matrixRows);
  matrixTrailLen[c] = (uint8_t)random(5, 14);
  matrixNextStepMs[c] = now + (uint32_t)random(0, 120);
  matrixBaseColor[c] = panelColor(0, (uint8_t)random(130, 240), 0);
}

static void initParticlesForWarp() {
  for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
    particles[i].x = (int16_t)random(145, 176);
    particles[i].y = (int16_t)random(95, 146);
    particles[i].vx = (int8_t)random(-3, 4);
    particles[i].vy = (int8_t)random(-3, 4);
    if (particles[i].vx == 0 && particles[i].vy == 0) {
      particles[i].vx = 1;
    }
    particlePrevX[i] = particles[i].x;
    particlePrevY[i] = particles[i].y;
  }
}

static void initParticlesForPortal() {
  for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
    particles[i].x = 160;
    particles[i].y = 120;
    particles[i].vx = (int8_t)random(-4, 5);
    particles[i].vy = (int8_t)random(-4, 5);
    particles[i].life = (uint8_t)random(15, 60);
    particlePrevX[i] = particles[i].x;
    particlePrevY[i] = particles[i].y;
  }
}

static void initParticlesForSlime() {
  for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
    particles[i].x = (int16_t)random(10, DRAW_W - 10);
    particles[i].y = (int16_t)random(180, 236);
    particles[i].vx = (int8_t)random(-1, 2);
    particles[i].vy = -(int8_t)random(1, 3);
    particles[i].life = (uint8_t)random(20, 80);
    particlePrevX[i] = particles[i].x;
    particlePrevY[i] = particles[i].y;
  }
}

static void initEffect(uint8_t fx) {
  tft.fillScreen(ILI9341_BLACK);
  drawTitle(fx);
  frameCount = 0;
  fxTickMs = millis();

  if (fx == FX_MATRIX) {
    matrixCols = (uint8_t)(DRAW_W / CHAR_W);
    if (matrixCols > MATRIX_MAX_COLS) {
      matrixCols = MATRIX_MAX_COLS;
    }
    matrixRows = (uint8_t)((SCREEN_H - 16) / CHAR_H);
    uint32_t now = millis();
    for (uint8_t c = 0; c < matrixCols; c++) {
      matrixResetColumn(c, now);
    }
    matrixCursor = 0;
    tft.setTextSize(1);
    tft.setTextWrap(false);
  } else if (fx == FX_HYPERSPACE) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, ILI9341_BLACK);
    initParticlesForWarp();
  } else if (fx == FX_TRON_GRID) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, ILI9341_BLACK);
    uint16_t c = panelColor(0, 220, 240);
    for (int16_t y = 30; y < SCREEN_H; y += 18) {
      tft.drawLine(0, y, DRAW_W - 1, y + 4, c);
    }
    for (int16_t x = 0; x < DRAW_W; x += 24) {
      tft.drawLine(x, 28, 160 + (x / 12), SCREEN_H - 1, c);
    }
  } else if (fx == FX_AVENGERS_PORTAL) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, panelColor(5, 0, 12));
    tft.drawCircle(160, 120, 70, panelColor(120, 60, 255));
    tft.drawCircle(160, 120, 71, panelColor(180, 100, 255));
    initParticlesForPortal();
  } else if (fx == FX_GHOSTBUSTERS_SLIME) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, panelColor(0, 10, 0));
    initParticlesForSlime();
  } else if (fx == FX_BLADE_RUNNER_NEON) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, panelColor(4, 0, 12));
  } else if (fx == FX_BAT_SIGNAL) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, panelColor(5, 5, 12));
    prevBatX = -1000;
  } else if (fx == FX_JURASSIC_AMBER) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, panelColor(40, 18, 2));
    prevDna1X = -1000;
    prevDna2X = -1000;
  } else if (fx == FX_PAC_CHASE) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, ILI9341_BLACK);
    for (int16_t i = 0; i < DRAW_W; i += 20) {
      tft.fillCircle(i + 4, 130, 2, panelColor(255, 210, 80));
    }
    prevPacX = -1000;
    prevGhostX = -1000;
  }
}

static void stepMatrix(uint32_t now) {
  const uint8_t steps = 9;
  for (uint8_t i = 0; i < steps; i++) {
    uint8_t c = matrixCursor;
    matrixCursor = (uint8_t)((matrixCursor + 1) % matrixCols);

    if (now < matrixNextStepMs[c]) {
      continue;
    }

    int16_t head = matrixHeadRow[c];
    int16_t x = (int16_t)c * CHAR_W;
    if (head >= 0 && head < matrixRows) {
      int16_t y = 16 + head * CHAR_H;
      char ch = MATRIX_CHARS[random(0, (int)(sizeof(MATRIX_CHARS) - 1))];
      tft.setCursor(x, y);
      tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
      tft.print(ch);
    }
    int16_t body = head - 1;
    if (body >= 0 && body < matrixRows) {
      int16_t y = 16 + body * CHAR_H;
      char ch = MATRIX_CHARS[random(0, (int)(sizeof(MATRIX_CHARS) - 1))];
      tft.setCursor(x, y);
      tft.setTextColor(matrixBaseColor[c], ILI9341_BLACK);
      tft.print(ch);
    }
    int16_t tail = head - matrixTrailLen[c];
    if (tail >= 0 && tail < matrixRows) {
      int16_t y = 16 + tail * CHAR_H;
      tft.fillRect(x, y, CHAR_W, CHAR_H, ILI9341_BLACK);
    }

    head++;
    if (head - matrixTrailLen[c] > matrixRows) {
      matrixResetColumn(c, now);
    } else {
      matrixHeadRow[c] = head;
      matrixNextStepMs[c] = now + (uint32_t)random(20, 95);
    }
  }
}

static void stepHyperspace() {
  for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
    tft.drawPixel(particlePrevX[i], particlePrevY[i], ILI9341_BLACK);
    particles[i].x += particles[i].vx;
    particles[i].y += particles[i].vy;
    particles[i].vx += (particles[i].vx > 0) ? 1 : ((particles[i].vx < 0) ? -1 : 0);
    particles[i].vy += (particles[i].vy > 0) ? 1 : ((particles[i].vy < 0) ? -1 : 0);

    if (particles[i].x < 0 || particles[i].x >= DRAW_W || particles[i].y < 18 || particles[i].y >= SCREEN_H) {
      particles[i].x = 160;
      particles[i].y = 120;
      particles[i].vx = (int8_t)random(-3, 4);
      particles[i].vy = (int8_t)random(-3, 4);
      if (particles[i].vx == 0 && particles[i].vy == 0) {
        particles[i].vx = 1;
      }
    }
    particlePrevX[i] = particles[i].x;
    particlePrevY[i] = particles[i].y;
    tft.drawPixel(particles[i].x, particles[i].y, ILI9341_WHITE);
  }
}

static void stepTronGrid() {
  if (frameCount % 8 == 0) {
    uint16_t c = panelColor(0, (uint8_t)random(130, 255), (uint8_t)random(130, 255));
    int16_t y = (int16_t)random(24, 236);
    tft.drawFastHLine(0, y, DRAW_W, c);
    tft.drawFastHLine(0, y - 1, DRAW_W, ILI9341_BLACK);
  }
}

static void stepTerminatorGlitch() {
  if (frameCount % 2 == 0) {
    int16_t y = (int16_t)random(18, 236);
    int16_t h = (int16_t)random(2, 7);
    uint16_t c = panelColor((uint8_t)random(160, 255), 20, 20);
    tft.fillRect(0, y, DRAW_W, h, c);
    if (random(0, 4) == 0) {
      tft.fillRect(0, y, DRAW_W, 1, ILI9341_BLACK);
    }
  }
  if (frameCount % 25 == 0) {
    tft.fillRect(0, 16, DRAW_W, SCREEN_H - 16, ILI9341_BLACK);
  }
}

static void stepBladeRunnerNeon() {
  if (frameCount % 5 == 0) {
    int16_t x = (int16_t)random(0, DRAW_W);
    uint16_t c = panelColor((uint8_t)random(80, 255), 0, (uint8_t)random(120, 255));
    tft.drawFastVLine(x, 20, 200, c);
    if (x > 0) {
      tft.drawFastVLine(x - 1, 20, 200, panelColor(40, 0, 80));
    }
  }
  if (frameCount % 7 == 0) {
    int16_t x = (int16_t)random(0, DRAW_W);
    tft.drawFastVLine(x, 20, 200, panelColor(6, 0, 16));
  }
}

static void stepBatSignal() {
  if (frameCount % 4 == 0) {
    int16_t cx = (int16_t)(160 + 110 * sin(frameCount * 0.06));
    int16_t cy = 95;
    if (prevBatX > -500) {
      tft.fillCircle(prevBatX, cy, 30, panelColor(5, 5, 12));
    }
    tft.fillCircle(cx, cy, 28, panelColor(255, 230, 120));
    tft.setTextColor(ILI9341_BLACK, panelColor(255, 230, 120));
    tft.setTextSize(2);
    tft.setCursor(cx - 16, cy - 8);
    tft.print("BAT");
    prevBatX = cx;
  }
}

static void stepJurassicAmber() {
  if (frameCount % 2 == 0) {
    int16_t y = (int16_t)(120 + 70 * sin(frameCount * 0.11));
    int16_t x1 = (int16_t)(110 + 48 * sin(frameCount * 0.07));
    int16_t x2 = (int16_t)(210 + 48 * sin(frameCount * 0.07 + 3.14));
    if (prevDna1X > -500) {
      tft.fillCircle(prevDna1X, prevDna1Y, 5, panelColor(40, 18, 2));
      tft.fillCircle(prevDna2X, prevDna2Y, 5, panelColor(40, 18, 2));
    }
    for (int16_t i = 22; i < 235; i += 12) {
      uint16_t c = panelColor((uint8_t)random(160, 255), (uint8_t)random(90, 150), 10);
      tft.drawPixel((x1 + i / 8) % DRAW_W, i, c);
      tft.drawPixel((x2 + i / 8) % DRAW_W, i, c);
    }
    tft.fillCircle(x1, y, 4, panelColor(255, 180, 30));
    tft.fillCircle(x2, 240 - y + 16, 4, panelColor(255, 180, 30));
    prevDna1X = x1;
    prevDna1Y = y;
    prevDna2X = x2;
    prevDna2Y = 240 - y + 16;
  }
}

static void stepAvengersPortal() {
  for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
    tft.drawPixel(particlePrevX[i], particlePrevY[i], panelColor(5, 0, 12));
    particles[i].x += particles[i].vx;
    particles[i].y += particles[i].vy;
    particles[i].life--;
    particlePrevX[i] = particles[i].x;
    particlePrevY[i] = particles[i].y;
    tft.drawPixel(particles[i].x, particles[i].y, panelColor(200, (uint8_t)random(90, 200), 255));
    if (particles[i].life == 0 || particles[i].x < 80 || particles[i].x > 240 || particles[i].y < 40 || particles[i].y > 200) {
      particles[i].x = 160;
      particles[i].y = 120;
      particles[i].vx = (int8_t)random(-5, 6);
      particles[i].vy = (int8_t)random(-5, 6);
      particles[i].life = (uint8_t)random(15, 70);
    }
  }
}

static void stepGhostbustersSlime() {
  for (uint8_t i = 0; i < MAX_PARTICLES; i++) {
    tft.fillCircle(particlePrevX[i], particlePrevY[i], 2, panelColor(0, 10, 0));
    particles[i].x += particles[i].vx;
    particles[i].y += particles[i].vy;
    if (particles[i].y < 24 || particles[i].x < 2 || particles[i].x > (DRAW_W - 2)) {
      particles[i].x = (int16_t)random(10, DRAW_W - 10);
      particles[i].y = 236;
      particles[i].vx = (int8_t)random(-1, 2);
      particles[i].vy = -(int8_t)random(1, 4);
    }
    particlePrevX[i] = particles[i].x;
    particlePrevY[i] = particles[i].y;
    tft.fillCircle(particles[i].x, particles[i].y, 2, panelColor((uint8_t)random(80, 150), 255, (uint8_t)random(80, 130)));
  }
}

static void stepPacChase() {
  int16_t y = 130;
  int16_t pacX = (int16_t)((frameCount * 3) % 360) - 40;
  int16_t ghostX = pacX - 60;
  if (frameCount % 2 == 0) {
    if (prevPacX > -500) {
      tft.fillRect(prevPacX - 16, y - 16, 32, 32, ILI9341_BLACK);
      tft.fillRect(prevGhostX - 16, y - 16, 32, 32, ILI9341_BLACK);
      for (int16_t i = 0; i < DRAW_W; i += 20) {
        int16_t dotX = i + 4;
        if ((dotX > prevPacX - 18 && dotX < prevPacX + 18) || (dotX > prevGhostX - 18 && dotX < prevGhostX + 18)) {
          tft.fillCircle(dotX, y, 2, panelColor(255, 210, 80));
        }
      }
    }
    tft.fillCircle(pacX, y, 14, panelColor(255, 230, 0));
    uint16_t mouthColor = ILI9341_BLACK;
    if ((frameCount / 4) % 2 == 0) {
      tft.fillTriangle(pacX, y, pacX + 14, y - 8, pacX + 14, y + 8, mouthColor);
    } else {
      tft.fillTriangle(pacX, y, pacX + 14, y - 2, pacX + 14, y + 2, mouthColor);
    }
    tft.fillRect(ghostX - 12, y - 12, 24, 20, panelColor(255, 0, 120));
    tft.fillCircle(ghostX - 6, y - 12, 6, panelColor(255, 0, 120));
    tft.fillCircle(ghostX + 6, y - 12, 6, panelColor(255, 0, 120));
    tft.fillCircle(ghostX - 5, y - 8, 2, ILI9341_WHITE);
    tft.fillCircle(ghostX + 5, y - 8, 2, ILI9341_WHITE);
    prevPacX = pacX;
    prevGhostX = ghostX;
  }
}

static void stepEffect(uint8_t fx, uint32_t now) {
  switch (fx) {
    case FX_MATRIX: stepMatrix(now); break;
    case FX_HYPERSPACE: stepHyperspace(); break;
    case FX_TRON_GRID: stepTronGrid(); break;
    case FX_TERMINATOR_GLITCH: stepTerminatorGlitch(); break;
    case FX_BLADE_RUNNER_NEON: stepBladeRunnerNeon(); break;
    case FX_BAT_SIGNAL: stepBatSignal(); break;
    case FX_JURASSIC_AMBER: stepJurassicAmber(); break;
    case FX_AVENGERS_PORTAL: stepAvengersPortal(); break;
    case FX_GHOSTBUSTERS_SLIME: stepGhostbustersSlime(); break;
    case FX_PAC_CHASE: stepPacChase(); break;
    default: break;
  }
}

void setup() {
  tft.begin(TFT_SPI_HZ);
  tft.setRotation(1);
  randomSeed(analogRead(A0));
  currentFx = FX_MATRIX;
  fxStartMs = millis();
  initEffect(currentFx);
}

void loop() {
  uint32_t now = millis();
  if (now - fxStartMs >= FX_DURATION_MS) {
    currentFx = (uint8_t)((currentFx + 1) % FX_COUNT);
    fxStartMs = now;
    initEffect(currentFx);
  }

  if (now - fxTickMs >= 33UL) {  // ~30 FPS target
    fxTickMs = now;
    stepEffect(currentFx, now);
    frameCount++;
  }
}
