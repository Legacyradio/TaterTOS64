#ifndef TATER_PLATFORM_H
#define TATER_PLATFORM_H

#include <stdint.h>
#include "bus.h"
#include "device.h"
#include "driver.h"

typedef enum platform_id {
    PLATFORM_ID_UNKNOWN = 0,
    PLATFORM_ID_DELL_PRECISION_7530 = 1,
} platform_id_t;

typedef struct platform_profile {
    platform_id_t id;
    uint16_t lpc_vendor;
    uint16_t lpc_device;
    uint8_t acpi_oem_dell;
    uint8_t confidence; /* 0..100, based on available runtime signals */
} platform_profile_t;

void platform_init(void);
void platform_detect(void);
int platform_device_add(struct fry_device *dev);
const platform_profile_t *platform_get_profile(void);
const char *platform_id_name(platform_id_t id);

#endif
