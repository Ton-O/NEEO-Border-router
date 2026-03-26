#include <Arduino.h>
#include "wifi_provisioning.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <WiFiManager.h> 
#include "esp_bt.h"
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <IPAddress.h>
#include "time.h"
#include <WiFiUdp.h>
#include "index.h"


/* --- Forward Declarations --- */
void addToLog(const char* source, const char* service, const char* msg);
void processAction(const uint8_t* data, size_t len, const char* source, const char* service);
void processIncomingJennic(const uint8_t* data, size_t len);
String getFullLog();
void sendBrainRequest(String path);
void performPinDiagnostic();

/* --- Hardware Configuration --- */
#define RX2_PIN      16
#define TX2_PIN      17
#define BUTTON_PIN   4       
#define E75_BAUD     500000  

const char* ntpServer = "pool.ntp.org";

/* --- Static Ring Buffer --- */
const int MAX_LOG_LINES = 50;
const int MAX_LINE_LEN = 150; 
char logBuffer[MAX_LOG_LINES][MAX_LINE_LEN];
int logIndex = 0;
bool bufferFull = false;

/* --- State Management --- */
AsyncWebServer server(80);           
AsyncWebServer bridgeServer(8080);   
WiFiUDP udp;

Preferences preferences;

String brainName;
String timezonePosix;
String timezoneName; // This was missing
bool debugEnabled = true;
bool triggerShortPress = false;
bool triggerLongPress = false;

const int udpPort = 3201;
IPAddress targetIP(0,0,0,0);
unsigned long lastDiscovery = 0;


/* --- Core Functions --- */

void addToLog(const char* source, const char* service, const char* msg) {
    struct tm timeinfo;
    char tStr[25]; 
    if(!getLocalTime(&timeinfo)) {
        strcpy(tStr, "[00-00-00 00:00:00]");
    } else {
        strftime(tStr, sizeof(tStr), "[%y-%m-%d %H:%M:%S]", &timeinfo);
    }
    // Alignment: source at 6 chars, service at 11 chars
    snprintf(logBuffer[logIndex], MAX_LINE_LEN, "%s [%-6s] [%-11s] %s", tStr, source, service, msg);
    Serial.println(logBuffer[logIndex]); 
    logIndex = (logIndex + 1) % MAX_LOG_LINES;
    if (logIndex == 0) bufferFull = true;
}

String getFullLog() {
    String output = "";
    output.reserve(udpPort);
    int start = bufferFull ? logIndex : 0;
    int count = bufferFull ? MAX_LOG_LINES : logIndex;
    for (int i = 0; i < count; i++) {
        output += String(logBuffer[(start + i) % MAX_LOG_LINES]) + "\n";
    }
    return output;
}

void processAction(const uint8_t* data, size_t len, const char* source, const char* service) {
    Serial2.write(data, len);
    if (debugEnabled) {
        char hexData[100] = {0};
        for(size_t i = 0; i < len && i < 25; i++) {
            char buf[5];
            snprintf(buf, sizeof(buf), "%02X ", data[i]);
            strcat(hexData, buf);
        }
        char logEntry[MAX_LINE_LEN];
        snprintf(logEntry, sizeof(logEntry), "via %s (%d bytes) HEX: %s", source, (int)len, hexData);
        addToLog("jn5168", service, logEntry);
    }
}

void processIncomingJennic(const uint8_t* data, size_t len) {
    if (len < 1) return;
    const char* type = "unknown";
    bool isCoAP = false;

    // 1. Type recognition
    if (data[0] == '!') {
        if (data[1] == 'S') type = "!s (tx)";
        else if (data[1] == 'R') type = "!r (ack)";
        else if (data[1] == 'M') type = "!m (mgmt)";
    } 
    else if (data[0] == 0x49) type = "incoming";

    // 2. CoAP/UDP Detection (Often after 6LoWPAN header, look for 0x40/0x50/0x60/0x70)
    // CoAP version 1 always starts with 01 in the first two bits (0x40)
    for (size_t i = 0; i < len - 4; i++) {
        if (data[i] == 0x40 || data[i] == 0x50 || data[i] == 0x60) {
            isCoAP = true;
            break;
        }
    }

    // 3. ASCII/XML Scanner & Security Filter
    String readableData = "";
    for (size_t i = 0; i < len; i++) {
        if (data[i] >= 32 && data[i] <= 126) {
            readableData += (char)data[i];
        } else if (readableData.length() > 0 && !readableData.endsWith(" ")) {
            readableData += " ";
        }
    }
    
    // Translate XML entities
    readableData.replace("&#x2F;", "/");
    
    // SECURITY FILTER: Mask the WiFi password in the log
    if (readableData.indexOf("<p>") != -1) {
        int start = readableData.indexOf("<p>") + 3;
        int end = readableData.indexOf("</p>");
        if (end > start) {
            String pass = readableData.substring(start, end);
            readableData.replace(pass, "********");
        }
    }

    // 4. Build log entry
    char msg[MAX_LINE_LEN];
    if (isCoAP) {
        snprintf(msg, sizeof(msg), "COAP PUSH: %s", readableData.length() > 5 ? readableData.c_str() : "[Binary CoAP]");
    } else if (readableData.length() > 15) {
        snprintf(msg, sizeof(msg), "DATA: %s", readableData.substring(0, 110).c_str());
    } else {
        // Hex display for small packets/handshakes
        char hexBuf[40] = {0};
        for(size_t j=0; j<10 && j<len; j++) {
            char b[4]; snprintf(b, 4, "%02X ", data[j]); strcat(hexBuf, b);
        }
        snprintf(msg, sizeof(msg), "HEX: %s", hexBuf);
    }
    
    addToLog("jn5168", type, msg);
}

void sendBrainRequest(String path) {
    if (WiFi.status() == WL_CONNECTED && targetIP.toString() != "0.0.0.0") {
        HTTPClient http;
        String url = "http://" + targetIP.toString() + ":3000" + path;
        http.begin(url);
        int httpCode = http.GET();
        
        char logMsg[100];
        snprintf(logMsg, sizeof(logMsg), "REQ -> %s (Code: %d)", path.c_str(), httpCode);
        addToLog("http", "brain", logMsg);
        http.end();
        
        // If Brain doesn't respond (e.g. IP changed), reset targetIP to search again
        if (httpCode < 0) targetIP = IPAddress(0,0,0,0);
    }
}

void performPinDiagnostic() {
    addToLog("diag", "sequence", "Starting Pin-Check sequence");
    uint8_t c1 = 0x11; processAction(&c1, 1, "diag", "/led_red"); delay(500); 
    uint8_t c2 = 0x21; processAction(&c2, 1, "diag", "/led_white"); delay(500); 
    uint8_t dummyIR[] = {'!', 'I', 0x94, 0x70, 0x01, 0x00, 0x05, 'E'};
    processAction(dummyIR, sizeof(dummyIR), "diag", "/ir_test"); delay(500); 
    uint8_t c0 = 0x00; processAction(&c0, 1, "diag", "/led_off");
}

void handleIRRequest(AsyncWebServerRequest *request) {
    uint32_t vals[150];
    int count = 0;
    uint16_t freq = 38000;
    bool isPost = request->hasParam("s", true);
    if (request->hasParam("s") || isPost) {
        String pulseData = request->getParam("s", isPost)->value();
        pulseData.replace('.', ','); 
        if (request->hasParam("f", isPost)) freq = (uint16_t)request->getParam("f", isPost)->value().toInt();
        int lastPos = 0, nextPos = 0;
        while ((nextPos = pulseData.indexOf(',', lastPos)) != -1 && count < 150) {
            vals[count++] = (uint32_t)pulseData.substring(lastPos, nextPos).toInt();
            lastPos = nextPos + 1;
        }
        vals[count++] = (uint32_t)pulseData.substring(lastPos).toInt();
        size_t pSize = 5 + (count * 2) + 1;
        uint8_t* irBuf = (uint8_t*)malloc(pSize);
        if(irBuf) {
            int pIdx = 0;
            irBuf[pIdx++] = '!'; irBuf[pIdx++] = 'I';
            irBuf[pIdx++] = (uint8_t)(freq >> 8); irBuf[pIdx++] = (uint8_t)(freq & 0xFF);
            irBuf[pIdx++] = (uint8_t)count;
            for (int i = 0; i < count; i++) {
                irBuf[pIdx++] = (uint8_t)(vals[i] >> 8); irBuf[pIdx++] = (uint8_t)(vals[i] & 0xFF);
            }
            irBuf[pIdx++] = 'E';
            processAction(irBuf, pIdx, "web", "/ir_send");
            free(irBuf);
            request->send(200);
        }
    } else { request->send(400); }
}

void handleUnifiedCommand(AsyncWebServerRequest *request, const char* source) {
    String path = request->url();
    const char* pC = path.c_str();
    if (path.indexOf("/blink") != -1) {
        String mode = request->hasParam("mode") ? request->getParam("mode")->value() : "off";
        uint8_t cmd = (mode == "red") ? 0x11 : (mode == "white") ? 0x21 : 0x00;
        processAction(&cmd, 1, source, pC);
        request->send(200);
    } 
    else if (path.indexOf("/neighbors") != -1 || path.indexOf("/discovery") != -1) {
        uint8_t cmd = 0x05;
        processAction(&cmd, 1, source, pC);
        request->send(200);
    }
    else if (path.indexOf("/encryption") != -1) {
        uint8_t enc[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
        processAction(enc, sizeof(enc), source, pC);
        request->send(200);
    }
    else if (path.indexOf("/diag") != -1) { 
        performPinDiagnostic(); 
        request->send(200); 
    }
    else if (path.indexOf("/stats") != -1) {
        Serial.println("Stats need to be returned");
        request->send(200, "application/json", "{\"status\":\"ok\",\"value\":66}"); 
    }
    else if (path.indexOf("/ShortPress") != -1) {
        triggerShortPress = true; // Set flag only
        request->send(200, "text/plain", "Triggered");
    }
    else if (path.indexOf("/LongPress") != -1) { 
        triggerLongPress = true; // Set flag only
        request->send(200, "text/plain", "Triggered");
    }
}

void setup() {
    btStop();
    esp_bt_controller_disable();
    Serial.begin(115200); 
    Serial2.begin(E75_BAUD, SERIAL_8N1, RX2_PIN, TX2_PIN);
    
    preferences.begin("neeo", false);
    brainName = preferences.getString("brain_name", "neeo");
    timezonePosix = preferences.getString("tz_info", "CET-1CEST,M3.5.0,M10.5.0/3");
    debugEnabled = preferences.getBool("debug_logs", true); 
    preferences.end();
    
    connectWiFi(); 
    configTime(0, 0, ntpServer);     
    setenv("TZ", timezonePosix.c_str(), 1); 
    tzset();
    
    udp.begin(udpPort);
    if (MDNS.begin((brainName + "-jn5168").c_str())) {
        MDNS.addService("http", "tcp", 80);
    }

    auto sharedHandler = [](AsyncWebServerRequest *request) {
        if (request->client()->localPort() == 8080 && targetIP == IPAddress(0,0,0,0)) {
            targetIP = request->client()->remoteIP();
            
            String msg = "First Bridge Call! TargetIP set to: " + targetIP.toString();
            addToLog("sys", "bridge_init", msg.c_str());
            Serial.println("[BRIDGE] " + msg);
        }
        const char* source = (request->client()->localPort() == 8080) ? "p_8080" : "p_80";
        handleUnifiedCommand(request, source);
    };

server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = String(FPSTR(INDEX_HTML));

    html.replace("[[BRAIN]]", brainName);
    html.replace("[[POSIX]]", timezonePosix);
    html.replace("[[TZNAME]]", timezoneName);
    html.replace("[[DEBUG_CHECKED]]", debugEnabled ? "checked" : "");
    html.replace("[[LOG_CONTENT]]", getFullLog());
    html.replace("[[WIFI_SSID]]", WiFi.SSID());

    request->send(200, "text/html", html);
});
    server.on("/blink", HTTP_ANY, sharedHandler);
    bridgeServer.on("/blink", HTTP_ANY, sharedHandler);
    server.on("/neighbors", HTTP_ANY, sharedHandler);
    bridgeServer.on("/neighbors", HTTP_ANY, sharedHandler);
    bridgeServer.on("/stats", HTTP_ANY, sharedHandler);
    server.on("/discovery", HTTP_ANY, sharedHandler);
    bridgeServer.on("/discovery", HTTP_ANY, sharedHandler);
    server.on("/encryption", HTTP_ANY, sharedHandler);
    bridgeServer.on("/encryption", HTTP_ANY, sharedHandler);
    server.on("/diag", HTTP_ANY, sharedHandler);
    server.on("/ShortPress", HTTP_ANY, sharedHandler);
    server.on("/LongPress", HTTP_ANY, sharedHandler);
    server.on("/v1/api/ir", HTTP_POST, handleIRRequest);

    // Log endpoint for automatic refresh (AJAX)
    server.on("/log", HTTP_GET, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", getFullLog());
    });

    // Save settings
    server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request){
        if(request->hasParam("brain", true)) brainName = request->getParam("brain", true)->value();
        if(request->hasParam("tz_posix", true)) timezonePosix = request->getParam("tz_posix", true)->value();
        if(request->hasParam("tz_name", true)) timezoneName = request->getParam("tz_name", true)->value();
        debugEnabled = request->hasParam("debug", true);

        preferences.begin("neeo", false);
        preferences.putString("brain_name", brainName);
        preferences.putString("tz_info", timezonePosix);
        preferences.putString("tz_name", timezoneName);
        preferences.putBool("debug_logs", debugEnabled);
        preferences.end();

        request->send(200, "text/html", "Settings Saved. Restarting... <meta http-equiv='refresh' content='2;url=/'>");
        delay(1000);
        ESP.restart();
    });

    // Clear log
    server.on("/clearlog", HTTP_ANY, [](AsyncWebServerRequest *request){
        logIndex = 0;
        bufferFull = false;
        for(int i=0; i<MAX_LOG_LINES; i++) memset(logBuffer[i], 0, MAX_LINE_LEN);
        request->send(200);
    });

    // WiFi Reset
    server.on("/reset_wifi", HTTP_POST, [](AsyncWebServerRequest *request){
        request->send(200, "text/plain", "Resetting WiFi and Restarting...");
        delay(1000);
        WiFiManager wm;
        wm.resetSettings();
        ESP.restart();
    });

    server.onNotFound([](AsyncWebServerRequest *request) {
        addToLog("GUI", "404", request->url().c_str());
        request->send(404, "text/plain", "Not found");
    });

    bridgeServer.onNotFound([](AsyncWebServerRequest *request) {
        addToLog("8080", "404", request->url().c_str());
        request->send(404, "text/plain", "Not found");
    });    

    server.begin();
    bridgeServer.begin();
    addToLog("sys", "boot", "Dual Port Server Ready");
}

void loop() {
    static uint8_t sBuf[512];
    static size_t sIdx = 0;
    static bool escaped = false;

    // --- 1. SERIAL PROCESSING (SLIP Protocol) ---
    while (Serial2.available()) {
        uint8_t c = Serial2.read();
        if (c == 0xC0) { // SLIP Boundary (End of packet)
            if (sIdx > 0) {
                processIncomingJennic(sBuf, sIdx);
            }
            sIdx = 0;
            escaped = false;
        } else if (c == 0xDB) { // SLIP Escape character
            escaped = true;
        } else {
            if (escaped) {
                if (c == 0xDC) c = 0xC0; 
                else if (c == 0xDD) c = 0xDB;
                escaped = false;
            }
            if (sIdx < sizeof(sBuf) - 1) {
                sBuf[sIdx++] = c;
            }
        }
    }

    // --- 2. UDP DISCOVERY (Searching for the Brain) ---
    if (WiFi.status() == WL_CONNECTED && targetIP.toString() == "0.0.0.0") {
        if (millis() - lastDiscovery > 5000) { 
            lastDiscovery = millis();
            String Up_BrainName = brainName;
            Up_BrainName.toUpperCase();
            String LookFor = "WHO_IS_NEEO->" + Up_BrainName;
            addToLog("sys", "Get_BrainIP", LookFor.c_str() );
            udp.beginPacket("255.255.255.255", udpPort);
            udp.print("WHO_IS_NEEO->" + Up_BrainName);
            udp.endPacket();
        }
    }

    // Listen for response from Python script
    if (WiFi.status() == WL_CONNECTED) {
        int packetSize = udp.parsePacket();
        if (packetSize) {
            char reply[32];
            int len = udp.read(reply, sizeof(reply) - 1);
            if (len > 0) {
                reply[len] = 0;
                if (String(reply).indexOf("I_AM_NEEO") != -1) {
                    targetIP = udp.remoteIP();
                    String msg = "Brain found at: " + targetIP.toString();
                    addToLog("sys", "Find_Brain", msg.c_str());
                }
            }
        }
    }

    // --- 3. HTTP ACTIONS (Flag system to prevent Watchdog crashes) ---
    if (triggerShortPress) {
        triggerShortPress = false;
        if (targetIP.toString() != "0.0.0.0") {
            sendBrainRequest("/v1/api/TouchButton");
        } else {
            addToLog("http", "error", "No Brain IP; action cancelled");
        }
    }

    if (triggerLongPress) {
        triggerLongPress = false;
        if (targetIP.toString() != "0.0.0.0") {
            sendBrainRequest("/v1/api/longTouchButton");
        } else {
            addToLog("http", "error", "No Brain IP; action cancelled");
        }
    }
}
