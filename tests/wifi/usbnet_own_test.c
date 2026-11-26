/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 w0ts; in fm1-base: tests/wifi/run.sh */
/* The USB network's service in device/wifi_own.c (usb_net, un_*), on the host: its USB adapter the stand-in in wifi_own.c
 * (un_fake: the computer's link, a block from it, the last frame to it), the radio host/wifi_fake's.
 *   Wi-Fi OFF: no service until the computer's link is up; then the IP layer on the cable alone at MODE JANK's address,
 *   nothing on the air; the computer's ARP for it answered from mac_l, its ping answered, from a block of two datagrams
 *   MODE JANK: once its air is up the same address, the cable joined to the air: the computer's Link multicast goes out on
 *   the air from the radio's MAC, a unit's Link multicast heard on the air reaches the computer, the IP layer's own
 *   Link multicast goes both ways
 *   MODE AUTO (a station): the cable carries nothing; the computer's link down: no service; plat_usb_net's states
 * Exit status: the number of failed checks. */
#include <stdio.h>
#include <string.h>
#include "plat.h"

/* ---- the platform, as wifi_own.c uses it (tests/wifi/wifi_start_test.c's) */
static uint32_t ms;
uint32_t plat_ms(void) { return ms; }
uint32_t plat_ticks24(void) { return ms * 24000u; }
static const uint8_t UID[16] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xF1, 0x02};
const uint8_t *plat_chip_uid(void) { return UID; }
enum { OBJ_WIFI = 2 };
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { (void)obj, (void)dst, (void)max; return -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { (void)obj, (void)src, (void)len; return 0; }
static int bt_state = BLE_OFF;
int plat_ble_state(void) { return bt_state; }
int plat_ble_radio(void) { return bt_state; }
void plat_ble_start(void) { bt_state = BLE_READY; }
#include "../../device/wifi_own.c"

static int fails, checks;
static void check(int ok, const char *what)
{
    checks++;
    if (!ok) {
        fails++;
        printf("FAIL: %s\n", what);
    }
}
static void run(uint32_t n_ms)                    /* the main loop: a pass every millisecond */
{
    uint32_t i;
    for (i = 0; i < n_ms; i++) {
        ms++;
        wifi_poll();
    }
}

/* ---- the air: what the radio sent, and a frame to hear */
static uint8_t air_last[700];
static uint32_t air_n, air_len;
static int on_air(const uint8_t *frame, uint32_t len, uint32_t wcid, uint32_t ack)
{
    (void)wcid, (void)ack;
    air_n++;
    air_len = len < sizeof air_last ? len : sizeof air_last;
    memcpy(air_last, frame, air_len);
    return 0;
}
static uint8_t heard[700];
static uint32_t heard_n;
static int32_t hear(uint32_t *buf, uint32_t max)  /* the RX DMA's layout: length word (big endian), RXWI, the frame */
{
    uint8_t *b = (uint8_t *)buf;
    uint32_t n = FM1_WIFI_RXWI + heard_n;
    if (!heard_n || n > max)
        return 0;
    b[0] = (uint8_t)(n >> 24), b[1] = (uint8_t)(n >> 16), b[2] = (uint8_t)(n >> 8), b[3] = (uint8_t)n;
    memset(b + 4, 0, FM1_WIFI_RXWI);
    b[4 + 2] = (uint8_t)heard_n;
    b[4 + 3] = (uint8_t)(heard_n >> 8 & 0x0Fu);
    b[4 + 8] = (uint8_t)(int8_t)-50;
    memcpy(b + 4 + FM1_WIFI_RXWI, heard, heard_n);
    heard_n = 0;
    return (int32_t)n;
}

/* ---- the computer's frames */
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static const uint8_t LINKG[6] = {0x01, 0x00, 0x5E, 0x4C, 0x4E, 0x4B};
static const uint8_t PEER[6] = {0x02, 0x99, 0x88, 0x77, 0x66, 0x55};
#define COMP_IP 0xA9FE1415u
#define PEER_IP 0xA9FE1E1Fu
#define LINK_IP 0xE04C4E4Bu
static void w16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void w32(uint8_t *p, uint32_t v) { w16(p, v >> 16); w16(p + 2, v & 0xFFFFu); }
static uint32_t e_arp(uint8_t *e, uint32_t tip)
{
    memcpy(e, BCAST, 6);
    memcpy(e + 6, un.hub.mac, 6);
    w16(e + 12, 0x0806);
    w16(e + 14, 1), w16(e + 16, 0x0800), e[18] = 6, e[19] = 4, w16(e + 20, 1);
    memcpy(e + 22, un.hub.mac, 6);
    w32(e + 28, COMP_IP);
    memset(e + 32, 0, 6);
    w32(e + 38, tip);
    return 42;
}
static uint32_t e_ip(uint8_t *e, const uint8_t *dst, const uint8_t *src, uint32_t sip, uint32_t dip, uint32_t proto,
                     const uint8_t *pl, uint32_t n)
{
    uint8_t *p = e + 14;
    memcpy(e, dst, 6);
    memcpy(e + 6, src, 6);
    w16(e + 12, 0x0800);
    memset(p, 0, 20);
    p[0] = 0x45, p[8] = 64, p[9] = (uint8_t)proto;
    w16(p + 2, 20 + n);
    w32(p + 12, sip);
    w32(p + 16, dip);
    w16(p + 10, ip_chksum(p, 20));
    memcpy(p + 20, pl, n);
    return 34 + n;
}
static uint32_t e_link(uint8_t *e, const uint8_t *src, uint32_t sip)   /* a Link discovery multicast */
{
    uint8_t u[16] = {0x9C, 0x40, 0x51, 0x48, 0, 16, 0, 0, '_', 'a', 's', 'd', 'p', '_', 'v', 1};
    return e_ip(e, LINKG, src, sip, LINK_IP, 17, u, sizeof u);
}
static uint32_t un_dot(const uint8_t *e, uint32_t n)   /* the frame as a unit of the group sends it, into heard */
{
    static uint16_t seq;
    memset(heard, 0, 24);
    heard[0] = 0x08;
    memcpy(heard + 4, e, 6);
    memcpy(heard + 10, e + 6, 6);
    memcpy(heard + 16, jnk.n.bssid, 6);
    seq++;
    heard[22] = (uint8_t)(seq << 4), heard[23] = (uint8_t)(seq >> 4);
    memcpy(heard + 24, (const uint8_t[]){0xAA, 0xAA, 0x03, 0, 0, 0}, 6);
    memcpy(heard + 30, e + 12, 2);
    memcpy(heard + 32, e + 14, n - 14);
    return 32 + n - 14;
}
static uint8_t blk[2048];
static void computer_sends(const uint8_t *e, uint32_t n)
{
    un_fake.rx_n = ncm_ntb_one(blk, sizeof blk, 1, e, n);
    un_fake.rx = blk;
}
static void computer_sends2(const uint8_t *a, uint32_t an, const uint8_t *b, uint32_t bn)   /* two datagrams, one block */
{
    uint32_t off_b = (NCM_OUT_HDR + 4u + an + 3u) & ~3u, total = off_b + bn;
    memset(blk, 0, sizeof blk);
    memcpy(blk, "NCMH", 4), ncm_wr16(blk + 4, 12), ncm_wr16(blk + 8, total), ncm_wr16(blk + 10, 12);
    memcpy(blk + 12, "NCM0", 4), ncm_wr16(blk + 16, 20), ncm_wr16(blk + 18, 0);
    ncm_wr16(blk + 20, NCM_OUT_HDR + 4u), ncm_wr16(blk + 22, an);
    ncm_wr16(blk + 24, off_b), ncm_wr16(blk + 26, bn);
    memcpy(blk + NCM_OUT_HDR + 4u, a, an);
    memcpy(blk + off_b, b, bn);
    un_fake.rx = blk;
    un_fake.rx_n = total;
}

int main(void)
{
    static uint8_t e[700], e2[700];
    uint8_t icmp[12] = {8, 0, 0, 0, 0, 1, 0, 1, 'p', 'i', 'n', 'g'};
    uint32_t ip, n, n2, a0;
    fm1_fake_tx = on_air;
    fm1_fake_rx = hear;
    plat_ble_start();
    plat_wifi_mode(WIFI_MODE_OFF);
    run(10);
    check(plat_usb_net() == USB_NET_WAIT && !plat_usb_net_addr() && !wl.on, "OFF, the computer's link down: no service");
    un_fake.ready = 1;
    run(2);
    ip = plat_usb_net_addr();
    check(plat_usb_net() == USB_NET_UP && un.usb && wl.on, "the computer's link up: the service on the cable alone");
    check((ip >> 16) == 0xA9FEu && ip == wl.ip.ip, "its address a link-local one, MODE JANK's");
    check(!plat_wifi_addr() && !plat_wifi_ip(), ".. the cable's, not the Wi-Fi's (OFF: no Wi-Fi address shown)");
    /* the computer finds the FM-1 (ARP), then pings it and asks once more, in one block */
    n = e_arp(e, ip);
    computer_sends(e, n);
    run(1);
    check(un_fake.tx_n == 42 && !memcmp(un_fake.tx, un.hub.mac, 6) && !memcmp(un_fake.tx + 6, un.hub.mac_l, 6) &&
              un_fake.tx[21] == 2 && !memcmp(un_fake.tx + 22, un.hub.mac_l, 6), "the ARP answer, from mac_l");
    un_fake.tx_n = 0;
    n = e_ip(e, un.hub.mac_l, un.hub.mac, COMP_IP, ip, 1, icmp, sizeof icmp);
    n2 = e_arp(e2, ip);
    computer_sends2(e, n, e2, n2);
    run(1);
    check(un_fake.tx_n == 34 + sizeof icmp && un_fake.tx[34] == 0 && !memcmp(un_fake.tx, un.hub.mac, 6), "the ping answered");
    un_fake.tx_n = 0;
    run(2);
    check(un_fake.tx_n == 42 && un_fake.tx[21] == 2, ".. and the ARP after it, from the same block, next");
    un_fake.tx_n = 0;
    check(!air_n, "nothing on the air with the Wi-Fi OFF");
    /* MODE JANK: once the air is up, the cable joined to it */
    plat_wifi_mode(WIFI_MODE_JANK);
    run(3500);
    check(JNK_UP && wl.jn && !un.usb && un.up, "MODE JANK's air up: the cable joined to it");
    check(plat_usb_net_addr() == ip, ".. at the same address");
    un_fake.tx_n = 0;
    a0 = air_n;
    n = e_link(e, un.hub.mac, COMP_IP);
    computer_sends(e, n);
    run(1);
    check(air_n > a0 && !memcmp(air_last + 4, LINKG, 6) && !memcmp(air_last + 10, un.hub.mac, 6) &&
              !memcmp(air_last + 16, jnk.n.bssid, 6), "the computer's Link multicast on the air, from the radio's MAC");
    un_fake.tx_n = 0;
    n = e_link(e, PEER, PEER_IP);                 /* a unit's, heard */
    heard_n = un_dot(e, n);
    run(2);
    check(un_fake.tx_n == n && !memcmp(un_fake.tx, e, n), "a unit's Link multicast reaches the computer");
    un_fake.tx_n = 0;
    a0 = air_n;
    check(plat_net_udp_send(20808, LINK_IP, 20808, "_asdp_v\x01", 8) == 0, "the IP layer sends Link's multicast");
    run(1);
    check(air_n == a0 + 1 && un_fake.tx_n && !memcmp(un_fake.tx + 6, un.hub.mac_l, 6), ".. on the air and the cable");
    un_fake.tx_n = 0;
    /* a station: the cable carries nothing */
    plat_wifi_mode(WIFI_MODE_AUTO);
    run(5);
    check(!un.up && !un.usb && !plat_usb_net_addr(), "MODE AUTO: nothing on the cable");
    plat_wifi_mode(WIFI_MODE_OFF);
    run(5);
    check(un.up && un.usb && plat_usb_net_addr() == ip, "OFF again: the cable's service back, the same address");
    un_fake.ready = 0;
    run(2);
    check(!un.up && !un.usb && !wl.on && plat_usb_net() == USB_NET_WAIT, "the computer gone: no service");
    plat_usb_net_present(0);
    check(plat_usb_net() == USB_NET_OFF, "not presented: OFF");
    printf("usbnet_own: %d of %d checks failed\n", fails, checks);
    return fails;
}
