/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David */
/* MODE JAM's air (wifi_jam.h). */
#include "wifi_jam.h"

#ifndef __PI32V2__
#include <string.h>                      /* (the firmware's: felucca-bsp's libc.c, in the unity) */
#endif

void ja_init(struct ja_net *n, const uint8_t mac[6], uint32_t group)
{
    memset(n, 0, sizeof *n);
    memcpy(n->mac, mac, 6);
    memcpy(n->name, "fm1-jam-", 8);
    n->name[8] = (uint8_t)('0' + group);
    n->name_len = group < 2u ? 7u : 9u;
    n->ch = (uint8_t)(1u + 5u * ((group + 2u) % 3u));     /* 1 -> 1, 2 -> 6, 3 -> 11, 4 -> 1 .. */
    memcpy(n->bssid, "\x02" "fm1j", 5);
    n->bssid[5] = (uint8_t)group;
}
uint32_t ja_addr(const uint8_t b[2])
{
    return 0xA9FE0000u | (1u + (uint32_t)b[0] % 254u) << 8 | (1u + (uint32_t)b[1] % 254u);
}

uint32_t ja_heard(struct ja_net *n, const uint8_t *f, uint32_t len, int8_t rssi, uint32_t now_ms)
{
    struct ja_peer *p = 0, *q;
    if (len < 24u || (f[0] & 0x0Cu) != 0x08u || (f[1] & 0x43u) || (f[10] & 1u) || memcmp(f + 16, n->bssid, 6) || !memcmp(f + 10, n->mac, 6))
        return 0;                             /* (not data, an access point's, protected, another network's, our own) */
    for (q = n->peer; q < n->peer + JA_PEERS; q++) {
        if (q->used && !memcmp(q->mac, f + 10, 6)) {
            p = q;
            break;
        }
        if (!p && (!q->used || now_ms - q->t_heard >= JA_HEARD_MS))
            p = q;                            /* (a free place, or one gone) */
    }
    if (p) {                                  /* (full: it still plays, it is only not counted) */
        if (memcmp(p->mac, f + 10, 6) || !p->used) {
            memset(p, 0, sizeof *p);
            memcpy(p->mac, f + 10, 6);
            p->used = 1;
        }
        p->rssi = rssi;
        p->t_heard = now_ms;
        p->n_data++;
    }
    return (f[4] & 1u) || !memcmp(f + 4, n->mac, 6);
}

uint32_t ja_sender_ip(const uint8_t *f, uint32_t len)
{
    const uint8_t *e = f + 24u + 8u;         /* (our frames carry no QoS field; a computer's QoS data: not read as an address) */
    if (f[0] != 0x08u || len < 24u + 8u + 20u || e[-2] != 0x08u || e[-1] != 0x00u)
        return 0;
    return (uint32_t)e[12] << 24 | (uint32_t)e[13] << 16 | (uint32_t)e[14] << 8 | e[15];
}

uint32_t ja_count(const struct ja_net *n, uint32_t now_ms)
{
    uint32_t i, c = 0;
    for (i = 0; i < JA_PEERS; i++)
        c += n->peer[i].used && now_ms - n->peer[i].t_heard < JA_HEARD_MS;
    return c;
}
