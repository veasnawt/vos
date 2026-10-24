#!/usr/bin/env python3
"""Focused fail-closed admission tests; no VOS execution."""
import json
from pathlib import Path
from unittest import TestCase, main
from unittest.mock import patch
import storage_stress_host as host


class Guards(TestCase):
    def setUp(self):
        self.s = object.__new__(host.Supervisor)
        self.s.root = Path('/no-write-test-root')
        self.s.archive = Path('/no-write-test-copy')

    def reject(self, capacities, extra=0, used=0):
        with patch.object(host, 'allocated', return_value=used), \
             patch.object(host, 'host_free', return_value=capacities), \
             patch.object(host.Path, 'open', side_effect=AssertionError('must fail before writing')):
            with self.assertRaises(RuntimeError):
                self.s.guard(extra)

    def test_insufficient_c(self):
        self.reject(dict(C=10*host.GIB, E=100*host.GIB))

    def test_insufficient_e(self):
        self.reject(dict(C=20*host.GIB, E=0), extra=host.MIB)

    def test_aggregate_limit(self):
        self.reject(dict(C=20*host.GIB, E=100*host.GIB), extra=host.LIMIT)

    def test_negative_admission(self):
        self.reject({}, extra=-1)

    def test_temporary_limit(self):
        self.reject(dict(C=20*host.GIB, E=100*host.GIB), used=65*host.MIB)

    def test_insufficient_linux(self):
        class FS:
            f_bavail = 1
            f_frsize = 4096
        with patch.object(host, 'allocated', return_value=0), \
             patch.object(host, 'host_free', return_value=dict(C=20*host.GIB, E=100*host.GIB)), \
             patch.object(host.os, 'statvfs', return_value=FS()), \
             patch.object(host.Path, 'open', side_effect=AssertionError('no evidence write')):
            with self.assertRaises(RuntimeError):
                self.s.guard()

    def test_measurement_failure(self):
        with patch.object(host, 'allocated', return_value=0), \
             patch.object(host, 'host_free', side_effect=ValueError('invalid measurement')):
            with self.assertRaises(ValueError):
                self.s.guard()

    def test_low_space_never_starts_child(self):
        with patch.object(self.s, 'guard', side_effect=RuntimeError('reserve')), \
             patch.object(host.subprocess, 'Popen', side_effect=AssertionError('child must not start')):
            with self.assertRaises(RuntimeError):
                self.s.run('test', ['not-executed'], extra=1)


if __name__ == '__main__':
    main()
