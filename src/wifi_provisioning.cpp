#include "wifi_provisioning.h"
#include <WiFiManager.h>

void connectWiFi() {
    WiFiManager wm;
    // Configureer hier je WM instellingen indien nodig
    if(!wm.autoConnect("NEEO-Border-Router")) {
        ESP.restart();
    }
}