#pragma once

#ifdef HUB_MOSAICO
#define LAMABUDDY_DEVICE_MODEL "lamabuddy-esp32s31-mosaico"
#define LAMABUDDY_FIRMWARE_VERSION "0.3.0-mosaico-dev.1"
#define LAMABUDDY_MDNS_HOSTNAME "lamabuddy-mosaico"
#define LAMABUDDY_MDNS_INSTANCE "LamaBuddy ESP-Mosaico"
#elif defined(HUB_AMOLED_216)
#define LAMABUDDY_DEVICE_MODEL "lamabuddy-esp32s3-amoled216"
#define LAMABUDDY_FIRMWARE_VERSION "0.3.0-amoled216-dev.1"
#define LAMABUDDY_MDNS_HOSTNAME "lamabuddy-amoled216"
#define LAMABUDDY_MDNS_INSTANCE "LamaBuddy AMOLED 2.16"
#elif defined(HUB_TOUCHSCREEN)
#define LAMABUDDY_DEVICE_MODEL "lamabuddy-esp32s3-touch"
#define LAMABUDDY_FIRMWARE_VERSION "0.2.0-integrated.1"
#define LAMABUDDY_MDNS_HOSTNAME "lamabuddy-touch"
#define LAMABUDDY_MDNS_INSTANCE "LamaBuddy Touch"
#elif defined(HUB_GRINDER)
#define LAMABUDDY_DEVICE_MODEL "lamabuddy-esp32s3-integrated"
#define LAMABUDDY_FIRMWARE_VERSION "0.2.0-integrated.1"
#define LAMABUDDY_MDNS_HOSTNAME "lamabuddy-integrated"
#define LAMABUDDY_MDNS_INSTANCE "LamaBuddy Integrated"
#else
#define LAMABUDDY_DEVICE_MODEL "lamabuddy-esp32s3-hub"
#define LAMABUDDY_FIRMWARE_VERSION "0.1.0-dev.1"
#define LAMABUDDY_MDNS_HOSTNAME "lamabuddy-hub"
#define LAMABUDDY_MDNS_INSTANCE "LamaBuddy Hub"
#endif
#ifdef HUB_MOSAICO
#define LAMABUDDY_CHIP_NAME "esp32s31"
#define LAMABUDDY_TRANSPORT_SOURCE "esp32-s31"
#else
#define LAMABUDDY_CHIP_NAME "esp32s3"
#define LAMABUDDY_TRANSPORT_SOURCE "esp32-s3"
#endif
#define LAMABUDDY_FIRMWARE_BUILD_ID_ALGORITHM "elf-sha256-8"
#define LAMABUDDY_HTTP_PORT 8787
#define LAMABUDDY_HTTPS_PORT 8788
#define LAMABUDDY_LEGACY_MDNS_SERVICE_TYPE "_http"
#define LAMABUDDY_V2_MDNS_SERVICE_TYPE "_lamabuddy"
