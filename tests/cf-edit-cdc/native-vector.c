/* Verify Agent's deterministic cut vector against the native Rabin primitive.
 * This small check does not claim to run the complete server/SQL lifecycle. */
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include "rabin-checksum.h"
int main(void) {
    const size_t total = 32U << 20, min = 6U << 20, max = 10U << 20;
    const size_t expected[] = {9332497, 10485760, 7275587, 6298386, 162202};
    unsigned char *data = malloc(total);
    if (!data) return 2;
    uint64_t state = UINT64_C(0x0123456789abcdef);
    for (size_t i = 0; i < total; i += 8) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        for (int j = 0; j < 8; j++) data[i+j] = (state >> (8*j)) & 255;
    }
    rabin_init(48);
    size_t offset = 0, index = 0;
    while (offset < total) {
        size_t len = total - offset, cut = len < max ? len : max;
        if (cut >= min) {
            unsigned int fp = 0;
            for (size_t i = min-1; i < cut; i++) {
                fp = i == min-1 ? rabin_checksum((char *)data+offset+i+1-48,48)
                    : rabin_rolling_checksum(fp,48,data[offset+i-48],data[offset+i]);
                if ((fp & ((8U<<20)-1)) == 0x13 || i+1 == max) {cut=i+1;break;}
            }
        }
        if (index >= 5 || cut != expected[index++]) {free(data);return 1;}
        offset += cut;
    }
    free(data);
    if (index != 5) return 1;
    puts("native CDC cut vector: PASS");
    return 0;
}
