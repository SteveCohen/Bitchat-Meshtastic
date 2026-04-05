#include <Arduino.h>
#include <WiFi.h>
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

// ── WiFi connection ──────────────────────────────────

static bool wifi_connect() {
    Serial.printf("Connecting to WiFi '%s'...\n", WIFI_SSID);
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
        Serial.printf("WiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());
        return true;
    } else {
        Serial.println("WiFi connection failed!");
        return false;
    }
}

// ── Arduino entry points ─────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("=================================");
    Serial.println("  Meshtastic-Bitchat Bridge");
    Serial.println("  " BRIDGE_NAME);
    Serial.println("=================================");

    // Connect WiFi (needed for Meshtastic TCP)
    if (!wifi_connect()) {
        Serial.println("WARNING: No WiFi — Meshtastic side unavailable");
        Serial.println("         Bitchat BLE will still operate");
    } else {
        // Sync time via NTP (needed for bitchat packet timestamps)
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
        Serial.printf("[Status] Mesh:%s  BLE:%s  Bridged:%d msgs\n",
                      meshtastic.is_connected() ? "OK" : "DISCONNECTED",
                      bitchat.is_active() ? "OK" : "INACTIVE",
                      bridge ? bridge->messages_bridged() : 0);
    }

    delay(10); // Yield to FreeRTOS
}
