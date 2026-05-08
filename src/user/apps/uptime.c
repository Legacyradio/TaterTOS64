/*
 * uptime.c — TaterTOS Uptime Utility
 */

#include "libc.h"
#include "fry.h"

#include <stdio.h>
#include <time.h>

int main(void) {
    long ms = fry_gettime();
    long seconds = ms / 1000;
    long minutes = seconds / 60;
    long hours = minutes / 60;
    long days = hours / 24;

    printf("uptime: %ld days, %ld hours, %ld minutes, %ld seconds\n",
           days, hours % 24, minutes % 60, seconds % 60);

    return 0;
}
