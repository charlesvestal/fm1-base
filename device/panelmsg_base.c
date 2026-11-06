/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Bitbazooka LLC (Les, les.audio) */
/* Panel messages on fm1-base (fm1.toml panel_messages = true: FM1B_PANELMSG). The module is device/panelmsg.h and panelmsg.c (MIT OR
 * MPL-2.0, the same two files any FM-1 firmware can carry; the protocol is an open spec any app can read, Les is one
 * reader): silent until an app on the computer says HELLO, then it reports the panel as played (presses, releases,
 * turns, in order; what is held) and the firmware's name and version, over USB only. This file gives it the platform:
 *   panelmsg_tick()       the scan's tick (main_fm1.c's TIMER5 ISR calls it instead of fm1_input_tick): what one tick changed
 *                    (the debounced buttons and keys, the encoders' steps) is read around fm1_input_tick, so it works
 *                    the same whether felucca-bsp's fm1_input.h has the two input hooks or not. Nobody listening: one
 *                    byte read a tick
 *   panelmsg_frame(p, n)  a received SysEx: main_fm1.c's chain hands it over beside the screen reader's (ann_frame)
 *   panelmsg_poll()       the main loop, after ota_service: a frame only into an empty SysEx ring (panelmsg_wire_send)
 *   panelmsg_stop()       the update asked for: quiet until the next HELLO
 * Ids are the protocol's (Felucca's wiring: 0 OCT-, 1 OCT+, 2 FX .. 13 REC; keys 14..40; encoders 48 SELECT ..
 * 54 PRESETS), through the panel's map (panel.c): a panel taught by HARDWARE CALIBRATION reports its printed labels
 * as an untaught one does, clockwise +. Never in safe mode, never in the update session, no flash written. */
/* One input path, on purpose: fm1_input.h is compiled before this file, so if felucca-bsp's HAL carries the two input
 * hooks they are its empty defaults here and report nothing; panelmsg_tick stays the only reader (never twice). The module's
 * header refuses to come after a HAL with hooks (its own check), so they are dropped here first. */
#undef FM1_INPUT_EDGE
#undef FM1_INPUT_STEP
#undef FM1_INPUT_HOOKED
#include "panelmsg.h"
#define PANELMSG_FW_NAME APP_NAME
#define PANELMSG_FW_VERSION APP_VERSION
#define PANELMSG_PLATFORM "fm1-base"
#define PANELMSG_RESERVE ((ANN_MAX + 11u) / 3u)       /* the largest other frame, in packets: a screen reader's whole SAY */
#define PANELMSG_WIRE_SUPPLIED 1
#define PANELMSG_FRAMES_ROUTED 1
#define PANELMSG_HELD_BUTTONS() panelmsg_buttons(fm1_in.buttons)

static uint32_t panelmsg_button(uint32_t m)           /* matrix button m: the protocol's id (its label's on Felucca's wiring) */
{
    uint32_t i;
    for (i = 0; i < NB; i++)
        if (panel.btn[i] == m)
            return PANEL_DEFAULT.btn[i];
    return m;
}
static uint32_t panelmsg_buttons(uint32_t raw)        /* the matrix's buttons held: the protocol's bits */
{
    uint32_t out = 0, i;
    for (i = 0; i < NB; i++)
        if ((raw >> panel.btn[i]) & 1u)
            out |= 1u << PANEL_DEFAULT.btn[i];
    return out;
}
static int panelmsg_wire_idle(void)                   /* the SysEx ring empty and no one writing to it, no backup streaming */
{
    return usb.config && !usb.suspended && !backup_streaming() && !sx_busy && so_w == so_r;
}
static int panelmsg_wire_send(const uint8_t *p, uint32_t n)   /* never waits: idle, or not sent */
{
    if (!panelmsg_wire_idle())
        return -1;
    return ota_wire_send(p, n);
}
#include "panelmsg.c"

static void panelmsg_tick(void)
{
    uint32_t b, k, d, i, e;
    int16_t st[FM1_NENC];
    if (!panelmsg_on) {
        fm1_input_tick();
        return;
    }
    b = fm1_in.buttons;
    k = fm1_in.notes;
    for (e = 0; e < FM1_NENC; e++)
        st[e] = fm1_in.enc_steps[e];
    fm1_input_tick();                            /* one column's keys; at a frame's end, the encoders */
    for (d = b ^ fm1_in.buttons, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            panelmsg_isr_edge(panelmsg_button(i), (fm1_in.buttons >> i) & 1u);
    for (d = k ^ fm1_in.notes, i = 0; d; d >>= 1, i++)
        if (d & 1u)
            panelmsg_isr_edge(14u + i, (fm1_in.notes >> i) & 1u);
    for (e = 0; e < FM1_NENC; e++)
        if (fm1_in.enc_steps[e] != st[e])        /* (the main loop takes steps under the lock: not inside this ISR) */
            for (i = 0; i < NE; i++)
                if (panel.enc[i] == e)
                    panelmsg_isr_step(PANEL_DEFAULT.enc[i], (fm1_in.enc_steps[e] - st[e]) * panel.dir[i]);
}
