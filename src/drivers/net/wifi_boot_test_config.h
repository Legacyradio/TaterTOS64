#ifndef TATER_WIFI_BOOT_TEST_CONFIG_H
#define TATER_WIFI_BOOT_TEST_CONFIG_H

/*
 * Boot-time WiFi smoke test configuration.
 *
 * Leave disabled for normal boots. To exercise the full path on hardware:
 *   1. Set TATER_WIFI_BOOT_TEST_ENABLE to 1
 *   2. Fill in SSID and passphrase for a 2.4 GHz WPA2-PSK network
 *   3. Optionally override the hostname/port used for DNS/TCP checks
 */

#ifndef TATER_WIFI_BOOT_TEST_ENABLE
#define TATER_WIFI_BOOT_TEST_ENABLE 0
#endif

#ifndef TATER_WIFI_BOOT_TEST_SSID
#define TATER_WIFI_BOOT_TEST_SSID ""
#endif

#ifndef TATER_WIFI_BOOT_TEST_PASSPHRASE
#define TATER_WIFI_BOOT_TEST_PASSPHRASE ""
#endif

#ifndef TATER_WIFI_BOOT_TEST_HOSTNAME
#define TATER_WIFI_BOOT_TEST_HOSTNAME ""
#endif

#ifndef TATER_WIFI_BOOT_TEST_TCP_PORT
#define TATER_WIFI_BOOT_TEST_TCP_PORT 80
#endif

#endif
