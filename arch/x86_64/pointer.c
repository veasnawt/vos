#include "pointer.h"
/* Header bit 3 resynchronizes only at a packet boundary: data may also set it.
 * Consume overflowing packets completely before rejecting their displacement. */
int pointer_decode(struct pointer_decoder *d,uint8_t b,struct pointer_packet *p) {
    d->idle=0;
    if(!d->used && !(b&8))return -1;
    d->bytes[d->used++]=b;
    if(d->used<3)return 0;
    d->used=0;
    uint8_t h=d->bytes[0];
    if(h&0xc0)return -1;
    p->dx=(int16_t)((int)d->bytes[1]-((h&0x10)?256:0));
    p->dy=(int16_t)((int)d->bytes[2]-((h&0x20)?256:0));
    p->buttons=h&1; /* Only left-button authority is defined in version 1. */
    return 1;
}
int pointer_idle(struct pointer_decoder *d) {
    if(d->used && ++d->idle==16){d->used=d->idle=0;return 1;}
    return 0;
}
