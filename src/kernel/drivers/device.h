#ifndef TATER_DEVICE_H
#define TATER_DEVICE_H

#include <stdint.h>

struct fry_bus_type;
struct fry_driver;

struct fry_device {
    const char *name;
    struct fry_bus_type *bus;
    struct fry_driver *driver;
    void *driver_data;
};

void device_set_driver_data(struct fry_device *dev, void *data);
void *device_get_driver_data(struct fry_device *dev);

#endif
