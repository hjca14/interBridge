#pragma once

// Placeholders only, used by compile-only CI. For local credentials, use
// scripts/generate_dev_ble_mqtt_secrets_header.ps1; never hand-write
// multiline raw strings in macros and never commit the generated ignored
// header.
//
// Deliberately has NO Wi-Fi SSID/password fields: esp32-c3-dev-ble-mqtt
// gets Wi-Fi exclusively from the official BLE Unified Provisioning
// session and whatever the ESP-IDF Wi-Fi driver then persists in NVS -
// see docs/dev-ble-mqtt.md. Do not add either macro here; the composed
// entry point (src/dev/ble_mqtt_main.cpp) must never reference them.
#define INTERBRIDGE_DEV_AWS_ENDPOINT "REPLACE_WITH_AWS_IOT_ENDPOINT"
#define INTERBRIDGE_DEV_DEVICE_ID "REPLACE_WITH_UNIQUE_DEV_DEVICE_ID"
#define INTERBRIDGE_DEV_ROOT_CA_PEM "REPLACE_WITH_AMAZON_ROOT_CA_PEM"
#define INTERBRIDGE_DEV_CERTIFICATE_PEM "REPLACE_WITH_UNIQUE_DEV_CERTIFICATE_PEM"
#define INTERBRIDGE_DEV_PRIVATE_KEY_PEM "REPLACE_WITH_UNIQUE_DEV_PRIVATE_KEY_PEM"
#define INTERBRIDGE_DEV_BLE_POP "REPLACE_WITH_LOCAL_DEV_POP"
