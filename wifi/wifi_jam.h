/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David */
/* MODE JAM's air: FM-1s of a GROUP play together with no access point, no association, no DHCP and no keys. Each unit tunes to
 * its group's channel and sends ordinary IPv4 UDP (wifi_ip.c: Ableton Link, unchanged) in open ad-hoc (IBSS) data frames:
 * ToDS = FromDS = 0, addr1 the destination, addr2 the sender, addr3 the group's BSSID. Nobody hosts, nobody waits: a unit
 * is in the group from the moment it sends.
 *   the name     fm1-jam for GROUP 1, fm1-jam-2 .. fm1-jam-9 (the user, 2026-11-04); the BSSID 02:66:6D:31:6A:GROUP says the
 *                same in six bytes ("fm1j"; the local bit set, not a group address), so no hash is needed: what identifies a
 *                group in a data frame is addr3, in the MAC's filter (DROP_NOT_MYBSS) and again here
 *   the channel  1, 6, 11 repeated with the GROUP: the three that do not overlap, allowed in every region. A busy channel
 *                needs no setting: another GROUP is another channel
 *   the address  169.254.x.y (RFC 3927), x, y 1..254, from the radio address; anyone else heard using it: drawn again
 *   ARP          each frame received says who sent it (the IP header's source, addr2), so a peer's address is known from
 *                its first multicast and no request is ever sent; a who-has from a computer is answered (wifi_ip.c)
 *   no beacon    a data frame says all a peer needs. A beacon a second (0.7 ms of air) would show the name in a computer's scan;
 *                measured 2026-11-04 it cost 300 B of flash, and the flash is what is short. The frames are IBSS's but the MAC's
 *                IBSS engine (its timed beacons, the TSF sync) is not used; a computer with a fixed BSSID can still join
 * Plain data: no radio here (device/wifi_own.c runs it on the FM-1; tests/wifi/jam_air_test.c runs several in one process). */
#ifndef WIFI_JAM_H
#define WIFI_JAM_H
#include <stdint.h>

#define JA_NAME_MAX 9u                     /* "fm1-jam-9" */
#define JA_PEERS 6u                        /* the units kept (a group larger than that still plays: Link has its own table) */
#define JA_HEARD_MS 3000u                  /* a unit not heard this long is gone (Link's alive every 250 ms, ttl 5 s) */


struct ja_peer {
    uint8_t mac[6];
    int8_t rssi;                           /* the last frame's, dBm */
    uint8_t used;
    uint32_t t_heard;                      /* ms */
    uint32_t n_data;                       /* frames received from it */
};

struct ja_net {
    uint8_t name[JA_NAME_MAX], name_len, ch;
    uint8_t mac[6], bssid[6];
    struct ja_peer peer[JA_PEERS];
};

void ja_init(struct ja_net *n, const uint8_t mac[6], uint32_t group);
uint32_t ja_addr(const uint8_t b[2]);                     /* 169.254.x.y (host order) from two bytes */
/* a frame received: 0 not ours; from a unit of the group it is noted, and 1 when it is for us to go on */
uint32_t ja_heard(struct ja_net *n, const uint8_t *f, uint32_t len, int8_t rssi, uint32_t now_ms);
uint32_t ja_sender_ip(const uint8_t *f, uint32_t len);    /* the IPv4 sender of a data frame, host order (0: none) */
uint32_t ja_count(const struct ja_net *n, uint32_t now_ms);        /* units heard lately */
#endif
