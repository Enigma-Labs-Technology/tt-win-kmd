// SPDX-License-Identifier: GPL-2.0-only
//
// ttkmd_bench — driver-level PCIe/BAR microbenchmark for Tenstorrent Blackhole.
//
// Purpose: produce a Linux ground-truth number for the host<->device MMIO path
// that a Windows KMDF port of tt-kmd must reproduce. It exercises ONLY the
// kernel-driver ABI (ALLOCATE_TLB / CONFIGURE_TLB / mmap of the BAR window),
// not any compute/runtime stack, so it is runnable identically on both OSes.
//
// What it measures (host CPU driving a BAR window aimed at a device DRAM core
// over the NoC):
//   1. WC write bandwidth  — host -> device, write-combining mapping (the fast,
//      burst path used to push data/programs to the device).
//   2. UC write bandwidth  — host -> device, uncached mapping (per-store,
//      posted-write path).
//   3. UC read  bandwidth  — device -> host, uncached (non-posted, latency-bound).
//   4. MMIO read latency   — dependent single-dword reads, ns per round trip.
//                            This is the sharpest Linux-vs-Windows discriminator:
//                            it is a pure PCIe non-posted completion round trip
//                            and is very sensitive to driver/BAR/MPS setup.
//
// PORTING NOTE for the Windows port: everything below the line "=== timing
// loops (platform-neutral) ===" is identical across OSes; only get_dram_window()
// (the ioctl/mmap dance) is driver-specific. Reproduce that to get comparable
// numbers.

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "ioctl.h"

#if defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#define STORE_FENCE() _mm_sfence()
#else
#define STORE_FENCE() __sync_synchronize()
#endif

namespace {

constexpr size_t TWO_MEG = 1ULL << 21;
constexpr uint64_t NIU_CFG_BAR0_OFFSET = 0x1FD04100; // for NoC-translation probe
constexpr size_t BAR0_SIZE = 1ULL << 29;             // 512 MiB

using clk = std::chrono::steady_clock;

double secs(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

[[noreturn]] void die(const char *msg) {
    std::perror(msg);
    std::exit(1);
}

bool noc_translation_enabled(int fd) {
    void *mem = mmap(nullptr, BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED)
        die("mmap BAR0 for translation probe");
    auto *base = static_cast<volatile uint8_t *>(mem);
    uint32_t niu_cfg = *reinterpret_cast<volatile uint32_t *>(base + NIU_CFG_BAR0_OFFSET);
    munmap(mem, BAR0_SIZE);
    return (niu_cfg >> 14) & 1;
}

// === driver-specific setup (the part a Windows port must re-implement) ===
// Allocate a 2 MiB TLB window, point it at a DRAM NoC core at `addr`, and map it
// both write-combining and uncached. Returns the two user pointers.
struct Window {
    int fd = -1;
    uint32_t id = 0;
    void *wc = nullptr;
    void *uc = nullptr;
    size_t size = 0;
};

Window get_dram_window(int fd, uint64_t addr) {
    bool translated = noc_translation_enabled(fd);
    // DRAM NoC core: (17,12) with NoC translation on, (0,0) with it off.
    uint16_t x = translated ? 17 : 0;
    uint16_t y = translated ? 12 : 0;

    tenstorrent_allocate_tlb alloc{};
    alloc.in.size = TWO_MEG;
    if (ioctl(fd, TENSTORRENT_IOCTL_ALLOCATE_TLB, &alloc) != 0)
        die("ALLOCATE_TLB");

    tenstorrent_configure_tlb cfg{};
    cfg.in.id = alloc.out.id;
    cfg.in.config.addr = addr & ~(TWO_MEG - 1);
    cfg.in.config.x_end = x;
    cfg.in.config.y_end = y;
    if (ioctl(fd, TENSTORRENT_IOCTL_CONFIGURE_TLB, &cfg) != 0)
        die("CONFIGURE_TLB");

    void *wc = mmap(nullptr, TWO_MEG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, alloc.out.mmap_offset_wc);
    if (wc == MAP_FAILED) die("mmap WC");
    void *uc = mmap(nullptr, TWO_MEG, PROT_READ | PROT_WRITE, MAP_SHARED, fd, alloc.out.mmap_offset_uc);
    if (uc == MAP_FAILED) die("mmap UC");

    return Window{fd, alloc.out.id, wc, uc, TWO_MEG};
}

void put_window(Window &w) {
    if (w.wc) munmap(w.wc, w.size);
    if (w.uc) munmap(w.uc, w.size);
    tenstorrent_free_tlb ft{};
    ft.in.id = w.id;
    ioctl(w.fd, TENSTORRENT_IOCTL_FREE_TLB, &ft);
}

// === timing loops (platform-neutral) ===

// Bandwidth: move `total` bytes through the window in `size`-byte memcpys.
double bw_write(void *dst, const void *src, size_t size, size_t total) {
    size_t passes = total / size;
    auto t0 = clk::now();
    for (size_t i = 0; i < passes; ++i)
        std::memcpy(dst, src, size);
    STORE_FENCE(); // flush any write-combining buffers before we stop the clock
    auto t1 = clk::now();
    return (double)(passes * size) / secs(t0, t1) / 1e9; // GB/s
}

volatile uint64_t g_sink = 0; // consumed at exit to defeat dead-store elimination

double bw_read(void *src, void *dst, size_t size, size_t total) {
    size_t passes = total / size;
    auto t0 = clk::now();
    for (size_t i = 0; i < passes; ++i)
        std::memcpy(dst, src, size);
    auto t1 = clk::now();
    // fold the read data into a sink so the compiler cannot delete the memcpy
    uint64_t acc = 0;
    auto *p = static_cast<uint64_t *>(dst);
    for (size_t i = 0; i < size / sizeof(uint64_t); ++i) acc += p[i];
    g_sink += acc;
    return (double)(passes * size) / secs(t0, t1) / 1e9; // GB/s
}

double best_of(double (*fn)(void *, void *, size_t, size_t),
               void *a, void *b, size_t size, size_t total, int iters) {
    double best = 0;
    for (int i = 0; i < iters; ++i) best = std::max(best, fn(a, b, size, total));
    return best;
}

double best_of(double (*fn)(void *, const void *, size_t, size_t),
               void *a, const void *b, size_t size, size_t total, int iters) {
    double best = 0;
    for (int i = 0; i < iters; ++i) best = std::max(best, fn(a, b, size, total));
    return best;
}

// Latency: dependent chain of single-dword uncached reads. Each read's value is
// folded into the next address so the CPU cannot pipeline them — this times one
// PCIe non-posted read round trip.
double read_latency_ns(void *uc_base, size_t count) {
    volatile uint32_t *p = static_cast<volatile uint32_t *>(uc_base);
    uint32_t acc = 0;
    auto t0 = clk::now();
    for (size_t i = 0; i < count; ++i) {
        // stay within the 2M window; index depends on previously read data
        size_t idx = (acc & 0x3ffff); // 0..256K dwords = 1 MiB
        acc += p[idx];
    }
    auto t1 = clk::now();
    // consume acc so the loop isn't optimized away
    if (acc == 0xdeadbeef) std::fprintf(stderr, "%u", acc);
    return secs(t0, t1) / count * 1e9;
}

} // namespace

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/dev/tenstorrent/0";
    // DRAM byte offset to hit; aligned to window size, kept in low DRAM (scratch
    // on an idle card, exactly as the upstream TLB tests write/read it).
    uint64_t dram_addr = (argc > 2) ? strtoull(argv[2], nullptr, 0) : 0x10000000ULL;

    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) die(path);

    tenstorrent_get_device_info info{};
    info.in.output_size_bytes = sizeof(info.out);
    if (ioctl(fd, TENSTORRENT_IOCTL_GET_DEVICE_INFO, &info) != 0)
        die("GET_DEVICE_INFO");

    tenstorrent_get_driver_info dinfo{};
    dinfo.in.output_size_bytes = sizeof(dinfo.out);
    ioctl(fd, TENSTORRENT_IOCTL_GET_DRIVER_INFO, &dinfo);

    std::printf("device %04x:%04x  bus %02x:%02x.%x  driver ABI v%u (%u.%u.%u)\n",
                info.out.vendor_id, info.out.device_id,
                (info.out.bus_dev_fn >> 8) & 0xff, (info.out.bus_dev_fn >> 3) & 0x1f,
                info.out.bus_dev_fn & 0x7,
                dinfo.out.driver_version, dinfo.out.driver_version_major,
                dinfo.out.driver_version_minor, dinfo.out.driver_version_patch);

    Window w = get_dram_window(fd, dram_addr);

    // host-side scratch buffers
    std::vector<uint8_t> hostbuf(TWO_MEG);
    for (size_t i = 0; i < TWO_MEG; ++i) hostbuf[i] = (uint8_t)i;
    std::vector<uint8_t> readback(TWO_MEG);

    constexpr size_t WR_TOTAL = 512ULL << 20; // 512 MiB per write test (posted, fast)
    constexpr size_t RD_TOTAL = 16ULL << 20;  // 16 MiB per read test (uncached, latency-bound)
    constexpr size_t LAT_N = 200000;          // dependent reads for latency
    constexpr int ITERS = 3;                  // best-of, to shed scheduler noise

    // warm up (fault in mappings, prime the path)
    std::memcpy(w.wc, hostbuf.data(), TWO_MEG);
    STORE_FENCE();
    (void)read_latency_ns(w.uc, 1000);

    double wc_wr = best_of(bw_write, w.wc, hostbuf.data(), TWO_MEG, WR_TOTAL, ITERS);
    double uc_wr = best_of(bw_write, w.uc, hostbuf.data(), TWO_MEG, WR_TOTAL, ITERS);
    double uc_rd = best_of(bw_read,  w.uc, readback.data(), TWO_MEG, RD_TOTAL, ITERS);
    double lat   = 1e30;
    for (int i = 0; i < ITERS; ++i) lat = std::min(lat, read_latency_ns(w.uc, LAT_N));

    std::printf("\n  window: 2 MiB BAR aperture -> DRAM NoC core; best of %d\n", ITERS);
    std::printf("  %-30s %8.2f GB/s   (%zu MiB moved)\n", "WC write  (host->dev):", wc_wr, WR_TOTAL >> 20);
    std::printf("  %-30s %8.2f GB/s   (%zu MiB moved)\n", "UC write  (host->dev):", uc_wr, WR_TOTAL >> 20);
    std::printf("  %-30s %8.2f GB/s   (%zu MiB moved)\n", "UC read   (dev->host):", uc_rd, RD_TOTAL >> 20);
    std::printf("  %-30s %8.1f ns     (%zu reads)\n",     "MMIO read latency:", lat, LAT_N);

    // machine-readable line for easy Linux-vs-Windows diffing
    std::printf("\nJSON {\"wc_write_GBps\": %.3f, \"uc_write_GBps\": %.3f, "
                "\"uc_read_GBps\": %.3f, \"read_latency_ns\": %.2f}\n",
                wc_wr, uc_wr, uc_rd, lat);

    put_window(w);
    close(fd);
    return 0;
}
