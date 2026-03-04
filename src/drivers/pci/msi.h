#ifndef TATER_MSI_H
#define TATER_MSI_H

#include <stdint.h>

int msi_alloc_vector(void);
void msi_free_vector(int vector);

#endif
