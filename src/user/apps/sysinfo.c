/*
 * sysinfo.c — TaterTOS System Information Utility
 */

#include "libc.h"
#include "fry.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(void) {
    struct fry_fb_info fb;
    struct fry_storage_info st;
    struct fry_battery_status bt;

    puts("TaterTOS64v3 System Information\n");

    if (fry_fb_info(&fb) >= 0) {
        printf("Display: %ux%u stride %u format %u\n", fb.width, fb.height, fb.stride, fb.format);
    }

    if (fry_storage_info(&st) >= 0) {
        printf("Storage: %s detected, %lu total sectors\n", st.nvme_detected ? "NVMe" : "RAM", (unsigned long)st.total_sectors);
    }

    if (fry_getbattery(&bt) >= 0) {
        printf("Battery: state %u, remaining %u%%, voltage %umV\n", bt.state, bt.remaining_capacity, bt.present_voltage);
    }

    long procs = fry_proc_count();
    printf("Processes: %ld active\n", procs);

    return 0;
}
