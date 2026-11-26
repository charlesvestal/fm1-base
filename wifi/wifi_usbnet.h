/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 w0ts */
/* The USB network (fm1.toml usb_net: felucca-bsp's FELUCCA_NCM, a CDC-NCM adapter on the computer): where a frame goes
 * between the FM-1's IP layer (wifi_ip.c, its 802.11 frames), the air of MODE JANK and the cable. Plain data, no radio,
 * no USB (tests/wifi/usbnet_test.c).
 *   the computer  takes the radio's MAC (the adapter's descriptor offers it): on the air it is one more unit of the group,
 *                 whose unicast the radio acknowledges as its own (as a jank stick does). Nothing is routed, no DHCP:
 *                 the computer gives itself 169.254.x.y, as the units do
 *   the FM-1      on the air the radio's MAC, as ever; on the cable a second one (mac_l: the radio's, its local bit
 *                 flipped), since the computer cannot send to its own address. So its IP layer keeps one MAC and one
 *                 address, and un_* rewrites mac_l <-> mac for the cable (the Ethernet header, and ARP's sender and
 *                 target)
 *   with no air   (Wi-Fi off, or not MODE JANK) the same, the air left out: Link over the cable alone
 * What goes where:
 *   from the IP layer   a group frame to the air and the cable; one to the computer (mac) to the cable only; any other
 *                       to the air only
 *   from the air        a group frame (ARP, IPv4) to the cable too; one to mac to the cable unless it is the FM-1's
 *                       (its IPv4 destination, its ARP target, the FM-1's address); the IP layer takes its own as before
 *   from the computer   a group frame to the IP layer and, filtered (ARP, IPv4 unicast, Link's multicast: the air is
 *                       1 Mb/s), to the air; one to mac_l to the IP layer only; any other unicast to the air. A source
 *                       that is not mac (the computer overrode the address) is dropped: no reply could reach it */
#ifndef WIFI_USBNET_H
#define WIFI_USBNET_H
#include <stdint.h>

#define UN_ETH_HDR 14u
#define UN_DOT11_HDR 24u
#define UN_LLC 8u
#define UN_LINK_PORT 20808u                /* Ableton Link's discovery (224.76.78.75) */

enum { UN_AIR = 1, UN_USB = 2 };

struct un_stats {
    uint32_t to_usb, to_air, to_local;     /* frames passed each way */
    uint32_t drop_filter, drop_big, drop_bad, drop_foreign;
};

struct un_hub {
    uint8_t mac[6];                        /* the radio's: the computer's side of the cable, and the FM-1 on the air */
    uint8_t mac_l[6];                      /* the FM-1 on the cable */
    uint8_t bssid[6];                      /* the group's (the air's frames' addr3) */
    struct un_stats st;
};

void un_init(struct un_hub *h, const uint8_t mac[6], const uint8_t bssid[6]);
/* a frame the IP layer built (f, 802.11, len): UN_AIR and/or UN_USB; with UN_USB its Ethernet frame is in eth (cap
 * bytes), *eth_len long (0 when it does not fit: then UN_USB is not set) */
uint32_t un_from_local(struct un_hub *h, const uint8_t *f, uint32_t len, uint8_t *eth, uint32_t cap, uint32_t *eth_len);
/* a data frame of the group from the air (a, len; MODE JANK's, already the group's): its Ethernet frame for the cable in
 * eth (its length), or 0 when it is not for the computer. local_ip: the FM-1's address (host order) */
uint32_t un_air_to_usb(struct un_hub *h, const uint8_t *a, uint32_t len, uint32_t local_ip, uint8_t *eth, uint32_t cap);
/* a frame from the computer (eth, len): its 802.11 frame for the IP layer in f (its length), or 0 when not the FM-1's */
uint32_t un_usb_to_local(struct un_hub *h, const uint8_t *eth, uint32_t len, uint8_t *f, uint32_t cap);
/* .. and for the air (its length, the sequence from *seq, the IP layer's: one sender, one sequence), or 0 */
uint32_t un_usb_to_air(struct un_hub *h, const uint8_t *eth, uint32_t len, uint16_t *seq, uint8_t *air, uint32_t cap);
/* the IPv4 source of an Ethernet frame (host order; 0: none) */
uint32_t un_eth_src_ip(const uint8_t *eth, uint32_t len);
#endif
