#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "config.h"
#include "utils/time_util.h"
#include "meshtastic/meshtastic_tcp.h"
#include "bitchat/bitchat_ble.h"
#include "bridge/bridge_manager.h"

// ── Global instances ─────────────────────────────────

static MeshtasticTCP meshtastic;
static BitchatBLE bitchat;
// BridgeManager is initialized in setup() after identity is ready
static BridgeManager *bridge = nullptr;
static bool wifi_is_ap = false;

// ── WiFi: Station mode ──────────────────────────────

static bool wifi_connect_sta() {
    Serial.printf("WiFi STA: connecting to '%s'...\n", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 40) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("WiFi STA connected, IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    }

    Serial.println("WiFi STA connection failed");
    WiFi.disconnect(true);
    return false;
}

// ── WiFi: Access Point mode ─────────────────────────

static bool wifi_start_ap() {
    Serial.printf("WiFi AP: starting '%s'...\n", WIFI_AP_SSID);

    WiFi.mode(WIFI_AP);

    // Configure static IP for the AP
    IPAddress ip, gateway, subnet;
    ip.fromString(WIFI_AP_IP);
    gateway.fromString(WIFI_AP_GATEWAY);
    subnet.fromString(WIFI_AP_SUBNET);
    WiFi.softAPConfig(ip, gateway, subnet);

    bool open = (strlen(WIFI_AP_PASSWORD) == 0);
    bool ok;
    if (open) {
        ok = WiFi.softAP(WIFI_AP_SSID, nullptr, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
    } else {
        ok = WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD, WIFI_AP_CHANNEL, 0, WIFI_AP_MAX_CLIENTS);
    }

    if (!ok) {
        Serial.println("WiFi AP start failed");
        return false;
    }

    wifi_is_ap = true;
    Serial.printf("WiFi AP started, SSID: %s, IP: %s%s\n",
                  WIFI_AP_SSID, WiFi.softAPIP().toString().c_str(),
                  open ? " (open)" : "");
    Serial.println("Configure your Meshtastic node to connect to this network.");
    Serial.printf("Then the bridge will find it at %s or via mDNS.\n", MESHTASTIC_HOST);
    return true;
}

// ── WiFi: mode selection ────────────────────────────

static bool wifi_init() {
    const char *mode = WIFI_MODE;

    if (strcmp(mode, "AP") == 0) {
        return wifi_start_ap();
    }

    if (strcmp(mode, "STA") == 0) {
        return wifi_connect_sta();
    }

    // AUTO: try STA first, fall back to AP
    if (wifi_connect_sta()) {
        return true;
    }

    Serial.println("AUTO: STA failed, falling back to AP mode");
    return wifi_start_ap();
}

// ── Arduino entry points ─────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=================================");
    Serial.println("  Meshtastic-Bitchat Bridge");
    Serial.println("  " BRIDGE_NAME);
    Serial.println("=================================");

    // Initialize WiFi (STA, AP, or AUTO)
    if (!wifi_init()) {
        Serial.println("WARNING: No WiFi — Meshtastic side unavailable");
        Serial.println("         Bitchat BLE will still operate");
    } else {
        // Start mDNS for .local hostname resolution
        if (MDNS.begin(BRIDGE_NAME)) {
            Serial.printf("mDNS responder started as %s.local\n", BRIDGE_NAME);
        } else {
            Serial.println("mDNS init failed — .local resolution unavailable");
        }

        // NTP time sync (only meaningful in STA mode with internet access)
        if (!wifi_is_ap) {
            configTime(0, 0, "pool.ntp.org", "time.nist.gov");
            Serial.print("NTP sync");
            for (int i = 0; i < 10 && !time_is_synced(); i++) {
                delay(500);
                Serial.print(".");
            }
            if (time_is_synced()) {
                Serial.printf(" OK (epoch: %llu)\n", bitchat_epoch_ms());
            } else {
                Serial.println(" FAILED (using millis fallback)");
            }
        } else {
            Serial.println("AP mode: NTP unavailable, using millis() for timestamps");
        }
    }

    // Initialize bridge with the bitchat master keypair for virtual identity derivation
    static BridgeManager bridge_instance(meshtastic, bitchat, &bitchat.identity());
    bridge = &bridge_instance;

    // Start the bridge (initializes both interfaces)
    if (!bridge->begin()) {
        Serial.println("Bridge init failed — entering retry loop");
    }
}

void loop() {
    if (bridge) bridge->loop();

    // Status report every 30 seconds
    static unsigned long last_status = 0;
    if (millis() - last_status > 30000) {
        last_status = millis();
        if (wifi_is_ap) {
            Serial.printf("[Status] WiFi:AP(%d clients)  Mesh:%s  BLE:%s  Bridged:%d msgs\n",
                          WiFi.softAPgetStationNum(),
                          meshtastic.is_connected() ? "OK" : "DISCONNECTED",
                          bitchat.is_active() ? "OK" : "INACTIVE",
                          bridge ? bridge->messages_bridged() : 0);
        } else {
            Serial.printf("[Status] Mesh:%s  BLE:%s  Bridged:%d msgs\n",
                          meshtastic.is_connected() ? "OK" : "DISCONNECTED",
                          bitchat.is_active() ? "OK" : "INACTIVE",
                          bridge ? bridge->messages_bridged() : 0);
        }
    }

    delay(10); // Yield to FreeRTOS
}
