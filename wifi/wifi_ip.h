/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David */
/* A small IP layer on the FM-1's Wi-Fi (no TCP, no DNS: what a session protocol such as Ableton Link needs):
 * - ARP responder & request sender
 * - DHCP client state machine (Discover -> Offer -> Request -> Ack / Bound)
 * - IGMPv2 membership reports for one multicast group
 * - UDP: up to WIFI_IP_PORTS ports bound, their datagrams (unicast, broadcast, the group's) to one receive hook
 * - an early drop filter right after the RX DMA
 * MODE AP: the access point's side (FromDS frames, a DHCP server hook, ping answered). MODE JAM: the same with no access
 * point (ad-hoc frames, wifi_ip_ibss_init). From Optimist's wifi/wifi_ip.c. */
#ifndef WIFI_IP_H
#define WIFI_IP_H
#include <stdint.h>

#define WIFI_IP_80211_HDR 24u
#define WIFI_IP_LLC_SNAP 8u
#define WIFI_IP_IP_HDR 20u
#define WIFI_IP_UDP_HDR 8u
#define WIFI_IP_TOTAL_HDR (WIFI_IP_80211_HDR + WIFI_IP_LLC_SNAP + WIFI_IP_IP_HDR + WIFI_IP_UDP_HDR) /* 60 bytes */

#define WIFI_IP_DHCP_PORT_SERVER 67u
#define WIFI_IP_DHCP_PORT_CLIENT 68u
#define WIFI_IP_PORTS 4u                    /* UDP ports bound at once */
#define WIFI_IP_IBSS 2u

enum wifi_ip_dhcp_state {
    WIFI_IP_DHCP_IDLE = 0,
    WIFI_IP_DHCP_DISCOVERING,
    WIFI_IP_DHCP_REQUESTING,
    WIFI_IP_DHCP_BOUND,
    WIFI_IP_DHCP_FAILED
};

struct wifi_arp_entry {
    uint32_t ip;                           /* host order */
    uint8_t mac[6];
    uint8_t valid;
};

struct wifi_ip {
    uint8_t mac[6];                        /* our station MAC */
    uint8_t bssid[6];                      /* AP BSSID */
    uint32_t ip;                           /* our IPv4 address (host order), 0 = unassigned */
    uint32_t netmask;                      /* subnet mask (host order) */
    uint32_t gw;                           /* gateway IP (host order) */
    uint32_t dns;                          /* DNS server (host order) */
    uint32_t dhcp_server_ip;               /* DHCP server IP (host order) */
    uint32_t dhcp_offered_ip;              /* DHCP offered IP (host order) */
    uint32_t dhcp_xid;                     /* DHCP transaction ID */
    enum wifi_ip_dhcp_state dhcp_state;
    int64_t dhcp_t0;                       /* host time (us) of current state */
    uint8_t dhcp_retries;
    uint16_t ports[WIFI_IP_PORTS];         /* the UDP ports bound (0: a free slot) */
    uint32_t group;                        /* the multicast group joined (host order), 0: none */
    /* a datagram to a bound port (unicast to us, broadcast, or to the group), with its arrival time */
    void (*udp_rx)(void *ctx, int64_t now_us, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port,
                   const uint8_t *p, uint32_t n);
    void (*on_addr)(void *ctx, uint32_t ip);   /* DHCP bound: our address */
    void *ctx;                             /* for both */
    struct wifi_arp_entry arp_tbl[4];
    uint16_t tx_seq;                       /* 802.11 sequence counter */
    uint8_t protect;                       /* 1: our data frames carry the Protected bit (keys installed: the MAC encrypts) */
    uint8_t ap;                            /* 1: MODE AP: we are the access point (our data frames are FromDS, mac == bssid);
                                            * WIFI_IP_IBSS: MODE JAM: no access point, an address of our own, ad-hoc frames */
    void (*send)(uint32_t len);            /* a finished frame (at tx_buf, len bytes) out: the owner's transmit, else the station's */
    uint32_t (*udp_srv)(struct wifi_ip *wip, const uint8_t *src_mac, const uint8_t *msg, uint32_t n, uint8_t *tx_buf);
                                           /* AP: a datagram to UDP port 67 (the DHCP server): the reply frame's length, 0 for none */
    int64_t igmp_t;                        /* host time (us) the next membership report is due */
    void (*rand)(uint8_t *out, uint8_t n);
};

/* Initialize IP layer state */
void wifi_ip_init(struct wifi_ip *wip, const uint8_t sta_mac[6], const uint8_t bssid[6], void (*rand_fn)(uint8_t *out, uint8_t n));

/* MODE AP: this is the access point at ip (host order): FromDS frames, the /24 netmask, no DHCP client, no gateway */
void wifi_ip_ap_init(struct wifi_ip *wip, const uint8_t ap_mac[6], uint32_t ip, void (*rand_fn)(uint8_t *out, uint8_t n));
/* MODE JAM: an ad-hoc network: our address at ip (host order), a /16; ARP as MODE AP's (nothing sent to an unknown address) */
void wifi_ip_ibss_init(struct wifi_ip *wip, const uint8_t mac[6], const uint8_t bssid[6], uint32_t ip, void (*rand_fn)(uint8_t *out, uint8_t n));
/* a station's address, learned (the DHCP server's lease, a packet's source) */
void wifi_ip_arp_learn(struct wifi_ip *wip, uint32_t ip, const uint8_t mac[6]);
/* one UDP datagram to a given MAC and IP, from any source port (the DHCP server's reply). Returns the frame length. */
uint32_t wifi_ip_reply_udp(struct wifi_ip *wip, const uint8_t dst_mac[6], uint32_t dst_ip, uint16_t src_port, uint16_t dst_port, const uint8_t *payload, uint32_t payload_len, uint8_t *tx_buf);

/* bind a UDP port (its datagrams to wip->udp_rx): 0, or -1 (no free slot, port 0); unbind lets it go */
int wifi_ip_bind(struct wifi_ip *wip, uint16_t port);
void wifi_ip_unbind(struct wifi_ip *wip, uint16_t port);
int wifi_ip_bound(const struct wifi_ip *wip, uint16_t port);
/* join a multicast group (0: none): its membership report goes out at the next wifi_ip_poll once there is an address,
 * then every 30 s */
void wifi_ip_join(struct wifi_ip *wip, uint32_t group);

/* Start DHCP client negotiation. Returns frame length written to tx_buf (or 0). */
uint32_t wifi_ip_dhcp_start(struct wifi_ip *wip, uint8_t *tx_buf);

/* Periodic timer polling (DHCP timeouts / retransmissions). Returns frame length written to tx_buf if transmit needed. */
uint32_t wifi_ip_poll(struct wifi_ip *wip, int64_t now_us, uint8_t *tx_buf);

/* Fast early drop filter (Optimist's docs/LINK-DESIGN.md §2.7).
 * Call immediately after RX DMA (or after decryption).
 * Returns 1 to keep and process frame, 0 to drop immediately. */
int wifi_ip_early_filter(const struct wifi_ip *wip, const uint8_t *frame, uint32_t len);

/* Process an incoming 802.11 Data frame (LLC/SNAP encapsulated ARP, IPv4/IGMP, IPv4/UDP).
 * Delivers the datagrams of bound ports to wip->udp_rx. If a response frame (ARP reply, DHCP req, etc.) is generated,
 * writes to tx_buf and sets tx_len, returning 1; returns 0 if consumed, -1 if ignored/error. */
int wifi_ip_rx_frame(struct wifi_ip *wip, const uint8_t *frame, uint32_t len, int64_t now_us, uint8_t *tx_buf, uint32_t *tx_len);

/* A UDP datagram from src_port. Encapsulates in 802.11 Data + LLC/SNAP + IPv4 + UDP.
 * Returns the total frame length written to tx_buf (0: no way to the address, an AP's unknown station). */
uint32_t wifi_ip_send_udp(struct wifi_ip *wip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, const uint8_t *payload, uint32_t payload_len, uint8_t *tx_buf);

/* Construct an IGMPv2 Membership Report for a multicast group.
 * Returns the total frame length written to tx_buf. */
uint32_t wifi_ip_igmp_join(struct wifi_ip *wip, uint32_t group_ip, uint8_t *tx_buf);

#endif
