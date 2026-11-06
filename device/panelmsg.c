/* SPDX-License-Identifier: MIT OR MPL-2.0
 * Copyright (C) 2026 Bitbazooka LLC (Les, les.audio)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice
 * and this permission notice shall be included in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * (Or, at your option, the Mozilla Public License 2.0, https://mozilla.org/MPL/2.0/.)
 *
 * Open panel messages for the FM-1, protocol 1: the main-loop side (module v3). The protocol is an open spec any app
 * may read or implement (the panel-messages spec); Les is one reader. Include once, after usb.c and ota.c (it uses their SysEx
 * plumbing: ota_frame_get / ota_frame_done / ota_wire_send and the SysEx out ring), and call panelmsg_poll() from the main
 * loop next to ota_service() (before or after it). Never from the USB rescue, never inside the update session.
 * Before the include, the firmware names itself (required, no fallback):
 *   #define PANELMSG_FW_NAME     its own name macro, or "Its Name"   (up to 23 ASCII characters are sent)
 *   #define PANELMSG_FW_VERSION  its own version macro, the one its release build sets   (up to 15)
 * and may set PANELMSG_ALLOWED() to its own switch (a menu setting): 0 = never listen, answer HELLO with "switched off".
 *
 * Wire format: SysEx F0 <id1 id2 id3> 50 4D <cmd> <args> F7. The ID is a registered three-byte MIDI manufacturer ID.
 *   computer -> FM-1 (cmd 1..15)
 *     HELLO 1 <proto>   listen for PANELMSG_ALIVE_MS (send again every ~2 s); proto = the newest the computer speaks (>= 1).
 *                       A HELLO without one is ignored
 *     BYE   2           stop now
 *   FM-1 -> computer (cmd 16..31; the FM-1 never acts on these, so its own output looped back to it does nothing)
 *     EVENTS   16 <seq2> <t2> then up to PANELMSG_PER_FRAME x (<kind> <id> <value>), in the order played:
 *                 kind 1 press, 2 release (id 0..13 button, 14..40 key; value 0); kind 3 turn (id 48 + encoder 0..6;
 *                 value 64 + detents, 1..127)
 *     HELD     17 <seq2> <t2> <lost2> <buttons2> <keys4>   what is held down right now; first in every listening
 *                 session and again after anything was dropped (then lost > 0)
 *     IDENTITY 18 <proto=1> <flags> <name> 0 <version> 0 <platform> 0   the answer to each HELLO. flags bit 0: 1 = it
 *                 is listening and will send EVENTS / HELD; 0 = this firmware has the protocol switched off
 *   seq: frames sent since the HELLO (HELD and EVENTS share it), so a frame lost on the wire shows as a gap.
 *   t: the FM-1's millisecond clock, low 14 bits, when the frame was built (the frame's events happened, in order,
 *   after the previous frame's t). Two-byte numbers are 7 bits each, low first.
 * Rules: sends nothing until a HELLO; never waits and never competes: a frame goes out only into an EMPTY SysEx out
 * ring, so the updater, the editor and a screen reader always find all of it but one frame free; yields a pass
 * whenever another frame is waiting; goes quiet the moment an update is asked for; never writes flash.
 * A platform with its own SysEx plumbing (fm1-base: device/panelmsg_base.c) defines, before this file:
 *   PANELMSG_WIRE_SUPPLIED   it supplies panelmsg_wire_idle() (1 = the USB is up and its SysEx out is empty) and
 *                       panelmsg_wire_send(p, n) (one whole F0..F7 frame, only if idle; 0 = sent): the module reads no ring
 *   PANELMSG_FRAMES_ROUTED   its main loop hands each received SysEx to panelmsg_frame(p, n) (1 = this protocol's, handled)
 *                       beside its other readers, before its updater's; panelmsg_poll() then only sends
 * Without them, the Felucca layout above. */
#ifndef PANELMSG_ON
static void panelmsg_poll(void) {}                  /* a board without panelmsg.h (another HAL): nothing */
#else
#if !defined(PANELMSG_FW_NAME) || !defined(PANELMSG_FW_VERSION)
#error "panelmsg.c: define PANELMSG_FW_NAME and PANELMSG_FW_VERSION before it (this firmware's own name, and the version macro its release build sets)"
#endif
#ifndef PANELMSG_PLATFORM
#define PANELMSG_PLATFORM "felucca-hal"
#endif
#ifndef PANELMSG_SYSEX_ID
/* The MIDI manufacturer ID 00H 03H 0CH, issued by the MIDI Association to Bitbazooka LLC (Les) on 2026-11-06. */
#define PANELMSG_SYSEX_ID 0x00u, 0x03u, 0x0Cu
#endif
static const uint8_t panelmsg_id[] = { PANELMSG_SYSEX_ID };
#define PANELMSG_IDN 3u
_Static_assert(sizeof panelmsg_id == PANELMSG_IDN, "a registered MIDI manufacturer ID is three bytes (00 xx yy)");
#ifndef PANELMSG_ALLOWED
#define PANELMSG_ALLOWED() 1                         /* the firmware's own switch, if it has one: 0 = never listen */
#endif
#ifndef PANELMSG_HELD_BUTTONS
#define PANELMSG_HELD_BUTTONS() (fm1_in.buttons)     /* the HAL's debounced state, bit = id */
#endif
#ifndef PANELMSG_HELD_KEYS
#define PANELMSG_HELD_KEYS() (fm1_in.notes)          /* bit = id - 14 */
#endif
#ifndef PANELMSG_UPDATE_ASKED
#define PANELMSG_UPDATE_ASKED() (usb.ota_req)        /* the updater's "upgrade" command arrived */
#endif

#define PANELMSG_H2 0x50u
#define PANELMSG_H3 0x4Du
#define PANELMSG_PROTO 1u
#define PANELMSG_HELLO 1u                            /* computer -> FM-1 */
#define PANELMSG_BYE 2u
#define PANELMSG_EVENTS 16u                          /* FM-1 -> computer */
#define PANELMSG_HELD 17u
#define PANELMSG_IDENTITY 18u
#define PANELMSG_ALIVE_MS 5000u
#define PANELMSG_MIN_MS 8u                           /* at most 125 frames a second, always */
#define PANELMSG_PER_FRAME 16u
#ifndef PANELMSG_RESERVE
#define PANELMSG_RESERVE 24u                         /* the largest message anyone else sends, in packets (Dinghy: 17) */
#endif
#define PANELMSG_FLAG_LISTENING 1u
#define PANELMSG_NAME_MAX 23u
#define PANELMSG_VER_MAX 15u
#define PANELMSG_HEAD (1u + PANELMSG_IDN + 2u)            /* F0 id 50 4D */
#define PANELMSG_EV_BYTES (PANELMSG_HEAD + 5u + 3u * PANELMSG_PER_FRAME + 1u)   /* cmd seq2 t2, 3 per event, F7: 60 */
#define PANELMSG_ID_BYTES (PANELMSG_HEAD + 3u + (PANELMSG_NAME_MAX + 1u) + 2u * (PANELMSG_VER_MAX + 1u) + 1u)   /* cmd proto flags, 3 strings, F7: 66 */
#define PANELMSG_MAX_BYTES (PANELMSG_ID_BYTES > PANELMSG_EV_BYTES ? PANELMSG_ID_BYTES : PANELMSG_EV_BYTES)
#define PANELMSG_USB_UP() (*(volatile uint8_t *)&usb.config)
_Static_assert((PANELMSG_MAX_BYTES + 2u) / 3u + 1u + PANELMSG_RESERVE <= SXQ, "one frame of this protocol + the largest other message must fit");

static uint32_t panelmsg_heard, panelmsg_sent_ms, panelmsg_seq, panelmsg_held_lost, panelmsg_held_b, panelmsg_held_k;
static uint8_t panelmsg_hello_owed, panelmsg_held_owed;
static uint8_t panelmsg_out[PANELMSG_MAX_BYTES];

#ifndef PANELMSG_WIRE_SUPPLIED
static int panelmsg_wire_idle(void)                  /* USB up and the SysEx out ring empty (only the USB interrupt drains it) */
{
    return PANELMSG_USB_UP() && so_w == so_r;
}
static int panelmsg_wire_send(const uint8_t *p, uint32_t n) { return ota_wire_send(p, n); }
#endif
static int panelmsg_send(const uint8_t *p, uint32_t n)   /* whole frame or nothing; never waits, never queues behind */
{
    if (n > PANELMSG_MAX_BYTES || !panelmsg_wire_idle())  /* (it fits: the _Static_assert above) */
        return -1;
    return panelmsg_wire_send(p, n);
}
static uint32_t panelmsg_str(uint8_t *o, uint32_t i, uint32_t max, const char *s)
{
    uint32_t k;
    for (k = 0; s[k] && k < max; k++)
        o[i++] = (uint8_t)(s[k] & 0x7F);
    o[i++] = 0;
    return i;
}
static uint32_t panelmsg_prefix(uint8_t *o, uint32_t cmd)   /* F0 <id> 50 4D <cmd> */
{
    uint32_t i = 0, k;
    o[i++] = 0xF0;
    for (k = 0; k < PANELMSG_IDN; k++)
        o[i++] = panelmsg_id[k];
    o[i++] = PANELMSG_H2;
    o[i++] = PANELMSG_H3;
    o[i++] = (uint8_t)cmd;
    return i;
}
static uint32_t panelmsg_head(uint8_t *o, uint32_t cmd, uint32_t now)   /* F0 id 50 4D cmd seq2 t2 */
{
    uint32_t i = panelmsg_prefix(o, cmd);
    o[i++] = (uint8_t)(panelmsg_seq & 0x7Fu); o[i++] = (uint8_t)((panelmsg_seq >> 7) & 0x7Fu);
    o[i++] = (uint8_t)(now & 0x7Fu); o[i++] = (uint8_t)((now >> 7) & 0x7Fu);
    return i;
}
static int panelmsg_identity(void)
{
    uint32_t i = panelmsg_prefix(panelmsg_out, PANELMSG_IDENTITY);
    panelmsg_out[i++] = PANELMSG_PROTO;
    panelmsg_out[i++] = panelmsg_on ? PANELMSG_FLAG_LISTENING : 0u;
    i = panelmsg_str(panelmsg_out, i, PANELMSG_NAME_MAX, PANELMSG_FW_NAME);
    i = panelmsg_str(panelmsg_out, i, PANELMSG_VER_MAX, PANELMSG_FW_VERSION);
    i = panelmsg_str(panelmsg_out, i, PANELMSG_VER_MAX, PANELMSG_PLATFORM);
    panelmsg_out[i++] = 0xF7;
    return panelmsg_send(panelmsg_out, i);
}
/* "what is held right now", placed exactly where it belongs in the stream: every entry queued before this moment goes
 * out first, then the HELD frame, then what came after. Under the lock, so the snapshot, the seal and the loss count
 * agree. dropped = entries this side lost itself (a frame the USB swallowed). */
static void panelmsg_mark(uint32_t dropped)
{
    uint32_t k = fm1__lock();
    panelmsg_held_lost += dropped + panelmsg_lost;
    panelmsg_lost = 0;
    panelmsg_ev_seal = panelmsg_ev_w;
    panelmsg_held_b = (uint32_t)PANELMSG_HELD_BUTTONS();
    panelmsg_held_k = (uint32_t)PANELMSG_HELD_KEYS();
    fm1__unlock(k);
    panelmsg_held_owed = 1;
}
static void panelmsg_stop(void)
{
    panelmsg_on = 0;
    panelmsg_hello_owed = panelmsg_held_owed = 0;
}
/* a received SysEx (the bytes between F0 and F7): 1 if it carries this protocol's header (handled; the caller releases
 * it). Only HELLO and BYE are acted on: anything else with the header (another FM-1's frames, or this one's own output
 * looped back by a MIDI route) is taken and ignored. */
static int panelmsg_frame(const uint8_t *p, uint32_t n)
{
    uint32_t cmd, k;
    if (n < PANELMSG_IDN + 3u || p[PANELMSG_IDN] != PANELMSG_H2 || p[PANELMSG_IDN + 1u] != PANELMSG_H3)
        return 0;
    for (k = 0; k < PANELMSG_IDN; k++)
        if (p[k] != panelmsg_id[k])
            return 0;
    cmd = p[PANELMSG_IDN + 2u];
    if (cmd == PANELMSG_HELLO && (n < PANELMSG_IDN + 4u || p[PANELMSG_IDN + 3u] < 1u))
        cmd = 0;                                /* a HELLO without a version: ignored (everything read from p here) */
    if (cmd == PANELMSG_HELLO) {
        if (!panelmsg_on && PANELMSG_ALLOWED()) {         /* a new session, in ONE locked step: nothing stale, held state first */
            k = fm1__lock();
            panelmsg_ev_r = panelmsg_ev_w;
            panelmsg_ev_seal = panelmsg_ev_w;
            panelmsg_lost = 0;
            panelmsg_held_b = (uint32_t)PANELMSG_HELD_BUTTONS();
            panelmsg_held_k = (uint32_t)PANELMSG_HELD_KEYS();
            panelmsg_on = 1;
            fm1__unlock(k);
            panelmsg_seq = 0;
            panelmsg_held_lost = 0;
            panelmsg_held_owed = 1;
        }
        panelmsg_heard = fm1_ms;
        panelmsg_hello_owed = 1;                     /* IDENTITY answers, listening or switched off */
    } else if (cmd == PANELMSG_BYE) {
        panelmsg_stop();
    }
    return 1;
}
#ifndef PANELMSG_FRAMES_ROUTED
/* 1 = a frame is waiting that is not ours (left alone for the updater and the editor) */
static int panelmsg_take_frame(void)
{
    const uint8_t *p;
    uint32_t n;
    if (!ota_frame_get(&p, &n))
        return 0;
    if (!panelmsg_frame(p, n))
        return 1;
    ota_frame_done();
    return 0;
}
#endif
static void panelmsg_poll(void)
{
    uint32_t now, i, k, n, end;
#ifndef PANELMSG_FRAMES_ROUTED
    if (panelmsg_take_frame())
        return;                                 /* someone else's frame first: send nothing this pass */
#endif
    if (!panelmsg_on && !panelmsg_hello_owed)
        return;
    now = fm1_ms;                               /* (read once) */
    if (PANELMSG_UPDATE_ASKED() || !PANELMSG_USB_UP() || (panelmsg_on && (!PANELMSG_ALLOWED() || (uint32_t)(now - panelmsg_heard) > PANELMSG_ALIVE_MS))) {
        panelmsg_stop();                             /* an update, a lost USB, switched off, or a computer that went away */
        return;
    }
    if (panelmsg_lost)
        panelmsg_mark(0);                            /* the queue overflowed: say what is held after it, never guess */
    if ((uint32_t)(now - panelmsg_sent_ms) < PANELMSG_MIN_MS)
        return;                                 /* at most 125 frames a second, IDENTITY answers included */
    if (panelmsg_hello_owed) {
        if (panelmsg_identity())
            return;                             /* the ring is busy: next poll */
        panelmsg_hello_owed = 0;
        panelmsg_sent_ms = now;
        return;
    }
    if (panelmsg_held_owed && panelmsg_ev_r == panelmsg_ev_seal) {
        i = panelmsg_head(panelmsg_out, PANELMSG_HELD, now);
        n = panelmsg_held_lost > 0x3FFFu ? 0x3FFFu : panelmsg_held_lost;   /* (16383 means "at least") */
        panelmsg_out[i++] = (uint8_t)(n & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)((n >> 7) & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)(panelmsg_held_b & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)((panelmsg_held_b >> 7) & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)(panelmsg_held_k & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)((panelmsg_held_k >> 7) & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)((panelmsg_held_k >> 14) & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)((panelmsg_held_k >> 21) & 0x7Fu);
        panelmsg_out[i++] = 0xF7;
        if (panelmsg_send(panelmsg_out, i))
            return;                             /* no room: the snapshot stays valid, the queue waits behind it */
        panelmsg_held_owed = 0;
        panelmsg_held_lost = 0;
        panelmsg_seq++;
        panelmsg_sent_ms = now;
        return;
    }
    if (panelmsg_ev_w == panelmsg_ev_r)
        return;
    if (!panelmsg_wire_idle())
        return;                                 /* wait for an empty ring before taking anything: the ISR keeps queueing */
    i = panelmsg_head(panelmsg_out, PANELMSG_EVENTS, now);
    k = fm1__lock();                            /* (the ISR may still merge a detent into the newest entry) */
    end = panelmsg_held_owed ? panelmsg_ev_seal : panelmsg_ev_w;   /* a HELD frame owed: only what came before it */
    for (n = 0; panelmsg_ev_r != end && n < PANELMSG_PER_FRAME; n++) {
        uint32_t v = panelmsg_ev[panelmsg_ev_r % PANELMSG_RING];
        panelmsg_out[i++] = (uint8_t)(v >> 14);
        panelmsg_out[i++] = (uint8_t)(v & 0x7Fu);
        panelmsg_out[i++] = (uint8_t)((v >> 7) & 0x7Fu);
        panelmsg_ev_r++;
    }
    fm1__unlock(k);
    if (!n)
        return;
    panelmsg_out[i++] = 0xF7;
    if (panelmsg_send(panelmsg_out, i)) {                 /* (the USB went away between the check and here) */
        panelmsg_mark(n);                            /* what this frame held is counted, never hidden */
        return;
    }
    panelmsg_seq++;
    panelmsg_sent_ms = now;
}
#endif
