#ifndef TATER_PCI_H
#define TATER_PCI_H

#include <stdint.h>

struct pci_device_info {
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint32_t bar0;
    uint32_t bar1;
    uint16_t command;
    uint8_t intx_pin;
    uint32_t gsi;
};

uint32_t pci_ecam_read32(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
void pci_ecam_write32(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint32_t val);
uint8_t pci_ecam_read8(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset);
void pci_ecam_write8(uint16_t segment, uint8_t bus, uint8_t slot, uint8_t func, uint16_t offset, uint8_t val);
void pci_enum_all(void);
struct pci_device_info *pci_get_devices(uint32_t *count);
int pci_set_irq(uint8_t bus, uint8_t slot, uint8_t func, uint8_t pin, uint32_t gsi);
/* Inject a device discovered behind VMD (or another virtual bus) into the PCI table.
   bar0/bar1 and command are taken as-is from the device's config space. */
void pci_inject_device(uint8_t bus, uint8_t slot, uint8_t func,
                       uint16_t vendor_id, uint16_t device_id,
                       uint8_t class_code, uint8_t subclass, uint8_t prog_if,
                       uint32_t bar0, uint32_t bar1, uint16_t command);

#endif
