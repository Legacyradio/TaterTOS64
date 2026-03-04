#ifndef TATER_DRIVER_H
#define TATER_DRIVER_H

#include <stdint.h>

struct fry_bus_type;
struct fry_device;

struct fry_driver {
    const char *name;
    struct fry_bus_type *bus;
    int (*probe)(struct fry_device *dev);
    void (*remove)(struct fry_device *dev);
};

const char *driver_name(const struct fry_driver *drv);

#endif
