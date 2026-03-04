// Driver core

#include "driver.h"

const char *driver_name(const struct fry_driver *drv) {
    return drv ? drv->name : 0;
}
