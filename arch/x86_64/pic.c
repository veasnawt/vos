#include "pic.h"
#include "cpu.h"

static uint8_t port_in(uint16_t port)
{
    return inb(port);
}

static void port_out(uint16_t port, uint8_t value)
{
    outb(port, value);
    outb(0x80, 0);                      /* the classic I/O delay for the old chips */
}

const struct pic_io pic_port_io = { port_in, port_out };

void pic_init(const struct pic_io *io, uint8_t base)
{
    io->out(PIC_MASTER_DATA, 0xFF);     /* nothing may be delivered while the chips are set up */
    io->out(PIC_SLAVE_DATA, 0xFF);
    io->out(PIC_MASTER_CMD, PIC_ICW1_INIT);
    io->out(PIC_SLAVE_CMD, PIC_ICW1_INIT);
    io->out(PIC_MASTER_DATA, base);                     /* ICW2: vector base */
    io->out(PIC_SLAVE_DATA, (uint8_t)(base + 8));
    io->out(PIC_MASTER_DATA, PIC_ICW3_MASTER);
    io->out(PIC_SLAVE_DATA, PIC_ICW3_SLAVE);
    io->out(PIC_MASTER_DATA, PIC_ICW4_8086);
    io->out(PIC_SLAVE_DATA, PIC_ICW4_8086);
    io->out(PIC_MASTER_DATA, 0xFF);     /* OCW1: every line masked */
    io->out(PIC_SLAVE_DATA, 0xFF);
}

void pic_set_mask(const struct pic_io *io, unsigned irq, int masked)
{
    uint16_t port = irq < 8 ? PIC_MASTER_DATA : PIC_SLAVE_DATA;
    uint8_t bit = (uint8_t)(1u << (irq & 7)), m = io->in(port);

    io->out(port, masked ? (uint8_t)(m | bit) : (uint8_t)(m & ~bit));
}

uint16_t pic_masks(const struct pic_io *io)
{
    return (uint16_t)(io->in(PIC_SLAVE_DATA) << 8 | io->in(PIC_MASTER_DATA));
}

static uint16_t read_reg(const struct pic_io *io, uint8_t ocw3)
{
    io->out(PIC_MASTER_CMD, ocw3);
    io->out(PIC_SLAVE_CMD, ocw3);
    return (uint16_t)(io->in(PIC_SLAVE_CMD) << 8 | io->in(PIC_MASTER_CMD));
}

uint16_t pic_isr(const struct pic_io *io)
{
    return read_reg(io, PIC_OCW3_READ_ISR);
}

uint16_t pic_irr(const struct pic_io *io)
{
    return read_reg(io, PIC_OCW3_READ_IRR);
}

int pic_begin(const struct pic_io *io, unsigned irq)
{
    uint16_t isr = pic_isr(io);
    int cascade = irq < 8 || (isr & (1u << PIC_CASCADE_IRQ));

    if (irq < 16 && (isr & (1u << irq)) && cascade)
        return PIC_GENUINE;
    if (irq == 7)
        return PIC_SPURIOUS;
    if (irq == 15 && cascade) {
        io->out(PIC_MASTER_CMD, PIC_OCW2_EOI);  /* the master's IRQ2 was real */
        return PIC_SPURIOUS;
    }
    return PIC_NOT_IN_SERVICE;
}

void pic_eoi(const struct pic_io *io, unsigned irq)
{
    if (irq >= 8)
        io->out(PIC_SLAVE_CMD, PIC_OCW2_EOI);
    io->out(PIC_MASTER_CMD, PIC_OCW2_EOI);
}
