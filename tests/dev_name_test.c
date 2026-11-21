/* SPDX-License-Identifier: MPL-2.0 */
/* plat_uid.h: the four hex digits of this FM-1 (plat_dev_id: the hash of the flash's unique ID, the low two octets of the
 * Bluetooth address bt_own.c makes) and the name "FM1-XXXX" built from them (plat_dev_name). The expected values are
 * computed apart from the code (FNV-1a over the 16 octets, h ^= h >> 16, the low 16 bits). */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../include/plat_uid.h"

int main(void)
{
    static const uint8_t SEQ[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    static const uint8_t HOST[16] = {0x46, 0x4D, 0x31, 0x2D, 0x68, 0x6F, 0x73, 0x74};
    static const uint8_t FF[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    static const uint8_t ZERO[16] = {0};
    char n[9];
    assert(plat_uid_hash16(SEQ) == 0x2FBB);
    assert(plat_uid_hash16(HOST) == 0x3B9C);
    assert(plat_uid_hash16(FF) == -1 && plat_uid_hash16(ZERO) == -1 && plat_uid_hash16(0) == -1);
    plat_dev_name(n, 0x1A2B);
    assert(!strcmp(n, "FM1-1A2B"));
    plat_dev_name(n, 0);
    assert(!strcmp(n, "FM1-0000"));
    plat_dev_name(n, 0xFFFF);
    assert(!strcmp(n, "FM1-FFFF"));
    plat_dev_name(n, -1);
    assert(!strcmp(n, "FM1-0000"));
    plat_dev_name(n, plat_uid_hash16(SEQ));          /* upper case, four digits, the id's high digit first (as the BLE name) */
    assert(n[8] == 0 && !strncmp(n, "FM1-", 4) && strspn(n + 4, "0123456789ABCDEF") == 4u);
    return 0;
}
