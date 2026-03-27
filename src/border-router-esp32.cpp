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

/* --- SLIP RX Buffer --- */
#define MAX_SLIP_FRAME 512
uint8_t slipRxBuf[MAX_SLIP_FRAME];
int slipRxPtr = 0;
bool isEscaping = false;



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

void sendSlipPacket(const uint8_t* data, size_t len) {
    Serial2.write(0xC0); // Start-of-Frame
    for (size_t i = 0; i < len; i++) {
        if (data[i] == 0xC0) {      // this is data but acts as the end-of-record indicator for a slip record, escape it first with DB, then DC
            Serial2.write(0xDB);
            Serial2.write(0xDC);
        } else if (data[i] == 0xDB) { // this is the normal escape character for a slip record but now data, make sure it is handled correctly
            Serial2.write(0xDB);
            Serial2.write(0xDD);
        } else {
            Serial2.write(data[i]);
        }
    }
    Serial2.write(0xC0); // End-of-Frame
}


void processAction(const uint8_t* data, size_t len, const char* source, const char* service) {
    //Serial2.write(data, len);
    sendSlipPacket(data, len); //Write slip-format
    if (debugEnabled) {
        char hexData[100] = {0};
        for(size_t i = 0; i < len && i < 20; i++) {
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
    if (len < 2) return; 

    const char* type = "unknown";
    char msg[MAX_LINE_LEN];
    memset(msg, 0, MAX_LINE_LEN);

    // 1. Label herkenning op basis van de logs
    if (data[0] == 0x21) { // De '!' prefix
        switch(data[1]) {
            case 0x4D: // 'M' - Management/Blink
                type = "!m (status)";
                // In jouw log: 21 4d 01 (Aan) of 21 4d 00 (Uit/Ready)
                snprintf(msg, sizeof(msg), "JN Mode: %s", (data[2] == 0x01) ? "ACTIVE/BLINK" : "READY/IDLE");
                break;

            case 0x52: // 'R' - Neighbor Ack
                type = "!r (ack)";
                // Jouw log: 21 52 [index] 00 01 [LQI]
                if (len >= 5) {
                    snprintf(msg, sizeof(msg), "NBR Ack Index %02X | LQI: %02X", data[2], data[5]);
                }
                break;

            case 0x49: // 'I' - Infrarood Echo
                type = "!i (ir-echo)";
                snprintf(msg, sizeof(msg), "IR Data Block Received (%d bytes)", (int)len);
                break;
        }
    } 
    else if (data[0] == 0x49) { // 'I' (Inbound van Node)
        type = "incoming";
        // Jouw log: 49 d8 [SEQ] ... [NODE ID]
        snprintf(msg, sizeof(msg), "Node Data | Seq: %02X | Node: %02X:%02X:%02X", data[2], data[8], data[9], data[10]);
    }
    else if (data[0] == 0x69 && data[1] == 0xDC) { // 'id' (Neighbor Dump)
        type = "nbr-dump";
        snprintf(msg, sizeof(msg), "Full Neighbor Table Dump (%d bytes)", (int)len);
    }

    // 2. XML/ASCII Scanner voor TR2 Provisioning (indien aanwezig in data)
    String readable = "";
    for (size_t i = 0; i < len; i++) {
        if (data[i] >= 32 && data[i] <= 126) readable += (char)data[i];
    }
    
    // Als er leesbare XML data in zit, voeg deze toe aan het bericht
    if (readable.indexOf("gui_xml") != -1) {
        readable.replace("&#x2F;", "/");
        snprintf(msg, sizeof(msg), "TR2 REQ: %s", readable.substring(0, 100).c_str());
    }

    // 3. Fallback naar Hex als msg nog leeg is (voor onbekende records)
    if (strlen(msg) == 0) {
        char hexBuf[50] = {0};
        for(size_t j=0; j<len && j<15; j++) {
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
    uint16_t vals[150]; // Use uint16_t for 2-byte pulses
    int count = 0;
    uint16_t freq = 38000; // Default 38kHz
    bool isPost = request->hasParam("s", true);
    if (request->hasParam("s") || isPost) {
        String pulseData = request->getParam("s", isPost)->value();
        pulseData.replace('.', ','); 
        
        if (request->hasParam("f", isPost)) {
            freq = (uint16_t)request->getParam("f", isPost)->value().toInt();
        }

        // Parse the comma-separated string
        int lastPos = 0, nextPos = 0;
        while ((nextPos = pulseData.indexOf(',', lastPos)) != -1 && count < 150) {
            vals[count++] = (uint16_t)pulseData.substring(lastPos, nextPos).toInt();
            lastPos = nextPos + 1;
        }
        vals[count++] = (uint16_t)pulseData.substring(lastPos).toInt();

        // IR-data conform what we saw in the UART-logs: !I (21 49) + Header + Payload
        // Header: 12 bytes. Payload: count * 2 bytes. Footer: 1 byte ('E' of extra data)
        size_t pSize = 12 + (count * 2); 
        uint8_t* irBuf = (uint8_t*)malloc(pSize);
        
        if(irBuf) {
            memset(irBuf, 0, pSize);
            irBuf[0] = 0x21; irBuf[1] = 0x49; // !I
            irBuf[4] = (uint8_t)(freq >> 8);   // 0x94 (voor 38000)
            irBuf[5] = (uint8_t)(freq & 0xFF); // 0x70
            irBuf[9] = 0x04;                   // Inferred from log (type/flags)
            irBuf[11] = 0x01;                  // Inferred from log (repeats?)

            int pIdx = 12;
            for (int i = 0; i < count; i++) {
                irBuf[pIdx++] = (uint8_t)(vals[i] >> 8);   // MSB
                irBuf[pIdx++] = (uint8_t)(vals[i] & 0xFF); // LSB
            }

            processAction(irBuf, pIdx, "web", "/ir_send");
            free(irBuf);
            request->send(200, "text/plain", "OK");
        }
    } else {
        request->send(400, "text/plain", "Missing pulses 's'");
    }
}

void HandleBlinkCommand(AsyncWebServerRequest *request, const char* source,const char* pC) {
    // command based on UART-log: 21 4d [01 or 00]
    uint8_t modeVal = 0x00; 
    if (request->hasParam("mode")) {
        String m = request->getParam("mode")->value();
        if (m == "on" || m == "1") modeVal = 0x01; // Conform data:: 21 4d 01
    }    
    uint8_t blinkPkg[3] = {0x21, 0x4d, modeVal};
    processAction(blinkPkg, 3, source, pC);
    request->send(200, "text/plain", "Blink sent");
}

void HandleNBRCommand(AsyncWebServerRequest *request, const char* source,const char* pC) {
    // Request 6lowpan IPV6-table within the JN516x module
    uint8_t nbrPkg[3] = {0x21, 0x53, 0x00}; 
    processAction(nbrPkg, 3, source, pC);
    request->send(200, "text/plain", "NBR Query index 00 sent");
}

void HandleDiscoCommand(AsyncWebServerRequest *request, const char* source,const char* pC) {
    // Conform log: 21 5a a0
    uint8_t discPkg[] = {0x21, 0x5a, 0x0a0}; 
    processAction(discPkg, 3, source, pC);
    request->send(200, "text/plain", "Discovery (PJOIN) mode activated");
}

void HandleEncryptionCommand(AsyncWebServerRequest *request, const char* source,const char* pC) {
    uint8_t keyPkg[18];
    keyPkg[0] = 0x21; // '!'
    keyPkg[1] = 0x4B; // 'K' (Key)

    // Als de key via een parameter 'k' komt (hex string), anders de default uit je log:
    if (request->hasParam("k")) {
        String hexKey = request->getParam("k")->value();
        // Logica om hex-string naar bytes te converteren...
    } else {
        // Default NEEO test-key uit jouw log
        uint8_t defaultKey[] = {0x77, 0x59, 0x7a, 0x6c, 0x91, 0x7c, 0x49, 0x78, 0x97, 0x7b, 0x4b, 0x38, 0x95, 0xb8, 0xfb, 0x80};
        memcpy(&keyPkg[2], defaultKey, 16);
    }

    processAction(keyPkg, 18, source, pC);
    request->send(200, "text/plain", "Airkey pushed to JN5168");

}

void handleUnifiedCommand(AsyncWebServerRequest *request, const char* source) {
    String path = request->url();
    const char* pC = path.c_str();
    if (path.indexOf("/blink") != -1) 
        HandleBlinkCommand(request, source,  pC);
    else if (path.indexOf("/neighbors") != -1) 
        HandleNBRCommand(request, source,  pC);
    else if (path.indexOf("/discovery") != -1) 
        HandleDiscoCommand(request, source,  pC);
    else if (path.indexOf("/encryption") != -1) 
        HandleEncryptionCommand(request, source,  pC);
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
    bridgeServer.on("/sendir", HTTP_ANY, handleIRRequest);
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

        if (c == 0xC0) { // End of Frame
            if (slipRxPtr > 0) {
                processIncomingJennic(slipRxBuf, slipRxPtr);
                slipRxPtr = 0; // Reset voor volgend pakket
            }
        } 
        else if (c == 0xDB) { // Escape start
            isEscaping = true;
        } 
        else {
            if (isEscaping) {
                if (c == 0xDC) c = 0xC0;
                else if (c == 0xDD) c = 0xDB;
                isEscaping = false;
            }
            if (slipRxPtr < MAX_SLIP_FRAME) {
                slipRxBuf[slipRxPtr++] = c;
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

    // Listen for response from Brain
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

    // Here we execute commands that we need to execute, but need to be done outside handlers (in normal loop, to prevent watchdog crashes)
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