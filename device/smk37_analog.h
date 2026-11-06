/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The SMK-37's wheels and sustain pedal (PLAT_SMK37): plat_wheel and plat_pedal from felucca-bsp's raw readings.
 * Included by plat_smk37.c; on its own it needs fm1_in (frames, wheel[2], pedal_raw), fm1_ms, clamp and
 * PLAT_WHEEL_*, so tests/smk37_analog_test.c can drive it with readings of its own.
 *
 * Nothing is read until the analog readings settle (SMK_SETTLE_FRAMES scan frames); the first look after that is
 * the rest: the pitch wheel's centre and the pedal jack's level. The main loop starts after the settling window
 * (the boot's own delays are most of it), so a rest taken during the window would never be taken at all.
 * The pitch wheel's centre then follows a slow drift while it rests; each wheel's range grows to what it has reached
 * (the travel is short). The pedal is down when the jack reads SMK_PEDAL_STEP away from its rest; held down for
 * SMK_PEDAL_RELEARN_MS, it is a pedal plugged in after power-on, and that reading is the new rest. */

#define SMK_SETTLE_FRAMES 300u                     /* ~0.5 s of scan frames: the analog readings settle before use */
#define SMK_WHEEL_DEAD 12                          /* raw counts around the pitch wheel's centre that read 0 */
#define SMK_WHEEL_SPAN0 80                         /* its range to start with; each end grows to what it has reached */
#define SMK_MOD_LO 485                             /* the mod wheel's raw range to start with (up = lower raw); it grows too */
#define SMK_MOD_HI 615
#define SMK_PEDAL_STEP 200                         /* the jack's reading this far from its rest: the pedal is down */
#define SMK_PEDAL_RELEARN_MS 60000u                /* down this long: a pedal plugged in after power-on, the new rest */

static int32_t smk_bend, smk_mod, smk_pc, smk_plo, smk_phi, smk_mlo, smk_mhi, smk_rest_n, smk_pedal_rest;
static uint8_t smk_pedal_down;
static uint32_t smk_pedal_t;
static void smk_analog_poll(void)                  /* from the main loop */
{
    int32_t p = fm1_in.wheel[0], m = fm1_in.wheel[1], r = fm1_in.pedal_raw, d;
    if (fm1_in.frames < SMK_SETTLE_FRAMES)
        return;
    if (!smk_pc) {
        smk_pedal_rest = r;
        smk_pc = p;
        smk_plo = p - SMK_WHEEL_SPAN0;
        smk_phi = p + SMK_WHEEL_SPAN0;
        smk_mlo = m < SMK_MOD_LO ? m : SMK_MOD_LO;
        smk_mhi = m > SMK_MOD_HI ? m : SMK_MOD_HI;
    }
    smk_plo = p < smk_plo ? p : smk_plo;
    smk_phi = p > smk_phi ? p : smk_phi;
    smk_mlo = m < smk_mlo ? m : smk_mlo;
    smk_mhi = m > smk_mhi ? m : smk_mhi;
    d = p - smk_pc;
    if (d > -SMK_WHEEL_DEAD && d < SMK_WHEEL_DEAD) {
        smk_bend = 0;
        if (++smk_rest_n > 200) {
            smk_pc += d > 0 ? 1 : d < 0 ? -1 : 0;
            smk_rest_n = 0;
        }
    } else {
        int32_t up = smk_phi - smk_pc - SMK_WHEEL_DEAD, dn = smk_pc - smk_plo - SMK_WHEEL_DEAD;
        smk_rest_n = 0;
        smk_bend = clamp(d > 0 ? (d - SMK_WHEEL_DEAD) * 8191 / (up > 1 ? up : 1) : (d + SMK_WHEEL_DEAD) * 8192 / (dn > 1 ? dn : 1),
                         -8192, 8191);
    }
    d = smk_mhi - smk_mlo - SMK_WHEEL_DEAD;
    smk_mod = smk_mhi - m <= SMK_WHEEL_DEAD ? 0 : clamp((smk_mhi - m - SMK_WHEEL_DEAD) * 127 / (d > 1 ? d : 1), 0, 127);
    {
        int32_t o = r > smk_pedal_rest ? r - smk_pedal_rest : smk_pedal_rest - r;
        uint32_t down = o > SMK_PEDAL_STEP;
        if (down && smk_pedal_down && fm1_ms - smk_pedal_t > SMK_PEDAL_RELEARN_MS) {
            smk_pedal_rest = r;
            down = 0;
        }
        if (down != smk_pedal_down) {
            smk_pedal_down = (uint8_t)down;
            smk_pedal_t = fm1_ms;
        }
    }
}
int plat_wheel(int w) { return w == PLAT_WHEEL_PITCH ? smk_bend : w == PLAT_WHEEL_MOD ? smk_mod : 0; }
int plat_pedal(void) { return smk_pedal_down; }
