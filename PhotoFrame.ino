/* 
 * ESP8266 + ST7735S (128x160)
 * PhotoFrame JPEG + QR + Game (Tetris-like, 12 cột)
 * WiFi AP: PhotoFrameDemo / 12345678  ->  http://192.168.4.1
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// ================= TFT & BUTTON PINS =================
#define TFT_CS   16  // D0
#define TFT_DC   4   // D2
#define TFT_RST  2   // D4

const uint8_t BTN_TOGGLE = D1; // GPIO5  - nút chọn / menu / xoay khối
const uint8_t BTN_NEXT   = 3;  // RX/GPIO3 - nút NEXT (phải)
const uint8_t BTN_PREV   = D3; // GPIO0    - nút PREV (trái)

// ================= GENERAL CONFIG =================
#define SPI_SPEED_HZ 40000000UL

const char* ap_ssid     = "PhotoFrameDemo";
const char* ap_password = "12345678";

const int   MAX_IMAGES       = 16;
const size_t MAX_UPLOAD_SIZE = 450000;

#define CONFIG_FILE "/config.txt"

// ================= GLOBALS =================
ESP8266WebServer server(80);
Adafruit_ST7735   tft(TFT_CS, TFT_DC, TFT_RST);

String images[MAX_IMAGES];
int    numImages    = 0;
int    currentIndex = 0;

String qrImage;
bool   viewingQR = false;

// --------- UI modes ----------
enum UiMode : uint8_t {
  MODE_SHOW = 0,   // PhotoFrame
  MODE_INFO = 1,   // Màn hình thông tin + WiFi AP
  MODE_MENU = 2,   // Menu cài đặt
  MODE_GAME = 3    // Game Tetris-like
};

volatile UiMode uiMode = MODE_SHOW;

// “Chế độ nội dung” khi thoát INFO / MENU
// false = PhotoFrame, true = Game
bool activeGame = false;

// ================= UI COLOR THEME =================
const uint16_t uiColorVals[] = {
  ST77XX_RED,
  ST77XX_GREEN,
  ST77XX_BLUE,
  ST77XX_CYAN,
  ST77XX_MAGENTA,
  ST77XX_YELLOW,
  ST77XX_WHITE
};

const char* uiColorNames[] = {
  "DO",
  "XANH LA",
  "XANH DUONG",
  "CYAN",
  "HONG",
  "VANG",
  "TRANG"
};

const uint8_t UI_COLOR_COUNT = sizeof(uiColorVals) / sizeof(uiColorVals[0]);

uint8_t uiColorIdx = 3; // mặc định CYAN
#define UI_COLOR (uiColorVals[uiColorIdx])

// ================= MENU STATE =================
const uint8_t MENU_ITEMS = 3; // 0: UI Color, 1: Reset, 2: Game/PhotoFrame
uint8_t menuIndex  = 0;
bool    menuEditing = false; // chỉ dùng cho mục 0 (chọn màu)

// ================= FILE UPLOAD / WIFI STATE =================
File     fsUploadFile;
volatile bool isUploading  = false;
bool     apEnabled         = false;
bool     apArmPending      = false;
uint32_t apArmTimeMs       = 0;

// ================= BUTTON HELPER =================
const uint16_t LONG_PRESS_MS      = 1000;
const uint16_t REPEAT_DELAY_MS    = 220;  // giữ bao lâu thì bắt đầu chạy liên tục
const uint16_t REPEAT_RATE_MS     = 80;   // tốc độ chạy liên tục (ms/step)

class Button {
  uint8_t  pin;
  uint32_t pressStart  = 0;
  uint32_t lastRelease = 0;
  uint16_t debounceMs;
  bool     wasPressed  = false;
  bool     longHandled = false;

public:
  Button(uint8_t p, uint16_t db = 50) : pin(p), debounceMs(db) {}

  void begin() {
    pinMode(pin, INPUT_PULLUP);
  }

  bool clicked() {
    bool s = (digitalRead(pin) == LOW);
    uint32_t now = millis();

    if (s && !wasPressed) {
      if (now - lastRelease > debounceMs) {
        pressStart = now;
        wasPressed = true;
        longHandled = false;
      }
    } else if (!s && wasPressed) {
      wasPressed   = false;
      lastRelease  = now;
      uint32_t held = now - pressStart;
      if (held >= debounceMs && held < LONG_PRESS_MS && !longHandled) {
        return true;
      }
    }
    return false;
  }

  bool longPressed() {
    if (!wasPressed || longHandled) return false;
    if (millis() - pressStart >= LONG_PRESS_MS) {
      longHandled = true;
      return true;
    }
    return false;
  }

  bool isDown() const {
    return (digitalRead(pin) == LOW);
  }

  uint32_t holdMs() const {
    if (isDown()) return millis() - pressStart;
    return 0;
  }
};

Button btnToggle(BTN_TOGGLE);
Button btnNext(BTN_NEXT);
Button btnPrev(BTN_PREV);

// auto-repeat cho NEXT / PREV trong game
uint32_t repeatNextAt = 0;
uint32_t repeatPrevAt = 0;
bool     hardDropLatched = false;

// ================= GAME CONFIG & STATE =================
#define COLOR_GRID 0x4208  // xám đậm

// mở rộng 12 cột, 20 hàng, mỗi ô 6px
const uint8_t GAME_COLS  = 12;
const uint8_t GAME_ROWS  = 20;
const uint8_t GAME_CELL  = 6;
const int16_t GAME_ORIGIN_X = 4;
const int16_t GAME_ORIGIN_Y = 4;

struct GameConfig {
  uint8_t  cols;
  uint8_t  rows;
  uint8_t  cell;
  uint16_t bgColor;
  uint16_t gridColor;
  uint16_t borderColor;
  uint16_t pieceColor;
  uint16_t lockedColor;
  uint16_t textColor;
  uint16_t dropMs;
};

GameConfig gameCfg = {
  GAME_COLS,
  GAME_ROWS,
  GAME_CELL,
  ST77XX_BLACK,
  COLOR_GRID,
  ST77XX_CYAN,
  ST77XX_CYAN,
  ST77XX_WHITE,
  ST77XX_YELLOW,
  500
};

uint8_t gameBoard[GAME_ROWS][GAME_COLS];

enum GameState : uint8_t {
  GAME_IDLE = 0,
  GAME_PLAYING,
  GAME_OVER
};

GameState gameState   = GAME_IDLE;
uint32_t  gameScore   = 0;
uint32_t  gameBestScore = 0;   // điểm cao nhất đã lưu
uint32_t  nextDropMs  = 0;
uint16_t  dropIntervalMs = 500;

// ----- Tetris pieces (4-block) -----
struct PieceRotation {
  int8_t x[4];
  int8_t y[4];
};

struct PieceDef {
  uint8_t       rotationCount;
  PieceRotation rotation[4];
};

// 7 khối: I, O, T, L, J, S, Z
const PieceDef pieces[7] = {
  // I
  {
    2,
    {
      { {0,1,2,3}, {1,1,1,1} }, // ----
      { {2,2,2,2}, {0,1,2,3} }, // |
      { {0,0,0,0}, {0,0,0,0} },
      { {0,0,0,0}, {0,0,0,0} }
    }
  },
  // O
  {
    1,
    {
      { {1,2,1,2}, {1,1,2,2} },
      { {0,0,0,0}, {0,0,0,0} },
      { {0,0,0,0}, {0,0,0,0} },
      { {0,0,0,0}, {0,0,0,0} }
    }
  },
  // T
  {
    4,
    {
      { {1,0,1,2}, {0,1,1,1} },
      { {1,1,1,2}, {0,1,2,1} },
      { {0,1,2,1}, {1,1,1,2} },
      { {1,1,1,0}, {0,1,2,1} }
    }
  },
  // L
  {
    4,
    {
      { {2,0,1,2}, {0,1,1,1} },
      { {1,1,1,2}, {0,1,2,2} },
      { {0,1,2,0}, {1,1,1,2} },
      { {0,1,1,1}, {0,0,1,2} }
    }
  },
  // J
  {
    4,
    {
      { {0,0,1,2}, {0,1,1,1} },
      { {1,1,1,2}, {0,1,2,0} },
      { {0,1,2,2}, {1,1,1,2} },
      { {1,1,0,1}, {0,1,2,2} }
    }
  },
  // S
  {
    2,
    {
      { {1,2,0,1}, {0,0,1,1} },
      { {0,0,1,1}, {0,1,1,2} },
      { {0,0,0,0}, {0,0,0,0} },
      { {0,0,0,0}, {0,0,0,0} }
    }
  },
  // Z
  {
    2,
    {
      { {0,1,1,2}, {0,0,1,1} },
      { {1,0,1,0}, {0,1,1,2} },
      { {0,0,0,0}, {0,0,0,0} },
      { {0,0,0,0}, {0,0,0,0} }
    }
  }
};

uint8_t currentPieceIndex = 0;
uint8_t currentRotation   = 0;
int8_t  currentX          = 3;
int8_t  currentY          = -3;

uint8_t nextPieceIndex    = 0;  // preview

// lưu vị trí cũ để erase từng bước
int8_t  lastX = 0;
int8_t  lastY = 0;
uint8_t lastRot = 0;

// ================= FORWARD DECLARATIONS =================
void scanImages();
void drawCurrentImage();
void showWelcome();
void showInfoScreen();
void showMenuScreen();
void gameStart();
void goToActiveMode();
bool gameCheckCollision(int8_t testX, int8_t testY, uint8_t testRot);
void gameLockPiece();
void gameSpawnPiece();

// ================= CONFIG SAVE / LOAD =================
void onUiColorChanged() {
  gameCfg.borderColor = UI_COLOR;
  gameCfg.pieceColor  = UI_COLOR;
}

void saveConfig() {
  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) {
    Serial.println("✗ Khong the luu config");
    return;
  }
  // Dòng 1: uiColorIdx
  // Dòng 2: gameBestScore
  f.printf("%d\n", (int)uiColorIdx);
  f.printf("%lu\n", (unsigned long)gameBestScore);
  f.close();
  Serial.printf("✓ Luu config: color=%d (%s), best=%lu\n",
                (int)uiColorIdx, uiColorNames[uiColorIdx],
                (unsigned long)gameBestScore);
}

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) {
    Serial.println("⚠ Khong tim thay config, dung mac dinh");
    uiColorIdx = 3;
    gameBestScore = 0;
    onUiColorChanged();
    return false;
  }
  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) {
    Serial.println("✗ Khong the doc config");
    uiColorIdx = 3;
    gameBestScore = 0;
    onUiColorChanged();
    return false;
  }
  String line1 = f.readStringUntil('\n');
  String line2 = f.readStringUntil('\n');
  f.close();

  int color = line1.toInt();
  if (color < 0 || color >= UI_COLOR_COUNT) color = 3;
  uiColorIdx = (uint8_t)color;
  onUiColorChanged();

  if (line2.length() > 0) {
    gameBestScore = (uint32_t)line2.toInt();
  } else {
    gameBestScore = 0;
  }

  Serial.printf("✓ Tai config: color=%d (%s), best=%lu\n",
                (int)uiColorIdx, uiColorNames[uiColorIdx],
                (unsigned long)gameBestScore);
  return true;
}

void resetConfig() {
  if (LittleFS.exists(CONFIG_FILE)) {
    LittleFS.remove(CONFIG_FILE);
    Serial.println("✓ Da xoa config");
  }
  uiColorIdx = 3; // CYAN
  gameBestScore = 0;
  onUiColorChanged();
  saveConfig();
  Serial.println("✓ Dat lai config ve mac dinh (CYAN, best=0)");
}

// ================= STRING & IMAGE HELPERS =================
bool endsWithIC(const String& s, const char* suf) {
  String a = s;
  a.toLowerCase();
  String b = suf;
  b.toLowerCase();
  return a.endsWith(b);
}

bool isJPG(const String& s) {
  return endsWithIC(s, ".jpg") || endsWithIC(s, ".jpeg");
}

void scanImages() {
  numImages = 0;
  qrImage   = "";

  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    String fn = dir.fileName();
    if (!fn.startsWith("/")) fn = "/" + fn;

    if (isJPG(fn)) {
      if (fn.indexOf("qr") >= 0 || fn.indexOf("QR") >= 0) {
        if (qrImage.length() == 0) qrImage = fn;
      } else if (numImages < MAX_IMAGES) {
        images[numImages++] = fn;
      }
    }
  }

  Serial.printf("Tim thay %d anh, QR: %s\n", numImages, qrImage.c_str());
}

// ================= TEXT HELPERS =================
void centerText(const char* txt, int y, uint8_t size = 1, uint16_t color = ST77XX_WHITE) {
  tft.setTextSize(size);
  tft.setTextColor(color);

  int16_t x1, y1;
  uint16_t w, h;
  tft.getTextBounds(String(txt), 0, 0, &x1, &y1, &w, &h);
  int x = (tft.width() - w) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(txt);
}

// ================= JPEG CALLBACK =================
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if (y >= tft.height()) return false;
  tft.startWrite();
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  tft.endWrite();
  return true;
}

// ================= PHOTOFRAME SCREENS =================
void showWelcome() {
  tft.fillScreen(ST77XX_WHITE);
  int cx = 64, cy = 60;
  tft.drawCircle(cx, cy, 30, ST77XX_BLACK);
  tft.drawCircle(cx, cy, 31, ST77XX_BLACK);
  tft.fillCircle(cx - 12, cy - 8, 3, ST77XX_BLACK);
  tft.fillCircle(cx + 12, cy - 8, 3, ST77XX_BLACK);
  for (int i = -15; i <= 15; i++) {
    int x = cx + i;
    int y = cy + 8 + (int)(sqrt(225 - i * i) / 3);
    tft.drawPixel(x, y, ST77XX_BLACK);
    tft.drawPixel(x, y + 1, ST77XX_BLACK);
  }
  centerText("Chua co anh!", 120, 1, ST77XX_BLACK);
}

void showInfoScreen() {
  tft.fillScreen(ST77XX_BLACK);
  centerText("HAOTAI", 8, 3, UI_COLOR);
  centerText("PhotoFrame", 36, 2, ST77XX_WHITE);

  centerText("WiFi: PhotoFrameDemo", 80, 1, ST77XX_YELLOW);
  centerText("Pass: 12345678", 95, 1, ST77XX_YELLOW);
  centerText("IP: 192.168.4.1", 110, 1, ST77XX_YELLOW);
}

static void printMenuLine(int y, const char* text, bool selected, uint16_t color = ST77XX_WHITE) {
  tft.setTextSize(1);
  tft.setTextColor(selected ? UI_COLOR : color);
  tft.setCursor(10, y);
  if (selected) tft.print("> ");
  else          tft.print("  ");
  tft.print(text);
}

void showMenuScreen() {
  tft.fillScreen(ST77XX_BLACK);
  centerText("SETTINGS", 10, 2, UI_COLOR);

  // 0. UI Color
  printMenuLine(40, "1. Mau UI", (menuIndex == 0));
  if (menuIndex == 0) {
    tft.setTextSize(1);
    tft.setCursor(28, 56);
    tft.setTextColor(uiColorVals[uiColorIdx]);
    tft.print(uiColorNames[uiColorIdx]);
  }

  // 1. Reset config
  printMenuLine(70, "2. Dat lai config", (menuIndex == 1));
  if (menuIndex == 1) {
    centerText("(Ve mac dinh)", 86, 1, ST77XX_YELLOW);
  }

  // 2. Game / PhotoFrame
  String line3 = activeGame ? "3. Che do PhotoFrame" : "3. Che do choi game";
  printMenuLine(100, line3.c_str(), (menuIndex == 2));
}

// ================= PHOTOFRAME DISPLAY =================
void drawCurrentImage() {
  if (uiMode != MODE_SHOW) return;

  String path;
  if (viewingQR) {
    if (qrImage.length() == 0) {
      tft.fillScreen(ST77XX_BLACK);
      centerText("Khong co anh QR", 70, 1, ST77XX_RED);
      return;
    }
    path = qrImage;
  } else {
    if (numImages == 0) {
      showWelcome();
      return;
    }
    path = images[currentIndex];
  }

  tft.fillScreen(ST77XX_BLACK);
  TJpgDec.setSwapBytes(false);
  int rc = TJpgDec.drawFsJpg(0, 0, path.c_str(), LittleFS);
  if (rc != 0) {
    tft.setCursor(8, 70);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(1);
    tft.print("JPEG ERROR: ");
    tft.println(rc);
  }
}

// ================= GAME HELPERS =================
void gameClearBoard() {
  for (uint8_t y = 0; y < GAME_ROWS; y++)
    for (uint8_t x = 0; x < GAME_COLS; x++)
      gameBoard[y][x] = 0;
}

bool gameCheckCollision(int8_t testX, int8_t testY, uint8_t testRot) {
  const PieceDef& p = pieces[currentPieceIndex];
  const PieceRotation& r = p.rotation[testRot];

  for (uint8_t i = 0; i < 4; i++) {
    int8_t bx = testX + r.x[i];
    int8_t by = testY + r.y[i];

    if (bx < 0 || bx >= GAME_COLS) return true;
    if (by >= GAME_ROWS) return true;
    if (by >= 0 && gameBoard[by][bx]) return true;
  }
  return false;
}

void gameDrawScorePanel() {
  int16_t bh = gameCfg.rows * gameCfg.cell;
  int16_t y  = GAME_ORIGIN_Y + bh + 4;

  tft.fillRect(0, y, 128, 160 - y, gameCfg.bgColor);
  tft.setTextSize(1);
  tft.setTextColor(gameCfg.textColor);
  tft.setCursor(4, y);
  tft.print("Score: ");
  tft.print(gameScore);
  tft.setCursor(4, y + 10);
  tft.print("Best : ");
  tft.print(gameBestScore);
}

void gameDrawNextBox() {
  int16_t bw = gameCfg.cols * gameCfg.cell;
  int16_t panelX = GAME_ORIGIN_X + bw + 6;
  int16_t panelY = GAME_ORIGIN_Y + 8;
  int16_t boxSize = gameCfg.cell * 4 + 2;

  tft.setTextSize(1);
  tft.setTextColor(gameCfg.textColor);
  tft.setCursor(panelX, panelY - 8);
  tft.print("NEXT");

  tft.drawRect(panelX, panelY, boxSize, boxSize, gameCfg.borderColor);
  tft.fillRect(panelX + 1, panelY + 1, boxSize - 2, boxSize - 2, gameCfg.bgColor);

  const PieceDef& p = pieces[nextPieceIndex];
  const PieceRotation& r = p.rotation[0];

  for (uint8_t i = 0; i < 4; i++) {
    int8_t bx = r.x[i];
    int8_t by = r.y[i];
    if (bx < 0 || bx > 3 || by < 0 || by > 3) continue;

    int16_t px = panelX + 1 + bx * gameCfg.cell;
    int16_t py = panelY + 1 + by * gameCfg.cell;
    tft.fillRect(px + 1, py + 1, gameCfg.cell - 2, gameCfg.cell - 2, gameCfg.pieceColor);
  }
}

void gameDrawStatic() {
  // Vẽ toàn bộ board + lưới + khối đã khóa + next + score
  tft.fillScreen(gameCfg.bgColor);

  int16_t bw = gameCfg.cols * gameCfg.cell;
  int16_t bh = gameCfg.rows * gameCfg.cell;

  // border
  tft.drawRect(GAME_ORIGIN_X - 1, GAME_ORIGIN_Y - 1, bw + 2, bh + 2, gameCfg.borderColor);

  // board cells + grid / locked
  for (uint8_t y = 0; y < GAME_ROWS; y++) {
    for (uint8_t x = 0; x < GAME_COLS; x++) {
      int16_t px = GAME_ORIGIN_X + x * gameCfg.cell;
      int16_t py = GAME_ORIGIN_Y + y * gameCfg.cell;

      if (gameBoard[y][x]) {
        tft.fillRect(px + 1, py + 1, gameCfg.cell - 1, gameCfg.cell - 1, gameCfg.lockedColor);
      } else {
        tft.drawPixel(px + gameCfg.cell / 2, py + gameCfg.cell / 2, gameCfg.gridColor);
      }
    }
  }

  gameDrawNextBox();
  gameDrawScorePanel();
}

// erase khối tại vị trí cũ, chỉ xoá ô nền (không đụng khối đã khóa)
void gameErasePieceAt(int8_t ox, int8_t oy, uint8_t orot) {
  const PieceDef& p = pieces[currentPieceIndex];
  const PieceRotation& r = p.rotation[orot];

  for (uint8_t i = 0; i < 4; i++) {
    int8_t bx = ox + r.x[i];
    int8_t by = oy + r.y[i];
    if (bx < 0 || bx >= GAME_COLS || by < 0 || by >= GAME_ROWS) continue;
    if (gameBoard[by][bx]) continue; // đã khóa thì không xóa

    int16_t px = GAME_ORIGIN_X + bx * gameCfg.cell;
    int16_t py = GAME_ORIGIN_Y + by * gameCfg.cell;

    tft.fillRect(px + 1, py + 1, gameCfg.cell - 1, gameCfg.cell - 1, gameCfg.bgColor);
    tft.drawPixel(px + gameCfg.cell / 2, py + gameCfg.cell / 2, gameCfg.gridColor);
  }
}

void gameDrawCurrentPiece() {
  const PieceDef& p = pieces[currentPieceIndex];
  const PieceRotation& r = p.rotation[currentRotation];

  for (uint8_t i = 0; i < 4; i++) {
    int8_t bx = currentX + r.x[i];
    int8_t by = currentY + r.y[i];
    if (bx < 0 || bx >= GAME_COLS || by < 0 || by >= GAME_ROWS) continue;

    int16_t px = GAME_ORIGIN_X + bx * gameCfg.cell;
    int16_t py = GAME_ORIGIN_Y + by * gameCfg.cell;
    tft.fillRect(px + 1, py + 1, gameCfg.cell - 1, gameCfg.cell - 1, gameCfg.pieceColor);
  }

  lastX   = currentX;
  lastY   = currentY;
  lastRot = currentRotation;
}

void gameClearLines() {
  uint8_t lines = 0;

  for (int8_t y = GAME_ROWS - 1; y >= 0; y--) {
    bool full = true;
    for (uint8_t x = 0; x < GAME_COLS; x++) {
      if (!gameBoard[y][x]) {
        full = false;
        break;
      }
    }
    if (full) {
      lines++;
      // dồn các hàng trên xuống
      for (int8_t yy = y; yy > 0; yy--) {
        for (uint8_t x = 0; x < GAME_COLS; x++) {
          gameBoard[yy][x] = gameBoard[yy - 1][x];
        }
      }
      for (uint8_t x = 0; x < GAME_COLS; x++) gameBoard[0][x] = 0;
      y++; // kiểm tra lại hàng này sau khi dồn
    }
  }

  if (lines) {
    gameScore += lines * 100;
    if (dropIntervalMs > 120) {
      uint16_t reduce = lines * 15;
      if (reduce > dropIntervalMs - 120) reduce = dropIntervalMs - 120;
      dropIntervalMs -= reduce;
    }
  }
}

// spawn khối mới, nếu va chạm ngay từ đầu thì GAME OVER
void gameSpawnPiece() {
  currentPieceIndex = nextPieceIndex;
  nextPieceIndex    = random(0, 7);
  currentRotation   = 0;
  currentX          = gameCfg.cols / 2 - 2;
  currentY          = -3;

  if (gameCheckCollision(currentX, currentY, currentRotation)) {
    gameState = GAME_OVER;

    if (gameScore > gameBestScore) {
      gameBestScore = gameScore;
      saveConfig();
    }

    gameDrawStatic();
    centerText("GAME OVER", 60, 2, ST77XX_RED);
    delay(700);

    activeGame  = false;
    uiMode      = MODE_MENU;
    menuIndex   = 0;
    menuEditing = false;
    showMenuScreen();
    return;
  }

  gameDrawStatic();
  gameDrawCurrentPiece();
  nextDropMs = millis() + dropIntervalMs;
}

void gameLockPiece() {
  const PieceDef& p = pieces[currentPieceIndex];
  const PieceRotation& r = p.rotation[currentRotation];

  for (uint8_t i = 0; i < 4; i++) {
    int8_t bx = currentX + r.x[i];
    int8_t by = currentY + r.y[i];
    if (by < 0 || by >= GAME_ROWS || bx < 0 || bx >= GAME_COLS) continue;
    gameBoard[by][bx] = 1;
  }

  // kiểm tra và xóa hàng
  gameClearLines();

  // nếu sau khi khóa, hàng trên cùng có ô nào != 0 -> thua
  bool topBlocked = false;
  for (uint8_t x = 0; x < GAME_COLS; x++) {
    if (gameBoard[0][x]) {
      topBlocked = true;
      break;
    }
  }

  if (topBlocked) {
    gameState = GAME_OVER;
    if (gameScore > gameBestScore) {
      gameBestScore = gameScore;
      saveConfig();
    }

    gameDrawStatic();
    centerText("GAME OVER", 60, 2, ST77XX_RED);
    delay(700);

    activeGame  = false;
    uiMode      = MODE_MENU;
    menuIndex   = 0;
    menuEditing = false;
    showMenuScreen();
    return;
  }

  // tiếp tục chơi: spawn khối mới
  if (gameState == GAME_PLAYING) {
    gameSpawnPiece();
  }
}

void gameMove(int8_t dx) {
  if (gameState != GAME_PLAYING) return;
  int8_t nx = currentX + dx;
  if (!gameCheckCollision(nx, currentY, currentRotation)) {
    gameErasePieceAt(lastX, lastY, lastRot);
    currentX = nx;
    gameDrawCurrentPiece();
  }
}

void gameRotateCurrent() {
  if (gameState != GAME_PLAYING) return;
  const PieceDef& p = pieces[currentPieceIndex];
  uint8_t nr = (currentRotation + 1) % p.rotationCount;
  if (!gameCheckCollision(currentX, currentY, nr)) {
    gameErasePieceAt(lastX, lastY, lastRot);
    currentRotation = nr;
    gameDrawCurrentPiece();
  }
}

void gameTick() {
  if (uiMode != MODE_GAME) return;
  if (gameState != GAME_PLAYING) return;

  uint32_t now = millis();
  if ((int32_t)(now - nextDropMs) >= 0) {
    nextDropMs = now + dropIntervalMs;
    int8_t ny = currentY + 1;
    if (!gameCheckCollision(currentX, ny, currentRotation)) {
      gameErasePieceAt(lastX, lastY, lastRot);
      currentY = ny;
      gameDrawCurrentPiece();
    } else {
      gameLockPiece();
    }
  }
}

// hard drop: rơi thẳng xuống
void gameHardDrop() {
  if (gameState != GAME_PLAYING) return;
  while (!gameCheckCollision(currentX, currentY + 1, currentRotation)) {
    gameErasePieceAt(lastX, lastY, lastRot);
    currentY++;
    gameDrawCurrentPiece();
  }
  gameLockPiece();
}

void gameStart() {
  gameClearBoard();
  gameScore      = 0;
  dropIntervalMs = gameCfg.dropMs;
  gameState      = GAME_PLAYING;

  nextPieceIndex = random(0, 7);
  repeatNextAt   = millis() + REPEAT_DELAY_MS;
  repeatPrevAt   = millis() + REPEAT_DELAY_MS;
  hardDropLatched = false;

  gameSpawnPiece();
}

// ================= WIFI AP =================
void enableAP() {
  if (apEnabled) return;
  Serial.println("Bat AP...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  IPAddress ip(192, 168, 4, 1), gw(192, 168, 4, 1), mask(255, 255, 255, 0);
  WiFi.softAPConfig(ip, gw, mask);
  WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
  apEnabled = true;
  Serial.println("AP ON: " + WiFi.softAPIP().toString());
}

void disableAP() {
  if (!apEnabled) return;
  Serial.println("Tat AP...");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apEnabled = false;
}

// ================= WEB HANDLERS =================
void handleUploadDone() {
  server.send(200, "text/html",
              F("<!DOCTYPE html><meta charset='utf-8'><meta http-equiv='refresh' content='1;url=/'/>OK"));
}

void handleFileUpload() {
  HTTPUpload& up = server.upload();
  static size_t written = 0;

  if (up.status == UPLOAD_FILE_START) {
    isUploading = true;
    written = 0;

    String name = up.filename;
    name.toLowerCase();
    if (!isJPG(name)) {
      Serial.println("Tu choi: khong phai JPG");
      return;
    }

    bool isQR = (name.indexOf("qr") >= 0);
    if (!isQR && numImages >= MAX_IMAGES) {
      Serial.println("Day anh");
      return;
    }

    String newName = isQR ? "/qrcode.jpg" : ("/image" + String(numImages + 1) + ".jpg");
    Serial.println("Upload -> " + newName);
    fsUploadFile = LittleFS.open(newName, "w");
    if (!fsUploadFile) {
      Serial.println("Loi open file");
      return;
    }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (fsUploadFile) {
      fsUploadFile.write(up.buf, up.currentSize);
      written += up.currentSize;
      if (written > MAX_UPLOAD_SIZE) {
        String fname = fsUploadFile.name();
        fsUploadFile.close();
        LittleFS.remove(fname);
        Serial.println("File qua lon, xoa");
      }
    }
    yield();
    ESP.wdtFeed();
  } else if (up.status == UPLOAD_FILE_END) {
    if (fsUploadFile) {
      fsUploadFile.close();
      Serial.println("Upload xong: " + String(written) + " bytes");
    }
    delay(80);
    scanImages();
    if (numImages == 1 && uiMode == MODE_SHOW) {
      currentIndex = 0;
      drawCurrentImage();
    }
    isUploading = false;
  }
}

void handleList() {
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>File PhotoFrame</title>"
    "<style>"
    "body{font-family:system-ui,Arial;margin:0;padding:0;background:#0b1120;color:#e5e7eb}"
    ".wrap{max-width:720px;margin:24px auto;padding:16px}"
    ".card{background:#020617;border-radius:12px;padding:16px 18px;box-shadow:0 10px 30px rgba(0,0,0,.35)}"
    "h2{margin:0 0 12px;font-size:20px}"
    ".item{display:flex;justify-content:space-between;align-items:center;padding:8px 4px;border-bottom:1px solid #1f2937;font-size:14px}"
    ".item:last-child{border-bottom:none}"
    ".btn{padding:6px 10px;border-radius:999px;border:none;cursor:pointer;font-size:13px;text-decoration:none;color:#fff;background:#ef4444;box-shadow:0 6px 18px rgba(248,113,113,.4);transition:.15s}"
    ".btn:hover{transform:translateY(-1px);box-shadow:0 10px 25px rgba(248,113,113,.5)}"
    ".btn-back{background:#3b82f6;box-shadow:0 6px 18px rgba(59,130,246,.4)}"
    ".btn-back:hover{box-shadow:0 10px 25px rgba(59,130,246,.6)}"
    ".top-actions{margin-top:14px;display:flex;gap:8px;flex-wrap:wrap}"
    "</style></head><body><div class='wrap'><div class='card'>";

  html += "<h2>Danh sach anh (" + String(numImages) + ")</h2>";

  if (numImages > 0) {
    for (int i = 0; i < numImages; i++) {
      html += "<div class='item'><span>" + String(i + 1) + ". " + images[i] + "</span>";
      html += "<a class='btn' href='/delete?f=" + String(i) + "' onclick='return confirm(\"Xoa anh nay?\")'>Xoa</a></div>";
    }
  } else {
    html += "<p>Chua co anh nao.</p>";
  }

  if (qrImage.length() > 0) {
    html += "<h2 style='margin-top:18px'>Anh QR</h2>";
    html += "<div class='item'><span>" + qrImage + "</span>";
    html += "<a class='btn' href='/deleteqr' onclick='return confirm(\"Xoa anh QR?\")'>Xoa</a></div>";
  }

  html += "<div class='top-actions'><a class='btn btn-back' href='/'>Quay lai</a>";
  if (numImages > 0) {
    html += "<a class='btn' href='/deleteall' onclick='return confirm(\"Xoa TAT CA anh?\")'>Xoa tat ca</a>";
  }
  html += "</div></div></div></body></html>";

  server.send(200, "text/html", html);
}

void handleDelete() {
  if (!server.hasArg("f")) {
    server.send(400, "text/plain", "Missing param");
    return;
  }
  int idx = server.arg("f").toInt();
  if (idx < 0 || idx >= numImages) {
    server.send(400, "text/plain", "Bad index");
    return;
  }
  String fn = images[idx];
  if (LittleFS.remove(fn)) {
    for (int i = idx; i < numImages - 1; i++) images[i] = images[i + 1];
    numImages--;
    if (currentIndex >= numImages && numImages > 0) currentIndex = numImages - 1;
    if (numImages == 0) currentIndex = 0;
    if (uiMode == MODE_SHOW && !viewingQR) {
      if (numImages > 0) drawCurrentImage();
      else showWelcome();
    }
    server.send(200, "text/html",
                F("<meta http-equiv='refresh' content='1;url=/list'/>OK"));
  } else {
    server.send(500, "text/plain", "Delete error");
  }
}

void handleDeleteQR() {
  if (qrImage.length() > 0 && LittleFS.remove(qrImage)) {
    qrImage   = "";
    viewingQR = false;
    server.send(200, "text/html",
                F("<meta http-equiv='refresh' content='1;url=/list'/>OK"));
  } else {
    server.send(500, "text/plain", "Delete error");
  }
}

void handleDeleteAll() {
  for (int i = 0; i < numImages; i++) LittleFS.remove(images[i]);
  numImages    = 0;
  currentIndex = 0;
  viewingQR    = false;
  if (uiMode == MODE_SHOW) showWelcome();
  server.send(200, "text/html",
              F("<meta http-equiv='refresh' content='1;url=/'/>OK"));
}

void handleNotFound() {
  String path = server.uri();
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    String ct = "text/plain";
    if (path.endsWith(".js"))      ct = "application/javascript";
    else if (path.endsWith(".html")) ct = "text/html";
    else if (path.endsWith(".jpg")) ct = "image/jpeg";
    server.streamFile(f, ct);
    f.close();
    return;
  }
  server.send(404, "text/plain", "404");
}

void handleRoot() {
  if (LittleFS.exists("/index.html")) {
    File f = LittleFS.open("/index.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  } else {
    server.send(200, "text/html",
                "<html><body><h1>PhotoFrame</h1><p>Hay upload index.html len LittleFS.</p></body></html>");
  }
}

void setupWeb() {
  server.on("/",        HTTP_GET,  handleRoot);
  server.on("/upload",  HTTP_POST, handleUploadDone, handleFileUpload);
  server.on("/list",    HTTP_GET,  handleList);
  server.on("/delete",  HTTP_GET,  handleDelete);
  server.on("/deleteqr",HTTP_GET,  handleDeleteQR);
  server.on("/deleteall",HTTP_GET, handleDeleteAll);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server ready");
}

// ================= MODE SWITCH HELPER =================
void goToActiveMode() {
  if (activeGame) {
    uiMode = MODE_GAME;
    if (gameState == GAME_IDLE) {
      gameStart();
    } else {
      gameDrawStatic();
      gameDrawCurrentPiece();
    }
  } else {
    uiMode    = MODE_SHOW;
    viewingQR = false;
    if (numImages > 0) drawCurrentImage();
    else showWelcome();
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(100);
  Serial.println();
  Serial.println("╔═════════════════════╗");
  Serial.println("║   HAOTAI PhotoFrame ║");
  Serial.println("╚═════════════════════╝");

  // TFT
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.setSPISpeed(SPI_SPEED_HZ);
  tft.fillScreen(ST77XX_BLUE);
  centerText("Khoi tao...", 70, 1, ST77XX_WHITE);

  // FS
  if (!LittleFS.begin()) {
    tft.fillScreen(ST77XX_RED);
    centerText("Loi LittleFS!", 70, 1, ST77XX_WHITE);
    while (1) delay(1000);
  }

  // Load config
  loadConfig();

  // JPEG
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(false);
  TJpgDec.setCallback(tft_output);

  // Buttons
  btnToggle.begin();
  btnNext.begin();
  btnPrev.begin();

  // Web
  setupWeb();

  // Images
  scanImages();

  uiMode     = MODE_SHOW;
  activeGame = false;

  if (numImages > 0) drawCurrentImage();
  else showWelcome();

  randomSeed(ESP.getChipId());

  Serial.println("✓ Setup xong!");
  Serial.println("==============================");
}

// ================= LOOP =================
void loop() {
  if (apEnabled) server.handleClient();

  // INFO -> sau 1s bat AP
  if (uiMode == MODE_INFO && apArmPending && (int32_t)(millis() - apArmTimeMs) >= 0) {
    apArmPending = false;
    if (!apEnabled) enableAP();
  }

  // ---------- TOGGLE LONG PRESS ----------
  if (btnToggle.longPressed()) {
    if (uiMode == MODE_MENU) {
      // Trong MENU: nhấn giữ 1s -> về PhotoFrame
      activeGame  = false;
      uiMode      = MODE_SHOW;
      viewingQR   = false;
      menuEditing = false;
      if (numImages > 0) drawCurrentImage();
      else showWelcome();
      Serial.println("LONG TOGGLE in MENU -> PhotoFrame");
    } else {
      // Ở các chế độ khác: long press -> vào MENU
      uiMode      = MODE_MENU;
      viewingQR   = false;
      menuIndex   = 0;
      menuEditing = false;
      showMenuScreen();
      apArmPending = false;
      Serial.println("LONG TOGGLE -> MENU");
    }
  }

  // ---------- GAME TICK + AUTO-REPEAT + HARD DROP ----------
  if (uiMode == MODE_GAME) {
    gameTick();

    uint32_t now = millis();

    // Hard drop: NEXT + PREV cùng lúc
    if (gameState == GAME_PLAYING) {
      bool bothDown = btnNext.isDown() && btnPrev.isDown();
      if (bothDown && !hardDropLatched) {
        hardDropLatched = true;
        gameHardDrop();
      } else if (!bothDown) {
        hardDropLatched = false;
      }
    }

    // Auto-repeat trái/phải khi giữ
    if (uiMode == MODE_GAME && gameState == GAME_PLAYING) {
      // NEXT (phải)
      if (btnNext.isDown()) {
        uint32_t held = btnNext.holdMs();
        if (held > REPEAT_DELAY_MS && (int32_t)(now - repeatNextAt) >= 0) {
          gameMove(+1);
          repeatNextAt = now + REPEAT_RATE_MS;
        }
      } else {
        repeatNextAt = now + REPEAT_DELAY_MS;
      }

      // PREV (trái)
      if (btnPrev.isDown()) {
        uint32_t held = btnPrev.holdMs();
        if (held > REPEAT_DELAY_MS && (int32_t)(now - repeatPrevAt) >= 0) {
          gameMove(-1);
          repeatPrevAt = now + REPEAT_RATE_MS;
        }
      } else {
        repeatPrevAt = now + REPEAT_DELAY_MS;
      }
    }
  }

  // ---------- TOGGLE CLICK ----------
  if (btnToggle.clicked()) {
    if (uiMode == MODE_MENU) {
      // CHON trong MENU
      if (menuIndex == 0 && menuEditing) {
        // Lưu màu UI, thoát về chế độ đang active
        menuEditing = false;
        saveConfig();
        onUiColorChanged();
        goToActiveMode();
      } else {
        if (menuIndex == 0) {
          // Màu UI -> bật edit, dùng NEXT/PREV đổi màu
          menuEditing = true;
          showMenuScreen();
        } else if (menuIndex == 1) {
          // Reset config
          tft.fillScreen(ST77XX_BLACK);
          centerText("Dang reset...", 70, 1, ST77XX_YELLOW);
          resetConfig();
          delay(800);
          tft.fillScreen(ST77XX_BLACK);
          centerText("Da reset!", 70, 2, ST77XX_GREEN);
          centerText("Mau: CYAN", 96, 1, ST77XX_CYAN);
          delay(1200);
          goToActiveMode();
        } else if (menuIndex == 2) {
          // Game / PhotoFrame toggle
          activeGame = !activeGame;
          if (activeGame) {
            uiMode = MODE_GAME;
            gameStart();
          } else {
            uiMode = MODE_SHOW;
            viewingQR = false;
            if (numImages > 0) drawCurrentImage();
            else showWelcome();
          }
        }
      }
    } else if (uiMode == MODE_INFO) {
      // Thoat INFO
      apArmPending = false;
      disableAP();
      goToActiveMode();
      Serial.println("Thoat INFO");
    } else if (uiMode == MODE_SHOW) {
      // SHOW -> INFO
      uiMode = MODE_INFO;
      viewingQR = false;
      showInfoScreen();
      apArmPending = true;
      apArmTimeMs  = millis() + 1000;
      Serial.println("Vao INFO (giu 1s bat AP)");
    } else if (uiMode == MODE_GAME) {
      // Trong GAME: nhan TOGGLE de xoay khối
      gameRotateCurrent();
    }
  }

  // ---------- NEXT BUTTON ----------
  if (btnNext.longPressed()) {
    if (uiMode == MODE_SHOW) {
      // giu NEXT de toggle xem QR
      viewingQR = !viewingQR;
      drawCurrentImage();
      Serial.println("NEXT long -> toggle QR");
    }
  }

  if (btnNext.clicked()) {
    if (uiMode == MODE_GAME) {
      // GAME: nhấn 1 lần vẫn di chuyển 1 ô
      gameMove(+1);
    } else if (uiMode == MODE_SHOW && !viewingQR && numImages > 1) {
      currentIndex = (currentIndex + 1) % numImages;
      drawCurrentImage();
      Serial.printf("NEXT photo: %d/%d\n", currentIndex + 1, numImages);
    } else if (uiMode == MODE_MENU) {
      if (menuEditing && menuIndex == 0) {
        uiColorIdx = (uiColorIdx + 1) % UI_COLOR_COUNT;
        onUiColorChanged();
        showMenuScreen();
      } else {
        menuIndex = (menuIndex + 1) % MENU_ITEMS;
        showMenuScreen();
      }
    }
  }

  // ---------- PREV BUTTON ----------
  if (btnPrev.clicked()) {
    if (uiMode == MODE_GAME) {
      gameMove(-1);
    } else if (uiMode == MODE_SHOW && !viewingQR && numImages > 1) {
      currentIndex = (currentIndex - 1 + numImages) % numImages;
      drawCurrentImage();
      Serial.printf("PREV photo: %d/%d\n", currentIndex + 1, numImages);
    } else if (uiMode == MODE_MENU) {
      if (menuEditing && menuIndex == 0) {
        uiColorIdx = (uiColorIdx + UI_COLOR_COUNT - 1) % UI_COLOR_COUNT;
        onUiColorChanged();
        showMenuScreen();
      } else {
        menuIndex = (menuIndex + MENU_ITEMS - 1) % MENU_ITEMS;
        showMenuScreen();
      }
    }
  }

  yield();
  ESP.wdtFeed();
}
