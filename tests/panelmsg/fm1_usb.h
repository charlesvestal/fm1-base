/* SPDX-License-Identifier: MIT OR MPL-2.0
 * A host model of the FM-1's USB0 register layer (the API of Dinghy's hal/fm1_usb.h), so the firmware's own usb.c and
 * ota.c run unchanged on a computer. Only what the MIDI endpoint uses is modelled: the SIE register file per endpoint
 * index, EP1 IN (a packet armed with TXCSR1 bit 0 is taken by the "computer" when it reads) and EP1 OUT (the test puts
 * a packet in ep1rx and sets RXCSR1 bit 0). Anything else is a no-op. */
#pragma once
#include <stdint.h>
#include <string.h>
#define FM1_INLINE static inline

static uint32_t hu_reg[8][32];                  /* [endpoint index][SIE register] */
static uint32_t hu_index, hu_data;
static const uint8_t *hu_ep1_buf;               /* armed EP1 IN packet */
static uint32_t hu_ep1_n;
static uint32_t hu_bad;                         /* register misuse seen by the model: asserted 0 by the test */

FM1_INLINE void fm1_usb_reset(void) {}
FM1_INLINE void fm1_usb_attach(void *ep0) { (void)ep0; }
FM1_INLINE void fm1_usb_off(void) {}
FM1_INLINE uint32_t fm1_usb_sie_on(void) { return 4u; }
FM1_INLINE void fm1_usb_sie_wr_start(uint32_t r, uint32_t v)
{
    if (r >= 32u) { hu_bad++; return; }
    if (r == 14u) { hu_index = v & 7u; return; }      /* S_INDEX */
    if (r == 20u && (v & 0x10u)) {                    /* RXCSR1 FlushFIFO (MUSB): drops the packet, clears RxPktRdy */
        hu_reg[hu_index][r] = v & 0xFFu & ~0x11u;
        return;
    }
    hu_reg[hu_index][r] = v & 0xFFu;
}
FM1_INLINE void fm1_usb_sie_rd_start(uint32_t r) { hu_data = r < 32u ? hu_reg[hu_index][r] : (hu_bad++, 0u); }
FM1_INLINE uint32_t fm1_usb_sie_done(void) { return 0x8000u; }
FM1_INLINE uint32_t fm1_usb_sie_data(void) { return hu_data & 0xFFu; }
FM1_INLINE void fm1_usb_ep0_buf(void *p) { (void)p; }
FM1_INLINE void fm1_usb_ep_txbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
FM1_INLINE void fm1_usb_ep_rxbuf(uint32_t ep, void *p) { (void)ep; (void)p; }
FM1_INLINE void fm1_usb_ep0_send(void *p, uint32_t n) { (void)p; (void)n; }
FM1_INLINE void fm1_usb_ep_send(uint32_t ep, void *p, uint32_t n)
{
    if (ep != 1u || n > 64u || (n & 3u)) { hu_bad++; return; }
    if (hu_reg[1][17] & 1u) hu_bad++;           /* armed over a packet the computer has not read */
    hu_ep1_buf = p; hu_ep1_n = n;
}
FM1_INLINE void fm1_usb_ep4_txbuf(void *p) { (void)p; }
FM1_INLINE void fm1_usb_ep4_send(void *p, uint32_t n) { (void)p; (void)n; }
FM1_INLINE void fm1_usb_rx_sync(void) {}
FM1_INLINE void fm1_usb_ep_enable(uint32_t eps) { (void)eps; }
FM1_INLINE uint32_t fm1_usb_sof_take(void) { return 0; }
