#ifndef TATER_PLATFORM_H
#define TATER_PLATFORM_H

#include "bus.h"
#include "device.h"
#include "driver.h"

void platform_init(void);
int platform_device_add(struct fry_device *dev);

#endif
