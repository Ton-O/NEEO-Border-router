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
const long  gmtOffset_sec = 3600;    
const int   daylightOffset_sec = 3600; 

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
bool debugEnabled = true;

/* --- Helpers --- */
String getParamValue(AsyncWebServerRequest *request, const char* paramName) {
  if (request->hasParam(paramName)) return request->getParam(paramName)->value();
  if (request->hasParam(paramName, true)) return request->getParam(paramName, true)->value();
  return "";
}

void sendBrainRequest(String path) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    String url = "http://" + brainName + ":3000" + path;
    http.begin(url);
    int httpCode = http.GET();
    char logMsg[80];
    snprintf(logMsg, sizeof(logMsg), "BRAIN -> %s (Code: %d)", path.c_str(), httpCode);
    addToLog(logMsg);
    http.end();
  }
}

void performPinDiagnostic() {
  addToLog("DIAG: Start Pin-Check sequence");
  Serial2.write(0x11); delay(500); 
  Serial2.write(0x21); delay(500); 
  uint8_t dummyIR[] = {'!', 'I', 0x94, 0x70, 0x01, 0x00, 0x05, 'E'};
  Serial2.write(dummyIR, sizeof(dummyIR));
  delay(500); Serial2.write(0x00); 
  addToLog("DIAG: Sequence finished");
}

/* --- Handlers --- */
void handleIRRequest(AsyncWebServerRequest *request) {
  String pulseData = getParamValue(request, "s");
  if (pulseData == "") {
    request->send(400, "text/plain", "Missing 's' parameter");
    return;
  }
  
  uint16_t freq = 38000;
  String fVal = getParamValue(request, "f");
  if (fVal != "") freq = fVal.toInt();

  pulseData.replace('.', ','); 
  uint32_t vals[150];
  int count = 0;
  int lastPos = 0, nextPos = 0;
  while ((nextPos = pulseData.indexOf(',', lastPos)) != -1 && count < 150) {
    vals[count++] = pulseData.substring(lastPos, nextPos).toInt();
    lastPos = nextPos + 1;
  }
  vals[count++] = pulseData.substring(lastPos).toInt();

  Serial2.write('!'); Serial2.write('I');
  Serial2.write(highByte(freq)); Serial2.write(lowByte(freq));
  Serial2.write((uint8_t)count);
  for (int i = 0; i < count; i++) {
    uint16_t t = (uint16_t)vals[i];
    Serial2.write(highByte(t)); Serial2.write(lowByte(t));
  }
  Serial2.write('E');
  
  if(debugEnabled) addToLog("IR: Sent signal to JN5168");
  request->send(200, "text/plain", "OK");
}

/* --- Setup --- */
void setup() {
  Serial.begin(115200); 
  Serial2.begin(E75_BAUD, SERIAL_8N1, RX2_PIN, TX2_PIN);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  preferences.begin("neeo", false);
  brainName = preferences.getString("brain_name", "neeo");
  debugEnabled = preferences.getBool("debug_logs", true); 
  preferences.end();

  connectWiFi(); 
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  if (MDNS.begin((brainName + "-jn5168").c_str())) {
      MDNS.addService("http", "tcp", 8080);
      MDNS.addService("neeo-bridge", "tcp", 60001);
  }

  // --- Routes ---

  server.on("/", HTTP_ANY, [](AsyncWebServerRequest *request){
    String html = "<html><head><title>NEEO Border Router</title><style>body{font-family:sans-serif;margin:20px;background:#f0f2f5;}.card{background:white;padding:20px;border-radius:12px;box-shadow:0 4px 6px rgba(0,0,0,0.1);margin-bottom:20px;}.grid{display:grid;grid-template-columns:repeat(auto-fit, minmax(180px, 1fr));gap:10px;}button{padding:12px;cursor:pointer;border-radius:6px;border:none;background:#007bff;color:white;font-weight:bold;}button.red{background:#dc3545;} button.white{background:#e9ecef;color:#333;border:1px solid #ccc;}pre{background:#222;color:#00ff00;padding:15px;border-radius:8px;height:250px;overflow-y:auto;font-size:12px;}</style></head><body>";
    html += "<h1>NEEO Router Dashboard</h1>";
    html += "<div class='card'><h3>Blink & Diag</h3><div class='grid'><button class='red' onclick=\"fetch('/blink?mode=red')\">RED</button><button class='white' onclick=\"fetch('/blink?mode=white')\">WHITE</button><button onclick=\"fetch('/diag')\">Run Diag</button><button onclick=\"fetch('/neighbors')\">Scan Neighbors</button></div></div>";
    html += "<div class='card'><h3>Config</h3><form action='/save' method='POST'>Brain: <input type='text' name='brain' value='" + brainName + "'> Debug: <input type='checkbox' name='debug' " + String(debugEnabled ? "checked" : "") + "> <input type='submit' value='Save'></form></div>";
    html += "<div class='card'><h3>Log</h3><pre id='log'>" + getFullLog() + "</pre><button onclick=\"fetch('/clearlog').then(()=>location.reload())\">Clear Log</button></div>";
    html += "<script>setInterval(()=>{fetch('/log').then(r=>r.text()).then(t=>{const e=document.getElementById('log');e.innerText=t;e.scrollTop=e.scrollHeight;});},2000);</script></body></html>";
    request->send(200, "text/html", html);
  });

  server.on("/save", HTTP_ANY, [](AsyncWebServerRequest *request){
    String b = getParamValue(request, "brain");
    preferences.begin("neeo", false);
    if (b != "") preferences.putString("brain_name", b);
    preferences.putBool("debug_logs", getParamValue(request, "debug") != "");
    preferences.end();
    addToLog("SYS: Settings updated. Rebooting...");
    request->send(200, "text/plain", "OK");
    delay(500); ESP.restart();
  });

  server.on("/blink", HTTP_ANY, [](AsyncWebServerRequest *request){
    String mode = getParamValue(request, "mode");
    uint8_t cmd = (mode == "red") ? 0x11 : (mode == "white") ? 0x21 : 0x00;
    Serial2.write(cmd);
    addToLog((String("LED: Set to ") + (mode == "" ? "OFF" : mode)).c_str());
    request->send(200, "text/plain", "OK");
  });

  server.on("/neighbors", HTTP_ANY, [](AsyncWebServerRequest *request){
    addToLog("ZB: Requesting neighbors...");
    Serial2.write(0x05); 
    String response = ""; unsigned long timeout = millis() + 450;
    while (millis() < timeout) { while (Serial2.available()) response += (char)Serial2.read(); yield(); }
    request->send(200, "text/plain", response);
  });

  auto handleSecurity = [](AsyncWebServerRequest *request) {
    String type = (request->url() == "/discovery") ? "K" : "E";
    String ak = getParamValue(request, "airkey");
    if (ak != "") {
      Serial2.write('!'); Serial2.write(type[0]); Serial2.print(ak); Serial2.write('\n');
      addToLog((String("SEC: Sent ") + (type=="K"?"Discovery":"Encryption") + " key").c_str());
      request->send(200, "text/plain", "OK");
    } else {
      request->send(400, "text/plain", "Missing airkey");
    }
  };
  server.on("/discovery", HTTP_ANY, handleSecurity);
  server.on("/encryption", HTTP_ANY, handleSecurity);

  server.on("/ir", HTTP_ANY, handleIRRequest);
  server.on("/sendir", HTTP_ANY, handleIRRequest);

  server.on("/diag", HTTP_ANY, [](AsyncWebServerRequest *request){ 
    performPinDiagnostic(); request->send(200, "text/plain", "Started"); 
  });

  server.on("/ShortPress", HTTP_ANY, [](AsyncWebServerRequest *request){ 
    addToLog("BTN: Web Simulation ShortPress");
    sendBrainRequest("/v1/api/TouchButton"); 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/LongPress", HTTP_ANY, [](AsyncWebServerRequest *request){ 
    addToLog("BTN: Web Simulation LongPress");
    sendBrainRequest("/v1/api/longTouchButton"); 
    request->send(200, "text/plain", "OK"); 
  });

  server.on("/log", HTTP_ANY, [](AsyncWebServerRequest *request){ request->send(200, "text/plain", getFullLog()); });
  server.on("/clearlog", HTTP_ANY, [](AsyncWebServerRequest *request){ bufferFull=false; logIndex=0; request->send(200, "text/plain", "Cleared"); });

  server.begin();
  tcpBridge.begin();
  tcpBridge.setNoDelay(true);
  addToLog("SYS: All services ready (GET/POST enabled)");
}

/* --- Loop --- */
void loop() {
  static unsigned long btnDownTime = 0;
  static bool lastBtnState = HIGH;
  bool btnState = digitalRead(BUTTON_PIN);

  if (btnState == LOW && lastBtnState == HIGH) btnDownTime = millis();
  else if (btnState == HIGH && lastBtnState == LOW) {
    if ((millis() - btnDownTime) > 50) {
        addToLog("BTN: Physical Button Press detected");
        sendBrainRequest("/v1/api/TouchButton");
    }
  }
  lastBtnState = btnState;

  if (tcpBridge.hasClient()) {
    if (bridgeClient) bridgeClient.stop();
    bridgeClient = tcpBridge.accept();
    bridgeClient.setNoDelay(true);
  }

  if (bridgeClient && bridgeClient.available()) {
    uint8_t buf[256];
    size_t len = bridgeClient.read(buf, sizeof(buf));
    Serial2.write(buf, len);
  }

  if (Serial2.available()) {
    uint8_t buf[256];
    size_t avail = (size_t)Serial2.available();
    size_t toRead = min(avail, (size_t)sizeof(buf));
    size_t len = Serial2.readBytes(buf, toRead);
    if (bridgeClient && bridgeClient.connected()) bridgeClient.write(buf, len);
    else if(debugEnabled) Serial.write(buf, len);
  }
}