/* SPDX-License-Identifier: MIT OR MPL-2.0 */
/* Copyright (c) 2026 Julius Vitkauskas */
/* Bounded main-context allocator for NimBLE's startup pools and GATT table.
 * No library allocator is linked. First fit and adjacent free-block coalescing
 * permit host reinitialisation without leaking the fixed arena. */
#include <stddef.h>
#include <stdint.h>
#include "ble_memory.h"
#define HEAP_SIZE 4096u
static union { uintptr_t align; uint8_t bytes[HEAP_SIZE]; } arena __attribute__((section(".pool.ble")));
typedef struct block { uint32_t size,used; } block_t;
static uint32_t used,peak,fail;
void *ble_nm_malloc(size_t n)
{
    uint32_t off=0,need;
    block_t *b;
    if(!n || n>HEAP_SIZE-2u*sizeof(block_t)) return 0;
    need=(uint32_t)((n+sizeof(uintptr_t)-1u)&~(sizeof(uintptr_t)-1u));
    if(!((block_t *)(void *)arena.bytes)->size) ((block_t *)(void *)arena.bytes)->size=HEAP_SIZE-sizeof(block_t);
    while(off<HEAP_SIZE) {
        b=(block_t *)(void *)(arena.bytes+off);
        if(!b->used && b->size>=need) {
            if(b->size>=need+sizeof(block_t)+sizeof(uintptr_t)) {
                block_t *tail=(block_t *)(void *)(arena.bytes+off+sizeof(block_t)+need);
                tail->size=b->size-need-sizeof(block_t); tail->used=0; b->size=need;
            }
            b->used=1; used+=b->size; if(used>peak) peak=used;
            return b+1;
        }
        off+=(uint32_t)sizeof(block_t)+b->size;
    }
    fail++; return 0;
}
void ble_nm_free(void *p)
{
    block_t *b; uint32_t off=0;
    if(!p) return;
    b=(block_t *)p-1;
    if((uint8_t *)(void *)b<arena.bytes || (uint8_t *)(void *)b>=arena.bytes+HEAP_SIZE || !b->used) { fail++; return; }
    b->used=0; used-=b->size;
    while(off<HEAP_SIZE) {
        b=(block_t *)(void *)(arena.bytes+off);
        if(!b->used && off+sizeof(block_t)+b->size<HEAP_SIZE) {
            block_t *next=(block_t *)(void *)(arena.bytes+off+sizeof(block_t)+b->size);
            if(!next->used) { b->size+=(uint32_t)sizeof(block_t)+next->size; continue; }
        }
        off+=(uint32_t)sizeof(block_t)+b->size;
    }
}
void *ble_nm_calloc(size_t n,size_t size)
{
    void *p;
    if(n && size>HEAP_SIZE/n) return 0;
    p=ble_nm_malloc(n*size); if(p) memset(p,0,(unsigned)(n*size)); return p;
}
void *ble_nm_realloc(void *old,size_t n)
{
    void *p; uint32_t size;
    if(!old) return ble_nm_malloc(n);
    if(!n) { ble_nm_free(old); return 0; }
    size=((block_t *)old-1)->size;
    if(n<=size) return old;
    p=ble_nm_malloc(n); if(!p) return 0;
    memcpy(p,old,size); ble_nm_free(old); return p;
}
void ble_heap_diag(uint32_t *out) { out[0]=used; out[1]=peak; out[2]=fail; }
#ifndef BLE_HOST_TEST
void *memmove(void *d,const void *s,size_t n)
{
    uint8_t *a=d; const uint8_t *b=s;
    if(a<b) { size_t i; for(i=0;i<n;i++) a[i]=b[i]; }
    else { while(n) { n--; a[n]=b[n]; } }
    return d;
}
size_t strlen(const char *s) { size_t n=0; while(s[n]) n++; return n; }
int strcmp(const char *a,const char *b) { while(*a && *a==*b) { a++; b++; } return (unsigned char)*a-(unsigned char)*b; }
int strncmp(const char *a,const char *b,size_t n) { while(n && *a && *a==*b) { a++; b++; n--; } return n ? (unsigned char)*a-(unsigned char)*b : 0; }
char *strncat(char *d,const char *s,size_t n) { size_t i=strlen(d),j=0; while(j<n && s[j]) { d[i+j]=s[j]; j++; } d[i+j]=0; return d; }
char *strncpy(char *d,const char *s,size_t n) { size_t i; for(i=0;i<n;i++) { d[i]=*s; if(*s) s++; } return d; }
#endif
