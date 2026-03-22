#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <IPAddress.h>
#include "time.h"
#include "wifi_provisioning.h"

/* --- Hardware Configuration --- */
#define RX2_PIN      16
#define TX2_PIN      17
#define BUTTON_PIN   4       
#define E75_BAUD     500000  

/* --- NTP Settings --- */
const char* ntpServer = "pool.ntp.org";

/* --- Static Ring Buffer (Memory Safe Logging) --- */
const int MAX_LOG_LINES = 50;
const int MAX_LINE_LEN = 120;
char logBuffer[MAX_LOG_LINES][MAX_LINE_LEN];
int logIndex = 0;
bool bufferFull = false;

void addToLog(const char* msg) {
  struct tm timeinfo;
  char tStr[12];
  if(!getLocalTime(&timeinfo)) strcpy(tStr, "[00:00:00]");
  else strftime(tStr, sizeof(tStr), "[%H:%M:%S]", &timeinfo);
  
  snprintf(logBuffer[logIndex], MAX_LINE_LEN, "%s %s", tStr, msg);
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
  if (logIndex == 0) bufferFull = true;
  Serial.println(msg); 
}

String getFullLog() {
  String output = "";
  output.reserve(4000); 
  int start = bufferFull ? logIndex : 0;
  int count = bufferFull ? MAX_LOG_LINES : logIndex;
  for (int i = 0; i < count; i++) {
    output += String(logBuffer[(start + i) % MAX_LOG_LINES]) + "\n";
  }
  return output;
}

/* --- Servers & State Management --- */
AsyncWebServer server(8080);
WiFiServer tcpBridge(60001); 
WiFiClient bridgeClient;      
Preferences preferences;

String brainName;
String timezonePosix;
bool debugEnabled = true;

/* --- Helpers --- */
void sendBrainRequest(String path) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + brainName + ":3000" + path;
    http.begin(url);
    int httpCode = http.GET();
    char logMsg[80];
    snprintf(logMsg, sizeof(logMsg), "BRAIN REQ -> %s (Code: %d)", path.c_str(), httpCode);
    addToLog(logMsg);
    http.end();
  }
}

void performPinDiagnostic() {
  addToLog("Diagnostic: Start Pin-Check sequence");
  Serial2.write(0x11); delay(500); 
  Serial2.write(0x21); delay(500); 
  uint8_t dummyIR[] = {'!', 'I', 0x94, 0x70, 0x01, 0x00, 0x05, 'E'};
  Serial2.write(dummyIR, sizeof(dummyIR));
  delay(500); Serial2.write(0x00); 
}

/* --- IR Request Handler --- */
void handleIRRequest(AsyncWebServerRequest *request) {
  uint32_t vals[150];
  int count = 0;
  uint16_t freq = 38000;
  String pulseData = "";

  bool hasS = request->hasParam("s") || request->hasParam("s", true);
  if (hasS) {
    pulseData = request->getParam("s", request->hasParam("s", true))->value();
    pulseData.replace('.', ','); 
    
    bool hasF = request->hasParam("f") || request->hasParam("f", true);
    if (hasF) freq = request->getParam("f", request->hasParam("f", true))->value().toInt();
    
    int lastPos = 0, nextPos = 0;
    while ((nextPos = pulseData.indexOf(',', lastPos)) != -1 && count < 150) {
      vals[count++] = pulseData.substring(lastPos, nextPos).toInt();
      lastPos = nextPos + 1;
    }
    vals[count++] = pulseData.substring(lastPos).toInt();
  } else {
    request->send(400, "text/plain", "Missing 's' parameter");
    return;
  }

  Serial2.write('!'); Serial2.write('I');
  Serial2.write(highByte(freq)); Serial2.write(lowByte(freq));
  Serial2.write((uint8_t)count);
  for (int i = 0; i < count; i++) {
    uint16_t t = (uint16_t)vals[i];
    Serial2.write(highByte(t)); Serial2.write(lowByte(t));
  }
  Serial2.write('E');
  
  if(debugEnabled) {
    char logMsg[64];
    snprintf(logMsg, sizeof(logMsg), "IR: Sent %d pulses @ %dHz", count, freq);
    addToLog(logMsg);
  }
  request->send(200, "text/plain", "OK");
}

/* --- Setup --- */
void setup() {
  Serial.begin(115200); 
  Serial2.begin(E75_BAUD, SERIAL_8N1, RX2_PIN, TX2_PIN);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  preferences.begin("neeo", false);
  brainName = preferences.getString("brain_name", "neeo");
  timezonePosix = preferences.getString("tz_info", "CET-1CEST,M3.5.0,M10.5.0/3");
  debugEnabled = preferences.getBool("debug_logs", true); 
  preferences.end();

  connectWiFi(); 
  
  configTime(0, 0, ntpServer);     
  setenv("TZ", timezonePosix.c_str(), 1);        
  tzset();                         

  if (MDNS.begin((brainName + "-jn5168").c_str())) {
      MDNS.addService("http", "tcp", 8080);
      MDNS.addService("neeo-bridge", "tcp", 60001);
  }

  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request){
    String html = "<html><head><title>NEEO Border Router</title>";
    html += "<style>body{font-family:sans-serif;margin:20px;background:#f0f2f5;}";
    html += ".card{background:white;padding:20px;border-radius:12px;box-shadow:0 4px 6px rgba(0,0,0,0.1);margin-bottom:20px;}";
    html += ".grid{display:grid;grid-template-columns:repeat(auto-fit, minmax(180px, 1fr));gap:10px;}";
    html += "select, input[type=text]{padding:8px;border-radius:4px;border:1px solid #ccc;margin:5px 0;width:100%;max-width:400px;}";
    html += "button, input[type=submit]{padding:12px;cursor:pointer;border-radius:6px;border:none;background:#007bff;color:white;font-weight:bold;}";
    html += "button.red{background:#dc3545;} button.white{background:#e9ecef;color:#333;border:1px solid #ccc;}";
    html += "pre{background:#222;color:#00ff00;padding:15px;border-radius:8px;height:250px;overflow-y:auto;font-size:12px;}</style></head><body>";
    
    html += "<h1>NEEO Dashboard</h1>";
    
    html += "<div class='card'><h3>Blink & Diag</h3><div class='grid'>";
    html += "<button class='red' onclick=\"fetch('/blink?mode=red')\">RED</button>";
    html += "<button class='white' onclick=\"fetch('/blink?mode=white')\">WHITE</button>";
    html += "<button onclick=\"fetch('/diag')\">Run Diag</button>";
    html += "<button onclick=\"fetch('/neighbors')\">Neighbors</button></div></div>";

    html += "<div class='card'><h3>Press Simulation</h3><div class='grid'>";
    html += "<button onclick=\"fetch('/ShortPress')\">ShortPress</button>";
    html += "<button onclick=\"fetch('/LongPress')\">LongPress</button></div></div>";

    html += "<div class='card'><h3>Configuratie</h3><form action='/save' method='POST'>";
    html += "Brain Naam:<br><input type='text' name='brain' value='" + brainName + "'><br><br>";
    html += "Locatie:<br><select id='tzSelect' onchange='document.getElementById(\"tzPosix\").value=this.value'><option>Lijst laden...</option></select><br><br>";
    html += "POSIX String:<br><input type='text' name='tz_posix' id='tzPosix' value='" + timezonePosix + "'><br><br>";
    html += "Debug Logs: <input type='checkbox' name='debug' " + String(debugEnabled ? "checked" : "") + "><br><br>";
    html += "<input type='submit' value='Opslaan & Herstarten'></form></div>";

    html += "<div class='card'><h3>Log</h3><pre id='log'>" + getFullLog() + "</pre>";
    html += "<button onclick=\"fetch('/clearlog').then(()=>location.reload())\">Log Wissen</button></div>";

    html += "<script>";
    html += "async function loadTZ() {";
    html += "  try {";
    html += "    const res = await fetch('https://raw.githubusercontent.com/nayarsystems/posix_tz_db/master/zones.csv');";
    html += "    const text = await res.text();";
    html += "    const lines = text.split('\\n');";
    html += "    const select = document.getElementById('tzSelect');";
    html += "    select.innerHTML = '<option value=\"\">Selecteer een locatie...</option>';";
    html += "    lines.forEach(line => {";
    html += "      const parts = line.split('\",\"');";
    html += "      if(parts.length >= 2) {";
    html += "        const name = parts[0].replace(/\"/g, '');";
    html += "        const posix = parts[1].replace(/\"/g, '');";
    html += "        const opt = document.createElement('option');";
    html += "        opt.value = posix; opt.innerText = name;";
    html += "        if(posix === '" + timezonePosix + "') opt.selected = true;";
    html += "        select.appendChild(opt);";
    html += "      }";
    html += "    });";
    html += "  } catch(e) { console.error('TZ Load error', e); }";
    html += "}";
    html += "loadTZ();";
    html += "setInterval(()=>{fetch('/log').then(r=>r.text()).then(t=>{const e=document.getElementById('log');e.innerText=t;e.scrollTop=e.scrollHeight;});},2000);";
    html += "</script></body></html>";
    
    request->send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
    preferences.begin("neeo", false);
    if (request->hasParam("brain", true)) preferences.putString("brain_name", request->getParam("brain", true)->value());
    if (request->hasParam("tz_posix", true)) preferences.putString("tz_info", request->getParam("tz_posix", true)->value());
    preferences.putBool("debug_logs", request->hasParam("debug", true));
    preferences.end();
    
    addToLog("SYS: Instellingen opgeslagen, herstarten...");
    request->send(200, "text/plain", "OK");
    delay(500); ESP.restart();
  });

  server.on("/blink", HTTP_ANY, [](AsyncWebServerRequest *request){
    String mode = request->hasParam("mode", true) ? request->getParam("mode", true)->value() : "off";
    uint8_t cmd = (mode == "red") ? 0x11 : (mode == "white") ? 0x21 : 0x00;
    Serial2.write(cmd);
    char logMsg[32];
    snprintf(logMsg, sizeof(logMsg), "WEB: Blink command [%s]", mode.c_str());
    addToLog(logMsg);
    request->send(200, "text/plain", "OK");
  });

  server.on("/neighbors", HTTP_ANY, [](AsyncWebServerRequest *request){
    addToLog("WEB: Fetching neighbors...");
    Serial2.write(0x05); 
    String response = ""; unsigned long timeout = millis() + 450;
    while (millis() < timeout) { while (Serial2.available()) response += (char)Serial2.read(); yield(); }
    request->send(200, "text/plain", response);
  });

  auto handleSecurity = [](AsyncWebServerRequest *request) {
    bool isDisc = (request->url() == "/discovery");
    char cmd = isDisc ? 'K' : 'E';
    if (request->hasParam("airkey", true)) {
      String ak = request->getParam("airkey", true)->value();
      Serial2.write('!'); Serial2.write(cmd); Serial2.print(ak); Serial2.write('\n');
      addToLog(("SECURITY: Update " + String(isDisc?"Disc":"Enc") + " key").c_str());
      request->send(200, "text/plain", "OK");
    } else { request->send(400, "text/plain", "Missing key"); }
  };
  server.on("/discovery", HTTP_ANY, handleSecurity);
  server.on("/encryption", HTTP_ANY, handleSecurity);

  server.on("/ir", HTTP_ANY, handleIRRequest);
  server.on("/sendir", HTTP_ANY, handleIRRequest);

  server.on("/diag", HTTP_ANY, [](AsyncWebServerRequest *request){ 
    addToLog("WEB: Triggered Pin Diagnostic");
    performPinDiagnostic(); 
    request->send(200, "text/plain", "Started"); 
  });

  server.on("/ShortPress", HTTP_ANY, [](AsyncWebServerRequest *request){ 
    addToLog("WEB: Simulated ShortPress");
    sendBrainRequest("/v1/api/TouchButton"); 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/LongPress", HTTP_ANY, [](AsyncWebServerRequest *request){ 
    addToLog("WEB: Simulated LongPress");
    sendBrainRequest("/v1/api/longTouchButton"); 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/log", HTTP_ANY, [](AsyncWebServerRequest *request){ request->send(200, "text/plain", getFullLog()); });
  server.on("/clearlog", HTTP_ANY, [](AsyncWebServerRequest *request){ 
    addToLog("SYS: Log cleared");
    bufferFull=false; logIndex=0; request->send(200, "text/plain", "Cleared"); 
  });

  server.begin();
  tcpBridge.begin();
  tcpBridge.setNoDelay(true);
  addToLog("Services Online");
}

void loop() {
  static unsigned long btnDownTime = 0;
  static bool lastBtnState = HIGH;
  bool btnState = digitalRead(BUTTON_PIN);

  if (btnState == LOW && lastBtnState == HIGH) btnDownTime = millis();
  else if (btnState == HIGH && lastBtnState == LOW) {
    if ((millis() - btnDownTime) > 50) {
      addToLog("BTN: Physical press");
      sendBrainRequest("/v1/api/TouchButton");
    }
  }
  lastBtnState = btnState;

  if (tcpBridge.hasClient()) {
    if (bridgeClient) bridgeClient.stop();
    bridgeClient = tcpBridge.accept();
    bridgeClient.setNoDelay(true);
    addToLog("TCP: Client connected");
  }

  if (bridgeClient && bridgeClient.available()) {
    uint8_t buf[256];
    size_t len = bridgeClient.read(buf, sizeof(buf));
    Serial2.write(buf, len);
  }

  if (Serial2.available()) {
    uint8_t buf[256];
    size_t len = Serial2.readBytes(buf, min((int)Serial2.available(), 256));
    if (bridgeClient && bridgeClient.connected()) bridgeClient.write(buf, len);
    else if(debugEnabled) Serial.write(buf, len);
  }
}