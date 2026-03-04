#ifndef TATER_BUS_H
#define TATER_BUS_H

#include <stdint.h>

struct fry_device;
struct fry_driver;

struct fry_bus_type {
    const char *name;
    int (*match)(struct fry_device *dev, struct fry_driver *drv);
};

int bus_register(struct fry_bus_type *bus);
int device_register(struct fry_device *dev);
int driver_register(struct fry_driver *drv);

#endif
