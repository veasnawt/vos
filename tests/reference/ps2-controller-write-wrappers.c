#include "ps2-controller-write.c"
int write_port_reference(struct keyboard *k, uint16_t port, uint8_t value) { return write_port(k, port, value); }
int command_reference(struct keyboard *k, uint8_t value) { return command(k, value); }
int aux_command_reference(struct keyboard *k, uint8_t value) { return aux_command(k, value); }
