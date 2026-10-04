// Cardputer ADV mini pet — пиксельный зверёк в стиле Claude.
// Ходит по экрану, реагирует на звуки (микрофон) и клавиши.
#include <M5Cardputer.h>
#include <Preferences.h>
#include <math.h>

static constexpr int W = 240, H = 135;
static constexpr int GROUND = 112;   // y пола (низ лапок)
static constexpr int U = 5;          // размер "пикселя" питомца в экранных пикселях
static constexpr int MIC_SR = 16000;
static constexpr int MIC_LEN = 512;
static constexpr int MIC_BUFS = 4;

// --- пороги реакции на звук (если реагирует слишком вяло/нервно — меняй тут; клавиша D показывает уровни) ---
static constexpr float SPIKE_MUL = 3.0f, SPIKE_ADD = 400;    // хлопок
static constexpr float BIG_MUL   = 7.0f, BIG_ADD   = 1800;   // очень громко
static constexpr float MUSIC_MUL = 1.8f, MUSIC_ADD = 150;    // музыка/шум

constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static constexpr uint16_t C_BODY   = rgb(217, 119, 87);   // фирменный терракотовый
static constexpr uint16_t C_EYE    = rgb(28, 22, 20);
static constexpr uint16_t C_BG     = rgb(38, 38, 36);
static constexpr uint16_t C_FLOOR  = rgb(58, 56, 52);
static constexpr uint16_t C_LINE   = rgb(96, 92, 84);
static constexpr uint16_t C_SHADOW = rgb(30, 30, 28);
static constexpr uint16_t C_CREAM  = rgb(240, 238, 230);
static constexpr uint16_t C_SPARK  = rgb(245, 170, 130);
static constexpr uint16_t C_GREY   = rgb(150, 146, 136);
static constexpr uint16_t C_RED    = rgb(214, 60, 60);
static constexpr uint16_t C_GREEN  = rgb(110, 190, 90);
static constexpr uint16_t C_CYAN   = rgb(90, 170, 200);
static constexpr uint16_t C_FOOD   = rgb(230, 160, 60);
static constexpr uint16_t C_JOY    = rgb(200, 110, 170);

M5Canvas cv(&M5Cardputer.Display);
Preferences prefs;

int16_t micBuf[MIC_BUFS][MIC_LEN];
int micIdx = 0, micFilled = 0;

enum State { IDLE, WALK, SLEEP, HAPPY, SCARED, EAT, PLAY, DANCE };
State state = IDLE;
uint32_t stateUntil = 0;

float px = 120, py = 0, vy = 0;    // py - высота над полом
int dir = 1;                       // 1 вправо, -1 влево
float food = 70, joy = 70, energy = 90;   // 0..100
uint32_t lastStat = 0, lastSave = 0;
uint32_t nextBlink = 0, blinkUntil = 0;
uint32_t frame = 0;
bool debugOverlay = false;

float level = 0, slowLevel = 300, noiseFloor = 300;
int loudFrames = 0;
uint32_t lastSpike = 0;

struct Spark { float x, y, vy; int life; };
Spark sparks[6];

void setState(State s, uint32_t dur) { state = s; stateUntil = millis() + dur; }
void jump(float power) { if (py <= 0.5f) vy = power; }

void emitSparks(int n) {
  for (int k = 0; k < n; k++)
    for (auto& s : sparks)
      if (s.life <= 0) {
        s.x = px + random(-34, 35); s.y = GROUND - 46 - random(0, 14);
        s.vy = -(0.5f + random(0, 10) / 10.0f); s.life = 28 + random(0, 14);
        break;
      }
}

void saveStats() {
  prefs.putUChar("food", (uint8_t)food);
  prefs.putUChar("joy", (uint8_t)joy);
  prefs.putUChar("energy", (uint8_t)energy);
}

void pickNext() {
  if (energy < 20) { setState(SLEEP, 90000); return; }
  if (random(100) < 65) { dir = random(2) ? 1 : -1; setState(WALK, 1500 + random(0, 3500)); }
  else setState(IDLE, 1000 + random(0, 3000));
}

// ---------- микрофон ----------
void analyze(const int16_t* b) {
  int64_t sum = 0;
  for (int i = 0; i < MIC_LEN; i++) sum += b[i];
  float mean = (float)sum / MIC_LEN, acc = 0;
  for (int i = 0; i < MIC_LEN; i++) { float d = b[i] - mean; acc += d * d; }
  float rms = sqrtf(acc / MIC_LEN);

  level = level * 0.6f + rms * 0.4f;
  if (level < noiseFloor * 1.6f) noiseFloor = noiseFloor * 0.995f + level * 0.005f;  // фон подстраивается только в тишине
  if (noiseFloor < 30) noiseFloor = 30;
  slowLevel = slowLevel * 0.97f + level * 0.03f;   // ~1 сек "память" громкости: отличает хлопок от музыки
}

void updateMic() {
  if (!M5Cardputer.Mic.isEnabled()) return;
  if (!M5Cardputer.Mic.record(micBuf[micIdx], MIC_LEN, MIC_SR)) return;
  // запись асинхронная: берём самый старый буфер, он точно уже заполнен
  int done = (micIdx + MIC_BUFS - 3) % MIC_BUFS;
  micIdx = (micIdx + 1) % MIC_BUFS;
  if (micFilled < MIC_BUFS) { micFilled++; return; }
  analyze(micBuf[done]);
}

void reactToSound() {
  uint32_t now = millis();
  float spikeTh = noiseFloor * SPIKE_MUL + SPIKE_ADD;
  float bigTh   = noiseFloor * BIG_MUL + BIG_ADD;
  float musicTh = noiseFloor * MUSIC_MUL + MUSIC_ADD;
  bool transient = level > slowLevel * 2.5f;     // резкий скачок, а не ровный шум

  if (level > musicTh) { if (loudFrames < 60) loudFrames++; }
  else loudFrames = loudFrames > 3 ? loudFrames - 3 : 0;

  if (transient && level > bigTh && now - lastSpike > 600) {
    lastSpike = now;
    setState(SCARED, 1800);
    jump(5.5f); dir = -dir; joy -= 3;
  } else if (transient && level > spikeTh && now - lastSpike > 400) {
    lastSpike = now;
    if (state == SLEEP) { setState(SCARED, 1200); jump(4.0f); }
    else { setState(HAPPY, 1800); emitSparks(4); jump(4.5f); joy += 4; }
  } else if (loudFrames > 45 && state != DANCE && state != SLEEP && state != EAT && state != PLAY) {
    setState(DANCE, 3000); joy += 3;
  }
}

// ---------- ввод ----------
void handleKeys() {
  if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
  auto st = M5Cardputer.Keyboard.keysState();
  for (char c : st.word) {
    switch (c) {
      case 'f': case 'F':
        setState(EAT, 2500); food = fminf(100, food + 25); joy += 2; break;
      case 'p': case 'P':
        setState(PLAY, 3000); joy = fminf(100, joy + 15); energy -= 5; jump(5.0f); break;
      case 's': case 'S':
        if (state == SLEEP) { setState(IDLE, 1000); }
        else { setState(SLEEP, 90000); }
        break;
      case 'd': case 'D': debugOverlay = !debugOverlay; break;
      default:
        if (state == SLEEP) { setState(IDLE, 800); }
        break;
    }
  }
}

// ---------- логика ----------
void updateStats() {
  uint32_t now = millis();
  if (now - lastStat >= 1000) {
    lastStat = now;
    food -= 0.04f;
    joy  -= (food < 25 ? 0.06f : 0.02f);
    if (state == SLEEP) energy += 0.5f; else energy -= 0.015f;
    if (state == SLEEP && energy >= 98) { setState(IDLE, 500); }
  }
  food = constrain(food, 0.0f, 100.0f);
  joy = constrain(joy, 0.0f, 100.0f);
  energy = constrain(energy, 0.0f, 100.0f);
  if (now - lastSave > 60000) { lastSave = now; saveStats(); }
}

void updatePet() {
  uint32_t now = millis();

  if (py > 0 || vy > 0) {
    py += vy; vy -= 0.45f;
    if (py <= 0) { py = 0; vy = 0; }
  }

  if (now > stateUntil) {
    if (state == DANCE && loudFrames > 20) setState(DANCE, 1500);
    else if (state == SLEEP) { setState(IDLE, 500); }
    else pickNext();
  }

  switch (state) {
    case WALK:   px += dir * 0.9f; break;
    case SCARED: px += dir * 2.2f; break;
    case DANCE:  if ((frame / 8) % 2 == 0) jump(2.2f); px += sinf(frame * 0.2f) * 0.8f; break;
    case PLAY:   px += dir * 1.6f; if (frame % 18 == 0) jump(3.5f); break;
    case HAPPY:  if (frame % 3 == 0) emitSparks(1); break;
    default: break;
  }
  if (px < 42)     { px = 42;     dir = 1; }
  if (px > W - 42) { px = W - 42; dir = -1; }

  for (auto& s : sparks) if (s.life > 0) { s.y += s.vy; s.life--; }

  if (now > nextBlink) { blinkUntil = now + 140; nextBlink = now + 2000 + random(0, 3000); }
}

// ---------- рисование ----------
void drawSpark(int cx, int cy, int r, uint16_t col) {
  int d = r * 2 / 3;
  cv.drawLine(cx - r, cy, cx + r, cy, col);
  cv.drawLine(cx, cy - r, cx, cy + r, col);
  cv.drawLine(cx - d, cy - d, cx + d, cy + d, col);
  cv.drawLine(cx - d, cy + d, cx + d, cy - d, col);
}

// Пиксельная буква z (сон)
void drawZ(int x, int y, int k) {
  static const uint8_t g[8][2] = {{0,0},{1,0},{2,0},{3,0},{2,1},{1,2},{0,3},{1,3}};
  for (auto& p : g) cv.fillRect(x + p[0] * k, y + p[1] * k, k, k, C_CREAM);
  cv.fillRect(x + 2 * k, y + 3 * k, k, k, C_CREAM);
  cv.fillRect(x + 3 * k, y + 3 * k, k, k, C_CREAM);
}

// Рисуем клетками сетки питомца: колонка c, строка r, ширина w, высота h
void pr(int ox, int oy, int c, int r, int w, int h, uint16_t col) {
  cv.fillRect(ox + c * U, oy + r * U, w * U, h * U, col);
}

void drawPet() {
  int x = (int)px;
  int feet = GROUND - (int)py;
  bool walking = (state == WALK || state == SCARED || state == PLAY);
  bool happy = (state == HAPPY || state == DANCE || state == PLAY);
  bool sad = (food < 25 || joy < 25);
  bool blink = millis() < blinkUntil;

  int ox = x - (13 * U) / 2;
  int oy = feet - 7 * U;
  if (state == SCARED) ox += (frame % 2) ? 1 : -1;       // дрожит
  if (state == SLEEP) oy += ((frame / 20) % 2) ? 1 : 0;  // дышит

  // тень
  int sw = 32 - (int)(py / 3); if (sw < 14) sw = 14;
  cv.fillRect(x - sw, GROUND, sw * 2, 3, C_SHADOW);

  // лапки (4 штуки), при ходьбе шагают парами
  int phase = (frame / 6) % 2;
  for (int i = 0; i < 4; i++) {
    int lh = (walking && (i % 2) == phase) ? 1 : 2;
    pr(ox, oy, 3 + i * 2, 5, 1, lh, C_BODY);
  }
  // тело
  pr(ox, oy, 2, 0, 9, 5, C_BODY);
  // ручки
  bool lUp = false, rUp = false;
  if (state == DANCE) { lUp = ((frame / 8) % 2) == 0; rUp = !lUp; }
  else if (happy || state == SCARED) { lUp = rUp = true; }
  // опущенные ручки - короткие "пеньки" по бокам, поднятые - вертикальные палочки вверх
  if (lUp) pr(ox, oy, 1, -2, 1, 4, C_BODY); else pr(ox, oy, 0, state == SLEEP ? 3 : 2, 2, 2, C_BODY);
  if (rUp) pr(ox, oy, 11, -2, 1, 4, C_BODY); else pr(ox, oy, 11, state == SLEEP ? 3 : 2, 2, 2, C_BODY);

  // глаза
  int cl = 4 + dir, cr = 8 + dir;
  int cs[2] = {cl, cr};
  for (int c : cs) {
    if (state == SLEEP)       pr(ox, oy, c - 1, 2, 3, 1, C_EYE);
    else if (happy) { pr(ox, oy, c - 1, 2, 1, 1, C_EYE); pr(ox, oy, c, 1, 1, 1, C_EYE); pr(ox, oy, c + 1, 2, 1, 1, C_EYE); }
    else if (state == SCARED) pr(ox, oy, c, 1, 1, 3, C_EYE);
    else if (blink)           pr(ox, oy, c, 2, 1, 1, C_EYE);
    else if (sad)             pr(ox, oy, c, 2, 1, 2, C_EYE);
    else                      pr(ox, oy, c, 1, 1, 2, C_EYE);
  }
  // рот только когда ест или пугается
  if (state == EAT) pr(ox, oy, 5, 3, 3, ((frame / 5) % 2) ? 2 : 1, C_EYE);
  if (state == SCARED) pr(ox, oy, 6, 3, 1, 2, C_EYE);

  // сон: плывущие вверх "z"
  if (state == SLEEP) {
    int t = (frame / 4) % 40;
    drawZ(x + 30 + t / 4, oy - 8 - t / 2, 4);
    drawZ(x + 50 + t / 5, oy - 30 - t / 2, 3);
  }

  // еда / мячик
  if (state == EAT) {
    int fx = x + dir * 46;
    cv.fillRect(fx - 5, GROUND - 10, 10, 10, C_RED);
    cv.fillRect(fx - 1, GROUND - 13, 2, 3, C_GREEN);
  }
  if (state == PLAY) {
    int bx = x + dir * 46;
    int by = GROUND - 8 - (int)fabsf(sinf(frame * 0.25f) * 22);
    cv.fillRect(bx - 4, by, 8, 8, C_CYAN);
    cv.fillRect(bx - 3, by + 1, 2, 2, C_CREAM);
  }
}

void draw() {
  cv.fillSprite(C_BG);
  cv.fillRect(0, GROUND, W, H - GROUND, C_FLOOR);
  cv.drawFastHLine(0, GROUND, W, C_LINE);

  drawPet();
  for (auto& s : sparks) if (s.life > 0) drawSpark((int)s.x, (int)s.y, s.life > 10 ? 4 : 2, C_SPARK);

  if (debugOverlay) {
    cv.setTextColor(C_CREAM);
    cv.setTextSize(1); cv.setCursor(4, 4); cv.printf("lvl %d  floor %d  slow %d", (int)level, (int)noiseFloor, (int)slowLevel);
  }
  cv.pushSprite(0, 0);
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(120);
  cv.createSprite(W, H);
  cv.setFont(&fonts::Font0);

  // микрофон и динамик делят один I2S -> динамик выключаем
  M5Cardputer.Speaker.end();
  auto mc = M5Cardputer.Mic.config();
  mc.sample_rate = MIC_SR;
  M5Cardputer.Mic.config(mc);
  M5Cardputer.Mic.begin();

  prefs.begin("pet", false);
  food   = prefs.getUChar("food", 70);
  joy    = prefs.getUChar("joy", 70);
  energy = prefs.getUChar("energy", 90);
  if (food < 20) food = 20;

  randomSeed(esp_random());
  setState(IDLE, 1000);
}

void loop() {
  M5Cardputer.update();
  frame++;
  handleKeys();
  updateMic();
  reactToSound();
  updateStats();
  updatePet();
  draw();
  delay(15);
}
