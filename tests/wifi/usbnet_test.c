/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 w0ts */
/* The USB network's frames (wifi/wifi_usbnet.c) with the FM-1's IP layer (wifi/wifi_ip.c, MODE JANK's ad-hoc form):
 *   the computer (the radio's MAC) finds the FM-1 by ARP on the cable and is answered from mac_l; pings it; its Link
 *   multicast reaches the IP layer's port and the air; its broadcast the IP layer only; a unicast to a unit of the
 *   group the air only; a frame from an address it was not given is dropped
 *   the IP layer's group frames go to the air and the cable (from mac_l there), its unicast to the computer the cable
 *   only, to a unit the air only
 *   from the air: the group's multicast and ARP reach the cable, a unit's unicast to the computer's address too, one
 *   to the FM-1's address (IPv4, ARP) not; another unit's, our own heard back, protected, ToDS, too large: not; a QoS
 *   data frame converts as a plain one
 *   random frames into each function: nothing read or written outside its buffers (with ASan) */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../wifi/wifi_ip.c"
#include "../../wifi/wifi_usbnet.c"

static const uint8_t MAC[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};     /* the radio's: the computer's on the cable */
static const uint8_t BSSID[6] = {0x02, 0x66, 0x6D, 0x31, 0x6A, 0x01};
static const uint8_t PEER[6] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};    /* a unit of the group */
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t LINKG[6] = {0x01, 0x00, 0x5E, 0x4C, 0x4E, 0x4B};
#define LOCAL_IP 0xA9FE0A0Bu                                              /* 169.254.10.11: the FM-1 */
#define COMP_IP 0xA9FE1415u                                               /* 169.254.20.21: the computer */
#define PEER_IP 0xA9FE1E1Fu                                               /* 169.254.30.31 */
#define LINK_IP 0xE04C4E4Bu                                               /* 224.76.78.75 */

static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v >> 16); wr16(p + 2, v & 0xFFFFu); }
static void test_rand(uint8_t *o, uint8_t n) { while (n--) o[n] = (uint8_t)rand(); }

static uint32_t eth_hdr(uint8_t *e, const uint8_t *dst, const uint8_t *src, uint32_t type)
{
    memcpy(e, dst, 6);
    memcpy(e + 6, src, 6);
    wr16(e + 12, type);
    return 14;
}
static uint32_t arp(uint8_t *e, const uint8_t *dst, const uint8_t *src, uint32_t op, const uint8_t *sha, uint32_t sip,
                    const uint8_t *tha, uint32_t tip)
{
    uint8_t *a = e + eth_hdr(e, dst, src, 0x0806);
    wr16(a, 1);
    wr16(a + 2, 0x0800);
    a[4] = 6, a[5] = 4;
    wr16(a + 6, op);
    memcpy(a + 8, sha, 6);
    wr32(a + 14, sip);
    memcpy(a + 18, tha, 6);
    wr32(a + 24, tip);
    return 14 + 28;
}
static uint32_t ipv4(uint8_t *e, const uint8_t *dst, const uint8_t *src, uint32_t proto, uint32_t sip, uint32_t dip,
                     const uint8_t *pl, uint32_t n)
{
    uint8_t *p = e + eth_hdr(e, dst, src, 0x0800);
    memset(p, 0, 20);
    p[0] = 0x45;
    wr16(p + 2, 20 + n);
    p[8] = 64;
    p[9] = (uint8_t)proto;
    wr32(p + 12, sip);
    wr32(p + 16, dip);
    wr16(p + 10, ip_chksum(p, 20));
    memcpy(p + 20, pl, n);
    return 14 + 20 + n;
}
static uint32_t udp(uint8_t *e, const uint8_t *dst, const uint8_t *src, uint32_t sip, uint32_t dip, uint32_t dport,
                    const uint8_t *pl, uint32_t n)
{
    uint8_t u[600];
    wr16(u, 50000);
    wr16(u + 2, dport);
    wr16(u + 4, 8 + n);
    wr16(u + 6, 0);                                     /* (no checksum: allowed in IPv4) */
    memcpy(u + 8, pl, n);
    return ipv4(e, dst, src, 17, sip, dip, u, 8 + n);
}
/* an air frame from an Ethernet one, as a unit sends it (or with flags/QoS for the odd ones) */
static uint32_t air_of(uint8_t *a, const uint8_t *e, uint32_t n, int qos, uint8_t fc1)
{
    uint32_t h = qos ? 26u : 24u;
    memset(a, 0, h);
    a[0] = qos ? 0x88 : 0x08;
    a[1] = fc1;
    memcpy(a + 4, e, 6);
    memcpy(a + 10, e + 6, 6);
    memcpy(a + 16, BSSID, 6);
    memcpy(a + h, SNAP, 6);
    memcpy(a + h + 6, e + 12, 2);
    memcpy(a + h + 8, e + 14, n - 14);
    return h + 8 + n - 14;
}

static uint32_t udp_seen, udp_seen_port, udp_seen_src;
static void on_udp(void *ctx, int64_t now, uint32_t sip, uint16_t sport, uint32_t dip, uint16_t dport, const uint8_t *p,
                   uint32_t n)
{
    (void)ctx, (void)now, (void)sport, (void)dip, (void)p, (void)n;
    udp_seen++;
    udp_seen_port = dport;
    udp_seen_src = sip;
}

static struct wifi_ip wip;
static struct un_hub hub;
static uint8_t eth[1600], f[1600], air[700], tx[700], out[1600];

/* a computer's frame into the IP layer: its reply (if any) out through un_from_local; the reply's routing, its Ethernet
 * frame in out (its length in *out_len) */
static uint32_t computer_to_local(uint32_t n, uint32_t *out_len)
{
    uint32_t fl = un_usb_to_local(&hub, eth, n, f, sizeof f), tl = 0;
    *out_len = 0;
    if (!fl)
        return 0;
    if (!wifi_ip_early_filter(&wip, f, fl))
        return 0;
    if (wifi_ip_rx_frame(&wip, f, fl, 0, tx, &tl) != 1 || !tl)
        return 0;
    return un_from_local(&hub, tx, tl, out, sizeof out, out_len);
}

static void test_computer(void)
{
    uint32_t n, w, ol;
    uint16_t seq = 7;
    uint8_t icmp[16] = {8, 0, 0, 0, 0x12, 0x34, 0, 1, 'p', 'i', 'n', 'g', '!', '!', '!', '!'};
    static const uint8_t z6[6];
    /* ARP for the FM-1: the air gets it too (a unit could be asked), the IP layer answers on the cable from mac_l */
    n = arp(eth, BCAST, MAC, 1, MAC, COMP_IP, z6, LOCAL_IP);
    assert(un_usb_to_air(&hub, eth, n, &seq, air, sizeof air) == 24 + 8 + 28 && seq == 8);
    assert(!memcmp(air + 4, BCAST, 6) && !memcmp(air + 10, MAC, 6) && !memcmp(air + 16, BSSID, 6) && air[22] == (7 << 4));
    w = computer_to_local(n, &ol);
    assert(w == UN_USB && ol == 42);                    /* the reply: the cable only */
    assert(!memcmp(out, MAC, 6) && !memcmp(out + 6, hub.mac_l, 6));
    assert(out[14 + 7] == 2 && !memcmp(out + 14 + 8, hub.mac_l, 6) && !memcmp(out + 14 + 18, MAC, 6));
    /* a ping to the FM-1 (to mac_l): not the air's; answered on the cable */
    n = ipv4(eth, hub.mac_l, MAC, 1, COMP_IP, LOCAL_IP, icmp, sizeof icmp);
    assert(!un_usb_to_air(&hub, eth, n, &seq, air, sizeof air) && seq == 8);
    w = computer_to_local(n, &ol);
    assert(w == UN_USB && !memcmp(out, MAC, 6) && !memcmp(out + 6, hub.mac_l, 6) && out[14 + 20] == 0);   /* echo reply */
    /* Link's multicast: the air and the IP layer's port */
    udp_seen = 0;
    n = udp(eth, LINKG, MAC, COMP_IP, LINK_IP, 20808, (const uint8_t *)"_asdp_v\x01", 8);
    assert(un_usb_to_air(&hub, eth, n, &seq, air, sizeof air) && !memcmp(air + 4, LINKG, 6));
    computer_to_local(n, &ol);
    assert(udp_seen == 1 && udp_seen_port == 20808 && udp_seen_src == COMP_IP);
    /* a broadcast: the IP layer's to look at, not the air's (1 Mb/s: only what Link needs goes out) */
    w = hub.st.drop_filter;
    n = udp(eth, BCAST, MAC, COMP_IP, 0xFFFFFFFFu, 9999, (const uint8_t *)"x", 1);
    assert(!un_usb_to_air(&hub, eth, n, &seq, air, sizeof air) && hub.st.drop_filter == w + 1);
    assert(un_usb_to_local(&hub, eth, n, f, sizeof f));
    /* another multicast (mDNS): neither the air's nor dropped from the IP layer's view */
    n = udp(eth, (const uint8_t[]){0x01, 0x00, 0x5E, 0x00, 0x00, 0xFB}, MAC, COMP_IP, 0xE00000FBu, 5353, (const uint8_t *)"m", 1);
    assert(!un_usb_to_air(&hub, eth, n, &seq, air, sizeof air));
    /* a unicast to a unit of the group: the air only */
    n = udp(eth, PEER, MAC, COMP_IP, PEER_IP, 20808, (const uint8_t *)"ping", 4);
    assert(un_usb_to_air(&hub, eth, n, &seq, air, sizeof air) && !memcmp(air + 4, PEER, 6) && !memcmp(air + 10, MAC, 6));
    assert(!un_usb_to_local(&hub, eth, n, f, sizeof f));
    /* IPv6 (the computer's own chatter): neither */
    eth_hdr(eth, (const uint8_t[]){0x33, 0x33, 0, 0, 0, 1}, MAC, 0x86DD);
    memset(eth + 14, 0, 40);
    assert(!un_usb_to_air(&hub, eth, 54, &seq, air, sizeof air) && !un_usb_to_local(&hub, eth, 54, f, sizeof f));
    /* from an address it was not given: dropped */
    w = hub.st.drop_foreign;
    n = udp(eth, LINKG, PEER, COMP_IP, LINK_IP, 20808, (const uint8_t *)"x", 1);
    assert(!un_usb_to_air(&hub, eth, n, &seq, air, sizeof air) && hub.st.drop_foreign == w + 1);
    assert(!un_usb_to_local(&hub, eth, n, f, sizeof f));
    /* too large for the air's buffer */
    w = hub.st.drop_big;
    memset(f, 0x33, 1400);
    n = udp(eth, PEER, MAC, COMP_IP, PEER_IP, 20808, f, 580);
    assert(!un_usb_to_air(&hub, eth, n, &seq, air, 576) && hub.st.drop_big == w + 1);   /* (576: wifi_own.c's frame) */
    /* the source address helper */
    n = udp(eth, PEER, MAC, COMP_IP, PEER_IP, 1, (const uint8_t *)"x", 1);
    assert(un_eth_src_ip(eth, n) == COMP_IP);
    n = arp(eth, BCAST, MAC, 1, MAC, COMP_IP, z6, PEER_IP);
    assert(un_eth_src_ip(eth, n) == COMP_IP && un_eth_src_ip(eth, 13) == 0);
    printf("usbnet: the computer's frames (ARP, ping, Link, broadcast, a unit's, IPv6, foreign, too large): OK\n");
}

static void test_local(void)
{
    uint32_t n, w, ol;
    /* Link's multicast from the IP layer: the air and the cable, from mac_l there */
    n = wifi_ip_send_udp(&wip, 20808, LINK_IP, 20808, (const uint8_t *)"_asdp_v\x01", 8, tx);
    assert(n);
    w = un_from_local(&hub, tx, n, out, sizeof out, &ol);
    assert(w == (UN_AIR | UN_USB) && ol == n - 32 + 14 && !memcmp(out, LINKG, 6) && !memcmp(out + 6, hub.mac_l, 6));
    /* to the computer (learned by its ARP, test_computer): the cable only */
    n = wifi_ip_send_udp(&wip, 20808, COMP_IP, 20808, (const uint8_t *)"pong", 4, tx);
    assert(n && un_from_local(&hub, tx, n, out, sizeof out, &ol) == UN_USB && !memcmp(out, MAC, 6));
    /* to a unit: the air only */
    wifi_ip_arp_learn(&wip, PEER_IP, PEER);
    n = wifi_ip_send_udp(&wip, 20808, PEER_IP, 20808, (const uint8_t *)"pong", 4, tx);
    assert(n && un_from_local(&hub, tx, n, out, sizeof out, &ol) == UN_AIR && ol == 0);
    /* a group frame that does not fit the cable's buffer: the air still */
    n = wifi_ip_send_udp(&wip, 20808, LINK_IP, 20808, (const uint8_t *)"_asdp_v\x01", 8, tx);
    assert(un_from_local(&hub, tx, n, out, 20, &ol) == UN_AIR && ol == 0);
    printf("usbnet: the IP layer's frames (multicast both ways, the computer's the cable only, a unit's the air only): OK\n");
}

static void test_air(void)
{
    uint32_t n, a;
    static const uint8_t z6[6];
    /* a unit's Link multicast: to the cable, as it came */
    n = udp(eth, LINKG, PEER, PEER_IP, LINK_IP, 20808, (const uint8_t *)"_asdp_v\x01", 8);
    a = air_of(air, eth, n, 0, 0);
    assert(un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out) == n && !memcmp(out, eth, n));
    /* the same in a QoS data frame */
    a = air_of(air, eth, n, 1, 0);
    assert(un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out) == n && !memcmp(out, eth, n));
    /* to the computer's address (sent to the radio's MAC): the cable's */
    n = udp(eth, MAC, PEER, PEER_IP, COMP_IP, 20808, (const uint8_t *)"hi", 2);
    a = air_of(air, eth, n, 0, 0);
    assert(un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out) == n);
    /* to the FM-1's: not */
    n = udp(eth, MAC, PEER, PEER_IP, LOCAL_IP, 20808, (const uint8_t *)"hi", 2);
    a = air_of(air, eth, n, 0, 0);
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out));
    /* ARP: a unit asking for the computer (broadcast), answering it, answering the FM-1 */
    n = arp(eth, BCAST, PEER, 1, PEER, PEER_IP, z6, COMP_IP);
    a = air_of(air, eth, n, 0, 0);
    assert(un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out) == n);
    n = arp(eth, MAC, PEER, 2, PEER, PEER_IP, MAC, COMP_IP);
    a = air_of(air, eth, n, 0, 0);
    assert(un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out) == n);
    n = arp(eth, MAC, PEER, 2, PEER, PEER_IP, MAC, LOCAL_IP);
    a = air_of(air, eth, n, 0, 0);
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out));
    /* another unit's, our own heard back, protected, ToDS, another group, too large: not */
    n = udp(eth, (const uint8_t[]){0x02, 1, 2, 3, 4, 5}, PEER, PEER_IP, 0xA9FE0101u, 20808, (const uint8_t *)"x", 1);
    a = air_of(air, eth, n, 0, 0);
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out));
    n = udp(eth, LINKG, MAC, COMP_IP, LINK_IP, 20808, (const uint8_t *)"x", 1);
    a = air_of(air, eth, n, 0, 0);
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out));
    n = udp(eth, LINKG, PEER, PEER_IP, LINK_IP, 20808, (const uint8_t *)"x", 1);
    a = air_of(air, eth, n, 0, 0x40);
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out));
    a = air_of(air, eth, n, 0, 0x01);
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out));
    a = air_of(air, eth, n, 0, 0);
    air[21] ^= 1;
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, sizeof out));
    a = air_of(air, eth, n, 0, 0);
    assert(!un_air_to_usb(&hub, air, a, LOCAL_IP, out, 20));
    printf("usbnet: the air's frames (multicast, QoS, to the computer, to the FM-1, ARP, not ours): OK\n");
}

static void test_random(void)
{
    static uint8_t buf[1600], o[1600];
    uint32_t i, k, n, ol;
    uint16_t seq = 0;
    srand(5);
    for (i = 0; i < 300000u; i++) {
        n = (uint32_t)rand() % 700u;
        uint8_t *p = malloc(n ? n : 1);
        for (k = 0; k < n; k++)
            p[k] = (uint8_t)rand();
        if (n >= 16u && (rand() & 1)) {                 /* often the addresses that get past the first checks */
            memcpy(p + 6, MAC, 6);
            if (rand() & 1)
                memcpy(p, hub.mac_l, 6);
        }
        if (n >= 32u && (rand() & 1)) {
            p[0] = 0x08, p[1] = 0;
            memcpy(p + 16, BSSID, 6);
            memcpy(p + 24, SNAP, 6);
            p[22] &= 0xF0u;
            if (rand() & 1)
                memcpy(p + 4, MAC, 6);
        }
        (void)un_air_to_usb(&hub, p, n, LOCAL_IP, o, (uint32_t)rand() % sizeof o);
        (void)un_usb_to_local(&hub, p, n, buf, (uint32_t)rand() % sizeof buf);
        (void)un_usb_to_air(&hub, p, n, &seq, buf, (uint32_t)rand() % sizeof buf);
        (void)un_from_local(&hub, p, n, o, (uint32_t)rand() % sizeof o, &ol);
        (void)un_eth_src_ip(p, n);
        free(p);
    }
    printf("usbnet: 300000 random frames into each: OK\n");
}

int main(void)
{
    srand(1);
    wifi_ip_ibss_init(&wip, MAC, BSSID, LOCAL_IP, test_rand);
    wip.udp_rx = on_udp;
    assert(wifi_ip_bind(&wip, 20808) == 0 || wifi_ip_bound(&wip, 20808));
    wifi_ip_join(&wip, LINK_IP);
    un_init(&hub, MAC, BSSID);
    assert(memcmp(hub.mac_l, MAC, 6) && (hub.mac_l[0] ^ MAC[0]) == 0x02);
    test_computer();
    test_local();
    test_air();
    test_random();
    return 0;
}
