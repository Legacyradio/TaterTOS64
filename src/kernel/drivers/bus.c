// Bus core

#include "bus.h"
#include "device.h"
#include "driver.h"

#define MAX_BUSES 16
#define MAX_DEVS 128
#define MAX_DRVS 128

static struct fry_bus_type *buses[MAX_BUSES];
static struct fry_device *devices[MAX_DEVS];
static struct fry_driver *drivers[MAX_DRVS];
static uint32_t bus_count;
static uint32_t dev_count;
static uint32_t drv_count;

int bus_register(struct fry_bus_type *bus) {
    if (!bus || bus_count >= MAX_BUSES) {
        return -1;
    }
    buses[bus_count++] = bus;
    return 0;
}

int device_register(struct fry_device *dev) {
    if (!dev || dev_count >= MAX_DEVS) {
        return -1;
    }
    devices[dev_count++] = dev;

    // Try match against existing drivers on same bus
    for (uint32_t i = 0; i < drv_count; i++) {
        struct fry_driver *drv = drivers[i];
        if (drv->bus == dev->bus && dev->bus && dev->bus->match) {
            if (dev->bus->match(dev, drv) && drv->probe) {
                if (drv->probe(dev) == 0) {
                    dev->driver = drv;
                    break;
                }
            }
        }
    }
    return 0;
}

int driver_register(struct fry_driver *drv) {
    if (!drv || drv_count >= MAX_DRVS) {
        return -1;
    }
    drivers[drv_count++] = drv;

    // Try match against existing devices on same bus
    for (uint32_t i = 0; i < dev_count; i++) {
        struct fry_device *dev = devices[i];
        if (dev->bus == drv->bus && dev->bus && dev->bus->match) {
            if (dev->bus->match(dev, drv) && drv->probe) {
                if (drv->probe(dev) == 0) {
                    dev->driver = drv;
                }
            }
        }
    }
    return 0;
}
