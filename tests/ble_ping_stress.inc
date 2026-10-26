/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Core central, independent counters/ARQ, with a 100 ms authenticated
 * payload timer. No feature-gating of ping (Vol 6 B 5.1.8). */
static void ping_stress(uint32_t events,uint16_t interval)
{
    start_interval=interval; uint32_t anchor=begin(); start_interval=12;
    assert(!oc); ow=orr=0;
    uint8_t csn=sn,crx=sim_core_start(sn),pending=0,air[31],plain[27];
    uint8_t length=0,llid=1,last_rx[31],last_length=0,last_llid=0;
    uint64_t ctx=next_counter,prx=1;
    uint32_t last_auth=0,pings=0,responses=0,writes=0,rx_repeat=0,tx_repeat=0,crc_count=0;
    uint32_t timer=0,delay=0;
    int ping_active=0;
    for(uint32_t e=0;e<events;e++) {
        uint32_t now=e*(interval*5u/4u);
        unsigned burst=(random32()%19u)==0 ? 2u+random32()%4u : 1u;
        for(unsigned j=0;j<burst;j++) {
            if(!pending) {
                length=0; llid=1;
                if(!ping_active && (uint32_t)(now-last_auth)>=75u) {
                    plain[0]=18; length=1; llid=3; ping_active=1; pings++; timer=now;
                } else if((random32()%13u)==0) {
                    /* L2CAP/ATT Write Command carrying a BLE MIDI note. */
                    const uint8_t write[]={8,0,4,0,0x52,0x10,0,0x80,0x80,0x90,60,100};
                    length=sizeof write; memcpy(plain,write,length); llid=2; writes++;
                }
                if(length) {
                    memcpy(air,plain,length);
                    assert(central_ccm(ctx++,1,llid,air,length,0)); length+=4;
                }
                pending=1;
            } else tx_repeat++;
            uint16_t ch=(uint16_t)((length<<8)|llid|(csn<<3)|(crx<<2)|((j+1<burst) ? 16u : 0));
            sim_ticks=anchor+(80u+8u*length+j*708u)*24u;
            unsigned roll=random32(); int crc=(roll%97u)!=0;
            if(!crc) crc_count++;
            int accepted=sim_core_receive(ch,air,(uint16_t)e,crc);
            if(accepted && llid==2 && length) {
                assert(oc<128); oracle[ow].n=(uint8_t)(length-4u);
                memcpy(oracle[ow].p,plain,length-4u); ow=(ow+1u)%128u; oc++;
            }
            if(!delay && (random32()%4u)) sim_radio_service(0);
            uint16_t ph; uint8_t p[31]; sim_core_transmit(&ph,p);
            if(random32()%41u && random32()%97u) {
                unsigned n=ph>>8;
                if(((ph>>3)&1u)==crx) {
                    crx^=1;
                    if(n) {
                        uint8_t decrypted[31]; memcpy(decrypted,p,n);
                        if(n<5 || !central_ccm(prx,0,(uint8_t)(ph&3u),decrypted,n-4u,1)) {
                            printf("PING CENTRAL MIC event=%u pings=%u responses=%u expected_counter=%" PRIu64 " pending_empty_loads=%u\n",e,pings,responses,prx,sim_core_empty_pending_loads);
                            fflush(stdout); abort();
                        }
                        prx++; last_auth=now;
                        if((ph&3u)==3 && n==6 && decrypted[0]==7 && decrypted[1]==18) {
                            assert(ping_active); ping_active=0; responses++;
                        }
                    }
                    last_length=(uint8_t)n; last_llid=(uint8_t)(ph&3u); memcpy(last_rx,p,n);
                } else {
                    rx_repeat++;
                    assert(n==last_length && (ph&3u)==last_llid && !memcmp(last_rx,p,n));
                }
                if(((ph>>2)&1u)!=csn) { csn^=1; pending=0; }
            }
            if(!delay) { sim_radio_service(0); drain(); }
            if(!(ph&16u) && !(ch&16u)) break;
        }
        sim_ticks=anchor+6000u*24u;
        if(!delay) { sim_radio_service(1); drain(); if(random32()%127u==0) delay=1u+random32()%4u; }
        else delay--;
        ble_ll_event((uint16_t)e,sim_ticks); ble_link.rx_seen=ble_link.valid_seen=0;
        assert(ble_link.state==BLE_LINK_UP);
        assert(!ping_active || now-timer<1000u);
        anchor+=(uint32_t)interval*30000u;
        if((e+1u)%1000000u==0) {
            printf("ping interval=%u events=%u requests=%u replies=%u writes=%u central_repeats=%u peripheral_repeats=%u crc=%u empty_pending_loads=%u zero MIC failures\n",
                   interval,e+1,pings,responses,writes,tx_repeat,rx_repeat,crc_count,sim_core_empty_pending_loads); fflush(stdout);
        }
    }
    ble_security_diag_t d; ble_security_get_diag(&d); assert(!d.mic_failures);
    sim_radio_service(1); drain(); assert(!oc);
    printf("ping final interval=%u events=%u requests=%u replies=%u writes=%u central_repeats=%u peripheral_repeats=%u crc=%u empty_pending_loads=%u: PASS\n",
           interval,events,pings,responses,writes,tx_repeat,rx_repeat,crc_count,sim_core_empty_pending_loads);
    assert(pings>100 && responses+1u>=pings && writes && rx_repeat && tx_repeat && crc_count);
}
