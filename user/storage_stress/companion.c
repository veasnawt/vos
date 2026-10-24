/* ABI v0 CPU and ABI v1 paced IPC companions, separate executables. */
#include <vos/syscall.h>
#include <stdint.h>
int main(void) {
#ifdef COMPANION_CPU
    volatile uint32_t value=0x4e360001U;
    for(unsigned phase=0;phase<5;phase++) {
        if(vos_debug_write(0,0)) return 1;
        if(phase<4) for(unsigned j=0;j<10000;j++) value=value*1664525U+1013904223U;
    }
    return value==CPU_EXPECTED ? 0 : 2;
#else
    for(uint64_t epoch=0;epoch<68;epoch++) {
        uint64_t value;
#ifdef COMPANION_INITIATOR
        if(vos_channel_recv(1,0,0)) return 1;
        value=0x4e36000100000000ULL|epoch;
        if(vos_channel_send(2,&value,8)) return 2;
        value=0;
        if(vos_channel_recv(2,&value,8)!=8 || value!=(0x4e36000100000000ULL|epoch)) return 3;
        if(vos_channel_send(1,0,0)) return 4;
#else
        if(vos_channel_recv(1,&value,8)!=8 || value!=(0x4e36000100000000ULL|epoch)) return 5;
        if(vos_channel_send(1,&value,8)) return 6;
#endif
    }
#ifdef COMPANION_INITIATOR
    if(vos_handle_close(2)) return 7;
#endif
    return vos_handle_close(1) ? 8 : 0;
#endif
}
