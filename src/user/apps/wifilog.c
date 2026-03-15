#include "../libc/libc.h"

int main(void) {
    static char buf[FRY_WIFI_DEBUG_MAX];
    long n = fry_wifi_debug(buf, sizeof(buf) - 1);
    
    if (n < 0) {
        printf("Error retrieving WiFi log: %ld\n", n);
        return 1;
    }
    
    if (n == 0) {
        printf("WiFi log is empty.\n");
        return 0;
    }

    buf[n] = 0;
    printf("%s", buf);
    return 0;
}
