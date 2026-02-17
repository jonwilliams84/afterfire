#include <FastLED.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <EEPROM.h>

// ESP32-S3 USB Support
// Arduino IDE Settings: Tools -> USB CDC On Boot -> "Disabled" for flashing
// The code will work with either Disabled or Enabled
#define USBSerial Serial

///////////////////////
// EEPROM SETTINGS
///////////////////////

#define EEPROM_SIZE 512
#define SETTINGS_VERSION 1
#define SETTINGS_START_ADDR 0

// Settings structure for persistent storage (94 bytes)
struct {
  uint8_t version;                    // 1 byte
  uint32_t crc;                       // 4 bytes (CRC32 of all other data)
  
  // WiFi credentials (64 bytes)
  char ssid[32];
  char password[32];
  
  // Calibration values (10 bytes)
  uint16_t neutralMin;
  uint16_t neutralMax;
  uint16_t minPulse;
  uint16_t maxPulse;
  uint16_t neutralPulse;
  
  // Effect toggles (4 bytes)
  bool enableBackfire;
  bool enableBrakeCrackle;
  bool enableIdleBurble;
  bool enableRPMFlicker;
  
  // Sensitivity thresholds (10 bytes)
  int16_t backfireThrottleMin;
  int16_t backfireReleaseMax;
  int16_t brakeThrottleMin;
  int16_t brakeThrottleMax;
  int16_t rpmFlickerThreshold;
} settings = {0};

// Forward declarations for settings management
void loadSettings();
void saveSettings();
uint32_t calculateSettingsCRC();
bool validateSettings();
void resetSettings();
void startAPMode();
void setupAPWebServer();

///////////////////////
// USER CONFIG
///////////////////////

// Web Server (always on port 80)
WebServer server(80);

// Hardware Config
#define THROTTLE_PIN 2
#define LED_PIN 3
#define NUM_LEDS 1            // change if dual exhaust
#define LED_TYPE WS2812B
#define COLOR_ORDER GRB
#define MAX_BRIGHTNESS 255

// WiFi mode flags
bool inAPMode = false;
unsigned long wifiConnectTimeout = 0;

///////////////////////

CRGB leds[NUM_LEDS];

volatile uint32_t pulseStart = 0;
volatile uint16_t pulseWidth = 1500;

uint16_t prevPulse = 1500;

unsigned long lastEffectTime = 0;
bool burstActive = false;
int burstCount = 0;
int burstIntensity = 0;

// Runtime calibration variables (loaded from EEPROM on boot)
uint16_t NEUTRAL_MIN = 1890;
uint16_t NEUTRAL_MAX = 1930;
uint16_t MIN_PULSE = 1496;  // Full brake/reverse
uint16_t MAX_PULSE = 2000;  // Full throttle
uint16_t NEUTRAL_PULSE = 1916;  // Center neutral

// Runtime effect toggles (loaded from EEPROM on boot)
bool enableBackfire = true;
bool enableBrakeCrackle = true;
bool enableIdleBurble = true;
bool enableRPMFlicker = true;

// Runtime sensitivity thresholds (loaded from EEPROM on boot)
int backfireThrottleMin = 30;  // Minimum throttle before release to trigger
int backfireReleaseMax = 15;   // Maximum throttle after release to trigger
int brakeThrottleMin = 20;     // Throttle needed before braking
int brakeThrottleMax = -20;    // Brake position to trigger
int rpmFlickerThreshold = 30;  // Throttle % needed before RPM flicker starts

// Calibration state
enum CalibrationStep { CAL_IDLE, CAL_NEUTRAL, CAL_THROTTLE, CAL_BRAKE, CAL_COMPLETE };
CalibrationStep calibrationStep = CAL_IDLE;
uint16_t calibratedNeutral = 0;
uint16_t calibratedThrottle = 0;
uint16_t calibratedBrake = 0;

///////////////////////
// FORWARD DECLARATIONS
///////////////////////

void setupWebServer();
void handleRPMFlicker(int throttle);
void detectBackfire(int prev, int now);
void detectBrakeCrackle(int prev, int now);
void handleBurst();
void idleBurble(int throttle);
void setFlame(int heat);

///////////////////////
// EEPROM MANAGEMENT
///////////////////////

uint32_t calculateSettingsCRC() {
  uint32_t crc = 0xFFFFFFFF;
  uint8_t* data = (uint8_t*)&settings + 5; // Skip version and crc fields
  size_t len = sizeof(settings) - 5;
  
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
    }
  }
  return crc ^ 0xFFFFFFFF;
}

bool validateSettings() {
  return settings.version == SETTINGS_VERSION && settings.crc == calculateSettingsCRC();
}

void loadSettings() {
  USBSerial.println("[Settings] Loading from EEPROM...");
  EEPROM.readBytes(SETTINGS_START_ADDR, &settings, sizeof(settings));
  
  if (validateSettings()) {
    USBSerial.println("[Settings] ✓ Valid settings found");
    
    // Load into runtime variables
    NEUTRAL_MIN = settings.neutralMin;
    NEUTRAL_MAX = settings.neutralMax;
    MIN_PULSE = settings.minPulse;
    MAX_PULSE = settings.maxPulse;
    NEUTRAL_PULSE = settings.neutralPulse;
    
    enableBackfire = settings.enableBackfire;
    enableBrakeCrackle = settings.enableBrakeCrackle;
    enableIdleBurble = settings.enableIdleBurble;
    enableRPMFlicker = settings.enableRPMFlicker;
    
    backfireThrottleMin = settings.backfireThrottleMin;
    backfireReleaseMax = settings.backfireReleaseMax;
    brakeThrottleMin = settings.brakeThrottleMin;
    brakeThrottleMax = settings.brakeThrottleMax;
    rpmFlickerThreshold = settings.rpmFlickerThreshold;
    
    USBSerial.println("[Settings] Calibration loaded from EEPROM");
  } else {
    USBSerial.println("[Settings] No valid settings in EEPROM, using defaults");
  }
}

void saveSettings() {
  // Update struct from runtime variables
  settings.version = SETTINGS_VERSION;
  settings.neutralMin = NEUTRAL_MIN;
  settings.neutralMax = NEUTRAL_MAX;
  settings.minPulse = MIN_PULSE;
  settings.maxPulse = MAX_PULSE;
  settings.neutralPulse = NEUTRAL_PULSE;
  
  settings.enableBackfire = enableBackfire;
  settings.enableBrakeCrackle = enableBrakeCrackle;
  settings.enableIdleBurble = enableIdleBurble;
  settings.enableRPMFlicker = enableRPMFlicker;
  
  settings.backfireThrottleMin = backfireThrottleMin;
  settings.backfireReleaseMax = backfireReleaseMax;
  settings.brakeThrottleMin = brakeThrottleMin;
  settings.brakeThrottleMax = brakeThrottleMax;
  settings.rpmFlickerThreshold = rpmFlickerThreshold;
  
  // Calculate and store CRC
  settings.crc = calculateSettingsCRC();
  
  // Write to EEPROM
  EEPROM.writeBytes(SETTINGS_START_ADDR, &settings, sizeof(settings));
  EEPROM.commit();
  
  USBSerial.println("[Settings] ✓ Settings saved to EEPROM");
}

void resetSettings() {
  memset(&settings, 0, sizeof(settings));
  memset(settings.ssid, 0, sizeof(settings.ssid));
  memset(settings.password, 0, sizeof(settings.password));
  EEPROM.writeBytes(SETTINGS_START_ADDR, &settings, sizeof(settings));
  EEPROM.commit();
  USBSerial.println("[Settings] EEPROM cleared");
}

///////////////////////
// ACCESS POINT MODE
///////////////////////

void startAPMode() {
  inAPMode = true;
  USBSerial.println("\n[WiFi] Starting in AP (Access Point) mode for setup");
  WiFi.mode(WIFI_AP);
  WiFi.softAP("afterfire-setup", "afterfire");
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  
  USBSerial.println("[AP] SSID: afterfire-setup");
  USBSerial.println("[AP] Password: afterfire");
  USBSerial.println("[AP] IP: 192.168.4.1");
  USBSerial.println("[AP] Connect to WiFi and visit http://192.168.4.1 to configure");
  
  // LED indication: fast orange blink
  for (int i = 0; i < 10; i++) {
    fill_solid(leds, NUM_LEDS, CRGB(255, 165, 0));
    FastLED.show();
    delay(100);
    FastLED.clear();
    FastLED.show();
    delay(100);
  }
}

void setupAPWebServer() {
  // Root page - Setup UI
  server.on("/", []() {
    String html = R"=====(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Afterfire Setup</title>
  <style>
    body {
      font-family: 'Segoe UI', Arial, sans-serif;
      background: linear-gradient(135deg, #1e1e1e 0%, #2d2d2d 100%);
      color: #fff;
      margin: 0;
      padding: 20px;
      text-align: center;
    }
    .container {
      max-width: 600px;
      margin: 50px auto;
    }
    h1 {
      color: #ff6b35;
      text-shadow: 0 0 10px rgba(255,107,53,0.5);
      margin-bottom: 10px;
    }
    .subtitle {
      color: #aaa;
      margin-bottom: 30px;
    }
    .card {
      background: rgba(255,255,255,0.1);
      border-radius: 10px;
      padding: 30px;
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255,255,255,0.2);
      text-align: left;
    }
    .form-group {
      margin-bottom: 20px;
    }
    label {
      display: block;
      margin-bottom: 8px;
      color: #aaa;
      font-size: 0.9em;
    }
    input[type="text"], input[type="password"], select {
      width: 100%;
      padding: 12px;
      box-sizing: border-box;
      background: rgba(255,255,255,0.1);
      border: 1px solid rgba(255,255,255,0.3);
      border-radius: 5px;
      color: #fff;
      font-size: 16px;
    }
    input[type="text"]:focus, input[type="password"]:focus, select:focus {
      outline: none;
      border-color: #ff6b35;
      background: rgba(255,107,53,0.1);
    }
    select {
      cursor: pointer;
    }
    option {
      background: #2d2d2d;
      color: #fff;
    }
    button {
      width: 100%;
      padding: 14px;
      background: #ff6b35;
      color: white;
      border: none;
      border-radius: 5px;
      cursor: pointer;
      font-size: 16px;
      font-weight: bold;
      margin-top: 10px;
    }
    button:hover {
      background: #ff8555;
    }
    button:disabled {
      background: #555;
      cursor: not-allowed;
    }
    .loading {
      display: none;
      text-align: center;
      color: #ff6b35;
    }
    .spinner {
      border: 3px solid rgba(255,255,255,0.3);
      border-top: 3px solid #ff6b35;
      border-radius: 50%;
      width: 40px;
      height: 40px;
      animation: spin 1s linear infinite;
      margin: 20px auto;
    }
    @keyframes spin {
      0% { transform: rotate(0deg); }
      100% { transform: rotate(360deg); }
    }
    .footer {
      margin-top: 20px;
      font-size: 0.85em;
      color: #666;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>🔥 Afterfire Setup</h1>
    <p class="subtitle">First-time configuration</p>
    
    <div class="card">
      <div id="setupForm">
        <div class="form-group">
          <label for="networkSelect">Available WiFi Networks:</label>
          <select id="networkSelect">
            <option value="">Scanning networks...</option>
          </select>
        </div>
        
        <div class="form-group">
          <label for="ssid">Network Name (SSID):</label>
          <input type="text" id="ssid" placeholder="Enter manually if not in list">
        </div>
        
        <div class="form-group">
          <label for="password">Password:</label>
          <input type="password" id="password" placeholder="WiFi password">
        </div>
        
        <button onclick="saveCredentials()">Connect & Save</button>
        <button onclick="location.reload()" style="background: #666; margin-top: 5px;">Rescan</button>
      </div>
      
      <div class="loading" id="loading">
        <p>Connecting to WiFi and saving settings...</p>
        <div class="spinner"></div>
        <p>Device will reboot in a moment...</p>
      </div>
    </div>
    
    <div class="footer">
      <p>Connected to: <strong>afterfire-setup</strong> (192.168.4.1)</p>
    </div>
  </div>

  <script>
    function scanNetworks() {
      document.getElementById('networkSelect').innerHTML = '<option value="">Scanning...</option>';
      fetch('/api/scan-networks')
        .then(r => r.json())
        .then(data => {
          const select = document.getElementById('networkSelect');
          select.innerHTML = '<option value="">-- Select network --</option>';
          data.networks.forEach(net => {
            const option = document.createElement('option');
            option.value = net.ssid;
            option.text = net.ssid + ' (' + net.rssi + ' dBm)';
            select.appendChild(option);
          });
        })
        .catch(err => {
          document.getElementById('networkSelect').innerHTML = '<option value="">Scan failed</option>';
          console.error(err);
        });
    }
    
    function saveCredentials() {
      const select = document.getElementById('networkSelect');
      let ssid = document.getElementById('ssid').value;
      const password = document.getElementById('password').value;
      
      if (select.value) {
        ssid = select.value;
      }
      
      if (!ssid || !password) {
        alert('Please enter both SSID and password');
        return;
      }
      
      document.getElementById('setupForm').style.display = 'none';
      document.getElementById('loading').style.display = 'block';
      
      fetch('/api/wifi/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ ssid: ssid, password: password })
      })
      .then(r => r.json())
      .then(data => {
        if (data.success) {
          console.log('Settings saved, rebooting...');
          setTimeout(() => location.reload(), 3000);
        } else {
          alert('Failed to save: ' + (data.error || 'Unknown error'));
          document.getElementById('setupForm').style.display = 'block';
          document.getElementById('loading').style.display = 'none';
        }
      })
      .catch(err => {
        alert('Error: ' + err);
        document.getElementById('setupForm').style.display = 'block';
        document.getElementById('loading').style.display = 'none';
      });
    }
    
    // Scan on load
    scanNetworks();
  </script>
</body>
</html>)=====";
    server.send(200, "text/html", html);
  });
  
  // Scan WiFi networks
  server.on("/api/scan-networks", []() {
    int n = WiFi.scanNetworks();
    String json = "{\"networks\":[";
    for (int i = 0; i < n; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  // Save WiFi credentials
  server.on("/api/wifi/save", HTTP_POST, []() {
    String body = server.arg("plain");
    USBSerial.print("[AP] WiFi config received: ");
    USBSerial.println(body);
    
    // Parse JSON (simple parsing without library to save memory)
    int ssidStart = body.indexOf("\"ssid\":\"") + 8;
    int ssidEnd = body.indexOf("\"", ssidStart);
    String newSSID = body.substring(ssidStart, ssidEnd);
    
    int pwdStart = body.indexOf("\"password\":\"") + 12;
    int pwdEnd = body.indexOf("\"", pwdStart);
    String newPwd = body.substring(pwdStart, pwdEnd);
    
    if (newSSID.length() > 0 && newPwd.length() > 0) {
      strncpy(settings.ssid, newSSID.c_str(), sizeof(settings.ssid) - 1);
      strncpy(settings.password, newPwd.c_str(), sizeof(settings.password) - 1);
      saveSettings();
      
      USBSerial.println("[AP] Settings saved, rebooting...");
      server.send(200, "application/json", "{\"success\":true}");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "application/json", "{\"success\":false,\"error\":\"Invalid credentials\"}");
    }
  });
}

///////////////////////
// BOOT LED SEQUENCE
///////////////////////

void bootSequence() {
  USBSerial.println("Starting boot sequence...");
  
  // Pulse red
  for (int i = 0; i < 255; i += 5) {
    fill_solid(leds, NUM_LEDS, CRGB(i, 0, 0));
    FastLED.show();
    delay(5);
  }
  for (int i = 255; i > 0; i -= 5) {
    fill_solid(leds, NUM_LEDS, CRGB(i, 0, 0));
    FastLED.show();
    delay(5);
  }
  
  // Pulse orange
  for (int i = 0; i < 255; i += 5) {
    fill_solid(leds, NUM_LEDS, CRGB(255, i, 0));
    FastLED.show();
    delay(5);
  }
  for (int i = 255; i > 0; i -= 5) {
    fill_solid(leds, NUM_LEDS, CRGB(255, i, 0));
    FastLED.show();
    delay(5);
  }
  
  // Pulse yellow-white
  for (int i = 0; i < 255; i += 5) {
    fill_solid(leds, NUM_LEDS, CRGB(255, 255, i));
    FastLED.show();
    delay(5);
  }
  for (int i = 255; i > 0; i -= 5) {
    fill_solid(leds, NUM_LEDS, CRGB(255, 255, i));
    FastLED.show();
    delay(5);
  }
  
  // Flash 3 times
  for (int j = 0; j < 3; j++) {
    fill_solid(leds, NUM_LEDS, CRGB(255, 140, 0));
    FastLED.show();
    delay(100);
    FastLED.clear();
    FastLED.show();
    delay(100);
  }
  
  USBSerial.println("Boot sequence complete!");
}

///////////////////////
// INTERRUPT
///////////////////////

void IRAM_ATTR readThrottle() {
  if (digitalRead(THROTTLE_PIN)) {
    pulseStart = micros();
  } else {
    pulseWidth = micros() - pulseStart;
  }
}

///////////////////////
// SETUP
///////////////////////

void setup() {
  // Initialize Serial for debugging
  USBSerial.begin(115200);
  delay(1000); // Wait for serial connection
  
  USBSerial.println("\n\n==================================");
  USBSerial.println("ESP32-S3 Afterfire Effect v1.0");
  USBSerial.println("WaveShare ESP32-S3-Zero");
  USBSerial.println("==================================");
  USBSerial.print("Chip Model: ");
  USBSerial.println(ESP.getChipModel());
  USBSerial.print("Chip Revision: ");
  USBSerial.println(ESP.getChipRevision());
  USBSerial.print("CPU Frequency: ");
  USBSerial.print(ESP.getCpuFreqMHz());
  USBSerial.println(" MHz");
  USBSerial.println("==================================");
  
  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadSettings();
  
  // Initialize throttle input
  pinMode(THROTTLE_PIN, INPUT);
  attachInterrupt(digitalPinToInterrupt(THROTTLE_PIN), readThrottle, CHANGE);
  USBSerial.println("Throttle interrupt attached to pin 2");

  // Initialize FastLED
  FastLED.addLeds<LED_TYPE, LED_PIN, COLOR_ORDER>(leds, NUM_LEDS);
  FastLED.setBrightness(MAX_BRIGHTNESS);
  FastLED.clear();
  FastLED.show();
  USBSerial.print("FastLED initialized: ");
  USBSerial.print(NUM_LEDS);
  USBSerial.println(" LED(s) on pin 3");
  
  // Run boot sequence
  bootSequence();
  
  // Check if WiFi credentials exist
  bool hasCredentials = (strlen(settings.ssid) > 0 && strlen(settings.password) > 0);
  
  if (!hasCredentials) {
    USBSerial.println("\n[WiFi] No credentials in EEPROM - starting AP mode");
    startAPMode();
    setupAPWebServer();
    server.begin();
    USBSerial.println("[AP] Web server started on port 80");
    return;
  }
  
  // Try to connect to saved WiFi
  USBSerial.println("\n[WiFi] Connecting to saved network...");
  USBSerial.print("[WiFi] SSID: ");
  USBSerial.println(settings.ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(settings.ssid, settings.password);
  
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 20) {
    delay(500);
    USBSerial.print(".");
    wifiAttempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    USBSerial.println("\n[WiFi] Connected!");
    USBSerial.print("[WiFi] IP Address: ");
    USBSerial.println(WiFi.localIP());
    USBSerial.print("[WiFi] Signal Strength: ");
    USBSerial.print(WiFi.RSSI());
    USBSerial.println(" dBm");
    
    // Setup OTA updates
    ArduinoOTA.setHostname("afterfire-esp32");
    ArduinoOTA.setPassword("afterfire2026");
    
    ArduinoOTA.onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else {
        type = "filesystem";
      }
      USBSerial.println("[OTA] Start updating " + type);
      fill_solid(leds, NUM_LEDS, CRGB::Red);
      FastLED.show();
    });
    
    ArduinoOTA.onEnd([]() {
      USBSerial.println("\n[OTA] Update complete!");
      fill_solid(leds, NUM_LEDS, CRGB::Green);
      FastLED.show();
    });
    
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      USBSerial.printf("[OTA] Progress: %u%%\r", (progress / (total / 100)));
    });
    
    ArduinoOTA.onError([](ota_error_t error) {
      USBSerial.printf("[OTA] Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) USBSerial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) USBSerial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) USBSerial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) USBSerial.println("Receive Failed");
      else if (error == OTA_END_ERROR) USBSerial.println("End Failed");
      fill_solid(leds, NUM_LEDS, CRGB::Red);
      FastLED.show();
    });
    
    ArduinoOTA.begin();
    USBSerial.println("[OTA] Ready for updates");
    
    // Setup web server routes
    setupWebServer();
    server.begin();
    USBSerial.println("[Web] Server started on port 80");
  } else {
    USBSerial.println("\n[WiFi] Connection failed - starting AP mode for reconfiguration");
    startAPMode();
    setupAPWebServer();
    server.begin();
    USBSerial.println("[AP] Web server started on port 80");
  }
  
  USBSerial.println("\nSystem ready!\n");
}

///////////////////////
// LOOP
///////////////////////

void loop() {

  // Handle web server requests (both AP and normal mode)
  server.handleClient();
  
  // Handle OTA updates (only in normal WiFi mode)
  if (!inAPMode && WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }

  uint16_t current = pulseWidth;

  // Handle calibration mode - manual step confirmation
  if (calibrationStep != CAL_IDLE && calibrationStep != CAL_COMPLETE) {
    // Just keep LEDs on during calibration to show it's active
    fill_solid(leds, NUM_LEDS, CRGB::Blue);
    FastLED.show();
    delay(5);
    return; // Don't run normal effects during calibration
  }
  
  if (calibrationStep == CAL_COMPLETE) {
    // Show green to indicate completion
    fill_solid(leds, NUM_LEDS, CRGB::Green);
    FastLED.show();
    delay(1000);
    calibrationStep = CAL_IDLE;
    return;
  }

  // Map throttle: brake to neutral to throttle
  int throttle;
  if (current >= NEUTRAL_MIN && current <= NEUTRAL_MAX) {
    throttle = 0;  // In neutral dead zone
  } else if (current > NEUTRAL_MAX) {
    // Forward throttle: neutral to max
    throttle = map(current, NEUTRAL_MAX, MAX_PULSE, 0, 100);
  } else {
    // Reverse/brake: min to neutral
    throttle = map(current, MIN_PULSE, NEUTRAL_MIN, -100, 0);
  }
  throttle = constrain(throttle, -100, 100);
  
  int prevThrottle;
  if (prevPulse >= NEUTRAL_MIN && prevPulse <= NEUTRAL_MAX) {
    prevThrottle = 0;
  } else if (prevPulse > NEUTRAL_MAX) {
    prevThrottle = map(prevPulse, NEUTRAL_MAX, MAX_PULSE, 0, 100);
  } else {
    prevThrottle = map(prevPulse, MIN_PULSE, NEUTRAL_MIN, -100, 0);
  }
  prevThrottle = constrain(prevThrottle, -100, 100);

  // Debug output every 500ms
  static unsigned long lastDebug = 0;
  if (millis() - lastDebug > 500) {
    USBSerial.print("PWM: ");
    USBSerial.print(current);
    USBSerial.print(" | Neutral Range: ");
    USBSerial.print(NEUTRAL_MIN);
    USBSerial.print("-");
    USBSerial.print(NEUTRAL_MAX);
    USBSerial.print(" | Throttle: ");
    USBSerial.print(throttle);
    USBSerial.print("% | Prev: ");
    USBSerial.print(prevThrottle);
    USBSerial.print("% | Burst: ");
    USBSerial.print(burstActive ? "YES" : "NO");
    USBSerial.print(" | BF:");
    USBSerial.print(enableBackfire ? "ON" : "OFF");
    USBSerial.print(" | BC:");
    USBSerial.print(enableBrakeCrackle ? "ON" : "OFF");
    USBSerial.print(" | IB:");
    USBSerial.println(enableIdleBurble ? "ON" : "OFF");
    lastDebug = millis();
  }

  handleRPMFlicker(throttle);
  detectBackfire(prevThrottle, throttle);
  detectBrakeCrackle(prevThrottle, throttle);
  idleBurble(throttle);
  handleBurst();
  
  // Turn off LEDs if no active effects and no burst
  if (!burstActive && !enableRPMFlicker && !enableIdleBurble) {
    fadeToBlackBy(leds, NUM_LEDS, 50);
  }

  prevPulse = current;

  FastLED.show();
  delay(5);
}

///////////////////////
// RPM FLICKER
///////////////////////

void handleRPMFlicker(int throttle) {
  if (!enableRPMFlicker) return;
  if (burstActive) return;

  if (throttle > rpmFlickerThreshold) {

    int baseHeat = map(throttle, rpmFlickerThreshold, 100, 120, 255);
    int flicker = random(-40, 40);

    setFlame(constrain(baseHeat + flicker, 80, 255));

  } else {
    fadeToBlackBy(leds, NUM_LEDS, 40);
  }
}

///////////////////////
// BACKFIRE DETECTION
///////////////////////

void detectBackfire(int prev, int now) {
  if (!enableBackfire) return;

  // Detect throttle release: was high throttle, now at neutral or low
  if (prev > backfireThrottleMin && now < backfireReleaseMax) {

    USBSerial.println("\n*** [BACKFIRE DETECTED] ***");
    USBSerial.print("prev: "); USBSerial.print(prev);
    USBSerial.print(" now: "); USBSerial.print(now);
    USBSerial.print(" threshold: >"); USBSerial.print(backfireThrottleMin);
    USBSerial.print(" release: <"); USBSerial.println(backfireReleaseMax);
    burstActive = true;
    burstCount = map(prev, backfireThrottleMin, 100, 3, 8);
    burstIntensity = map(prev, backfireThrottleMin, 100, 180, 255);
    lastEffectTime = millis();
  }
}

///////////////////////
// BRAKE CRACKLE
///////////////////////

void detectBrakeCrackle(int prev, int now) {
  if (!enableBrakeCrackle) return;

  if (prev > brakeThrottleMin && now < brakeThrottleMax && !burstActive) {

    USBSerial.println("\n*** [BRAKE CRACKLE DETECTED] ***");
    USBSerial.print("prev: "); USBSerial.print(prev);
    USBSerial.print(" now: "); USBSerial.println(now);
    burstActive = true;
    burstCount = random(3, 7);
    burstIntensity = random(160, 230);
    lastEffectTime = millis();
  }
}

///////////////////////
// HANDLE BURST (NON BLOCKING)
///////////////////////

void handleBurst() {

  if (!burstActive) return;

  if (millis() - lastEffectTime > random(20, 80)) {

    if (burstCount > 0) {

      // Backfire colors: blue, purple, red, orange at random
      int colorChoice = random(0, 10);
      CRGB color;
      
      if (colorChoice < 2) {
        // Blue flame (hot combustion)
        color = CRGB(random(0, 50), random(50, 150), random(180, 255));
      } else if (colorChoice < 4) {
        // Purple flame (fuel-rich)
        color = CRGB(random(100, 200), random(0, 80), random(150, 255));
      } else if (colorChoice < 7) {
        // Red-orange (unburned fuel)
        color = CRGB(255, random(50, 150), random(0, 30));
      } else {
        // Bright orange-yellow (hot flash)
        color = CRGB(255, random(150, 255), random(0, 100));
      }
      
      fill_solid(leds, NUM_LEDS, color);
      burstCount--;

    } else {

      // Fully turn off LEDs after burst completes
      fill_solid(leds, NUM_LEDS, CRGB::Black);
      burstActive = false;
    }

    lastEffectTime = millis();
  }
}

///////////////////////
// IDLE BURBLE
///////////////////////

void idleBurble(int throttle) {
  if (!enableIdleBurble) return;
  if (burstActive) return;

  if (abs(throttle) < 5) {
    if (random(0, 1000) < 4) {
      setFlame(random(100, 160));
    }
  }
}

///////////////////////
// FLAME COLOR MODEL
///////////////////////

void setFlame(int heat) {

  heat = constrain(heat, 0, 255);

  CRGB color;

  if (heat < 120) {
    color = CRGB(heat, heat / 4, 0);               // deep red
  }
  else if (heat < 200) {
    color = CRGB(255, heat, 0);                    // orange
  }
  else {
    color = CRGB(255, 255, heat - 200);            // yellow-white tip
  }

  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = color;
  }
}

///////////////////////
// WEB SERVER
///////////////////////

void setupWebServer() {
  
  // Root page - Web UI
  server.on("/", []() {
    String html = R"=====(<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Afterfire Effect Monitor</title>
  <style>
    body {
      font-family: 'Segoe UI', Arial, sans-serif;
      background: linear-gradient(135deg, #1e1e1e 0%, #2d2d2d 100%);
      color: #fff;
      margin: 0;
      padding: 20px;
    }
    .container {
      max-width: 800px;
      margin: 0 auto;
    }
    h1 {
      text-align: center;
      color: #ff6b35;
      text-shadow: 0 0 10px rgba(255,107,53,0.5);
    }
    .card {
      background: rgba(255,255,255,0.1);
      border-radius: 10px;
      padding: 20px;
      margin: 20px 0;
      backdrop-filter: blur(10px);
      border: 1px solid rgba(255,255,255,0.2);
    }
    .stat {
      display: flex;
      justify-content: space-between;
      padding: 10px 0;
      border-bottom: 1px solid rgba(255,255,255,0.1);
    }
    .stat:last-child { border-bottom: none; }
    .label { color: #aaa; }
    .value { 
      color: #ff6b35;
      font-weight: bold;
      font-size: 1.2em;
      display: flex;
      align-items: center;
      justify-content: flex-end;
    }
    .status-ok { color: #4caf50; }
    .status-warn { color: #ff9800; }
    .status-error { color: #f44336; }
    .burst-indicator {
      display: inline-block;
      width: 12px;
      height: 12px;
      border-radius: 50%;
      margin-left: 10px;
      background: #4caf50;
    }
    .burst-active {
      background: #ff6b35;
      animation: pulse 0.5s infinite;
    }
    @keyframes pulse {
      0%, 100% { opacity: 1; }
      50% { opacity: 0.5; }
    }
    button {
      background: #ff6b35;
      color: white;
      border: none;
      padding: 10px 20px;
      border-radius: 5px;
      cursor: pointer;
      font-size: 16px;
      margin: 5px;
    }
    button:hover { background: #ff8555; }
    .footer {
      text-align: center;
      margin-top: 30px;
      color: #888;
      font-size: 0.9em;
    }
    input[type="checkbox"] {
      width: 20px;
      height: 20px;
      cursor: pointer;
      vertical-align: middle;
      margin-right: 5px;
    }
    input[type="range"] {
      width: 150px;
      height: 6px;
      border-radius: 5px;
      background: rgba(255,255,255,0.2);
      outline: none;
      vertical-align: middle;
      margin-right: 10px;
      cursor: pointer;
    }
    input[type="range"]::-webkit-slider-thumb {
      -webkit-appearance: none;
      appearance: none;
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: #ff6b35;
      cursor: pointer;
    }
    input[type="range"]::-moz-range-thumb {
      width: 18px;
      height: 18px;
      border-radius: 50%;
      background: #ff6b35;
      cursor: pointer;
      border: none;
    }
    .toggle-btn {
      display: inline-block;
      width: 50px;
      height: 26px;
      background: #555;
      border-radius: 13px;
      position: relative;
      cursor: pointer;
      transition: background 0.3s;
      vertical-align: middle;
      margin-right: 10px;
    }
    .toggle-btn.active {
      background: #4caf50;
    }
    .toggle-btn:after {
      content: '';
      position: absolute;
      width: 20px;
      height: 20px;
      border-radius: 50%;
      background: white;
      top: 3px;
      left: 3px;
      transition: left 0.3s;
    }
    .toggle-btn.active:after {
      left: 27px;
    }
    .effect-status {
      font-size: 0.9em;
      color: #aaa;
    }
    .effect-status.active {
      color: #4caf50;
    }
  </style>
</head>
<body>
  <div class="container">
    <h1>🔥 Afterfire Effect Monitor</h1>
    
    <div class="card">
      <h2>System Status</h2>
      <div class="stat">
        <span class="label">Device</span>
        <span class="value">ESP32-S3-Zero</span>
      </div>
      <div class="stat">
        <span class="label">IP Address</span>
        <span class="value" id="ip">Loading...</span>
      </div>
      <div class="stat">
        <span class="label">Uptime</span>
        <span class="value" id="uptime">Loading...</span>
      </div>
      <div class="stat">
        <span class="label">WiFi Signal</span>
        <span class="value" id="rssi">Loading...</span>
      </div>
    </div>

    <div class="card">
      <h2>Throttle Status</h2>
      <div class="stat">
        <span class="label">PWM Signal</span>
        <span class="value" id="pwm">0 μs</span>
      </div>
      <div class="stat">
        <span class="label">Throttle Position</span>
        <span class="value" id="throttle">0%</span>
      </div>
      <div class="stat">
        <span class="label">Burst Active</span>
        <span class="value" id="burst">NO<span class="burst-indicator" id="burst-led"></span></span>
      </div>
    </div>

    <div class="card">
      <h2>Effect Controls</h2>
      <div class="stat">
        <span class="label">🔥 Backfire</span>
        <span class="value">
          <span class="toggle-btn active" id="toggleBackfire" onclick="toggleEffectBtn('backfire')"></span>
          <span class="effect-status active" id="backfireStatus">ON</span>
        </span>
      </div>
      <div class="stat">
        <span class="label">⚡ Brake Crackle</span>
        <span class="value">
          <span class="toggle-btn active" id="toggleBrake" onclick="toggleEffectBtn('brake')"></span>
          <span class="effect-status active" id="brakeStatus">ON</span>
        </span>
      </div>
      <div class="stat">
        <span class="label">💨 Idle Burble</span>
        <span class="value">
          <span class="toggle-btn active" id="toggleIdle" onclick="toggleEffectBtn('idle')"></span>
          <span class="effect-status active" id="idleStatus">ON</span>
        </span>
      </div>
      <div class="stat">
        <span class="label">🌡️ RPM Flicker</span>
        <span class="value">
          <span class="toggle-btn active" id="toggleRpm" onclick="toggleEffectBtn('rpm')"></span>
          <span class="effect-status active" id="rpmStatus">ON</span>
        </span>
      </div>
    </div>

    <div class="card">
      <h2>Backfire Sensitivity</h2>
      <div class="stat">
        <span class="label">Throttle Min</span>
        <span class="value"><input type="range" id="backfireMin" min="10" max="60" value="30" onchange="updateThreshold('backfireMin', this.value)"> <span id="backfireMinVal">30</span>%</span>
      </div>
      <div class="stat">
        <span class="label">Release Max</span>
        <span class="value"><input type="range" id="backfireMax" min="5" max="40" value="15" onchange="updateThreshold('backfireMax', this.value)"> <span id="backfireMaxVal">15</span>%</span>
      </div>
    </div>

    <div class="card">
      <h2>RPM Flicker Settings</h2>
      <div class="stat">
        <span class="label">Start Threshold</span>
        <span class="value"><input type="range" id="rpmThreshold" min="0" max="50" value="10" onchange="updateThreshold('rpmThreshold', this.value)"> <span id="rpmThresholdVal">10</span>%</span>
      </div>
      <p style="color:#aaa; font-size:0.9em; margin-top:10px;">Throttle position where LEDs start glowing (0% = immediate, 50% = near WOT)</p>
    </div>

    <div class="card">
      <h2>Controls</h2>
      <button onclick="testBackfire()">🔥 Test Backfire</button>
      <button onclick="testCrackle()">⚡ Test Crackle</button>
      <button onclick="calibrate()" id="calibrateBtn">🎯 Calibrate (10s)</button>
      <button onclick="location.reload()">🔄 Refresh</button>
    </div>

    <div class="card" id="calibrationCard" style="display:none; background: rgba(255,107,53,0.2);">
      <h2>🎯 Calibration Mode</h2>
      <div id="calStep1" style="display:none;">
        <h3>Step 1 of 3: Neutral Position</h3>
        <p><strong>1. Move throttle stick to CENTER/NEUTRAL position</strong></p>
        <p><strong>2. Click "Capture Neutral" when ready</strong></p>
        <p>Current PWM: <span id="currentPWM1" style="color:#4caf50; font-size:1.3em;">---</span> μs</p>
        <button onclick="captureNeutral()" style="background:#4caf50; font-size:18px; padding:15px 30px;">✓ Capture Neutral</button>
      </div>
      <div id="calStep2" style="display:none;">
        <h3>Step 2 of 3: Full Throttle</h3>
        <p><strong>1. Move throttle stick to FULL FORWARD position</strong></p>
        <p><strong>2. Click "Capture Throttle" when ready</strong></p>
        <p>Current PWM: <span id="currentPWM2" style="color:#4caf50; font-size:1.3em;">---</span> μs</p>
        <p style="color:#aaa;">✓ Neutral: <span id="savedNeutral">---</span> μs</p>
        <button onclick="captureThrottle()" style="background:#4caf50; font-size:18px; padding:15px 30px;">✓ Capture Throttle</button>
      </div>
      <div id="calStep3" style="display:none;">
        <h3>Step 3 of 3: Full Brake</h3>
        <p><strong>1. Move throttle stick to FULL REVERSE/BRAKE position</strong></p>
        <p><strong>2. Click "Capture Brake" when ready</strong></p>
        <p>Current PWM: <span id="currentPWM3" style="color:#4caf50; font-size:1.3em;">---</span> μs</p>
        <p style="color:#aaa;">✓ Neutral: <span id="savedNeutral2">---</span> μs</p>
        <p style="color:#aaa;">✓ Throttle: <span id="savedThrottle">---</span> μs</p>
        <button onclick="captureBrake()" style="background:#4caf50; font-size:18px; padding:15px 30px;">✓ Capture Brake</button>
      </div>
      <div id="calComplete" style="display:none;">
        <h3>✅ Calibration Complete!</h3>
        <p>Neutral: <span id="calNeutral">-</span> μs (±25 μs)</p>
        <p>Full Throttle: <span id="calThrottle">-</span> μs</p>
        <p>Full Brake: <span id="calBrake">-</span> μs</p>
        <p><em>Reloading in 2 seconds...</em></p>
      </div>
    </div>

    <div class="footer">
      ESP32-S3 Afterfire Effect v1.0<br>
      Auto-refresh every 2 seconds
    </div>
  </div>

  <script>
    function updateStats() {
      fetch('/api/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('ip').textContent = data.ip;
          document.getElementById('uptime').textContent = data.uptime;
          document.getElementById('rssi').textContent = data.rssi + ' dBm';
          document.getElementById('pwm').textContent = data.pwm + ' μs';
          document.getElementById('throttle').textContent = data.throttle + '%';
          document.getElementById('burst').innerHTML = data.burst + 
            '<span class="burst-indicator ' + (data.burst === 'YES' ? 'burst-active' : '') + '"></span>';
        });
    }
    
    function testBackfire() {
      fetch('/api/test/backfire').then(() => alert('Backfire triggered!'));
    }
    
    function testCrackle() {
      fetch('/api/test/crackle').then(() => alert('Crackle triggered!'));
    }
    
    function calibrate() {
      if (!confirm('Start manual calibration?\n\nYou will set each position individually with confirmation buttons.')) return;
      
      document.getElementById('calibrationCard').style.display = 'block';
      document.getElementById('calibrateBtn').disabled = true;
      
      fetch('/api/calibrate/start').then(r => r.json()).then(data => {
        if (data.status === 'started') {
          showCalibrationStep();
          startPWMMonitor();
        }
      });
    }
    
    let pwmMonitorInterval = null;
    
    function startPWMMonitor() {
      // Update current PWM reading every 200ms
      pwmMonitorInterval = setInterval(() => {
        fetch('/api/status').then(r => r.json()).then(data => {
          document.getElementById('currentPWM1').textContent = data.pwm;
          document.getElementById('currentPWM2').textContent = data.pwm;
          document.getElementById('currentPWM3').textContent = data.pwm;
        });
      }, 200);
    }
    
    function stopPWMMonitor() {
      if (pwmMonitorInterval) {
        clearInterval(pwmMonitorInterval);
        pwmMonitorInterval = null;
      }
    }
    
    function showCalibrationStep() {
      console.log('Checking calibration step...');
      fetch('/api/calibrate/status')
        .then(r => r.json())
        .then(data => {
          console.log('Current calibration step:', data);
          // Hide all steps first
          document.getElementById('calStep1').style.display = 'none';
          document.getElementById('calStep2').style.display = 'none';
          document.getElementById('calStep3').style.display = 'none';
          document.getElementById('calComplete').style.display = 'none';
          
          if (data.stepName === 'neutral') {
            document.getElementById('calStep1').style.display = 'block';
          } else if (data.stepName === 'throttle') {
            console.log('Showing throttle step');
            document.getElementById('calStep2').style.display = 'block';
          } else if (data.stepName === 'brake') {
            document.getElementById('calStep3').style.display = 'block';
          } else if (data.stepName === 'complete') {
            stopPWMMonitor();
            fetch('/api/calibrate/results').then(r => r.json()).then(d => {
              document.getElementById('calComplete').style.display = 'block';
              document.getElementById('calNeutral').textContent = d.neutral;
              document.getElementById('calThrottle').textContent = d.max;
              document.getElementById('calBrake').textContent = d.min;
              
              setTimeout(() => {
                document.getElementById('calibrationCard').style.display = 'none';
                document.getElementById('calibrateBtn').disabled = false;
                location.reload();
              }, 2000);
            });
          } else {
            stopPWMMonitor();
            document.getElementById('calibrationCard').style.display = 'none';
            document.getElementById('calibrateBtn').disabled = false;
          }
        })
        .catch(err => {
          console.error('Error checking calibration step:', err);
        });
    }
    
    function captureNeutral() {
      fetch('/api/calibrate/capture/neutral')
        .then(r => r.json())
        .then(data => {
          if (data.captured) {
            document.getElementById('savedNeutral').textContent = data.value;
            document.getElementById('savedNeutral2').textContent = data.value;
            showCalibrationStep();
          } else {
            alert('Failed to capture neutral: ' + (data.error || 'Unknown error'));
          }
        })
        .catch(err => {
          alert('Error capturing neutral: ' + err);
          console.error('Capture neutral error:', err);
        });
    }
    
    function captureThrottle() {
      console.log('Capturing throttle...');
      fetch('/api/calibrate/capture/throttle')
        .then(r => {
          console.log('Response status:', r.status);
          return r.json();
        })
        .then(data => {
          console.log('Response data:', data);
          if (data.captured) {
            document.getElementById('savedThrottle').textContent = data.value;
            showCalibrationStep();
          } else {
            alert('Failed to capture throttle: ' + (data.error || 'Unknown error'));
          }
        })
        .catch(err => {
          alert('Error capturing throttle: ' + err);
          console.error('Capture throttle error:', err);
        });
    }
    
    function captureBrake() {
      fetch('/api/calibrate/capture/brake')
        .then(r => r.json())
        .then(data => {
          if (data.captured) {
            showCalibrationStep();
          } else {
            alert('Failed to capture brake: ' + (data.error || 'Unknown error'));
          }
        })
        .catch(err => {
          alert('Error capturing brake: ' + err);
          console.error('Capture brake error:', err);
        });
    }
    
    function toggleEffectBtn(effect) {
      const toggleMap = {
        'backfire': 'toggleBackfire',
        'brake': 'toggleBrake',
        'idle': 'toggleIdle',
        'rpm': 'toggleRpm'
      };
      const statusMap = {
        'backfire': 'backfireStatus',
        'brake': 'brakeStatus',
        'idle': 'idleStatus',
        'rpm': 'rpmStatus'
      };
      
      const toggleBtn = document.getElementById(toggleMap[effect]);
      const isActive = toggleBtn.classList.contains('active');
      
      fetch('/api/effects/' + effect + '/' + (isActive ? 'off' : 'on'))
        .then(r => r.json())
        .then(d => {
          if (d.enabled) {
            toggleBtn.classList.add('active');
            document.getElementById(statusMap[effect]).classList.add('active');
            document.getElementById(statusMap[effect]).textContent = 'ON';
          } else {
            toggleBtn.classList.remove('active');
            document.getElementById(statusMap[effect]).classList.remove('active');
            document.getElementById(statusMap[effect]).textContent = 'OFF';
          }
        });
    }
    
    function updateThreshold(param, value) {
      document.getElementById(param + 'Val').textContent = value;
      fetch('/api/threshold?param=' + param + '&value=' + value);
    }
    
    updateStats();
    setInterval(updateStats, 2000);
  </script>
</body>
</html>)=====";
    server.send(200, "text/html", html);
  });
  
  // API endpoint - Status
  server.on("/api/status", []() {
    // Calculate throttle position the same way as loop()
    uint16_t current = pulseWidth;
    int throttle;
    if (current >= NEUTRAL_MIN && current <= NEUTRAL_MAX) {
      throttle = 0;  // In neutral dead zone
    } else if (current > NEUTRAL_MAX) {
      throttle = map(current, NEUTRAL_MAX, MAX_PULSE, 0, 100);
    } else {
      throttle = map(current, MIN_PULSE, NEUTRAL_MIN, -100, 0);
    }
    throttle = constrain(throttle, -100, 100);
    
    unsigned long upSeconds = millis() / 1000;
    unsigned long days = upSeconds / 86400;
    unsigned long hours = (upSeconds % 86400) / 3600;
    unsigned long mins = (upSeconds % 3600) / 60;
    unsigned long secs = upSeconds % 60;
    
    String uptime = String(days) + "d " + String(hours) + "h " + String(mins) + "m " + String(secs) + "s";
    
    String json = "{";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"uptime\":\"" + uptime + "\",";
    json += "\"rssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"pwm\":" + String(current) + ",";
    json += "\"throttle\":" + String(throttle) + ",";
    json += "\"burst\":\"" + String(burstActive ? "YES" : "NO") + "\"";
    json += "}";
    
    server.send(200, "application/json", json);
  });
  
  // API endpoint - Test Backfire
  server.on("/api/test/backfire", []() {
    USBSerial.println("[Web] Manual backfire triggered");
    burstActive = true;
    burstCount = 5;
    burstIntensity = 240;
    lastEffectTime = millis();
    server.send(200, "text/plain", "Backfire triggered");
  });
  
  // API endpoint - Test Crackle
  server.on("/api/test/crackle", []() {
    USBSerial.println("[Web] Manual crackle triggered");
    burstActive = true;
    burstCount = 6;
    burstIntensity = 200;
    lastEffectTime = millis();
    server.send(200, "text/plain", "Crackle triggered");
  });
  
  // API endpoint - Get Calibration Status
  server.on("/api/calibrate/status", []() {
    String json = "{";
    json += "\"step\":" + String((int)calibrationStep) + ",";
    json += "\"stepName\":";
    if (calibrationStep == CAL_IDLE) json += "\"idle\"";
    else if (calibrationStep == CAL_NEUTRAL) json += "\"neutral\"";
    else if (calibrationStep == CAL_THROTTLE) json += "\"throttle\"";
    else if (calibrationStep == CAL_BRAKE) json += "\"brake\"";
    else if (calibrationStep == CAL_COMPLETE) json += "\"complete\"";
    json += "}";
    server.send(200, "application/json", json);
  });
  
  // API endpoint - Start Calibration
  server.on("/api/calibrate/start", []() {
    USBSerial.println("\n[Cal] === STARTING MANUAL CALIBRATION ===");
    USBSerial.println("[Cal] Step 1: Waiting for NEUTRAL capture...");
    calibrationStep = CAL_NEUTRAL;
    
    String json = "{\"status\":\"started\"}";
    server.send(200, "application/json", json);
  });
  
  // API endpoint - Capture Neutral
  server.on("/api/calibrate/capture/neutral", []() {
    if (calibrationStep == CAL_NEUTRAL) {
      calibratedNeutral = pulseWidth;
      NEUTRAL_PULSE = calibratedNeutral;
      NEUTRAL_MIN = calibratedNeutral - 25;
      NEUTRAL_MAX = calibratedNeutral + 25;
      USBSerial.print("[Cal] ✓ Neutral captured: "); USBSerial.println(calibratedNeutral);
      USBSerial.println("[Cal] Step 2: Waiting for THROTTLE capture...");
      calibrationStep = CAL_THROTTLE;
      
      String json = "{\"captured\":true,\"value\":" + String(calibratedNeutral) + "}";
      server.send(200, "application/json", json);
    } else {
      server.send(400, "application/json", "{\"captured\":false,\"error\":\"Wrong step\"}");
    }
  });
  
  // API endpoint - Capture Throttle
  server.on("/api/calibrate/capture/throttle", []() {
    USBSerial.print("[Cal] Throttle capture request received. Current step: ");
    USBSerial.println(calibrationStep);
    
    if (calibrationStep == CAL_THROTTLE) {
      calibratedThrottle = pulseWidth;
      MAX_PULSE = calibratedThrottle;
      USBSerial.print("[Cal] ✓ Throttle captured: "); USBSerial.println(calibratedThrottle);
      USBSerial.println("[Cal] Step 3: Waiting for BRAKE capture...");
      calibrationStep = CAL_BRAKE;
      
      String json = "{\"captured\":true,\"value\":" + String(calibratedThrottle) + "}";
      server.send(200, "application/json", json);
    } else {
      USBSerial.println("[Cal] ERROR: Wrong step for throttle capture!");
      String json = "{\"captured\":false,\"error\":\"Wrong step (expected CAL_THROTTLE)\"}";
      server.send(400, "application/json", json);
    }
  });
  
  // API endpoint - Capture Brake
  server.on("/api/calibrate/capture/brake", []() {
    if (calibrationStep == CAL_BRAKE) {
      calibratedBrake = pulseWidth;
      MIN_PULSE = calibratedBrake;
      USBSerial.print("[Cal] ✓ Brake captured: "); USBSerial.println(calibratedBrake);
      
      USBSerial.println("\n[Cal] === CALIBRATION COMPLETE ===");
      USBSerial.print("Neutral: "); USBSerial.print(NEUTRAL_PULSE);
      USBSerial.print(" (range: "); USBSerial.print(NEUTRAL_MIN);
      USBSerial.print("-"); USBSerial.print(NEUTRAL_MAX); USBSerial.println(")");
      USBSerial.print("Full Throttle: "); USBSerial.println(MAX_PULSE);
      USBSerial.print("Full Brake: "); USBSerial.println(MIN_PULSE);
      
      calibrationStep = CAL_COMPLETE;
      
      // Save calibration to EEPROM
      saveSettings();
      
      String json = "{\"captured\":true,\"value\":" + String(calibratedBrake) + "}";
      server.send(200, "application/json", json);
    } else {
      server.send(400, "application/json", "{\"captured\":false,\"error\":\"Wrong step\"}");
    }
  });
  
  // API endpoint - Get Calibration Results
  server.on("/api/calibrate/results", []() {
    String json = "{";
    json += "\"min\":" + String(MIN_PULSE) + ",";
    json += "\"max\":" + String(MAX_PULSE) + ",";
    json += "\"neutral\":" + String(NEUTRAL_PULSE) + ",";
    json += "\"neutral_min\":" + String(NEUTRAL_MIN) + ",";
    json += "\"neutral_max\":" + String(NEUTRAL_MAX);
    json += "}";
    server.send(200, "application/json", json);
  });
  
  // API endpoints - Toggle Effects
  server.on("/api/effects/backfire/on", []() { enableBackfire = true; saveSettings(); server.send(200, "application/json", "{\"enabled\":true}"); });
  server.on("/api/effects/backfire/off", []() { enableBackfire = false; saveSettings(); server.send(200, "application/json", "{\"enabled\":false}"); });
  server.on("/api/effects/brake/on", []() { enableBrakeCrackle = true; saveSettings(); server.send(200, "application/json", "{\"enabled\":true}"); });
  server.on("/api/effects/brake/off", []() { enableBrakeCrackle = false; saveSettings(); server.send(200, "application/json", "{\"enabled\":false}"); });
  server.on("/api/effects/idle/on", []() { enableIdleBurble = true; saveSettings(); server.send(200, "application/json", "{\"enabled\":true}"); });
  server.on("/api/effects/idle/off", []() { enableIdleBurble = false; saveSettings(); server.send(200, "application/json", "{\"enabled\":false}"); });
  server.on("/api/effects/rpm/on", []() { enableRPMFlicker = true; saveSettings(); server.send(200, "application/json", "{\"enabled\":true}"); });
  server.on("/api/effects/rpm/off", []() { enableRPMFlicker = false; saveSettings(); server.send(200, "application/json", "{\"enabled\":false}"); });
  
  // API endpoints - Threshold Adjustments (using query params)
  server.on("/api/threshold", []() { 
    if (server.hasArg("param") && server.hasArg("value")) {
      String param = server.arg("param");
      int value = server.arg("value").toInt();
      
      if (param == "backfireMin") {
        backfireThrottleMin = value;
        USBSerial.print("[Web] Backfire throttle min set to: "); USBSerial.println(value);
      } else if (param == "backfireMax") {
        backfireReleaseMax = value;
        USBSerial.print("[Web] Backfire release max set to: "); USBSerial.println(value);
      } else if (param == "rpmThreshold") {
        rpmFlickerThreshold = value;
        USBSerial.print("[Web] RPM flicker threshold set to: "); USBSerial.print(value); USBSerial.println("%");
      }
      
      // Save settings after any threshold change
      saveSettings();
    }
    server.send(200, "text/plain", "OK");
  });
}
