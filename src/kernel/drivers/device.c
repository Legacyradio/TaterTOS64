// Device core

#include "device.h"

void device_set_driver_data(struct fry_device *dev, void *data) {
    if (dev) {
        dev->driver_data = data;
    }
}

void *device_get_driver_data(struct fry_device *dev) {
    return dev ? dev->driver_data : 0;
}
