#!/usr/bin/env python3
"""Minimal GDB remote-serial-protocol client for QEMU's gdbstub (no gdb needed).

Used only as an independent observer: breakpoints/watchpoints are kept by QEMU
(TCG) and never modify guest memory; memory is read with 'm' packets. The
client never writes guest memory or registers.
"""
import socket


class GdbStub:
    def __init__(self, path, timeout=30):
        self.s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.s.settimeout(timeout)
        self.s.connect(str(path))
        self.buf = b''
        self.ack = True
        self.command('QStartNoAckMode')
        self.ack = False

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass

    def _send(self, data):
        body = data.encode()
        self.s.sendall(b'$' + body + b'#' + f'{sum(body) % 256:02x}'.encode())

    def _packet(self):
        while True:
            if b'$' in self.buf:
                start = self.buf.index(b'$')
                end = self.buf.find(b'#', start)
                if end >= 0 and len(self.buf) >= end + 3:
                    body = self.buf[start + 1:end]
                    if int(self.buf[end + 1:end + 3], 16) != sum(body) % 256:
                        raise RuntimeError('gdbstub checksum mismatch')
                    self.buf = self.buf[end + 3:]
                    if self.ack:
                        self.s.sendall(b'+')
                    return body.decode()
            chunk = self.s.recv(65536)
            if not chunk:
                raise RuntimeError('gdbstub closed')
            self.buf += chunk

    def command(self, data):
        self._send(data)
        reply = self._packet()
        while reply.startswith('O') and reply != 'OK':   # console output packets
            reply = self._packet()
        return reply

    def stop_reason(self):
        return self.command('?')

    def insert(self, kind, addr, length):
        r = self.command(f'Z{kind},{addr:x},{length:x}')
        if r != 'OK':
            raise RuntimeError(f'Z{kind} refused: {r!r}')

    def remove(self, kind, addr, length):
        r = self.command(f'z{kind},{addr:x},{length:x}')
        if r != 'OK':
            raise RuntimeError(f'z{kind} refused: {r!r}')

    def cont(self):
        """Resume and wait for the next stop reply."""
        self._send('c')
        reply = self._packet()
        while reply.startswith('O') and reply != 'OK':
            reply = self._packet()
        return reply

    def read(self, addr, n):
        out = b''
        while len(out) < n:
            k = min(1024, n - len(out))
            r = self.command(f'm{addr + len(out):x},{k:x}')
            if r.startswith('E') or len(r) != 2 * k:
                raise RuntimeError(f'memory read refused at {addr + len(out):#x}: {r[:8]!r}')
            out += bytes.fromhex(r)
        return out

    def u64(self, addr):
        return int.from_bytes(self.read(addr, 8), 'little')

    def detach(self):
        self._send('D')
        try:
            self._packet()
        except (RuntimeError, OSError):
            pass
