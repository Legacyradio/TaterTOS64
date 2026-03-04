// Platform bus

#include "platform.h"

void kprint(const char *fmt, ...);

static int platform_match(struct fry_device *dev, struct fry_driver *drv) {
    if (!dev || !drv || !dev->name || !drv->name) {
        return 0;
    }
    const char *a = dev->name;
    const char *b = drv->name;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++; b++;
    }
    return *a == 0 && *b == 0;
}

static struct fry_bus_type platform_bus = {
    .name = "platform",
    .match = platform_match,
};

void platform_init(void) {
    bus_register(&platform_bus);
}

int platform_device_add(struct fry_device *dev) {
    if (!dev) return -1;
    dev->bus = &platform_bus;
    return device_register(dev);
}
