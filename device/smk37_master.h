/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Kerem Kilic (Ellic Studio) */
/* The SMK-37's virtual MASTER (PLAT_SMK37): K5's detents on a level 0..4096, SMK_MASTER_STEP a detent, kept in the
 * panel calibration object (OBJ_PANEL) SMK_MASTER_SAVE_MS after the last turn, so it survives power-off;
 * plat_master_set lets an app put its own saved level there at start. Included by plat_smk37.c; on its own it needs
 * fm1_ms, clamp, panel (.master), plat_store_save, OBJ_PANEL, master_pos, mirror_in_master and remote (.master), so
 * tests/smk37_master_test.c can drive it.
 *
 * The save waits for a quiet moment: a store write holds the interrupts for a while (the key scan and the sound wait),
 * so it comes once, after the turning, never during it. */

#define SMK_MASTER_STEP 96                         /* a detent of K5 */
#define SMK_MASTER_SAVE_MS 2000u

static int32_t smk_master = 2896;                  /* half gain, until the panel object or the app says */
static uint32_t smk_master_t;                      /* the last turn (fm1_ms) */
static uint8_t smk_master_due;                     /* 1: a level to save SMK_MASTER_SAVE_MS after smk_master_t */
static void smk_master_show(void)
{
    master_pos = mirror_in_master(remote.master >= 0 ? (uint32_t)remote.master : (uint32_t)smk_master);
}
void plat_master_set(uint32_t q12)
{
    smk_master = (int32_t)(q12 > 4096u ? 4096u : q12);
    smk_master_show();
    smk_master_t = fm1_ms;                         /* kept like a turn of K5 (the save skips a level already there) */
    smk_master_due = 1;
}
static void smk_master_load(void)                  /* after panel_init: the level saved with the calibration */
{
    if (panel.master <= 4096u)
        smk_master = (int32_t)panel.master;
    master_pos = (uint32_t)smk_master;
}
static void smk_master_poll(int32_t steps)         /* the main loop: K5's detents since the last pass */
{
    if (steps) {
        smk_master = clamp(smk_master + steps * SMK_MASTER_STEP, 0, 4096);
        smk_master_t = fm1_ms;
        smk_master_due = 1;
    }
    smk_master_show();
    if (smk_master_due && fm1_ms - smk_master_t >= SMK_MASTER_SAVE_MS) {
        smk_master_due = 0;
        if (panel.master != (uint32_t)smk_master) {
            panel.master = (uint32_t)smk_master;
            plat_store_save(OBJ_PANEL, &panel, sizeof panel);
        }
    }
}
