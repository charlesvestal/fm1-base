/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* R6: metadata only. Included inside ble_security.inc's diagnostics guard.
 * Writers run with LL exclusion. Keep the first failure until power-off;
 * session/security Reset must never clear this evidence. */
static struct {
    ble_radio_event_t events[BLE_RADIO_EVENTS];
    uint32_t serial;
    uint8_t frozen,reason;
} radio_diag __attribute__((section(".pool.ble")));
void ble_radio_event(uint16_t event,int closed)
{
    ble_radio_event_t *e=&radio_diag.events[radio_diag.serial%BLE_RADIO_EVENTS];
    if(radio_diag.frozen) return;
    /* Silent supervision-wait events must not evict the final exchange.
     * Only events with descriptor activity enter the ring. */
    if(closed && (!radio_diag.serial || e->epoch!=ble_link.epoch || e->event!=event)) return;
    if(!radio_diag.serial || e->epoch!=ble_link.epoch || e->event!=event) {
        uint32_t serial=++radio_diag.serial;
        e=&radio_diag.events[serial%BLE_RADIO_EVENTS];
        e->serial=serial; e->epoch=ble_link.epoch; e->event=event;
        e->count=e->overflow=0; e->closed=0;
    }
    if(closed) e->closed=1;
}
static uint64_t radio_add(uint8_t kind,uint16_t event,uint16_t header,uint16_t status,
                          uint8_t slot,uint8_t id,uint8_t flags,uint8_t decision,uint64_t counter)
{
    ble_radio_event_t *e; ble_radio_row_t *r; unsigned index;
    if(radio_diag.frozen) return 0;
    ble_radio_event(event,0); e=&radio_diag.events[radio_diag.serial%BLE_RADIO_EVENTS];
    if(e->count==BLE_RADIO_ROWS) { if(e->overflow!=65535u) e->overflow++; return 0; }
    index=e->count++; r=&e->row[index];
    r->counter=counter; r->ticks=ble_port_ticks(); r->header=header; r->status=status;
    r->kind=kind; r->slot=slot; r->id=id; r->decision=decision; r->flags=flags;
    return (uint64_t)e->serial*BLE_RADIO_ROWS+index;
}
uint64_t ble_radio_rx(uint16_t event,uint16_t status,uint16_t header,uint8_t slot,uint8_t flags)
{
    unsigned cls=ble_rx_class(status,(uint8_t)(header>>8));
    uint8_t decision=cls==0 ? BLE_RADIO_ACCEPT : cls==1 ? BLE_RADIO_CRC :
        cls==2 ? BLE_RADIO_DUPLICATE : BLE_RADIO_OTHER;
    return radio_add(BLE_RADIO_RX,event,header,status,slot,255,flags,decision,BLE_RADIO_NO_COUNTER);
}
static void radio_auth(uint64_t id,uint64_t counter,int ok)
{
    uint32_t serial=(uint32_t)(id/BLE_RADIO_ROWS);
    ble_radio_event_t *e=&radio_diag.events[serial%BLE_RADIO_EVENTS];
    if(radio_diag.frozen || !id || e->serial!=serial || id%BLE_RADIO_ROWS>=e->count) return;
    ble_radio_row_t *r=&e->row[id%BLE_RADIO_ROWS];
    r->counter=counter; r->decision=ok ? BLE_RADIO_AUTH_OK : BLE_RADIO_AUTH_BAD;
}
void ble_radio_tx(uint8_t kind,uint16_t event,unsigned slot,int id,uint16_t header,uint16_t status)
{
    uint64_t counter=BLE_RADIO_NO_COUNTER;
    if(kind!=BLE_RADIO_PREP && id>=0 && (unsigned)id<BLE_TX_COUNT) counter=queues.tx[id].radio_counter;
    /* Descriptor TX SN/MD are bits 2/3; NESN is not exposed here. Preserve
     * the raw word, rather than manufacturing an on-air header. */
    (void)radio_add(kind,event,header,status,(uint8_t)slot,(uint8_t)(id<0 ? 255 : id),0,0,counter);
}
void ble_radio_freeze(uint8_t reason)
{
    if(radio_diag.frozen || (reason!=0x3d && reason!=8)) return;
    radio_diag.frozen=1; radio_diag.reason=reason;
}
void ble_radio_summary(ble_radio_summary_t *out)
{
    uint32_t key=ble_port_lock();
    out->last=radio_diag.serial;
    out->first=out->last>=BLE_RADIO_EVENTS ? out->last-BLE_RADIO_EVENTS+1u : out->last ? 1u : 0u;
    out->frozen=radio_diag.frozen; out->reason=radio_diag.reason;
    ble_port_unlock(key);
}
int ble_radio_read(uint32_t serial,ble_radio_event_t *out)
{
    uint32_t key=ble_port_lock();
    const ble_radio_event_t *e=&radio_diag.events[serial%BLE_RADIO_EVENTS];
    int valid=serial && e->serial==serial;
    /* Only copy occupied rows. Avoid a multi-kilobyte critical section for
     * the ordinary one-exchange event. */
    if(valid) {
        out->serial=e->serial; out->epoch=e->epoch; out->event=e->event;
        out->count=e->count; out->overflow=e->overflow; out->closed=e->closed;
        memcpy(out->row,e->row,e->count*sizeof e->row[0]);
    }
    ble_port_unlock(key); return valid;
}
