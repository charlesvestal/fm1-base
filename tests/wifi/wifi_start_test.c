/* SPDX-License-Identifier: GPL-3.0-only OR MPL-2.0 */
/* Copyright (C) 2026 Henri David; in fm1-base: tests/wifi/run.sh */
/* WiFi's start on the radio Bluetooth shares (device/wifi_own.c own_radio_up), on the host: the Bluetooth side is a
 * stand-in with device/bt_own.c's rules, line for line (plat_ble_state reads BLE_OFF while the player has Bluetooth
 * switched off; plat_ble_enable(0) leaves a started radio up and quiet; plat_ble_start does nothing once started;
 * plat_ble_radio is the radio's own state). The bug (2026-10-31, the user's FM-1, the release): Bluetooth switched
 * off after the boot (BT MIDI OFF, or BT AUDIO on with BT MIDI OFF: ui/opt_bt.h opt_bta_go), then WiFi turned on from
 * OFF: own_radio_up read plat_ble_state, BLE_OFF for ever, and WiFi stayed in WC_RADIO (the page: STARTING).
 * Covers: WiFi at boot with Bluetooth on; started from OFF after Bluetooth was switched off (MAN, AUTO, MODE AP), the
 * switch left as the player set it (no advertising again); Bluetooth never started (WiFi starts it); and the radio
 * failing (FAIL, "radio did not start"). Exit status: the number of failed checks. */
#include <stdio.h>
#include <string.h>
#include "plat.h"

/* ---- the platform, as wifi_own.c uses it */
static uint32_t ms;
uint32_t plat_ms(void) { return ms; }
uint32_t plat_ticks24(void) { return ms * 24000u; }
const uint8_t *plat_chip_uid(void) { return 0; }
enum { OBJ_WIFI = 2 };
int plat_store_load(uint32_t obj, void *dst, uint32_t max) { (void)obj, (void)dst, (void)max; return -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { (void)obj, (void)src, (void)len; return 0; }

/* ---- Bluetooth, with device/bt_own.c's rules (its ble_state, ble_off, ble_start, plat_ble_enable) */
static int bt_state = BLE_OFF, bt_off, bt_starts, bt_adv, bt_fail;
static void bt_start(void)
{
    if (bt_state != BLE_OFF)
        return;
    bt_starts++;
    bt_state = bt_fail ? BLE_FAILED : BLE_READY;  /* (bt_own.c's start is synchronous) */
    bt_adv = bt_state == BLE_READY && !bt_off;    /* (ble_enable(!ble_off)) */
}
int plat_ble_state(void) { return bt_off ? BLE_OFF : bt_state; }
int plat_ble_radio(void) { return bt_state; }
void plat_ble_start(void) { bt_start(); }
static void bt_enable(int on)                     /* (plat_ble_enable: the app's, not wifi_own.c's) */
{
    bt_off = !on;
    if (on && bt_state == BLE_OFF)
        bt_start();
    else if (bt_state == BLE_READY)
        bt_adv = on;
}
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
static uint32_t sent;
static int count_tx(const uint8_t *frame, uint32_t len, uint32_t wcid, uint32_t ack)
{
    (void)frame, (void)len, (void)wcid, (void)ack;
    sent++;
    return 0;
}
static void run(uint32_t n_ms)                    /* the main loop: a pass every millisecond */
{
    uint32_t i;
    for (i = 0; i < n_ms; i++) {
        ms++;
        wifi_poll();
    }
}
static void reboot(void)                          /* a power-on: WiFi's state and the radio as at boot (the record kept) */
{
    plat_wifi_mode(WIFI_MODE_OFF);
    wifi_poll();
    memset(&wc, 0, sizeof wc);
    fm1_fake_up = 0;
    bt_state = BLE_OFF, bt_off = bt_starts = bt_adv = bt_fail = 0;
    ms = 0;
}

int main(void)
{
    fm1_fake_tx = count_tx;

    /* 1. at boot: Bluetooth on (started at power-on), the record's MODE MAN: up after WC_RADIO_MS */
    reboot();
    bt_start();
    plat_wifi_mode(WIFI_MODE_MAN);
    run(WC_RADIO_MS - 10u);
    check(plat_wifi_step() == WIFI_STEP_RADIO, "at boot: waits for WC_RADIO_MS");
    run(50u);
    check(plat_wifi_step() == WIFI_STEP_IDLE && fm1_fake_up, "at boot, Bluetooth on: up (IDLE)");

    /* 2. the bug: Bluetooth on at boot, switched off at 20 s (BT AUDIO with BT MIDI OFF), WiFi OFF since boot, MAN at 60 s */
    reboot();
    bt_start();
    run(20000u);
    bt_enable(0);
    check(plat_ble_state() == BLE_OFF && plat_ble_radio() == BLE_READY && !bt_adv, "Bluetooth switched off: the radio up, quiet");
    run(40000u);
    check(plat_wifi_step() == WIFI_STEP_OFF && !fm1_fake_up, "MODE OFF since boot: nothing started");
    plat_wifi_mode(WIFI_MODE_MAN);
    run(100u);
    check(plat_wifi_step() == WIFI_STEP_IDLE, "started from OFF with Bluetooth switched off: up (IDLE), not STARTING");
    check(plat_wifi_state() == WIFI_ON && fm1_fake_up, "the WiFi side up (WIFI_ON)");
    check(plat_ble_state() == BLE_OFF && !bt_adv && bt_starts == 1, "Bluetooth left switched off, not advertising, not restarted");
    run(10000u);
    check(plat_wifi_step() == WIFI_STEP_IDLE, "still up 10 s later");

    /* 3. the same with MODE AUTO (no network kept: it rests in IDLE) */
    reboot();
    bt_start();
    run(5000u);
    bt_enable(0);
    run(30000u);
    plat_wifi_mode(WIFI_MODE_AUTO);
    run(100u);
    check(plat_wifi_step() == WIFI_STEP_IDLE, "AUTO from OFF with Bluetooth switched off: up");

    /* 4. Bluetooth never started (BT MIDI OFF at boot, no audio): WiFi starts the radio */
    reboot();
    run(30000u);
    check(bt_starts == 0, "nothing started the radio");
    plat_wifi_mode(WIFI_MODE_MAN);
    run(100u);
    check(bt_starts == 1 && plat_ble_radio() == BLE_READY, "WiFi started the radio once");
    check(plat_wifi_step() == WIFI_STEP_IDLE, "and is up (IDLE)");

    /* 5. switched off before it was ever started (the last Bluetooth run crashed: no start at boot) */
    reboot();
    bt_enable(0);
    run(30000u);
    plat_wifi_mode(WIFI_MODE_MAN);
    run(100u);
    check(bt_starts == 1 && plat_wifi_step() == WIFI_STEP_IDLE, "switched off, never started: WiFi starts the radio, up");
    check(plat_ble_state() == BLE_OFF && !bt_adv, ".. Bluetooth still switched off, not advertising");

    /* 6. the radio's start fails: FAIL, "radio did not start", not STARTING for ever */
    reboot();
    bt_fail = 1;
    plat_wifi_mode(WIFI_MODE_MAN);
    run(WC_RADIO_MS + 100u);
    check(plat_wifi_step() == WIFI_STEP_FAIL && plat_wifi_fail() == WIFI_FAIL_RADIO, "the radio failed: FAIL (radio did not start)");

#if FM1B_WIFI_AP
    /* 7. MODE AP from OFF, Bluetooth switched off: the access point comes up (the same own_radio_up) */
    reboot();
    bt_start();
    run(5000u);
    bt_enable(0);
    run(30000u);
    plat_wifi_mode(WIFI_MODE_AP);
    run(2000u);
    check(plat_wifi_ap_state() == WIFI_AP_UP, "MODE AP from OFF with Bluetooth switched off: up");
    check(plat_ble_state() == BLE_OFF && !bt_adv, "Bluetooth left switched off");
    plat_wifi_mode(WIFI_MODE_OFF);
    run(100u);
#endif

    printf("wifi_start: %d of %d checks failed\n", fails, checks);
    return fails;
}
