/*
 * ESP8266 + ST7735S (128x160): JPEG/QR PhotoFrame
 * WiFi AP: PhotoFrameDemo / 12345678 → http://192.168.4.1
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <FS.h>
#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// ===== TFT pins =====
#define TFT_CS    16
#define TFT_DC     4
#define TFT_RST    2

// ===== Buttons =====
const uint8_t BTN_TOGGLE = D1;  // GPIO5
const uint8_t BTN_NEXT   = 3;   // RX/GPIO3
const uint8_t BTN_PREV   = D3;  // GPIO0

// ===== Config =====
#define SPI_SPEED_HZ 40000000UL
const char* ap_ssid     = "PhotoFrameDemo";
const char* ap_password = "12345678";
const int   MAX_IMAGES  = 16;
const size_t MAX_UPLOAD_SIZE = 450000;

// ===== Config File =====
#define CONFIG_FILE "/config.txt"

// ===== Globals =====
ESP8266WebServer server(80);
Adafruit_ST7735 tft(TFT_CS, TFT_DC, TFT_RST);

String images[MAX_IMAGES];
int    numImages = 0;
int    currentIndex = 0;

String qrImage = "";
bool   viewingQR = false;

// UI Mode
enum UiMode : uint8_t { MODE_SHOW=0, MODE_INFO=1, MODE_MENU=2 };
volatile UiMode uiMode = MODE_SHOW;

// ===== Color UI theme =====
const uint16_t uiColorVals[] = {
  ST77XX_RED, ST77XX_GREEN, ST77XX_BLUE, ST77XX_CYAN,
  ST77XX_MAGENTA, ST77XX_YELLOW, ST77XX_ORANGE, ST77XX_WHITE
};
const char* uiColorNames[] = {
  "RED","GREEN","BLUE","CYAN","MAGENTA","YELLOW","ORANGE","WHITE"
};
const uint8_t UI_COLOR_COUNT = sizeof(uiColorVals)/sizeof(uiColorVals[0]);
uint8_t uiColorIdx = 3; // CYAN
#define UI_COLOR (uiColorVals[uiColorIdx])

// ===== Menu state =====
uint8_t menuIndex = 0;         // 0: Color UI, 1: Reset Config
bool    menuEditing = false;

File fsUploadFile;
volatile bool isUploading = false;

bool apEnabled = false;
bool apArmPending = false;
uint32_t apArmTimeMs = 0;

// Auto-repeat
const uint16_t REPEAT_START_MS = 1000;
const uint16_t REPEAT_STEP_MS  = 120;
uint32_t nextRepeatNextMs = 0;
uint32_t nextRepeatPrevMs = 0;

// ===== Button helper =====
class Button {
  uint8_t pin;
  uint32_t pressStart = 0;
  uint32_t lastRelease = 0;
  uint16_t debounceMs = 50;
  bool wasPressed = false;
  bool longHandled = false;
public:
  Button(uint8_t p, uint16_t db=50): pin(p), debounceMs(db) {}
  void begin(){ pinMode(pin, INPUT_PULLUP); }

  bool clicked(){
    bool s = digitalRead(pin);
    uint32_t now = millis();
    if(s == LOW && !wasPressed){
      if(now - lastRelease > debounceMs){ pressStart=now; wasPressed=true; longHandled=false; }
    } else if(s == HIGH && wasPressed){
      wasPressed=false; lastRelease=now;
      uint32_t dur = now - pressStart;
      if(dur >= debounceMs && dur < 1000 && !longHandled) return true;
    }
    return false;
  }
  bool longPressed(){
    if(!wasPressed || longHandled) return false;
    if(millis() - pressStart >= 1000){ longHandled = true; return true; }
    return false;
  }
  bool isDown(){ return digitalRead(pin)==LOW; }
  uint32_t holdMs(){ return (wasPressed && isDown()) ? (millis() - pressStart) : 0; }
};

Button btnToggle(BTN_TOGGLE), btnNext(BTN_NEXT), btnPrev(BTN_PREV);

// ===== Config Functions =====
void saveConfig(){
  File f = LittleFS.open(CONFIG_FILE, "w");
  if(!f){
    Serial.println("✗ Cannot save config");
    return;
  }
  
  // Format: colorIndex
  // Example: 5 (YELLOW)
  f.printf("%d\n", (int)uiColorIdx);
  f.close();
  
  Serial.printf("✓ Config saved: color=%d (%s)\n", 
                (int)uiColorIdx, uiColorNames[uiColorIdx]);
}

bool loadConfig(){
  if(!LittleFS.exists(CONFIG_FILE)){
    Serial.println("⚠ Config file not found, using defaults");
    return false;
  }
  
  File f = LittleFS.open(CONFIG_FILE, "r");
  if(!f){
    Serial.println("✗ Cannot read config");
    return false;
  }
  
  String line = f.readStringUntil('\n');
  f.close();
  
  int color = line.toInt();
  
  // Validate
  if(color < 0 || color >= UI_COLOR_COUNT) color = 3;  // Default CYAN
  
  uiColorIdx = (uint8_t)color;
  
  Serial.printf("✓ Config loaded: color=%d (%s)\n", 
                (int)uiColorIdx, uiColorNames[uiColorIdx]);
  return true;
}

void resetConfig(){
  if(LittleFS.exists(CONFIG_FILE)){
    LittleFS.remove(CONFIG_FILE);
    Serial.println("✓ Config deleted");
  }
  uiColorIdx = 3; // Reset to CYAN
  saveConfig();
  Serial.println("✓ Config reset to default (CYAN)");
}

// ===== Helpers =====
bool endsWithIC(const String& s, const char* suf){
  String a=s; a.toLowerCase(); String b=suf; b.toLowerCase(); return a.endsWith(b);
}
bool isJPG(const String& s){ return endsWithIC(s,".jpg") || endsWithIC(s,".jpeg"); }

void scanImages(){
  numImages = 0; qrImage = "";
  Dir dir = LittleFS.openDir("/");
  while (dir.next()) {
    String fn = dir.fileName();
    if (!fn.startsWith("/")) fn = "/" + fn;
    if (isJPG(fn)) {
      if(fn.indexOf("qr") >= 0 || fn.indexOf("QR") >= 0){
        if(qrImage.length() == 0) qrImage = fn;
      } else if(numImages < MAX_IMAGES){
        images[numImages++] = fn;
      }
    }
  }
  Serial.printf("Found %d images, QR: %s\n", numImages, qrImage.c_str());
}

void centerText(const char* txt, int y, uint8_t size=1, uint16_t color=ST77XX_WHITE){
  tft.setTextSize(size);
  tft.setTextColor(color);
  int16_t x1,y1; uint16_t w,h;
  tft.getTextBounds(String(txt), 0, 0, &x1, &y1, &w, &h);
  int x = (tft.width() - w)/2; if(x < 0) x = 0;
  tft.setCursor(x, y); tft.print(txt);
}

// ===== JPEG callback =====
bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap){
  if(y >= tft.height()) return false;
  tft.startWrite();
  tft.drawRGBBitmap(x, y, bitmap, w, h);
  tft.endWrite();
  return true;
}

// ===== Screens =====
void showWelcome(){
  tft.fillScreen(ST77XX_WHITE);
  int cx=64, cy=60;
  tft.drawCircle(cx,cy,30,ST77XX_BLACK);
  tft.drawCircle(cx,cy,31,ST77XX_BLACK);
  tft.fillCircle(cx-12, cy-8, 3, ST77XX_BLACK);
  tft.fillCircle(cx+12, cy-8, 3, ST77XX_BLACK);
  for(int i=-15;i<=15;i++){
    int x=cx+i; int y=cy+8 + (int)(sqrt(225 - i*i)/3);
    tft.drawPixel(x,y,ST77XX_BLACK); tft.drawPixel(x,y+1,ST77XX_BLACK);
  }
  centerText("Hay tai len anh!", 120, 1, ST77XX_BLACK);
}

void showInfoScreen(){
  tft.fillScreen(ST77XX_BLACK);
  centerText("HAOTAI", 8, 3, UI_COLOR);
  centerText("PhotoFrame", 36, 2, ST77XX_WHITE);
  
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  centerText("WiFi: PhotoFrameDemo", 80, 1, ST77XX_YELLOW);
  centerText("Pass: 12345678", 95, 1, ST77XX_YELLOW);
  centerText("IP: 192.168.4.1", 110, 1, ST77XX_YELLOW);
}

static void printMenuLine(int y, const char* text, bool selected, uint16_t color=ST77XX_WHITE){
  tft.setTextSize(1);
  tft.setTextColor(selected ? UI_COLOR : color);
  tft.setCursor(10, y);
  if(selected) tft.print("> "); else tft.print("  ");
  tft.print(text);
}

void showMenuScreen(){
  tft.fillScreen(ST77XX_BLACK);
  centerText("SETTINGS", 10, 2, UI_COLOR);

  // 1. Color UI
  printMenuLine(50, "1. Color UI", (menuIndex==0));
  if(menuIndex == 0){
    tft.setTextSize(1);
    tft.setCursor(28, 66);
    tft.setTextColor(uiColorVals[uiColorIdx]);
    tft.print(uiColorNames[uiColorIdx]);
  }
  
  // 2. Reset Config
  printMenuLine(90, "2. Reset Config", (menuIndex==1));
  if(menuIndex == 1){
    tft.setTextSize(1);
    tft.setTextColor(ST77XX_YELLOW);
    centerText("(Back to default)", 106, 1, ST77XX_YELLOW);
  }
}

void drawCurrentImage(){
  if(uiMode != MODE_SHOW) return;

  String path;
  if(viewingQR){
    if(qrImage.length() == 0){
      tft.fillScreen(ST77XX_BLACK); centerText("No QR Image", 70, 1, ST77XX_RED); return;
    }
    path = qrImage;
  } else {
    if(numImages == 0){ showWelcome(); return; }
    path = images[currentIndex];
  }

  tft.fillScreen(ST77XX_BLACK);
  TJpgDec.setSwapBytes(false);
  int rc = TJpgDec.drawFsJpg(0, 0, path.c_str(), LittleFS);
  if(rc != 0){
    tft.setCursor(8, 70); tft.setTextColor(ST77XX_RED); tft.setTextSize(1);
    tft.print("JPEG ERROR: "); tft.println(rc);
  }
}

// ===== WiFi AP =====
void enableAP(){
  if(apEnabled) return;
  Serial.println("Enabling AP...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  IPAddress ip(192,168,4,1), gw(192,168,4,1), mask(255,255,255,0);
  WiFi.softAPConfig(ip, gw, mask);
  WiFi.softAP(ap_ssid, ap_password, 1, 0, 4);
  apEnabled = true;
  Serial.println("AP enabled: " + WiFi.softAPIP().toString());
}

void disableAP(){
  if(!apEnabled) return;
  Serial.println("Disabling AP...");
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  apEnabled = false;
}

// ===== Web Handlers =====
void handleUploadDone(){ server.send(200,"text/html",F("<!DOCTYPE html><meta charset='utf-8'><meta http-equiv='refresh' content='1;url=/'>OK")); }

void handleFileUpload(){
  HTTPUpload& up = server.upload();
  static size_t written = 0;

  if(up.status == UPLOAD_FILE_START){
    isUploading = true; written = 0;
    String name = up.filename; name.toLowerCase();
    if(!isJPG(name)){ Serial.println("Reject: not jpg"); return; }
    bool isQR = (name.indexOf("qr") >= 0);
    if(!isQR && numImages >= MAX_IMAGES){ Serial.println("Full images"); return; }
    String newName = isQR ? "/qrcode.jpg" : ("/image" + String(numImages + 1) + ".jpg");
    Serial.println("Upload: " + newName);
    fsUploadFile = LittleFS.open(newName, "w"); if(!fsUploadFile){ Serial.println("File open error"); return; }
  }
  else if(up.status == UPLOAD_FILE_WRITE){
    if(fsUploadFile){
      fsUploadFile.write(up.buf, up.currentSize); written += up.currentSize;
      if(written > MAX_UPLOAD_SIZE){
        String fname = fsUploadFile.name(); fsUploadFile.close(); LittleFS.remove(fname);
        Serial.println("File too large");
      }
    }
    yield(); ESP.wdtFeed();
  }
  else if(up.status == UPLOAD_FILE_END){
    if(fsUploadFile){ fsUploadFile.close(); Serial.println("Upload complete: " + String(written) + " bytes"); }
    delay(80); scanImages();
    if(numImages == 1 && uiMode == MODE_SHOW){ currentIndex = 0; drawCurrentImage(); }
    isUploading = false;
  }
}

void handleList(){
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
                "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                "<title>Files</title><style>"
                "body{font-family:Arial;padding:20px;background:#f0f0f0}"
                ".box{max-width:600px;margin:auto;background:#fff;padding:20px;border-radius:10px}"
                ".item{display:flex;justify-content:space-between;padding:10px;border-bottom:1px solid #ddd}"
                ".btn{padding:8px 14px;background:#f44336;color:#fff;text-decoration:none;border-radius:6px}"
                ".back{background:#2196F3}"
                "</style></head><body><div class='box'><h2>Files (" + String(numImages) + ")</h2>";
  if(numImages > 0){
    for(int i=0; i<numImages; i++){
      html += "<div class='item'><span>" + String(i+1) + ". " + images[i] + "</span>";
      html += "<a class='btn' href='/delete?f=" + String(i) + "' onclick='return confirm(\"Xoa?\")'>Xoa</a></div>";
    }
  } else html += "<div>Chua co file</div>";

  if(qrImage.length() > 0){
    html += "<h3>QR Image</h3>";
    html += "<div class='item'><span>" + qrImage + "</span>";
    html += "<a class='btn' href='/deleteqr' onclick='return confirm(\"Xoa QR?\")'>Xoa</a></div>";
  }
  html += "<div style='margin-top:12px'><a class='btn back' href='/'>Quay lai</a>";
  if(numImages > 0) html += " <a class='btn' href='/deleteall' onclick='return confirm(\"Xoa het?\")'>Xoa tat ca</a>";
  html += "</div></div></body></html>";
  server.send(200, "text/html", html);
}

void handleDelete(){
  if(!server.hasArg("f")){ server.send(400,"text/plain","Missing param"); return; }
  int idx = server.arg("f").toInt();
  if(idx < 0 || idx >= numImages){ server.send(400,"text/plain","Invalid index"); return; }
  String fn = images[idx];
  if(LittleFS.remove(fn)){
    for(int i=idx; i<numImages-1; i++) images[i] = images[i+1];
    numImages--; if(currentIndex >= numImages && numImages > 0) currentIndex = numImages - 1; if(numImages == 0) currentIndex = 0;
    if(uiMode == MODE_SHOW && !viewingQR){ if(numImages > 0) drawCurrentImage(); else showWelcome(); }
    server.send(200,"text/html",F("<meta http-equiv='refresh' content='1;url=/list'>OK"));
  } else server.send(500,"text/plain","Delete error");
}

void handleDeleteQR(){
  if(qrImage.length() > 0 && LittleFS.remove(qrImage)){
    qrImage = ""; viewingQR = false;
    server.send(200,"text/html",F("<meta http-equiv='refresh' content='1;url=/list'>OK"));
  } else server.send(500,"text/plain","Delete error");
}

void handleDeleteAll(){
  for(int i=0; i<numImages; i++) LittleFS.remove(images[i]);
  numImages = 0; currentIndex = 0; viewingQR = false;
  if(uiMode == MODE_SHOW) showWelcome();
  server.send(200,"text/html",F("<meta http-equiv='refresh' content='1;url=/'>OK"));
}

void handleNotFound(){
  String path = server.uri();
  if(LittleFS.exists(path)){
    File f = LittleFS.open(path, "r");
    String ct = "text/plain";
    if(path.endsWith(".js")) ct = "application/javascript";
    else if(path.endsWith(".html")) ct = "text/html";
    else if(path.endsWith(".jpg")) ct = "image/jpeg";
    server.streamFile(f, ct); f.close(); return;
  }
  server.send(404,"text/plain","404");
}

void handleRoot(){
  if(LittleFS.exists("/index.html")){
    File f = LittleFS.open("/index.html", "r"); server.streamFile(f, "text/html"); f.close();
  } else {
    server.send(200,"text/html","<html><body><h1>PhotoFrame</h1><p>Upload index.html to LittleFS</p><a href='/list'>Files</a></body></html>");
  }
}

void setupWeb(){
  server.on("/", handleRoot);
  server.on("/upload", HTTP_POST, handleUploadDone, handleFileUpload);
  server.on("/list", HTTP_GET, handleList);
  server.on("/delete", HTTP_GET, handleDelete);
  server.on("/deleteqr", HTTP_GET, handleDeleteQR);
  server.on("/deleteall", HTTP_GET, handleDeleteAll);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("Web server ready");
}

// ===== Setup =====
void setup(){
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(100);
  Serial.println("\n\n╔════════════════════════════════════╗");
  Serial.println("║  HAOTAI PhotoFrame v4.1           ║");
  Serial.println("╚════════════════════════════════════╝\n");

  // TFT
  tft.initR(INITR_BLACKTAB); tft.setRotation(0); tft.setSPISpeed(SPI_SPEED_HZ);
  tft.fillScreen(ST77XX_BLUE); centerText("Khoi tao...", 70, 1, ST77XX_WHITE);

  // FS
  if(!LittleFS.begin()){
    tft.fillScreen(ST77XX_RED); centerText("LittleFS ERROR!", 70, 1, ST77XX_WHITE);
    while(1) delay(1000);
  }
  
  // Load config (color)
  loadConfig();

  // JPEG
  TJpgDec.setJpgScale(1); TJpgDec.setSwapBytes(false); TJpgDec.setCallback(tft_output);

  // Buttons
  btnToggle.begin(); btnNext.begin(); btnPrev.begin();

  // Web
  setupWeb();

  // Scan & show
  scanImages();
  uiMode = MODE_SHOW;
  if(numImages > 0) drawCurrentImage(); else showWelcome();

  Serial.println("✓ Setup complete!");
  Serial.println("═══════════════════════════════════\n");
}

// ===== Loop =====
void loop(){
  if(apEnabled) server.handleClient();

  // AP turn on 1s delay when in Info
  if(uiMode == MODE_INFO && apArmPending && (int32_t)(millis() - apArmTimeMs) >= 0){
    apArmPending = false;
    if(uiMode == MODE_INFO && !apEnabled) enableAP();
  }

  // === TOGGLE ===
  if(btnToggle.longPressed()){
    // Vào MENU
    uiMode = MODE_MENU; viewingQR = false;
    menuIndex = 0; menuEditing = false;
    showMenuScreen();
    apArmPending = false;
    Serial.println("TOGGLE LONG -> MENU");
  }
  else if(btnToggle.clicked()){
    if(uiMode == MODE_MENU){
      if(menuIndex == 0 && menuEditing){
        // Save UI Color
        menuEditing = false;
        saveConfig();
        uiMode = MODE_SHOW; viewingQR = false;
        if(numImages > 0) drawCurrentImage(); else showWelcome();
      } else {
        // select
        if(menuIndex == 0){                 // Color UI
          menuEditing = true;
          showMenuScreen();
        } else if(menuIndex == 1){          // Reset Config
          tft.fillScreen(ST77XX_BLACK);
          centerText("Resetting...", 70, 1, ST77XX_YELLOW);
          resetConfig();
          delay(1000);
          tft.fillScreen(ST77XX_BLACK);
          centerText("Config Reset!", 70, 2, ST77XX_GREEN);
          centerText("CYAN", 95, 1, ST77XX_CYAN);
          delay(1500);
          uiMode = MODE_SHOW; viewingQR = false;
          if(numImages > 0) drawCurrentImage(); else showWelcome();
        }
      }
    }
    else if(uiMode == MODE_INFO){
      // Out Info
      apArmPending = false;
      disableAP();
      uiMode = MODE_SHOW; viewingQR = false;
      if(numImages > 0) drawCurrentImage(); else showWelcome();
      Serial.println("Exit INFO");
    }
    else {
      // Show → Open Info
      uiMode = MODE_INFO; viewingQR = false;
      showInfoScreen();
      apArmPending = true;
      apArmTimeMs = millis() + 1000;
      Serial.println("Enter INFO (AP in 1s if stayed)");
    }
  }

  // === NEXT ===
  if(btnNext.longPressed()){
    if(uiMode == MODE_SHOW){
      viewingQR = !viewingQR; drawCurrentImage();
      Serial.println("NEXT LONG -> toggle QR view");
    }
  }

  if(btnNext.clicked()){
    if(uiMode == MODE_SHOW && !viewingQR && numImages > 1){
      currentIndex = (currentIndex + 1) % numImages; drawCurrentImage();
      Serial.printf("Next: %d/%d\n", currentIndex + 1, numImages);
    } else if(uiMode == MODE_MENU){
      if(menuEditing && menuIndex == 0){
        uiColorIdx = (uiColorIdx + 1) % UI_COLOR_COUNT;
        showMenuScreen();
      } else {
        menuIndex = (menuIndex + 1) % 2;  // 2 items
        showMenuScreen();
      }
    }
  }

  // === PREV ===
  if(btnPrev.clicked()){
    if(uiMode == MODE_SHOW && !viewingQR && numImages > 1){
      currentIndex = (currentIndex - 1 + numImages) % numImages; drawCurrentImage();
      Serial.printf("Prev: %d/%d\n", currentIndex + 1, numImages);
    } else if(uiMode == MODE_MENU){
      if(menuEditing && menuIndex == 0){
        uiColorIdx = (uiColorIdx + UI_COLOR_COUNT - 1) % UI_COLOR_COUNT;
        showMenuScreen();
      } else {
        menuIndex = (menuIndex + 1) % 2;  // 2 items (backward)
        showMenuScreen();
      }
    }
  }

  yield(); ESP.wdtFeed();
}