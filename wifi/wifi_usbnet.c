/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 w0ts */
/* The USB network's frames (wifi_usbnet.h). The air's format is MODE JANK's (wifi_jank.h); the conversions follow
 * jank's bridge (jank core/jank_core.c, the same author), with the FM-1's own IP layer beside the computer. */
#include "wifi_usbnet.h"
#ifndef __PI32V2__
#include <string.h>                      /* (the firmware's: felucca-bsp's libc.c, in the unity) */
#endif

#define FC_DATA 0x08u
#define FC_TYPE_MASK 0x0Cu
#define FC_SUB_QOS 0x80u
#define FC_SUB_NODATA 0x40u
#define FC1_DS 0x03u                       /* ToDS, FromDS */
#define FC1_MOREFRAG 0x04u
#define FC1_PROTECTED 0x40u
#define FC1_ORDER 0x80u
#define ETH_ARP 0x0806u
#define ETH_IPV4 0x0800u

static const uint8_t SNAP[6] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00};
static const uint8_t LINK_GROUP_MAC[6] = {0x01, 0x00, 0x5E, 0x4C, 0x4E, 0x4B};

static uint32_t rd16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static int is_group(const uint8_t *m) { return m[0] & 1u; }
static int same(const uint8_t *a, const uint8_t *b) { return !memcmp(a, b, 6); }

void un_init(struct un_hub *h, const uint8_t mac[6], const uint8_t bssid[6])
{
    memset(h, 0, sizeof *h);
    memcpy(h->mac, mac, 6);
    memcpy(h->mac_l, mac, 6);
    h->mac_l[0] ^= 0x02u;                  /* the local bit flipped: never the radio's own */
    memcpy(h->bssid, bssid, 6);
}

/* the offset of the LLC header of a plain data frame with neither ToDS nor FromDS (0: none such) */
static uint32_t data_offset(const uint8_t *a, uint32_t len)
{
    uint32_t hdr = UN_DOT11_HDR;
    if (len < UN_DOT11_HDR + UN_LLC || (a[0] & FC_TYPE_MASK) != FC_DATA || (a[0] & 0x03u) || (a[0] & FC_SUB_NODATA))
        return 0;
    if ((a[1] & (FC1_DS | FC1_PROTECTED | FC1_MOREFRAG)) || (a[22] & 0x0Fu) || is_group(a + 10))
        return 0;
    if (a[0] & FC_SUB_QOS) {
        hdr += 2u + ((a[1] & FC1_ORDER) ? 4u : 0u);
        if (len < hdr + UN_LLC || (a[24] & 0x80u))
            return 0;                       /* (an A-MSDU: not ours to unpack) */
    }
    if (memcmp(a + hdr, SNAP, 6) || rd16(a + hdr + 6u) < 0x0600u)
        return 0;
    return hdr;
}

/* one of ARP's hardware addresses (at: 8 the sender's, 18 the target's): from -> to. One field only: the FM-1 and the
 * computer both have the radio's MAC, so in the FM-1's answer to the computer the sender (the FM-1: mac_l on the cable)
 * and the target (the computer: mac) start out the same */
static void arp_swap(uint8_t *arp, uint32_t n, uint32_t at, const uint8_t from[6], const uint8_t to[6])
{
    if (n < 28u || rd16(arp) != 1u || rd16(arp + 2) != ETH_IPV4 || arp[4] != 6u)
        return;
    if (same(arp + at, from))
        memcpy(arp + at, to, 6);
}

/* an Ethernet frame from an 802.11 one: dst, src (src_from -> src_to), the EtherType and the payload; 0 if too large */
static uint32_t to_eth(const uint8_t *a, uint32_t len, uint32_t hdr, const uint8_t *dst, const uint8_t *src, uint8_t *eth,
                       uint32_t cap)
{
    uint32_t pl = len - hdr - UN_LLC;
    if (cap < UN_ETH_HDR + pl)
        return 0;
    memcpy(eth, dst, 6);                   /* (dst, src: in a, never in eth) */
    memcpy(eth + 6, src, 6);
    memcpy(eth + 12, a + hdr + 6u, 2);
    memcpy(eth + UN_ETH_HDR, a + hdr + UN_LLC, pl);
    return UN_ETH_HDR + pl;
}

static uint32_t to_dot11(const uint8_t *eth, uint32_t len, const uint8_t *dst, const uint8_t *src, const uint8_t *bssid,
                         uint32_t seq, uint8_t *f, uint32_t cap)
{
    uint32_t pl = len - UN_ETH_HDR;
    if (cap < UN_DOT11_HDR + UN_LLC + pl)
        return 0;
    memcpy(f + UN_DOT11_HDR + UN_LLC, eth + UN_ETH_HDR, pl);   /* (f and eth: two buffers) */
    memset(f, 0, UN_DOT11_HDR);
    f[0] = FC_DATA;
    memcpy(f + 4, dst, 6);
    memcpy(f + 10, src, 6);
    memcpy(f + 16, bssid, 6);
    f[22] = (uint8_t)(seq << 4);
    f[23] = (uint8_t)(seq >> 4);
    memcpy(f + UN_DOT11_HDR, SNAP, 6);
    f[UN_DOT11_HDR + 6] = eth[12];
    f[UN_DOT11_HDR + 7] = eth[13];
    return UN_DOT11_HDR + UN_LLC + pl;
}

uint32_t un_from_local(struct un_hub *h, const uint8_t *f, uint32_t len, uint8_t *eth, uint32_t cap, uint32_t *eth_len)
{
    const uint8_t *da = f + 4;
    uint32_t where, n;
    *eth_len = 0;
    if (len < UN_DOT11_HDR + UN_LLC || memcmp(f + UN_DOT11_HDR, SNAP, 6)) {
        h->st.drop_bad++;
        return UN_AIR;                     /* (not one of its data frames: as before, the air's) */
    }
    where = is_group(da) ? UN_AIR | UN_USB : same(da, h->mac) ? UN_USB : UN_AIR;
    if (!(where & UN_USB))
        return where;
    n = to_eth(f, len, UN_DOT11_HDR, da, h->mac_l, eth, cap);
    if (!n) {
        h->st.drop_big++;
        return where & ~(uint32_t)UN_USB;
    }
    if (rd16(eth + 12) == ETH_ARP)
        arp_swap(eth + UN_ETH_HDR, n - UN_ETH_HDR, 8, h->mac, h->mac_l);   /* (the sender: the FM-1) */
    *eth_len = n;
    h->st.to_usb++;
    return where;
}

/* the frame is the FM-1's own (to its address), not the computer's */
static int for_local(const uint8_t *type_p, const uint8_t *p, uint32_t n, uint32_t local_ip)
{
    uint32_t type = rd16(type_p);
    if (!local_ip)
        return 0;
    if (type == ETH_ARP)
        return n >= 28u && rd32(p + 24) == local_ip;
    if (type == ETH_IPV4)
        return n >= 20u && (p[0] >> 4) == 4u && rd32(p + 16) == local_ip;
    return 0;
}

uint32_t un_air_to_usb(struct un_hub *h, const uint8_t *a, uint32_t len, uint32_t local_ip, uint8_t *eth, uint32_t cap)
{
    uint32_t hdr = data_offset(a, len), type, n;
    const uint8_t *p;
    if (!hdr || !same(a + 16, h->bssid)) {
        h->st.drop_bad++;
        return 0;
    }
    if (same(a + 10, h->mac))
        return 0;                          /* (our own, heard back) */
    type = rd16(a + hdr + 6u);
    p = a + hdr + UN_LLC;
    if (type != ETH_ARP && type != ETH_IPV4) {
        h->st.drop_filter++;
        return 0;
    }
    if (!is_group(a + 4) && (!same(a + 4, h->mac) || for_local(a + hdr + 6u, p, len - hdr - UN_LLC, local_ip)))
        return 0;                          /* another unit's, or the FM-1's own */
    n = to_eth(a, len, hdr, a + 4, a + 10, eth, cap);
    if (!n) {
        h->st.drop_big++;
        return 0;
    }
    h->st.to_usb++;
    return n;
}

/* Link's multicast UDP (its discovery), with the bounds of a frame that may be cut short */
static int is_link_multicast(const uint8_t *eth, uint32_t len)
{
    const uint8_t *ip = eth + UN_ETH_HDR;
    uint32_t ihl;
    if (!same(eth, LINK_GROUP_MAC) || len < UN_ETH_HDR + 20u || (ip[0] >> 4) != 4u || ip[9] != 17u)
        return 0;
    ihl = (uint32_t)(ip[0] & 0x0Fu) * 4u;
    return ihl >= 20u && len >= UN_ETH_HDR + ihl + 8u && rd16(ip + ihl + 2u) == UN_LINK_PORT;
}

static int usb_frame_ok(struct un_hub *h, const uint8_t *eth, uint32_t len)
{
    if (len < UN_ETH_HDR || is_group(eth + 6) || rd16(eth + 12) < 0x0600u) {
        h->st.drop_bad++;
        return 0;
    }
    if (!same(eth + 6, h->mac)) {
        h->st.drop_foreign++;
        return 0;
    }
    return 1;
}

uint32_t un_usb_to_local(struct un_hub *h, const uint8_t *eth, uint32_t len, uint8_t *f, uint32_t cap)
{
    uint32_t type, n;
    const uint8_t *dst;
    if (len < UN_ETH_HDR || !same(eth + 6, h->mac))
        return 0;                          /* (counted once, by un_usb_to_air) */
    type = rd16(eth + 12);
    if (type != ETH_ARP && type != ETH_IPV4)
        return 0;
    if (is_group(eth))
        dst = eth;
    else if (same(eth, h->mac_l))
        dst = h->mac;                      /* the IP layer's own address */
    else
        return 0;
    n = to_dot11(eth, len, dst, eth + 6, h->bssid, 0, f, cap);
    if (!n) {
        h->st.drop_big++;
        return 0;
    }
    if (type == ETH_ARP)
        arp_swap(f + UN_DOT11_HDR + UN_LLC, n - UN_DOT11_HDR - UN_LLC, 18, h->mac_l, h->mac);   /* (the target) */
    h->st.to_local++;
    return n;
}

uint32_t un_usb_to_air(struct un_hub *h, const uint8_t *eth, uint32_t len, uint16_t *seq, uint8_t *air, uint32_t cap)
{
    uint32_t type, n;
    if (!usb_frame_ok(h, eth, len))
        return 0;
    if (same(eth, h->mac_l))
        return 0;                          /* the FM-1's alone */
    type = rd16(eth + 12);
    if (type != ETH_ARP && !(type == ETH_IPV4 && (!is_group(eth) || is_link_multicast(eth, len)))) {
        h->st.drop_filter++;
        return 0;
    }
    n = to_dot11(eth, len, eth, h->mac, h->bssid, *seq, air, cap);
    if (!n) {
        h->st.drop_big++;
        return 0;
    }
    *seq = (uint16_t)(*seq + 1u);
    h->st.to_air++;
    return n;
}

uint32_t un_eth_src_ip(const uint8_t *eth, uint32_t len)
{
    const uint8_t *p = eth + UN_ETH_HDR;
    if (len < UN_ETH_HDR + 20u)
        return 0;
    if (rd16(eth + 12) == ETH_IPV4 && (p[0] >> 4) == 4u)
        return rd32(p + 12);
    if (rd16(eth + 12) == ETH_ARP && len >= UN_ETH_HDR + 28u)
        return rd32(p + 14);
    return 0;
}
