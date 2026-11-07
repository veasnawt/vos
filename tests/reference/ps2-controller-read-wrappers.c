/* Test-only wrappers around the frozen accepted driver (byte-identical include).
 * Exposes the accepted static bounded controller reads; no behavior added. */
#include "ps2-controller-read.c"
int read_keyboard_reference(struct keyboard *k, uint8_t *value) { return read_keyboard(k, value); }
int read_aux_reference(struct keyboard *k, uint8_t *value) { return read_aux(k, value); }
