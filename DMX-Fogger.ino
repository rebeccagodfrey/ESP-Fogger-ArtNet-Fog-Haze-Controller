#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <AsyncUDP.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// -------------------------------------------------------------------------
// CONFIGURATION
// -------------------------------------------------------------------------

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Relay Pins
#define RELAY_FOG_PIN 26
#define RELAY_FAN_PIN 27

// Protocol settings
#define ARTNET_PORT 6454

// -------------------------------------------------------------------------
// CLASS DEFINITION
// -------------------------------------------------------------------------


class ProLightingInterface {
  private:
    WebServer server;
    WiFiManager wifiManager;
    Preferences preferences;
    Adafruit_SSD1306 display;
    AsyncUDP udp;

    // Configuration variables
    String nodeName = "SGP-Fogger-Control";
    int artNetNet = 0;
    int artNetSubnet = 0;
    int artNetUniverse = 0;
    int dmxStartAddr = 1; // [NEW] DMX Start Address
    
    // Runtime variables
    bool wifiConnected = false;
    unsigned long lastPacketTime = 0;
    bool dataReceiving = false;
    
    // Logic States
    uint8_t dmxValues[3] = {0, 0, 0}; // Ch1: Fog, Ch2: Fan, Ch3: Hazer
    bool relayFogState = false;
    bool relayFanState = false;
    
    // Hazer Timing
    unsigned long lastHazePulseTime = 0;
    bool isHazing = false; // logic state for haze cycle

    // Display
    unsigned long lastDisplayUpdate = 0;
    const unsigned long DISPLAY_UPDATE_INTERVAL = 1000;

  public:
    ProLightingInterface() : server(80), display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET) {}

    void begin() {
      Serial.begin(115200);
      delay(1000); // Give serial time to start
      Serial.println("Starting DMX-Fogger...");
      

      // Setup Pins
      pinMode(RELAY_FOG_PIN, OUTPUT);
      pinMode(RELAY_FAN_PIN, OUTPUT);
      digitalWrite(RELAY_FOG_PIN, HIGH); // OFF default (Active LOW)
      digitalWrite(RELAY_FAN_PIN, HIGH); // OFF default (Active LOW)

      // Setup Preferences
      preferences.begin("prolight", false);
      loadConfiguration();

      // Setup Display
      // Explicitly initialize I2C pins for ESP32
      Wire.begin(21, 22);

      // [NEW] I2C Scanner for debugging
      Serial.println("Scanning I2C bus...");
      byte error, address;
      int nDevices = 0;
      for(address = 1; address < 127; address++ ) {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        if (error == 0) {
          Serial.print("I2C device found at address 0x");
          if (address < 16) Serial.print("0");
          Serial.print(address, HEX);
          Serial.println("  !");
          nDevices++;
        }
      }
      if (nDevices == 0) Serial.println("No I2C devices found\n");
      else Serial.println("done\n");
      // End Scanner

      if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println(F("SSD1306 allocation failed"));
        // Blink LED to indicate error
        pinMode(2, OUTPUT);
        while(1) { digitalWrite(2, !digitalRead(2)); delay(100); }
      }
      Serial.println("SSD1306 Started");
      display.clearDisplay();
      display.setTextColor(SSD1306_WHITE);
      display.setTextSize(1);
      display.setCursor(0,0);
      display.println(F("Booting..."));
      display.println(nodeName);
      display.display();

      // Setup WiFi
      Serial.println("Setting up WiFiManager...");
      setupWiFi();

      // Setup Web & UDP
      if (wifiConnected) {
        setupWebServer();
        setupUDP();
      }
    }

    void loop() {
      // Handle WiFi Manager portal if active
      wifiManager.process();
      
      // Handle Web Server
      server.handleClient();

      // Logic Loop
      updateRelayLogic();
      
      // Display Loop
      if (millis() - lastDisplayUpdate > DISPLAY_UPDATE_INTERVAL) {
        updateDisplay();
        lastDisplayUpdate = millis();
      }

      // Data Timeout (5 seconds)
      if (millis() - lastPacketTime > 5000 && dataReceiving) {
        dataReceiving = false;
      }
    }

    void loadConfiguration() {
      nodeName = preferences.getString("name", "SGP-Fogger-Control");
      artNetNet = preferences.getInt("net", 0);
      artNetSubnet = preferences.getInt("subnet", 0);
      artNetUniverse = preferences.getInt("universe", 0);
      dmxStartAddr = preferences.getInt("dmxAddr", 1);
    }

    void saveConfiguration() {
      preferences.putString("name", nodeName);
      preferences.putInt("net", artNetNet);
      preferences.putInt("subnet", artNetSubnet);
      preferences.putInt("universe", artNetUniverse);
      preferences.putInt("dmxAddr", dmxStartAddr);
    }

    bool setupWiFi() {
      WiFi.mode(WIFI_AP_STA);
      
      wifiManager.setConfigPortalTimeout(180); // 3 minutes timeout
      wifiManager.setAPCallback([this](WiFiManager *myWiFiManager) {
        display.clearDisplay();
        display.setCursor(0,0);
        display.println("Setup Mode");
        display.println("Connect to:");
        display.println(myWiFiManager->getConfigPortalSSID());
        display.println("IP: 192.168.4.1");
        display.display();
      });

      if(!wifiManager.autoConnect("SGP-Fogger-Setup")) {
        Serial.println("failed to connect and hit timeout");
        // If we timeout, we still try to run, maybe no wifi but just loops
      } else {
        Serial.println("connected...yeey :)");
        wifiConnected = true;
      }
      
      return wifiConnected;
    }

    void setupWebServer() {
      server.on("/", HTTP_GET, [this]() {
        server.send(200, "text/html", getMainPage());
      });

      server.on("/save", HTTP_POST, [this]() {
        if (server.hasArg("name")) nodeName = server.arg("name");
        if (server.hasArg("net")) artNetNet = server.arg("net").toInt();
        if (server.hasArg("subnet")) artNetSubnet = server.arg("subnet").toInt();
        if (server.hasArg("universe")) artNetUniverse = server.arg("universe").toInt();
        if (server.hasArg("addr")) dmxStartAddr = server.arg("addr").toInt();
        
        saveConfiguration();
        
        server.sendHeader("Location", "/");
        server.send(303);
      });

      server.on("/reset", HTTP_POST, [this]() {
        wifiManager.resetSettings();
        ESP.restart();
      });

      // [NEW] Status Endpoint
      server.on("/status", HTTP_GET, [this]() {
         StaticJsonDocument<200> doc;
         JsonArray dmx = doc.createNestedArray("dmx");
         dmx.add(dmxValues[0]);
         dmx.add(dmxValues[1]);
         dmx.add(dmxValues[2]);
         
         doc["relayFog"] = relayFogState;
         doc["relayFan"] = relayFanState;
         
         if (dmxValues[2] > 10) {
             float onSec = map(dmxValues[2], 0, 255, 500, 10000) / 1000.0;
             float offSec = map(dmxValues[2], 0, 255, 60000, 2000) / 1000.0;
             JsonObject haze = doc.createNestedObject("haze");
             haze["val"] = dmxValues[2];
             haze["on"] = onSec;
             haze["off"] = offSec;
         } else {
             doc["haze"] = (char*)NULL;
         }
         
         String json;
         serializeJson(doc, json);
         server.send(200, "application/json", json);
      });

      server.begin();
    }

    void setupUDP() {
      if(udp.listen(ARTNET_PORT)) {
        Serial.print("UDP Listening on port: ");
        Serial.println(ARTNET_PORT);
        udp.onPacket([this](AsyncUDPPacket packet) {
          handleArtNetPacket(packet);
        });
      }
    }

    void handleArtNetPacket(AsyncUDPPacket packet) {
       // Simple ArtNet Check (Header "Art-Net" + OpCode 0x5000)
       if (packet.length() < 18) return;
       uint8_t* data = packet.data();
       
       if (memcmp(data, "Art-Net", 8) != 0) return; // ID
       if (data[8] != 0x00 || data[9] != 0x50) return; // OpOutput (0x5000 little endian)
       
       // Universe check
       uint8_t incomingSubUni = data[14];
       uint8_t incomingNet = data[15];

       uint8_t userSubUni = (artNetSubnet << 4) | (artNetUniverse & 0x0F);
       
       if (incomingNet == artNetNet && incomingSubUni == userSubUni) {
          int dmxDataLength = (data[16] << 8) | data[17];
          
          // Calculate indices based on DMX Start Address (1-based)
          int startIndex = dmxStartAddr - 1; // 0-based index in DMX data
          
          // Check if we have enough data for our 3 channels
          // DMX data starts at byte 18
          // We need bytes at 18+startIndex, 18+startIndex+1, 18+startIndex+2
          // Check if the packet actually contains these bytes
          if (dmxDataLength > startIndex + 2) {
             lastPacketTime = millis();
             dataReceiving = true;
             
             dmxValues[0] = data[18 + startIndex];
             dmxValues[1] = data[18 + startIndex + 1];
             dmxValues[2] = data[18 + startIndex + 2];
             
             // Debug print occasionally (every ~1s) to avoid flood
             if (millis() % 1000 < 20) {
                 Serial.printf("ArtNet OK: Ch%d=%d Ch%d=%d Ch%d=%d\n", dmxStartAddr, dmxValues[0], dmxStartAddr+1, dmxValues[1], dmxStartAddr+2, dmxValues[2]);
             }
          } else {
             Serial.printf("ArtNet Err: Packet too short. Need index %d, got %d\n", startIndex+2, dmxDataLength);
          }
       } else {
          // Debug universe mismatch (occasionally)
          if (millis() % 2000 < 20) { 
              Serial.printf("ArtNet Ignored: Net %d/%d SubUni %d/%d\n", incomingNet, artNetNet, incomingSubUni, userSubUni);
          }
       }
    }

    void updateRelayLogic() {
      // Channel 1: Fogger (Relay 1 ON if > 127)
      bool fogReq = (dmxValues[0] > 127);
      
      // Channel 2: Fan (Relay 2 ON if > 127)
      bool fanReq = (dmxValues[1] > 127);
      
      // Channel 3: Hazer (Graduated control on Relay 1)
      bool hazerPulse = false;
      uint8_t hazeVal = dmxValues[2];
      
      if (hazeVal > 10) { // Deadzone at bottom
         // Cycle Logic
         unsigned long onTime = map(hazeVal, 0, 255, 500, 10000); // 0.5s to 10s
         unsigned long offTime = map(hazeVal, 0, 255, 60000, 2000); // 60s to 2s
         
         unsigned long now = millis();
         if (isHazing) {
            // Currently ON
            if (now - lastHazePulseTime > onTime) {
               isHazing = false; // Turn OFF
               lastHazePulseTime = now;
            } else {
               hazerPulse = true;
            }
         } else {
            // Currently OFF
            if (now - lastHazePulseTime > offTime) {
               isHazing = true; // Turn ON
               lastHazePulseTime = now;
               hazerPulse = true;
            }
         }
      } else {
         isHazing = false;
      }

      // Final Relay States
      relayFogState = fogReq || hazerPulse;
      relayFanState = fanReq;

      // Active LOW logic: LOW is ON, HIGH is OFF
      digitalWrite(RELAY_FOG_PIN, relayFogState ? LOW : HIGH);
      digitalWrite(RELAY_FAN_PIN, relayFanState ? LOW : HIGH);
    }

    void updateDisplay() {
      display.clearDisplay();
      
      // Header
      display.setTextSize(1);
      display.setCursor(0,0);
      display.print(nodeName);
      
      // Network & Channel
      display.setCursor(0,10);
      if (wifiConnected) {
        display.print("IP:");
        display.print(WiFi.localIP());
        display.print(" Ch:");
        display.print(dmxStartAddr);
      } else {
        display.print("No WiFi");
      }
      
      
      // Rx Status
      display.setCursor(114, 0); 
      if (dataReceiving) display.print("RX");
      
      // Heartbeat dot to verify display is updating
      if ((millis() / 500) % 2 == 0) display.drawPixel(127, 0, SSD1306_WHITE);

      // Channel Values
      display.setCursor(0, 22);
      display.printf("FOG:%d FAN:%d U:%d", dmxValues[0], dmxValues[1], artNetUniverse);
      
      display.setCursor(0, 32);
      if (dmxValues[2] > 10) {
         // Re-calculate timings for display (same logic as Relay)
         float onSec = map(dmxValues[2], 0, 255, 500, 10000) / 1000.0;
         float offSec = map(dmxValues[2], 0, 255, 60000, 2000) / 1000.0;
         display.printf("H:%d On:%.1fs Off:%.0fs", dmxValues[2], onSec, offSec);
      } else {
         display.printf("HAZE:%3d", dmxValues[2]);
      }

      // Relay Status Visuals (Big Text)
      display.setTextSize(2);
      
      if (relayFogState) {
        display.setCursor(0, 48); 
        display.print("FOG");
      }
      
      if (relayFanState) {
        // Align Right: 128px - (3 chars * 12px/char) = 92
        display.setCursor(92, 48);
        display.print("FAN");
      }
      
      display.display();
    }

    String getMainPage() {
      // Re-read current config to ensure we show latest
      // (Though Variables should be up to date)
      
      String html = "<!DOCTYPE html><html><head><title>" + nodeName + "</title>";
      html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
      html += "<style>";
      html += "body{font-family:sans-serif;background:#1a1a1a;color:#fff;padding:20px;}";
      html += ".card{background:#2d2d2d;padding:20px;border-radius:8px;max-width:500px;margin:0 auto;margin-bottom:20px;}";
      html += "input,select{width:100%;padding:10px;margin:5px 0;background:#3d3d3d;border:1px solid #4d4d4d;color:#fff;border-radius:4px;}";
      html += "button{background:#007bff;color:white;padding:10px 20px;border:none;border-radius:4px;width:100%;margin-top:20px;cursor:pointer;}";
      html += "button.reset{background:#dc3545;margin-top:10px;}"; 
      html += "label{display:block;margin-top:10px;color:#aaa;}";
      html += "h1{text-align:center;color:#007bff;}";
      
      // Dashboard Styles
      html += ".status-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;margin-bottom:15px;}";
      html += ".status-item{background:#3d3d3d;padding:15px;border-radius:6px;text-align:center;}";
      html += ".badge{display:inline-block;padding:5px 10px;border-radius:4px;font-weight:bold;font-size:1.2em;}";
      html += ".badge.on{background:#28a745;color:white;}"; // Green
      html += ".badge.off{background:#6c757d;color:#ccc;}"; // Grey
      html += ".haze-info{grid-column: span 2; font-size: 0.9em; color:#ddd;}";
      html += "</style></head><body>";
      
      // DASHBOARD CARD
      html += "<div class='card'><h2>Live Status</h2>";
      html += "<div class='status-grid'>";
      
      // Fog Badge
      html += "<div class='status-item'><div id='fog-badge' class='badge off'>FOG</div><br><small id='fog-val'>0</small></div>";
      
      // Fan Badge
      html += "<div class='status-item'><div id='fan-badge' class='badge off'>FAN</div><br><small id='fan-val'>0</small></div>";
      
      // Haze Info
      html += "<div class='status-item haze-info'><strong>Haze:</strong> <span id='haze-status'>OFF</span></div>";
      
      html += "</div>"; // end grid
      html += "</div>"; // end card
      
      // CONFIG CARD
      html += "<div class='card'><h1>" + nodeName + "</h1>";
      html += "<form action='/save' method='POST'>";
      html += "<label>Node Name</label><input type='text' name='name' value='" + nodeName + "'>";
      
      html += "<h3>Art-Net Settings</h3>";
      html += "<label>Net (0-127)</label><input type='number' name='net' min='0' max='127' value='" + String(artNetNet) + "'>";
      html += "<label>Subnet (0-15)</label><input type='number' name='subnet' min='0' max='15' value='" + String(artNetSubnet) + "'>";
      html += "<label>Universe (0-15)</label><input type='number' name='universe' min='0' max='15' value='" + String(artNetUniverse) + "'>";
      html += "<label>DMX Start Address (1-512)</label><input type='number' name='addr' min='1' max='510' value='" + String(dmxStartAddr) + "'>";
      
      html += "<button type='submit'>Save Configuration</button>";
      html += "</form>";
      
      html += "<form action='/reset' method='POST' onsubmit='return confirm(\"Reset WiFi settings?\");'>";
      html += "<button type='submit' class='reset'>Reset WiFi Settings</button>";
      html += "</form>";
      
      html += "</div>"; // end config card
      
      // JAVASCRIPT
      html += "<script>";
      html += "function fetchStatus() {";
      html += "  fetch('/status').then(r => r.json()).then(data => {";
      
      // Update FOG
      html += "    const fogEl = document.getElementById('fog-badge');";
      html += "    if(data.relayFog) { fogEl.className = 'badge on'; } else { fogEl.className = 'badge off'; }";
      html += "    document.getElementById('fog-val').innerText = 'DMX: ' + data.dmx[0];";

      // Update FAN
      html += "    const fanEl = document.getElementById('fan-badge');";
      html += "    if(data.relayFan) { fanEl.className = 'badge on'; } else { fanEl.className = 'badge off'; }";
      html += "    document.getElementById('fan-val').innerText = 'DMX: ' + data.dmx[1];";
      
      // Update HAZE
      html += "    const hazeEl = document.getElementById('haze-status');";
      html += "    if(data.haze) {";
      html += "       hazeEl.innerText = 'Val: ' + data.haze.val + ' (' + data.haze.on.toFixed(1) + 's ON / ' + data.haze.off.toFixed(0) + 's OFF)';";
      html += "    } else {";
      html += "       hazeEl.innerText = 'OFF';";
      html += "    }";

      html += "  }).catch(e => console.log(e));";
      html += "}";
      html += "setInterval(fetchStatus, 1000);"; // Poll every second
      html += "fetchStatus();"; // Initial call
      html += "</script>";
      
      html += "</body></html>";
      return html;
    }
};

ProLightingInterface lightingInterface;

void setup() {
  lightingInterface.begin();
}

void loop() {
  lightingInterface.loop();
}