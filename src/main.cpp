#include <Arduino.h>

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include "MAX30105.h"
#include "heartRate.h"

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
MAX30105 particleSensor;

// --- State machine ---
// 0 = waiting for finger
// 1 = measuring (finger present)
// 2 = grace period (finger just lifted, graph still running)
byte deviceState = 0;

unsigned long fingerLiftTime = 0;   // millis() when finger was lifted
#define GRACE_PERIOD_MS 5000        // how long to keep graph alive after finger was lifted

bool wasWaiting  = true;
int  xPos        = 0;
int  lastY       = 42;

// --- DC filter state ---
float dcIR       = 0;
float dcRed      = 0;

// --- SpO2 amplitude tracking ---
float maxACIR    = 0, minACIR  = 0;
float maxACRed   = 0, minACRed = 0;

// --- Graph scaling ---
float graphMinAC = -50;
float graphMaxAC =  50;

// --- Heart rate ---
const byte RATE_SIZE = 4;
byte  rates[RATE_SIZE] = {0, 0, 0, 0};
byte  rateSpot         = 0;
byte  validRates       = 0;
long  lastBeat         = 0;
float beatsPerMinute   = 0;
int   beatAvg          = 0;

// --- SpO2 ---
const byte SPO2_SIZE = 4;
float spo2Readings[SPO2_SIZE] = {0, 0, 0, 0};
byte  spo2Spot    = 0;
byte  validSpo2   = 0;
float currentSpO2 = 0;
float spo2Avg     = 0;

// --- Display timing ---
unsigned long lastTextUpdate = 0;

// --- Waiting animation ---
unsigned long lastAnimFrame  = 0;
byte          animFrame      = 0;

// --- Grace period graph ---
float frozenACIR = 0;


void printCentered(const char* text, int y, int textSize = 1)
{
  display.setTextSize(textSize);
  int charW = 6 * textSize;
  int x     = (SCREEN_WIDTH - (int)strlen(text) * charW) / 2;
  if (x < 0) x = 0;
  display.setCursor(x, y);
  display.print(text);
}

void drawHeart(int cx, int cy, int size, bool filled)
{
  int lx = cx - size / 2;
  int rx = cx + size / 2;
  int ty = cy - size / 4;

  if (filled) {
    display.fillCircle(lx, ty, size / 2, SSD1306_WHITE);
    display.fillCircle(rx, ty, size / 2, SSD1306_WHITE);
    for (int i = 0; i <= size; i++) {
      int w = size - i + size / 2;
      display.drawFastHLine(cx - w / 2, ty + i, w, SSD1306_WHITE);
    }
  } else {
    display.drawCircle(lx, ty, size / 2, SSD1306_WHITE);
    display.drawCircle(rx, ty, size / 2, SSD1306_WHITE);
    display.drawLine(lx - size / 2, ty, cx, ty + size, SSD1306_WHITE);
    display.drawLine(rx + size / 2, ty, cx, ty + size, SSD1306_WHITE);
  }
}

// Startup animation
void playStartupAnimation()
{
  display.setTextColor(SSD1306_WHITE);

  // Phase 1: ECG sweep
  for (int x = 0; x < 128; x += 2) {
    display.clearDisplay();
    for (int i = max(0, x - 20); i < x; i++) {
      if (i % 2 == 0) display.drawPixel(i, 32, SSD1306_WHITE);
    }
    if (x > 10 && x < 118) {
      display.drawLine(x - 6, 32, x - 3, 22, SSD1306_WHITE);
      display.drawLine(x - 3, 22, x,     40, SSD1306_WHITE);
      display.drawLine(x,     40, x + 3, 32, SSD1306_WHITE);
    }
    display.display();
    delay(8);
  }
  delay(100);

  // Phase 2: Heart grows
  for (int s = 2; s <= 12; s += 2) {
    display.clearDisplay();
    drawHeart(64, 26, s, s >= 8);
    display.display();
    delay(70);
  }
  delay(200);

  // Phase 3: Title
  display.clearDisplay();
  drawHeart(64, 12, 10, true);
  printCentered("Oximeter & Heartrate", 28);
  printCentered("by Sadhya555", 39);
  display.display();
  delay(600);

  // Phase 4: Initializing dots
  for (byte d = 0; d <= 4; d++) {
    display.clearDisplay();
    drawHeart(64, 12, 10, true);
    printCentered("Oximeter & Heartrate", 28);
    printCentered("by Sadhya555", 39);
    char initStr[18];
    strcpy(initStr, "Initializing");
    for (byte i = 0; i < d; i++) strcat(initStr, ".");
    printCentered(initStr, 52);
    display.display();
    delay(300);
  }

  // Phase 5: Wipe down
  for (int y = 0; y < 64; y += 4) {
    display.fillRect(0, y, 128, 4, SSD1306_BLACK);
    display.display();
    delay(15);
  }
}

// Waiting screen
void drawWaitingScreen()
{
  unsigned long now = millis();
  if (now - lastAnimFrame < 500) return;
  lastAnimFrame = now;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Travelling ECG blip
  int blipX = (animFrame * 6) % 128;
  for (int x = 0; x < 128; x++)
    display.drawPixel(x, 48, SSD1306_WHITE);

  if (blipX > 6 && blipX < 122) {
    for (int x = blipX - 6; x <= blipX + 4; x++)
      display.drawPixel(x, 48, SSD1306_BLACK);
    display.drawLine(blipX - 4, 48, blipX - 2, 48, SSD1306_WHITE);
    display.drawLine(blipX - 2, 48, blipX - 1, 40, SSD1306_WHITE);
    display.drawLine(blipX - 1, 40, blipX,     55, SSD1306_WHITE);
    display.drawLine(blipX,     55, blipX + 1, 48, SSD1306_WHITE);
    display.drawLine(blipX + 1, 48, blipX + 3, 48, SSD1306_WHITE);
  }

  // Finger icon
  int fx = 59, fy = 4;
  display.drawRoundRect(fx, fy, 10, 20, 4, SSD1306_WHITE);
  display.drawFastHLine(fx - 3, fy + 20, 16, SSD1306_WHITE);

  // Pulsing arrow
  int arrowBase = 30 + (animFrame % 2) * 2;
  display.drawLine(64, arrowBase,     64, arrowBase + 5, SSD1306_WHITE);
  display.drawLine(64, arrowBase + 5, 61, arrowBase + 2, SSD1306_WHITE);
  display.drawLine(64, arrowBase + 5, 67, arrowBase + 2, SSD1306_WHITE);

  // Rotating message — 3 seconds each (6 frames × 500ms)
  const char* msgs[3] = {
    "Place on sensor",
    "Touch gently",
    "Rest firmly"
  };
  printCentered(msgs[(animFrame / 6) % 3], 56);

  display.display();
  animFrame++;
}

// Grace period header 
void drawGraceHeader(unsigned long elapsed)
{
  display.fillRect(0, 0, 128, 17, SSD1306_BLACK);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Row 1 — last known values, dimmed label
  display.setCursor(0, 0);
  display.print("SpO2:");
  display.print((int)currentSpO2);
  display.print("%      ");
  display.setCursor(92, 0);
  display.print("HR:");
  display.print((int)beatsPerMinute);

  // Row 2 — "Lift off" + countdown bar
  display.setCursor(0, 9);
  display.print("Finger off ");

  // Countdown bar: 5 filled blocks shrinking over 5 seconds
  // Bar lives from x=66 to x=126 (60px wide)
  unsigned long remaining = GRACE_PERIOD_MS - elapsed;
  int barWidth = map(remaining, 0, GRACE_PERIOD_MS, 0, 60);
  display.drawRect(66, 9, 60, 7, SSD1306_WHITE);           // outline
  display.fillRect(67, 10, barWidth - 2, 5, SSD1306_WHITE); // fill

  display.drawFastHLine(0, 17, 128, SSD1306_WHITE);
}

// Reset all measurement state (called when going to waiting)
void resetMeasurements()
{
  beatsPerMinute = 0;
  beatAvg        = 0;
  validRates     = 0;
  rateSpot       = 0;
  currentSpO2    = 0;
  spo2Avg        = 0;
  validSpo2      = 0;
  spo2Spot       = 0;
  graphMinAC     = -50;
  graphMaxAC     =  50;
  xPos           = 0;
  frozenACIR     = 0;
  for (byte i = 0; i < RATE_SIZE; i++) rates[i]        = 0;
  for (byte i = 0; i < SPO2_SIZE; i++) spo2Readings[i] = 0;
}


void setup()
{
  Serial.begin(115200);
  Wire.begin(21, 22);
  Wire.setClock(400000);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("OLED failed"));
    for (;;);
  }

  playStartupAnimation();

  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    printCentered("Sensor Error!", 20);
    printCentered("Check wiring.", 32);
    display.display();
    while (1);
  }

  particleSensor.setup(60, 4, 2, 400, 411, 4096);
  particleSensor.setPulseAmplitudeRed(0x1F);
  particleSensor.setPulseAmplitudeIR(0x1F);

  display.clearDisplay();
  display.display();

  deviceState = 0;   // start in waiting state
}


void loop()
{
  long ir  = particleSensor.getIR();
  long red = particleSensor.getRed();

  //  STATE 0: Waiting for finger
  if (deviceState == 0)
  {
    if (ir >= 50000) {
      // Finger placed — transition to measuring
      dcIR  = ir;
      dcRed = red;
      display.clearDisplay();
      deviceState = 1;
      return;
    }
    drawWaitingScreen();
    return;
  }

  //  STATE 1: Measuring
  if (deviceState == 1)
  {
    if (ir < 50000) {
      // Finger just lifted — enter grace period
      fingerLiftTime = millis();
      frozenACIR     = 0;   // graph will flatline from last position
      deviceState    = 2;
      return;
    }

    // Beat detection on RAW ir
    if (checkForBeat(ir))
    {
      long delta     = millis() - lastBeat;
      lastBeat       = millis();
      beatsPerMinute = 60.0f / (delta / 1000.0f);

      Serial.print("Beat! BPM=");
      Serial.println(beatsPerMinute);

      if (beatsPerMinute > 20 && beatsPerMinute < 255)
      {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;
        if (validRates < RATE_SIZE) validRates++;

        beatAvg = 0;
        for (byte x = 0; x < validRates; x++)
          beatAvg += rates[x];
        beatAvg /= validRates;
      }
    }

    // DC filter
    const float alpha = 0.95f;
    dcIR  = (alpha * dcIR)  + ((1.0f - alpha) * (float)ir);
    dcRed = (alpha * dcRed) + ((1.0f - alpha) * (float)red);

    float acIR  = (float)ir  - dcIR;
    float acRed = (float)red - dcRed;
    frozenACIR  = acIR;   // keep updating so grace period starts from here

    if (acIR  > maxACIR)  maxACIR  = acIR;
    if (acIR  < minACIR)  minACIR  = acIR;
    if (acRed > maxACRed) maxACRed = acRed;
    if (acRed < minACRed) minACRed = acRed;

    // Graph scaling
    if (acIR < graphMinAC) graphMinAC = acIR;
    if (acIR > graphMaxAC) graphMaxAC = acIR;
    if (graphMaxAC - graphMinAC < 10) { graphMaxAC = 10; graphMinAC = -10; }

    int yPos = map((long)acIR, (long)graphMinAC, (long)graphMaxAC, 63, 20);
    yPos = constrain(yPos, 20, 63);

    // SpO2 once per second
    if (millis() - lastTextUpdate > 1000)
    {
      float acAmpRed = maxACRed - minACRed;
      float acAmpIR  = maxACIR  - minACIR;

      if (acAmpIR > 0 && dcIR > 0) {
        float R     = (acAmpRed / dcRed) / (acAmpIR / dcIR);
        currentSpO2 = 110.0f - 25.0f * R;
        currentSpO2 = constrain(currentSpO2, 0.0f, 100.0f);

        spo2Readings[spo2Spot++] = currentSpO2;
        spo2Spot %= SPO2_SIZE;
        if (validSpo2 < SPO2_SIZE) validSpo2++;

        spo2Avg = 0;
        for (byte x = 0; x < validSpo2; x++)
          spo2Avg += spo2Readings[x];
        spo2Avg /= validSpo2;
      }

// Reset Peak trackers to the current wave position, not zero
      maxACIR = acIR; minACIR = acIR;
      maxACRed = acRed; minACRed = acRed;
      lastTextUpdate = millis();

      Serial.print("Live BPM=");   Serial.print((int)beatsPerMinute);
      Serial.print(" Avg BPM=");   Serial.print(beatAvg);
      Serial.print(" Live SpO2="); Serial.print((int)currentSpO2);
      Serial.print(" Avg SpO2=");  Serial.println((int)spo2Avg);
    }

    // Draw header
    display.fillRect(0, 0, 128, 17, SSD1306_BLACK);
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.print("SpO2:");
    if (validSpo2 > 0) {
      display.print((int)currentSpO2);
      display.print("%");
      if      (currentSpO2 >= 95) display.print("[OK]");
      else if (currentSpO2 >= 90) display.print("[-] ");
      else                         display.print("[!] ");
    } else {
      display.print("--%[--]");
    }
    display.setCursor(92, 0);
    display.print("HR:");
    if (beatsPerMinute > 0) display.print((int)beatsPerMinute);
    else                     display.print("--");

    display.setCursor(0, 9);
    display.print("AvgO2:");
    if (validSpo2 > 0) { display.print((int)spo2Avg); display.print("%"); }
    else                 display.print("--%");
    display.setCursor(72, 9);
    display.print("AvgHR:");
    if (validRates > 0) display.print(beatAvg);
    else                display.print("--");

    display.drawFastHLine(0, 17, 128, SSD1306_WHITE);
    
    // Waveform
    if (xPos > 0)
      display.drawLine(xPos - 1, lastY, xPos, yPos, SSD1306_WHITE);
    lastY = yPos;
    
    static unsigned long lastDrawTime = 0;
    if (millis() - lastDrawTime > 33) {
      display.display(); 
      lastDrawTime = millis();
    }

    xPos++;
    if (xPos >= 128) {
      xPos = 0;
      display.fillRect(0, 18, 128, 46, SSD1306_BLACK);
      graphMinAC = acIR - 10;
      graphMaxAC = acIR + 10;
    }

    return;
  }

  //  STATE 2: Grace period
  if (deviceState == 2)
  {
    unsigned long elapsed = millis() - fingerLiftTime;

    // Finger replaced — go straight back to measuring
    if (ir >= 50000) {
      dcIR  = ir;
      dcRed = red;
      display.clearDisplay();
      deviceState = 1;
      return;
    }

    // Grace period expired — go to waiting
    if (elapsed >= GRACE_PERIOD_MS) {
      resetMeasurements();
      display.clearDisplay();
      lastAnimFrame = 0;
      animFrame     = 0;
      deviceState   = 0;
      return;
    }

    // Grace period still running:
    // Draw the header with countdown bar
    drawGraceHeader(elapsed);

    // Draw a flatline at the last known y position
    // (graph scrolls but holds the last waveform position)
    int yPos = lastY;   // flatline at wherever the waveform last was

    if (xPos > 0)
      display.drawLine(xPos - 1, lastY, xPos, yPos, SSD1306_WHITE);
    lastY = yPos;
    display.display();

    xPos++;
    if (xPos >= 128) {
      xPos = 0;
      display.fillRect(0, 18, 128, 46, SSD1306_BLACK);
    }

    delay(20);   // small delay to control flatline draw speed
    return;
  }
}