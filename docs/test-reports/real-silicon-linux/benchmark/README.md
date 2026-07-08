# ttkmd_bench — driver-level PCIe/BAR microbenchmark (Linux ↔ Windows comparison)

This is the benchmark to use for comparing the **Linux tt-kmd** and your
**Windows KMDF port** on the *same* p150a. It deliberately exercises **only the
kernel-driver ABI** — allocate a TLB window, configure it to a DRAM NoC core,
`mmap` the BAR aperture, and drive host CPU reads/writes across PCIe. There is
**no compute/runtime stack** involved, so it runs identically on both OSes and
isolates exactly what the driver port changes: BAR mapping, TLB setup, and the
MMIO path. (tt-metal's GEMM benchmark — the ~580 TFLOPS headline number — is
**Linux-only** and cannot serve as a cross-OS yardstick during the port.)

## Linux ground truth (this card)

Card: p150a `0000:c1:00.0`, Gen5 x16, **MPS 512 / MRRS 4096**, IOMMU on,
fw bundle 19.6.0.0, tt-kmd 2.9.1-pre, kernel 7.0.10-arch1-1. Card idle.
Numbers are extremely stable (±0.003 GB/s, ±0.2 ns across runs):

| Metric | Value | What it stresses |
|---|---|---|
| WC write (host→dev) | **~7.99 GB/s** | write-combining BAR store path |
| UC write (host→dev) | **~7.99 GB/s** | uncached posted store path |
| UC read (dev→host)  | **~0.043 GB/s** (43 MB/s) | non-posted read, latency-bound |
| MMIO read latency   | **~741 ns** | one PCIe non-posted read round trip |

Full captured output: `results-linux.txt`. Machine-readable JSON is printed on
the last line of every run for easy diffing.

## How to read these (and what to expect on Windows)

- **MMIO read latency (~741 ns) is the sharpest discriminator.** It is a pure
  PCIe read completion round trip and is very sensitive to how the driver sets
  up the BAR (caching, MPS/MRRS, IOMMU). If the Windows number is materially
  higher, suspect BAR caching attributes (make sure the equivalent of the WC/UC
  split is right) or MPS/MRRS not being restored to 512/4096. It cross-checks
  the reference set's independent ~700 ns uncached-read observation.
- **UC read ~43 MB/s** is low *by design* — every dword is a serialized,
  latency-bound non-posted read. It is a direct function of the latency above;
  Windows should land in the same ballpark if latency matches.
- **Write ~8 GB/s** is host→device MMIO throughput to a single DRAM NoC core.
  WC and UC measure identically here because the throughput is bounded
  downstream (single NoC/DRAM endpoint) and glibc `memcpy` uses streaming
  stores for large copies — so the number to match is the ~8 GB/s, not a
  WC-vs-UC delta. Use the *same* copy method on Windows for a fair compare.

## Running it

Linux:
```
TTKMD_TEST_DIR=/path/to/tt-kmd/test ./build-and-run.sh            # default dev + addr
TTKMD_TEST_DIR=/path/to/tt-kmd/test ./build-and-run.sh /dev/tenstorrent/0 0x10000000
```

Windows KMDF port: port `get_dram_window()` — the only OS-specific function —
to your driver's equivalent of ALLOCATE_TLB → CONFIGURE_TLB (DRAM core (17,12)
with NoC translation on, else (0,0)) → map the WC and UC views of the aperture.
Everything below the `=== timing loops (platform-neutral) ===` line in
`ttkmd_bench.cpp` is portable as-is. Match transfer sizes (512 MiB write / 16
MiB read / 200k latency reads) so the numbers are comparable.

## Scope / caveats

- This measures the **host-driven MMIO/BAR path**, which is the first thing a
  driver port brings up and the right first comparison. It does **not** measure
  the on-device **DMA-engine** bandwidth (the tens-of-GB/s figure), which
  requires programming a NoC DMA transfer via tt-umd/tt-metal — a sensible
  follow-on once the Windows port's DMA-buf/pin-pages path is up. When you get
  there, the ABI hooks are `ALLOCATE_DMA_BUF` / `PIN_PAGES` (+ the
  `NOC_DMA` flags) in `ioctl.h`.
- Writes target low DRAM (scratch on an idle card) exactly as the upstream TLB
  tests do; it is read-only w.r.t. firmware/persistent state.
- Run on an **idle** card (no other workload holding the device) for stable
  numbers; the driver's `power_policy` keeps aiclk low at idle, which is the
  same on both OSes, so the comparison stays fair.
