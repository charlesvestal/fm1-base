/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Charles Vestal */
/* Bluetooth MIDI devices on the own stack (fm1.toml ble_central = true with ble_stack = "own"): the FM-1 scans for
 * BLE-MIDI peripherals and connects to one as the central (bt/ble: ble_ll_scan, ble_scan.c's table, ble_central.c;
 * docs/bt/BLE-STACK.md §9). One link at a time: while connected to a device, a computer cannot connect to the FM-1
 * (JieLi's build keeps both). Its MIDI comes in as PORT_BLE from TO_BLE_DEV (cable 1, bt_own.c ble_app_midi_in).
 * Part of bt_own.c. */
static struct ble_scan_tab own_scan;
static uint32_t own_scan_aged;

#if BLE_CEN_PAIR
BLE_API void ble_app_central_keys(const struct ble_keys *k)
{
    (void)k;                                      /* (a device's bond is not kept yet: it pairs again next time) */
}
#endif

static void own_central_poll(void)                /* ble_poll: the scan's reports into the table */
{
    uint8_t pdu[40];
    uint16_t rssi;
    uint8_t n;
    while ((n = ble_ll_scan_take(pdu, &rssi)) != 0)
        ble_scan_add(&own_scan, pdu, n, rssi, shim_ms());
    if (shim_ms() - own_scan_aged > 1000u) {
        own_scan_aged = shim_ms();
        ble_scan_age(&own_scan, own_scan_aged);
    }
}

void plat_ble_scan(int on)
{
    if (ble_state != BLE_READY)
        return;
    if (on)
        ble_scan_clear(&own_scan);
    own_held(1);
    ble_ll_scan(on);
    own_held(0);
}
int plat_ble_found(int i, char *name, int *rssi)
{
    uint8_t idx[BLE_SCAN_N];
    uint32_t n = ble_state == BLE_READY ? ble_scan_list(&own_scan, idx) : 0u, k;
    const struct ble_found *e;
    if (i < 0 || (uint32_t)i >= n)
        return 0;
    e = &own_scan.e[idx[i]];
    if (name) {
        for (k = 0; k < 20u && e->name[k]; k++)
            name[k] = e->name[k];
        name[k] = 0;
    }
    if (rssi)                                     /* (no gain table: the list's bars, as rough dBm) */
        *rssi = -100 + 20 * (int)ble_scan_bars(&own_scan, idx[i]);
    return 1;
}
int plat_ble_connect(int i)
{
    uint8_t idx[BLE_SCAN_N];
    uint32_t n = ble_state == BLE_READY ? ble_scan_list(&own_scan, idx) : 0u;
    struct ble_peer p;
    int ok;
    if (i < 0 || (uint32_t)i >= n)
        return -1;
    ble_zero((uint8_t *)&p, sizeof p);
    ble_cpy(p.addr, own_scan.e[idx[i]].addr, 6);
    p.addr_rand = own_scan.e[idx[i]].addr_rand;
    own_held(1);
    ok = ble_central_connect(&p);
    own_held(0);
    if (ok)
        shim_log("device: connecting");
    return ok ? 0 : -1;
}
void plat_ble_disconnect(void)
{
    if (ble_state != BLE_READY)
        return;
    own_held(1);
    ble_central_cancel();
    own_held(0);
}
int plat_ble_device(void)
{
    if (ble_state != BLE_READY)
        return BLE_DEV_OFF;
    switch (ble_central_state()) {
    case BLE_CS_CONNECTING:
        return BLE_DEV_CONNECTING;
    case BLE_CS_SETUP:
        return BLE_DEV_SEARCHING;
    case BLE_CS_READY:
        return BLE_DEV_READY;
    default:
        return ble_ll_scanning() ? BLE_DEV_SCANNING : BLE_DEV_OFF;
    }
}
