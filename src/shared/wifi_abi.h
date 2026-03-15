#ifndef TATER_WIFI_ABI_H
#define TATER_WIFI_ABI_H

#include <stdint.h>

#define FRY_WIFI_SSID_MAX 32u
#define FRY_WIFI_MAX_SCAN 32u
#define FRY_WIFI_DEBUG_MAX 131072u  /* 128K — dynamic kernel log can grow */

struct fry_wifi_scan_entry {
    uint8_t bssid[6];
    char    ssid[FRY_WIFI_SSID_MAX + 1];
    uint8_t channel;
    int8_t  rssi;
    uint8_t secure;
    uint8_t connected;
};

/* wifi_9260_init() progress steps */
enum {
    WIFI_STEP_NONE       = 0,
    WIFI_STEP_PCI_SCAN   = 1,
    WIFI_STEP_NIC_RESET  = 2,
    WIFI_STEP_FW_LOAD    = 3,
    WIFI_STEP_TLV_PARSE  = 4,
    WIFI_STEP_DMA_ALLOC  = 5,
    WIFI_STEP_TFH_INIT   = 6,
    WIFI_STEP_MSI        = 7,
    WIFI_STEP_FW_UPLOAD  = 8,
    WIFI_STEP_ALIVE      = 9,
    WIFI_STEP_HCMD_INIT  = 10,
    WIFI_STEP_MAC_INIT   = 11,
    WIFI_STEP_SCAN       = 12,
    WIFI_STEP_DONE       = 13
};

struct fry_wifi_status {
    uint8_t ready;
    uint8_t have_mac;
    uint8_t connected;
    uint8_t secure;
    uint8_t configured;
    uint8_t channel;
    int8_t  rssi;
    uint8_t init_step;     /* last completed or failed init step (WIFI_STEP_*) */
    int16_t init_rc;       /* return code from failed step (0 if ok) */
    uint8_t mac[6];
    uint8_t bssid[6];
    uint32_t ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns_server;
    char ssid[FRY_WIFI_SSID_MAX + 1];
};

#endif
