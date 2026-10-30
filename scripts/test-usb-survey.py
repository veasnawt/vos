#!/usr/bin/env python3
"""USB input bring-up, Increments 1 and 2: hosted gate for the read-only survey (kernel/rust/usb/xhci/survey.rs and firmware.rs).

Builds the portable xHCI crate with the survey and firmware modules, runs tests/rust/xhci_survey.rs and
tests/rust/xhci_firmware.rs, checks that the survey traits cannot be used to write (compile-fail controls) and that the
platform sources contain no write path, then runs mutation controls: each mutant changes one decision in survey.rs or
firmware.rs and the tests must fail. Sources are never modified; the mutants
are built in worker directories below build/. Nothing here touches PCI, MMIO or physical memory.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CRATE = ROOT / 'kernel/rust/usb/xhci'
SUITE = ROOT / 'tests/rust/xhci_survey.rs'
SUITE_FW = ROOT / 'tests/rust/xhci_firmware.rs'
M = []


def mut(name, why, old, new, file='survey.rs'):
    M.append((name, why, old, new, file))


# ---- scan ----
mut('scan-multifunction-ignored', 'every device is probed for eight functions', "let multi = (word(p, bus, dev, 0, 0x0c) >> 16) as u8 & 0x80 != 0;", "let multi = true;")
mut('scan-vendor-zero-accepted', 'a function with vendor 0 is real', "id as u16 != 0xffff && id as u16 != 0\n}", "id as u16 != 0xffff\n}")
mut('scan-all-ones-accepted', 'a function that answers all ones is real', "id as u16 != 0xffff && id as u16 != 0\n}", "id as u16 != 0\n}")
mut('scan-function-limit-moved', 'the function bound is not the bound', "if s.seen >= FUNCTION_LIMIT {", "if s.seen >= FUNCTION_LIMIT + 7 {")
mut('scan-usb-class', 'the wrong class is called USB', "0x0c03 => {", "0x0c04 => {")
mut('scan-iommu-class', 'the wrong class is called an IOMMU', "0x0806 => s.iommu_functions", "0x0807 => s.iommu_functions")
mut('scan-bridge-type', 'any header type is followed as a bridge', "0x0604 if header & 0x7f == 1 => {", "0x0604 => {")
mut('scan-bridge-upward-only', 'a bridge may send the walk back to a lower bus', "if secondary > bus && subordinate >= secondary && !queued[usize::from(secondary)] && tail < BUS_LIMIT {", "if subordinate >= secondary && !queued[usize::from(secondary)] && tail < BUS_LIMIT {")
mut('scan-bridge-subordinate', 'a reversed bus range is followed', "if secondary > bus && subordinate >= secondary && !queued", "if secondary > bus && !queued")
mut('scan-bridge-queued-twice', 'a bus named by two bridges is scanned twice', "&& !queued[usize::from(secondary)] && tail < BUS_LIMIT {", "&& tail < BUS_LIMIT {")
mut('scan-bridge-unmarked', 'a followed bus is not marked as queued', "queued[usize::from(secondary)] = true;\n                            if let Some(q)", "if let Some(q)")
mut('scan-queue-bound', 'the bus queue is larger than its array', "&& tail < BUS_LIMIT {\n                            queued", "&& tail < BUS_LIMIT + 8 {\n                            queued")
mut('scan-skip-count', 'unusable bridges are not counted', "                            s.skipped_bridges = s.skipped_bridges.saturating_add(1);", "                            let _ = 0;")
mut('scan-record-bound', 'controllers beyond the recording limit overwrite the last entry', "if let Some(slot) = s.usb.get_mut(usize::from(s.recorded)) {", "if let Some(slot) = s.usb.get_mut(usize::from(s.recorded).min(MAX_USB - 1)) {")
# ---- BAR decoding ----
mut('bar-memory-decoding-ignored', 'a controller with memory decoding off is read', "if self.command & 2 == 0 {", "if false {")
mut('bar-io-accepted', 'an I/O BAR is treated as memory', "if low & 1 != 0 {\n            return Err(\"I/O BAR\");", "if false {\n            return Err(\"I/O BAR\");")
mut('bar-64bit-high-ignored', 'the high half of a 64-bit BAR is dropped', "2 => (u64::from(self.bar1) << 32) | u64::from(low & !0xf),", "2 => u64::from(low & !0xf),")
mut('bar-below-1mib', 'a BAR below 1 MiB is read', "if base < 0x10_0000 {", "if base < 0x1000 {")
mut('bar-unaligned', 'an unaligned BAR is read', "if base % align != 0 {", "if false {")
mut('bar-header-type', 'a bridge header is read as a controller', "if self.header & 0x7f != 0 {\n            return Err(\"not a type-0 header\");", "if false {\n            return Err(\"not a type-0 header\");")
# ---- PCI capabilities ----
mut('pci-cap-cycle', 'a looping capability list is followed to the bound', "if seen & bit != 0 {", "if false && seen & bit != 0 {")
mut('pci-cap-devctl-offset', 'Device Control is read from the wrong offset', "i.devctl = word(p, b, d, n, ptr + 8) as u16;", "i.devctl = word(p, b, d, n, ptr + 4) as u16;")
mut('pci-cap-msix-bit', 'the MSI-X enable bit is wrong', "i.msix_on = (hdr >> 16) & 0x8000 != 0;", "i.msix_on = (hdr >> 16) & 0x4000 != 0;")
mut('pci-cap-msi-bit', 'the MSI enable bit is wrong', "i.msi_on = (hdr >> 16) & 1 != 0;", "i.msi_on = (hdr >> 16) & 2 != 0;")
mut('pci-cap-pm-end', 'a PM capability at the end of the space is read past it', "0x01 if i.pm == 0 && ptr <= 0xf8 => {", "0x01 if i.pm == 0 => {")
mut('pci-cap-below-header', 'a capability pointer inside the header is followed', "if ptr < 0x40 {\n            return i;", "if ptr < 0x10 {\n            return i;")
mut('pci-cap-status-bit', 'the capability-list status bit is ignored', "if f.status & 0x10 == 0 {\n        return i;", "if false {\n        return i;")
# ---- xHCI registers ----
mut('xhci-caplength-low', 'a CAPLENGTH below the register block is accepted', "if op < 0x20 || op % 4 != 0 {", "if op < 0x04 || op % 4 != 0 {")
mut('xhci-caplength-alignment', 'an unaligned CAPLENGTH is accepted', "if op < 0x20 || op % 4 != 0 {", "if op < 0x20 {")
mut('xhci-all-ones', 'a controller that reads all ones is decoded', "x.version = (w0 >> 16) as u16;\n    if w0 == 0xffff_ffff {\n        return Err(\"reads as all ones\");", "x.version = (w0 >> 16) as u16;\n    if false {\n        return Err(\"reads as all ones\");")
mut('xhci-scratchpad-hi', 'the scratchpad high field is read from the wrong bits', "(((self.hcs2 >> 21) & 0x1f) << 5)", "(((self.hcs2 >> 22) & 0x1f) << 5)")
mut('xhci-scratchpad-lo', 'the scratchpad low field is read from the wrong bits', "| ((self.hcs2 >> 27) & 0x1f)", "| ((self.hcs2 >> 26) & 0x1f)")
mut('xhci-port-limit', 'the port count is not bounded', "for p in 0..x.ports.min(PORT_LIMIT) {", "for p in 0..x.ports {")
mut('xhci-port-connect-bit', 'the connection bit is wrong', "if sc & 1 != 0 {\n            x.connected", "if sc & 2 != 0 {\n            x.connected")
mut('xhci-port-ordinal', 'ports are numbered from the wrong base', "*slot = Port { index: p, portsc: sc };", "*slot = Port { index: p + 1, portsc: sc };")
mut('xhci-xecp-shift', 'the extended capability pointer is in bytes', "let mut off = (x.hcc1 >> 16) << 2;", "let mut off = (x.hcc1 >> 16) << 1;")
mut('xhci-next-units', 'the next-capability pointer is in bytes', "let next = ((h >> 8) & 0xff) << 2;", "let next = ((h >> 8) & 0xff) << 1;")
mut('xhci-cap-limit', 'the capability chain is one longer than its bound', "if usize::from(x.caps) >= CAP_LIMIT {\n            return Err(\"capability list too long\");", "if usize::from(x.caps) > CAP_LIMIT {\n            return Err(\"capability list too long\");")
mut('xhci-legacy-first-only', 'a second legacy capability replaces the first', "1 if x.legacy.is_none() =>", "1 =>")
mut('xhci-debug-first-only', 'a second debug capability replaces the first', "10 if x.dbc.is_none() =>", "10 =>")
mut('xhci-debug-offset', 'the debug control register is read from the wrong offset', "x.dbc = Some((off as u16, r.word(off + 0x20)?))", "x.dbc = Some((off as u16, r.word(off + 0x1c)?))")
mut('xhci-protocol-name', 'a protocol capability with another name is listed', "if name == u32::from_le_bytes(*b\"USB \")\n                    && let Some(s)", "if let Some(s)")
mut('xhci-protocol-major', 'the protocol major revision is wrong', "Proto { major: (h >> 24) as u8, minor: (h >> 16) as u8,", "Proto { major: (h >> 23) as u8, minor: (h >> 16) as u8,")
mut('xhci-protocol-count', 'the protocol port count is wrong', "count: (ports >> 8) as u8 }", "count: (ports >> 9) as u8 }")
mut('xhci-window-bound', 'a register past the window end is requested', "if off > self.limit.saturating_sub(4) {", "if off > self.limit {")
mut('xhci-second-read-skipped', 'the second reading of USBCMD is not a reading', "x.usbcmd2 = r.word(op)?;", "x.usbcmd2 = x.usbcmd;")
mut('xhci-second-status-skipped', 'the second reading of USBSTS is not a reading', "x.usbsts2 = r.word(op + 4)?;", "x.usbsts2 = x.usbsts;")
mut('xhci-refusal-continues', 'a refused port read does not end the walk', "let sc = r.word(op + 0x400 + 0x10 * u32::from(p))?;", "let sc = r.word(op + 0x400 + 0x10 * u32::from(p)).unwrap_or(0);")
# ---- rendering ----
mut('line-capacity', 'a line can outgrow the room kept for its terminator', "if self.n < self.b.len() - 2\n            && let Some(slot)", "if self.n < self.b.len()\n            && let Some(slot)")
mut('line-printable', 'unprintable bytes reach the console', "*slot = if c.is_ascii_graphic() || c == b' ' { c } else { b'?' };", "*slot = c;")
mut('line-terminator', 'a line is not terminated', "for crlf in [b'\\r', b'\\n'] {", "for crlf in [b'\\r'] {")
mut('hex-width', 'hexadecimal values lose their leading zeros', "if nib != 0 || started || i < digits || i == 0 {", "if nib != 0 || started || i == 0 {")
mut('render-changed-warning', 'a controller that changes by itself is not flagged', "if x.usbcmd != x.usbcmd2 || x.usbsts != x.usbsts2 {", "if false {")
mut('render-incomplete-note', 'an incomplete scan is not flagged', "if s.limit_hit || s.skipped_bridges != 0 || s.usb_total > u16::from(s.recorded) {", "if false {")
mut('summary-not-read', 'a controller that was not read is summarised as if it were', "if x.reads == 0 {\n        l.s(\" NOT READ\");", "if false {\n        l.s(\" NOT READ\");")

# ---- Increment 2: ports, ring pointers ----
mut('v2-port-record-cut', 'ports past the sixteenth are not recorded', "if let Some(slot) = x.portsc.get_mut(usize::from(p)) {", "if let Some(slot) = x.portsc.get_mut(usize::from(p).min(15)) {")
mut('v2-dcbaap-halves', 'the DCBAAP halves are swapped', "x.dcbaap = u64::from(hi) << 32 | u64::from(lo);", "x.dcbaap = u64::from(lo) << 32 | u64::from(hi);")
mut('v2-crcr-offset', 'CRCR is read from the wrong offset', "x.crcr = r.word(op + 0x18)?;", "x.crcr = r.word(op + 0x1c)?;")
mut('v2-ir-offset', 'interrupter 0 is read from the wrong offset', "let ir = rt.checked_add(0x20).ok_or(\"RTSOFF overflows\")?;", "let ir = rt.checked_add(0x40).ok_or(\"RTSOFF overflows\")?;")
mut('v2-rtsoff-alignment', 'an unaligned RTSOFF is used', "if rt % 32 != 0 {", "if false {")
mut('v2-pointer-failure-aborts', 'a refused ring pointer ends the walk before the second USBCMD reading', "x.ptr_stopped = pointers(r, x, op).err();", "pointers(r, x, op)?;")
mut('v2-erdp-halves', 'the ERDP halves are swapped', "x.ir_erdp = u64::from(dhi) << 32 | u64::from(dlo);", "x.ir_erdp = u64::from(dlo) << 32 | u64::from(dhi);")
mut('v2-erstba-offset', 'ERSTBA is read from the wrong offset', "ir.checked_add(0x10).ok_or(\"RTSOFF overflows\")?", "ir.checked_add(0x0c).ok_or(\"RTSOFF overflows\")?")
mut('v2-port-count-connected', 'connected ports are counted from the wrong bit', "conn += sc & 1;", "conn += sc >> 1 & 1;")
mut('v2-port-count-enabled', 'enabled ports are counted from the wrong bit', "enabled += sc >> 1 & 1;", "enabled += sc >> 2 & 1;")
mut('v2-port-count-power', 'powered ports are counted from the wrong bit', "power += sc >> 9 & 1;", "power += sc >> 8 & 1;")
mut('v2-port-count-overcurrent', 'over-current ports are counted from the wrong bit', "oc += sc >> 3 & 1;", "oc += sc >> 4 & 1;")
mut('v2-port-cap-note', 'ports beyond the recorded ones are not flagged', "if usize::from(x.ports_read) > shown {", "if false {")
# ---- Increment 2: Intel routing ----
mut('v2-routing-vendor', 'any vendor has Intel routing registers', "if f.vendor != 0x8086 || !f.is_xhci() || f.header & 0x7f != 0 {", "if !f.is_xhci() || f.header & 0x7f != 0 {")
mut('v2-routing-class', 'an EHCI has Intel xHCI routing registers', "if f.vendor != 0x8086 || !f.is_xhci() || f.header & 0x7f != 0 {", "if f.vendor != 0x8086 || f.header & 0x7f != 0 {")
mut('v2-routing-header', 'a bridge header has routing registers', "if f.vendor != 0x8086 || !f.is_xhci() || f.header & 0x7f != 0 {", "if f.vendor != 0x8086 || !f.is_xhci() {")
mut('v2-routing-offset', 'XUSB2PR is read from the wrong offset', "xusb2pr: word(p, f.bus, f.dev, f.func, 0xd0),", "xusb2pr: word(p, f.bus, f.dev, f.func, 0xd4),")
mut('v2-routing-allones-ignored', 'a function without the registers is reported as having them', "if r.xusb2pr == 0xffff_ffff && r.xusb2prm == 0xffff_ffff && r.usb3_pssen == 0xffff_ffff && r.usb3prm == 0xffff_ffff {", "if false {")
mut('v2-routing-allones-any', 'one all-ones register hides the others', "if r.xusb2pr == 0xffff_ffff && r.xusb2prm == 0xffff_ffff", "if r.xusb2pr == 0xffff_ffff || r.xusb2prm == 0xffff_ffff")
mut('v2-routing-port-mask', 'the routable mask is not cut to the ports the PCH has', "pub const ROUTING_PORT_MASK: u32 = 0x7fff;", "pub const ROUTING_PORT_MASK: u32 = 0xffff_ffff;")
mut('v2-routing-to-ehci-unmasked', 'ports that cannot be routed are called left with the EHCI', "!self.xusb2pr & self.usb2_routable()", "!self.xusb2pr & ROUTING_PORT_MASK")
mut('v2-routing-to-xhci-unmasked', 'ports that cannot be routed are called routed', "pub fn usb2_to_xhci(&self) -> u32 {\n        self.xusb2pr & self.usb2_routable()", "pub fn usb2_to_xhci(&self) -> u32 {\n        self.xusb2pr & ROUTING_PORT_MASK")
mut('v2-routing-usb3-unmasked', 'SuperSpeed enables that cannot be set are reported', "pub fn usb3_enabled(&self) -> u32 {\n        self.usb3_pssen & self.usb3prm", "pub fn usb3_enabled(&self) -> u32 {\n        self.usb3_pssen")
mut('v2-routing-usb3-disabled-unmasked', 'SuperSpeed lanes that cannot be set are called disabled', "!self.usb3_pssen & self.usb3prm", "!self.usb3_pssen")
# ---- Increment 2: EHCI ----
mut('v2-ehci-caplength-low', 'an EHCI CAPLENGTH below the register block is accepted', "if !(0x10..=0x40).contains(&op) || op % 4 != 0 {", "if !(0x04..=0x40).contains(&op) || op % 4 != 0 {")
mut('v2-ehci-caplength-high', 'an EHCI CAPLENGTH past the window is accepted', "if !(0x10..=0x40).contains(&op) || op % 4 != 0 {", "if !(0x10..=0x80).contains(&op) || op % 4 != 0 {")
mut('v2-ehci-caplength-alignment', 'an unaligned EHCI CAPLENGTH is accepted', "if !(0x10..=0x40).contains(&op) || op % 4 != 0 {", "if !(0x10..=0x40).contains(&op) {")
mut('v2-ehci-port-count-mask', 'the EHCI port count is five bits wide', "e.ports = (e.hcs & 0xf) as u8;", "e.ports = (e.hcs & 0x1f) as u8;")
mut('v2-ehci-configflag-offset', 'CONFIGFLAG is read from the wrong offset', "e.configflag = r.word(op + 0x40)?;", "e.configflag = r.word(op + 0x3c)?;")
mut('v2-ehci-port-offset', 'EHCI ports are read from the wrong offset', "let sc = r.word(op + 0x44 + 4 * u32::from(i))?;", "let sc = r.word(op + 0x48 + 4 * u32::from(i))?;")
mut('v2-ehci-connected-bit', 'EHCI connection is read from the wrong bit', "e.connected += (sc & 1) as u8;", "e.connected += (sc >> 1 & 1) as u8;")
mut('v2-ehci-owner-bit', 'EHCI port ownership is read from the wrong bit', "e.companion_owned += (sc >> 13 & 1) as u8;", "e.companion_owned += (sc >> 12 & 1) as u8;")
mut('v2-ehci-halted-bit', 'EHCI halted is read from the wrong bit', "self.usbsts & (1 << 12) != 0", "self.usbsts & (1 << 11) != 0")
mut('v2-ehci-eecp-shift', 'the EHCI legacy pointer is read from the wrong bits', "let mut at = ((e.hcc >> 8) & 0xff) as u8;", "let mut at = ((e.hcc >> 9) & 0xff) as u8;")
mut('v2-ehci-eecp-range', 'the EHCI legacy pointer may point into the standard header', "if !(0x40..=0xf8).contains(&at) {", "if !(0x00..=0xf8).contains(&at) {")
mut('v2-ehci-legacy-id', 'any EHCI capability is taken for legacy support', "if h as u8 == 1 {", "if true {")
mut('v2-ehci-cap-bound', 'the EHCI legacy chain is not bounded by eight', "let mut at = ((e.hcc >> 8) & 0xff) as u8;\n    for _ in 0..8 {", "let mut at = ((e.hcc >> 8) & 0xff) as u8;\n    for _ in 0..64 {")
mut('v2-ehci-hub-note', 'the render does not say a connected root port may be a hub', " (a hub behind it? devices below it are not visible)", "")
mut('v2-ehci-all-ones', 'an EHCI that reads all ones is decoded', "if w0 == 0xffff_ffff {\n        return Err(\"reads as all ones\");\n    }\n    e.caplength", "if false {\n        return Err(\"reads as all ones\");\n    }\n    e.caplength")

# ---- Increment 2: firmware facts (firmware.rs) ----
def fw(name, why, old, new):
    mut(name, why, old, new, file='firmware.rs')

fw('fw-map-limit', 'the memory map has room for more entries than it records', "pub const MAP_LIMIT: usize = 128;", "pub const MAP_LIMIT: usize = 256;")
fw('fw-ram-overrides', 'a RAM entry hides a reserved one that overlaps it', "if k != MemKind::Available {\n                return k;\n            }", "if k != MemKind::Available && found != MemKind::Available {\n                return k;\n            }")
fw('fw-range-zero', 'an empty range is firmware memory', "if len == 0 || len > (1 << 20) {", "if len > (1 << 20) {")
fw('fw-range-cap', 'a range of any size is firmware memory', "if len == 0 || len > (1 << 20) {", "if len == 0 || len > (1 << 40) {")
fw('fw-range-ram', 'RAM counts as firmware memory', "if !matches!(self.kind(page), MemKind::Reserved | MemKind::AcpiReclaim | MemKind::AcpiNvs) {", "if !matches!(self.kind(page), MemKind::Available | MemKind::Reserved | MemKind::AcpiReclaim | MemKind::AcpiNvs) {")
fw('fw-range-unlisted', 'memory the map does not list counts as firmware memory', "if !matches!(self.kind(page), MemKind::Reserved | MemKind::AcpiReclaim | MemKind::AcpiNvs) {", "if !matches!(self.kind(page), MemKind::Unlisted | MemKind::Reserved | MemKind::AcpiReclaim | MemKind::AcpiNvs) {")
fw('fw-rsdp-new-wins', 'the old RSDP wins over the new one', "if rsdp.is_err() || ty == 15 {", "if rsdp.is_err() {")
fw('fw-rsdp-checksum', 'a wrong RSDP checksum is accepted', "if first.iter().fold(0u8, |a, &x| a.wrapping_add(x)) != 0 {", "if first.iter().fold(0u8, |a, &x| a.wrapping_add(x)) == 77 {")
fw('fw-rsdp-ext-checksum', 'a wrong extended RSDP checksum is accepted', "if whole.iter().fold(0u8, |a, &x| a.wrapping_add(x)) != 0 {", "if whole.iter().fold(0u8, |a, &x| a.wrapping_add(x)) == 77 {")
fw('fw-rsdp-revision', 'a revision 0 RSDP is read as if it had an XSDT', "if revision >= 2 {", "if true {")
fw('fw-rsdp-signature', 'the RSDP signature is not checked', "if b.get(..8) != Some(b\"RSD PTR \") {", "if false {")
fw('fw-xsdt-first', 'the RSDT is preferred to the XSDT', "let (root, entry, want) = if rsdp.revision >= 2 && rsdp.xsdt != 0 {", "let (root, entry, want) = if false {")
fw('fw-root-limit', 'an oversized root table is read', "if sig != want || len < 36 || len > ROOT_LIMIT {", "if sig != want || len < 36 || len > ROOT_LIMIT * 100 {")
fw('fw-root-signature', 'the root table signature is not checked', "if sig != want || len < 36 || len > ROOT_LIMIT {", "if len < 36 || len > ROOT_LIMIT {")
fw('fw-root-checksum', 'the root table checksum is not checked', "if checksum(p, root, len) != Some(0) {", "if false {")
fw('fw-entry-limit', 'every root entry is read', "let n = listed.min(ENTRY_LIMIT);", "let n = listed;")
fw('fw-entry-truncation-silent', 'a cut root list is not reported', "t.more_than_listed = listed > ENTRY_LIMIT;", "t.more_than_listed = false;")
fw('fw-sig-limit', 'signatures beyond the array are counted', "if let Some(s) = t.sigs.get_mut(usize::from(t.count)) {\n            *s = sig;\n            t.count += 1;", "if let Some(s) = t.sigs.get_mut(usize::from(t.count).min(SIG_LIMIT - 1)) {\n            *s = sig;\n            t.count += 1;")
fw('fw-unreadable-table-silent', 'an unreadable listed table is not reported', "t.err = Some(\"a listed table is unreadable\");", "let _ = 0;")
fw('fw-dmar-checksum', 'a wrong DMAR checksum is accepted', "if checksum(p, pa, len) != Some(0) {\n        d.err = Some(\"DMAR checksum wrong\");", "if false {\n        d.err = Some(\"DMAR checksum wrong\");")
fw('fw-dmar-min-length', 'a DMAR shorter than its header is parsed', "if &sig != b\"DMAR\" || !(48..=TABLE_LIMIT).contains(&len) {", "if &sig != b\"DMAR\" || !(0..=TABLE_LIMIT).contains(&len) {")
fw('fw-dmar-max-length', 'a DMAR longer than the read bound is parsed', "if &sig != b\"DMAR\" || !(48..=TABLE_LIMIT).contains(&len) {", "if &sig != b\"DMAR\" || !(48..=TABLE_LIMIT * 4).contains(&len) {")
fw('fw-dmar-signature', 'the DMAR signature is not checked', "if &sig != b\"DMAR\" || !(48..=TABLE_LIMIT).contains(&len) {", "if !(48..=TABLE_LIMIT).contains(&len) {")
fw('fw-dmar-host-width', 'the DMAR host address width is not incremented', "d.host_width = hw[0].wrapping_add(1);", "d.host_width = hw[0];")
fw('fw-dmar-struct-min', 'a zero-length DMAR structure is accepted', "let (ty, slen) = (u16::from_le_bytes([h[0], h[1]]), u64::from(u16::from_le_bytes([h[2], h[3]])));\n        if slen < 4 || at.saturating_add(slen) > end {", "let (ty, slen) = (u16::from_le_bytes([h[0], h[1]]), u64::from(u16::from_le_bytes([h[2], h[3]])));\n        if at.saturating_add(slen) > end {")
fw('fw-dmar-struct-end', 'a DMAR structure may run past the table end', "let (ty, slen) = (u16::from_le_bytes([h[0], h[1]]), u64::from(u16::from_le_bytes([h[2], h[3]])));\n        if slen < 4 || at.saturating_add(slen) > end {", "let (ty, slen) = (u16::from_le_bytes([h[0], h[1]]), u64::from(u16::from_le_bytes([h[2], h[3]])));\n        if slen < 4 {")
fw('fw-dmar-loop-bound', 'only three DMAR structures are walked', "for _ in 0..64 {\n        if at.saturating_add(4) > end {\n            break;\n        }\n        let mut h = [0u8; 4];\n        if !read_at(p, at, &mut h) {\n            d.err = Some(\"DMAR unreadable\");", "for _ in 0..3 {\n        if at.saturating_add(4) > end {\n            break;\n        }\n        let mut h = [0u8; 4];\n        if !read_at(p, at, &mut h) {\n            d.err = Some(\"DMAR unreadable\");")
fw('fw-drhd-all-flag', 'INCLUDE_PCI_ALL is read from the wrong flag bit', "let all = flags & 1 != 0;", "let all = flags & 2 != 0;")
fw('fw-cover-order', 'an all-devices flag hides a named scope', "if named {\n                    Cover::Specific\n                } else if all {\n                    Cover::PciAll", "if all {\n                    Cover::PciAll\n                } else if named {\n                    Cover::Specific")
fw('fw-pciall-not-demoted', 'an all-devices unit keeps covering a device another unit names', "if u.cover == Cover::PciAll {\n                u.cover = Cover::No;", "if u.cover == Cover::PciAll {\n                u.cover = Cover::PciAll;")
fw('fw-scope-bus', 'a scope on another bus names the xHCI', "named |= bus == target.0 && path[0] == target.1 && path[1] == target.2;", "named |= path[0] == target.1 && path[1] == target.2;")
fw('fw-scope-function', 'a scope for another function names the xHCI', "named |= bus == target.0 && path[0] == target.1 && path[1] == target.2;", "named |= bus == target.0 && path[0] == target.1;")
fw('fw-scope-bridge', 'a bridge scope is not an unresolved one', "(1, _) | (2, _) => unknown = true,", "(1, _) => unknown = true,")
fw('fw-scope-nested', 'a nested scope path is not an unresolved one', "(1, _) | (2, _) => unknown = true,", "(2, _) => unknown = true,")
fw('fw-scope-short', 'a malformed scope is not an unresolved one', "if len < 6 || at.saturating_add(len) > end {\n            return (named, true);", "if len < 6 || at.saturating_add(len) > end {\n            return (named, false);")
fw('fw-rmrr-named', 'an RMRR naming the xHCI is not reported', "d.rmrr_names_xhci |= named;", "let _ = named;")
fw('fw-rmrr-count', 'RMRRs are not counted', "d.rmrr = d.rmrr.saturating_add(1);", "let _ = 0;")
fw('fw-unit-bound', 'DMAR units beyond the array are counted as stored', "if let Some(u) = d.units.get_mut(usize::from(d.n_units)) {\n                    *u = Unit { base, flags, segment, cover };\n                    d.n_units += 1;", "if let Some(u) = d.units.get_mut(usize::from(d.n_units).min(UNIT_LIMIT - 1)) {\n                    *u = Unit { base, flags, segment, cover };\n                    d.n_units += 1;")
fw('fw-ivrs-types', 'the IVHD type 0x40 is not a remapping block', "0x10 | 0x11 | 0x40 if slen >= 24 => {", "0x10 | 0x11 if slen >= 24 => {")
fw('fw-ivrs-base-offset', 'the IVHD base is read from the wrong offset', "if read_at(p, at.saturating_add(8), &mut b) {", "if read_at(p, at.saturating_add(12), &mut b) {")
fw('fw-ivrs-checksum', 'a wrong IVRS checksum is accepted', "if checksum(p, pa, len) != Some(0) {\n        v.err = Some(\"IVRS checksum wrong\");", "if false {\n        v.err = Some(\"IVRS checksum wrong\");")
fw('fw-ivrs-signature', 'the IVRS signature is not checked', "if &sig != b\"IVRS\" || !(48..=TABLE_LIMIT).contains(&len) {", "if !(48..=TABLE_LIMIT).contains(&len) {")
fw('fw-ivrs-mem', 'IVMD blocks are not counted', "0x20..=0x22 => v.ivmd = v.ivmd.saturating_add(1),", "0x20..=0x22 => {}")
fw('fw-intel-tes-bit', 'translation enable is read from the wrong bit', "self.responds && self.status >> 31 & 1 != 0", "self.responds && self.status >> 30 & 1 != 0")
fw('fw-intel-responds', 'a version of zero is an answer', "u.responds = ver != 0 && ver != 0xffff_ffff;", "u.responds = ver != 0xffff_ffff;")
fw('fw-intel-ones', 'a version of all ones is an answer', "u.responds = ver != 0 && ver != 0xffff_ffff;", "u.responds = ver != 0;")
fw('fw-intel-gsts', 'GSTS is read from the wrong offset', "m.read32(0x14), m.read32(0x1c))", "m.read32(0x14), m.read32(0x18))")
fw('fw-amd-bit', 'IommuEn is read from the wrong bit', "self.responds && self.status & 1 != 0", "self.responds && self.status & 2 != 0")
fw('fw-amd-ctl', 'the AMD control register is read from the wrong offset', "(m.read32(0x18), m.read32(0x30), m.read32(0x34))", "(m.read32(0x1c), m.read32(0x30), m.read32(0x34))")
fw('fw-amd-ones', 'an AMD unit that reads all ones answers', "u.responds = ctl != 0xffff_ffff;", "u.responds = true;")
fw('fw-unit-window-top', 'a unit base above the chipset window is read', "base % 0x1000 == 0 && base >= UNIT_WINDOW.0 && base <= UNIT_WINDOW.1", "base % 0x1000 == 0 && base >= UNIT_WINDOW.0")
fw('fw-unit-window-bottom', 'a unit base below the chipset window is read', "base % 0x1000 == 0 && base >= UNIT_WINDOW.0 && base <= UNIT_WINDOW.1", "base % 0x1000 == 0 && base <= UNIT_WINDOW.1")
fw('fw-unit-alignment', 'an unaligned unit base is read', "base % 0x1000 == 0 && base >= UNIT_WINDOW.0 && base <= UNIT_WINDOW.1", "base >= UNIT_WINDOW.0 && base <= UNIT_WINDOW.1")
fw('fw-facts-absence', 'a table that could not be read proves absence', "} else if tables.err.is_none() && !tables.more_than_listed {", "} else if true {")
fw('fw-facts-ivrs', 'an IVRS does not count as an IOMMU table', "f.table_present = if tables.dmar.is_some() || tables.ivrs.is_some() {", "f.table_present = if tables.dmar.is_some() {")
fw('fw-facts-no-units', 'a table without readable units says nothing more', "if found_units == 0 {\n        return f;\n    }", "if false {\n        return f;\n    }")
fw('fw-facts-silent-no', 'a unit not read at all is called silent', "} else if read == usize::from(found_units) {", "} else if true {")
fw('fw-facts-translation-no', 'translation off is concluded from one unit of several', "f.translation_enabled = if enabled > 0 {\n        Tri::Yes\n    } else if answering == usize::from(found_units) {", "f.translation_enabled = if enabled > 0 {\n        Tri::Yes\n    } else if answering > 0 {")
fw('fw-facts-amd-translation', 'AMD translation is read from the Intel bit', "filter(|(_, r, amd)| if *amd { r.amd_enabled() } else { r.intel_translation() }).count();", "filter(|(_, r, _)| r.intel_translation()).count();")
fw('fw-facts-cover-ignored', 'an enabled unit that does not cover the xHCI translates it', "matches!(c, Cover::Specific | Cover::PciAll) &&", "true &&")
fw('fw-facts-maybe', 'an unresolved scope is called no coverage', "let maybe = units.iter().any(|(c, _, _)| *c == Cover::Unknown);", "let maybe = false;")
fw('fw-facts-xhci-no', 'the xHCI is called untranslated before every unit was read', "} else if answering == usize::from(found_units) && !maybe &&", "} else if !maybe &&")
fw('fw-render-isolation', 'the report stops saying isolation is not established', "DMA isolation NOT ESTABLISHED", "DMA isolation established")
fw('fw-render-sigrows', 'all signatures are put on one line', "if i % 12 == 0 {", "if i % 120 == 0 {")
fw('fw-render-unit-regs', 'a unit whose registers were not read is not said to be unread', "registers NOT read (outside the chipset window or unreadable)", "registers read")

CF = {
    'pci-trait-has-no-write': ('use vos_xhci::xhci::survey::*;\nfn f(p: &mut impl Pci) { p.write32(0, 0, 0, 0, 1); }\nfn main() {}\n', 'E0599'),
    'mmio-trait-has-no-write': ('use vos_xhci::xhci::survey::*;\nfn f(m: &mut impl Mmio) { m.write32(0, 1); }\nfn main() {}\n', 'E0599'),
    'phys-trait-has-no-write': ('use vos_xhci::xhci::firmware::*;\nfn f(p: &mut impl Phys) { p.write(0, &[1]); }\nfn main() {}\n', 'E0599'),
    'scan-needs-a-pci-reader': ('use vos_xhci::xhci::survey::*;\nfn main() { let _ = scan(&mut 5u32); }\n', 'E0277'),
}
BANNED = re.compile(r'\bunsafe\b|\bextern\b|\basm!|\bglobal_asm!|\balloc::|\bstd::|\bstatic\s+mut\b')


def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--rustc', required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--only', help='comma-separated mutant names (debugging)')
    ap.add_argument('--workers-dir', type=Path, help='reuse these mutant build directories in place (build caches, not evidence)')
    args = ap.parse_args()
    out = args.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT / 'build'):
        ap.error('fresh output below this checkout build/ required')
    out.mkdir(parents=True)
    names = [m[0] for m in M]
    if len(set(names)) != len(names):
        raise SystemExit('duplicate mutant names')
    srcs = {n: (CRATE / n).read_text() for n in ('survey.rs', 'firmware.rs')}
    for name, _why, old, new, file in M:
        if srcs[file].count(old) != 1 or old == new:
            raise SystemExit(f'mutant {name}: anchor occurs {srcs[file].count(old)} times in {file} (or no change)')
    report = {'hosted_only': True, 'device_access': False, 'source_sha256': {}, 'commands': []}
    for p in sorted(CRATE.glob('*.rs')) + [SUITE, SUITE_FW, Path(__file__), ROOT / 'arch/x86_64/xhci_survey.c', ROOT / 'arch/x86_64/rust/xhci_survey.rs']:
        report['source_sha256'][str(p.relative_to(ROOT))] = sha(p)
    for p in sorted(CRATE.glob('*.rs')):
        if BANNED.search(p.read_text()):
            raise SystemExit('nonportable/unsafe source: ' + str(p))
    rust = [args.rustc, '--edition=2024']

    def run(argv, name, expected=0, env=None, timeout=900):
        argv = list(map(str, argv))
        report['commands'].append(argv)
        with (out / name).open('x') as f:
            p = subprocess.run(argv, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT, timeout=timeout,
                               env=dict(os.environ, PYTHONDONTWRITEBYTECODE='1', **(env or {})))
        if p.returncode != expected:
            raise RuntimeError(f'{name}: exit {p.returncode}, expected {expected}')
        return (out / name).read_text()

    try:
        rlib = out / 'libvos_xhci.rlib'
        run(rust + ['-Dwarnings', '--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cstrip=symbols', CRATE / 'lib.rs', '-o', rlib], 'crate-build.txt')
        passed = {}
        for tag, suite in (('survey', SUITE), ('firmware', SUITE_FW)):
            run(rust + ['-Dwarnings', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '-Cstrip=symbols', '--extern', f'vos_xhci={rlib}', suite, '-o', out / f'{tag}-tests'], f'{tag}-tests-build.txt')
            text = run([out / f'{tag}-tests', '--test-threads=1'], f'{tag}-tests.txt')
            m = re.search(r'test result: ok\. (\d+) passed; 0 failed', text)
            if not m:
                raise RuntimeError(f'{tag} tests did not pass')
            passed[tag] = int(m.group(1))
        report['tests_passed'] = sum(passed.values())
        report['tests_passed_by_suite'] = passed
        # compile-fail controls
        cf = {}
        for name, (code, want) in CF.items():
            f = out / f'cf-{name}.rs'
            f.write_text(code)
            log = run(rust + ['--extern', f'vos_xhci={rlib}', f, '-o', out / f'cf-{name}'], f'cf-{name}.txt', expected=1)
            if want not in log:
                raise RuntimeError(f'compile-fail control {name}: expected {want}')
            cf[name] = want
        report['compile_fail_controls'] = cf
        # mutation controls
        todo = [m for m in M if not args.only or m[0] in args.only.split(',')]
        workers = args.workers_dir.resolve() if args.workers_dir else out / 'mutant-workers'
        if args.workers_dir:
            if not workers.is_relative_to(ROOT / 'build'):
                raise RuntimeError('--workers-dir must be below build/')
            report['workers_dir_reused'] = str(workers.relative_to(ROOT))
        workers.mkdir(exist_ok=True)
        free = [f's{k}' for k in range(args.jobs)]
        lock = threading.Lock()

        def one(m):
            name, why, old, new, file = m
            with lock:
                w = free.pop()
            try:
                d = workers / w
                d.mkdir(exist_ok=True)
                md = d / 'xhci'
                if md.exists():
                    for f in CRATE.glob('*.rs'):
                        (md / f.name).write_text(f.read_text())
                else:
                    shutil.copytree(CRATE, md, ignore=shutil.ignore_patterns('target'))
                (md / file).write_text(srcs[file].replace(old, new))
                res = {'name': name, 'breaks': why, 'killed_by': [], 'failing_tests': []}

                def sh(argv, timeout=300):
                    return subprocess.run(list(map(str, argv)), cwd=ROOT, capture_output=True, text=True, timeout=timeout)
                b = sh(rust + ['--cap-lints=allow', '--crate-name', 'vos_xhci', '--crate-type=rlib', '-Coverflow-checks=yes', '-Cstrip=symbols', md / 'lib.rs', '-o', d / 'lib.rlib'])
                if b.returncode:
                    res.update(killed_by=['INVALID: mutant does not compile'], detail=b.stderr[-300:])
                    return res
                # the suite for the mutated file runs first; the other one only if that did not already kill it
                order = [SUITE, SUITE_FW] if file == 'survey.rs' else [SUITE_FW, SUITE]
                for suite in order:
                    b = sh(rust + ['--cap-lints=allow', '--test', '-Copt-level=1', '-Coverflow-checks=yes', '-Cstrip=symbols', '--extern', f'vos_xhci={d / "lib.rlib"}', suite, '-o', d / 'tests'])
                    if b.returncode:
                        res.update(killed_by=['INVALID: tests do not compile against the mutant'], detail=b.stderr[-300:])
                        return res
                    try:
                        t = sh([d / 'tests', '--test-threads=1'], timeout=300)
                        if t.returncode:
                            res['killed_by'].append('rust-tests')
                            res['killed_in'] = suite.name
                            res['failing_tests'] = re.findall(r'^test (\S+) \.\.\. FAILED$', t.stdout, re.M)[:6]
                    except subprocess.TimeoutExpired:
                        res['killed_by'].append('timeout')
                    if res['killed_by']:
                        break
                return res
            finally:
                with lock:
                    free.append(w)

        with ThreadPoolExecutor(args.jobs) as pool:
            results = list(pool.map(one, todo))
        survived = [r['name'] for r in results if not r['killed_by']]
        invalid = [r['name'] for r in results if any(k.startswith('INVALID') for k in r['killed_by'])]
        report['mutation_controls'] = results
        report['mutation_summary'] = {'total': len(results), 'killed': len(results) - len(survived), 'survived': survived, 'invalid': invalid}
        if survived or invalid:
            raise RuntimeError(f'mutation controls: survived={survived} invalid={invalid}')
        report['pass'] = True
    finally:
        report.setdefault('pass', False)
        (out / 'result.json').write_text(json.dumps(report, indent=1) + '\n')
    print(json.dumps({k: report[k] for k in ('pass', 'tests_passed', 'compile_fail_controls', 'mutation_summary') if k in report}, indent=1))


if __name__ == '__main__':
    main()
