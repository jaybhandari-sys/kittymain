/* Stubs for SDK-specific functions not available outside the camera firmware */
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* mbedTLS custom hardware RNG callback used by the SDK's libmbedcrypto */
int OnGetHardwareRandom(void *data, unsigned char *output, size_t len) {
    (void)data;
    /* Use /dev/urandom-style seeded PRNG as fallback */
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    for (size_t i = 0; i < len; i++) {
        output[i] = (unsigned char)(rand() & 0xFF);
    }
    return 0;
}
