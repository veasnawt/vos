//! R3-N3: the pure validation decisions of the virtio-net driver, exactly as the
//! accepted C (`virtio_net.c` at afe33b8). Every input is a scalar (or a copy)
//! that the unchanged C access sequence has already read: PCI configuration,
//! register windows, virtqueue and DMA memory, volatile ring reads, fences,
//! reset, notification and ownership all stay in C. Nothing here touches device
//! memory, and nothing here keeps state between calls.
#![forbid(unsafe_code)]

pub const QUEUE: u32 = 8; // VNET_QUEUE
pub const FRAME_MAX: u32 = 1514; // VNET_FRAME_MAX
pub const HEADER: u32 = 12; // VNET_HEADER
/// VIRTIO_F_VERSION_1 and VIRTIO_NET_F_MAC only.
pub const FEATURES: u64 = (1 << 32) | (1 << 5);
/// VIRTIO_F_ACCESS_PLATFORM (IOMMU) is refused.
const ACCESS_PLATFORM: u64 = 1 << 33;
/// `used_entry` result for an invalid used-ring entry. A valid mask has at
/// most the low eight bits set, so it can never equal this value.
pub const USED_INVALID: u32 = u32::MAX;

/// `choose_features`: both required bits offered and no IOMMU requirement.
pub fn features_ok(offered: u64) -> bool {
    offered & FEATURES == FEATURES && offered & ACCESS_PLATFORM == 0
}

/// `queue_size_ok`: a power of two from VNET_QUEUE to 32,768 (C `unsigned`).
pub fn queue_size_ok(size: u32) -> bool {
    size >= QUEUE && size <= 32768 && size & size.wrapping_sub(1) == 0
}

/// The C `read_reg(NUMQ) < 2` test, inverted: at least the RX and TX queues.
pub fn queues_ok(count: u32) -> bool {
    count >= 2
}

/// `used_ok`, first step: the C `count > (tx ? 1 : VNET_QUEUE)` refusal,
/// decided before any ring entry is read.
pub fn used_count_ok(count: u32, tx: bool) -> bool {
    count <= if tx { 1 } else { QUEUE }
}

/// `used_ok`, one entry. The C reads `id` and then `len` of the entry (in that
/// order, through its volatile ring pointer) and passes both here with the mask
/// of IDs already seen. The checks run in the C order: ID range, duplicate ID,
/// then length. Returns the updated mask, or USED_INVALID.
pub fn used_entry(seen: u32, id: u32, len: u32, tx: bool) -> u32 {
    let limit = if tx { 1 } else { QUEUE };
    if id >= limit {
        return USED_INVALID;
    }
    let bit = 1u32 << (id & 31); // id < 8 here; the mask keeps the shift total
    if seen & bit != 0 {
        return USED_INVALID;
    }
    let ok = if tx { len == 0 } else { len >= HEADER + 14 && len <= HEADER + FRAME_MAX };
    if !ok {
        return USED_INVALID;
    }
    seen | bit
}

/// `header_ok`: the first two virtio-net header bytes (flags and GSO type)
/// must be zero; no offloads are negotiated.
pub fn header_ok(flags: u8, gso: u8) -> bool {
    flags == 0 && gso == 0
}

/// `setup_queue`'s notify-offset test, inverted: the 2-byte notify write at
/// `offset` lies inside the notify window of `length` bytes.
pub fn notify_ok(offset: u64, length: u32) -> bool {
    !(offset > length as u64 || (length as u64).wrapping_sub(offset) < 2)
}

/// `vnet_send`'s frame-length test, inverted (Ethernet header to frame max).
pub fn send_length_ok(length: u32) -> bool {
    !(length < 14 || length > FRAME_MAX)
}

/// `vnet_send`'s minimum Ethernet frame on the wire.
pub fn tx_wire(length: u32) -> u32 {
    if length < 60 { 60 } else { length }
}

/// `vnet_receive`'s capacity test, inverted.
pub fn capacity_ok(capacity: u32) -> bool {
    capacity != 0 && capacity <= FRAME_MAX
}

/// `vnet_receive`'s refusal of a length output inside the frame output: the C
/// `(a < b + sizeof(*length) && b < a + capacity)` with 64-bit wrapping sums.
pub fn rx_outputs_overlap(frame: u64, capacity: u32, length: u64) -> bool {
    frame < length.wrapping_add(4) && length < frame.wrapping_add(capacity as u64)
}
