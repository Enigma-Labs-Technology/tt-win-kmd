#!/usr/bin/env python3
"""Portable regression harness for selected actual driver function bodies.

The WDF queue/MMIO substitutes exercise algorithms, not Windows object/IRQL
semantics. This is developer evidence, never a replacement for build.ps1 -Test
or the VM/Verifier campaign. Requires Python 3 and a host C compiler.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    text = (ROOT / source).read_text()
    match = re.search(r"^(?:static )?(?:BOOLEAN|VOID|NTSTATUS)\n" + name + r"\(", text, re.M)
    if not match:
        raise AssertionError(f"Cannot locate {name}")
    start = text.index("{", match.start())
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


def run_c(source):
    with tempfile.TemporaryDirectory(prefix="tt-driver-contract-") as scratch:
        path = Path(scratch)
        (path / "test.c").write_text(source)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-pthread", "-Wall", "-Wextra", "-Werror",
                        "-I" + str(ROOT / "src/driver"), str(path / "test.c"), "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True, timeout=10)


class DriverContracts(unittest.TestCase):
    def test_refused_platform_reset_stays_closed_across_resume(self):
        harness = (ROOT / "test/host/lifecycle_harness.c").read_text()
        bodies = function("src/driver/reset.c", "TtPldrWorkItem") + "\n" + function("src/driver/device.c", "TtEvtDeviceD0Entry")
        run_c(harness.replace("/* DRIVER_FUNCTIONS */", bodies))

    def test_lock_waiters_beyond_previous_batch_limit(self):
        harness = (ROOT / "test/host/lock_queue_harness.c").read_text()
        bodies = function("src/driver/locks.c", "TtLockTryAcquire") + "\n" + function("src/driver/locks.c", "TtLocksWakeWaiters")
        run_c(harness.replace("/* DRIVER_FUNCTIONS */", bodies))

    def test_teardown_and_tlb_configuration_exclude_free(self):
        harness = (ROOT / "test/host/memory_harness.c").read_text()
        bodies = "\n".join(function("src/driver/memory.c", name) for name in
                           ["TtIoctlUnmap", "TtIoctlUnpinPages", "TtIoctlConfigureTlb", "TtMemoryReclaimStale"])
        bodies += "\n" + function("src/driver/ioctl.c", "TtCheckIoGates")
        run_c(harness.replace("/* DRIVER_FUNCTIONS */", bodies))

    def test_late_arc_reply_cannot_complete_another_command(self):
        harness = (ROOT / "test/host/arc_harness.c").read_text()
        run_c(harness.replace("/* DRIVER_FUNCTIONS */", function("src/driver/blackhole.c", "TtBhSendArcMessage")))

    def test_firmware_bounds_and_telemetry_publication(self):
        harness = (ROOT / "test/host/telemetry_harness.c").read_text()
        bodies = function("src/driver/blackhole.c", "TtBhCsmRangeValid") + "\n" + function("src/driver/blackhole.c", "TtBhTelemetryProbe")
        run_c(harness.replace("/* DRIVER_FUNCTIONS */", bodies))


if __name__ == "__main__":
    unittest.main()
