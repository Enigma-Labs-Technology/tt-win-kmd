// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd/ioctl.h (Windows ABI surface — M0 subset: GUIDs + CTL_CODE scheme)
//
// Shared between the driver and user mode. The full ioctl set lands milestone by
// milestone; every struct added here must carry static_asserts against
// docs/abi-ground-truth.txt (generated from the Linux header by tools/gen_abi_truth.sh).
//
// Kernel-mode consumers: include after ntddk.h/wdm.h.
// User-mode consumers: include after windows.h + winioctl.h.
// Exactly one translation unit per binary must include <initguid.h> beforehand to
// instantiate the GUIDs.

#ifndef TTKMD_IOCTL_H_INCLUDED
#define TTKMD_IOCTL_H_INCLUDED

#include <guiddef.h>

// Device interface for Tenstorrent accelerators (docs/design-decisions.md DD-2).
// Replaces /dev/tenstorrent/N discovery on Linux.
// {e2e020f2-998c-4b6f-b98e-b259372c7986}
DEFINE_GUID(GUID_DEVINTERFACE_TENSTORRENT,
    0xe2e020f2, 0x998c, 0x4b6f, 0xb9, 0x8e, 0xb2, 0x59, 0x37, 0x2c, 0x79, 0x86);

// CTL_CODE scheme (DD-4): custom device type echoing the Linux ioctl magic 0xFA
// (tt-kmd/ioctl.h:12); function code = 0x800 + Linux ioctl nr, METHOD_BUFFERED,
// FILE_ANY_ACCESS. See docs/ioctl-parity-matrix.md for the full table.
#define TT_DEVICE_TYPE 0x80FAu
#define TT_CTL(nr) CTL_CODE(TT_DEVICE_TYPE, 0x800u + (nr), METHOD_BUFFERED, FILE_ANY_ACCESS)

// IOCTL API version; parity with TENSTORRENT_DRIVER_VERSION (tt-kmd/ioctl.h:10).
#define TENSTORRENT_DRIVER_VERSION 2

// Linux ioctl magic (tt-kmd/ioctl.h:12). The shim uses it to decode the
// TENSTORRENT_IOCTL_* request values tt-umd passes (which are _IO(0xFA, nr)).
#define TENSTORRENT_IOCTL_MAGIC 0xFA

// Windows CTL_CODEs. One per Linux ioctl nr (tt-kmd/ioctl.h:14-30); codes are
// 0x80FA2000 + 4*nr. Handlers land milestone by milestone (parity matrix).
#define IOCTL_TENSTORRENT_GET_DEVICE_INFO   TT_CTL(0)
#define IOCTL_TENSTORRENT_GET_HARVESTING    TT_CTL(1)
#define IOCTL_TENSTORRENT_QUERY_MAPPINGS    TT_CTL(2)
#define IOCTL_TENSTORRENT_ALLOCATE_DMA_BUF  TT_CTL(3)
#define IOCTL_TENSTORRENT_FREE_DMA_BUF      TT_CTL(4)
#define IOCTL_TENSTORRENT_GET_DRIVER_INFO   TT_CTL(5)
#define IOCTL_TENSTORRENT_RESET_DEVICE      TT_CTL(6)
#define IOCTL_TENSTORRENT_PIN_PAGES         TT_CTL(7)
#define IOCTL_TENSTORRENT_LOCK_CTL          TT_CTL(8)
#define IOCTL_TENSTORRENT_MAP_PEER_BAR      TT_CTL(9)
#define IOCTL_TENSTORRENT_UNPIN_PAGES       TT_CTL(10)
#define IOCTL_TENSTORRENT_ALLOCATE_TLB      TT_CTL(11)
#define IOCTL_TENSTORRENT_FREE_TLB          TT_CTL(12)
#define IOCTL_TENSTORRENT_CONFIGURE_TLB     TT_CTL(13)
#define IOCTL_TENSTORRENT_SET_NOC_CLEANUP   TT_CTL(14)  // struct tenstorrent_set_noc_cleanup
#define IOCTL_TENSTORRENT_SET_POWER_STATE   TT_CTL(15)
#define IOCTL_TENSTORRENT_EXPORT_TLB_DMABUF TT_CTL(16)
#define IOCTL_TENSTORRENT_SMC_MSG           TT_CTL(17)  // tt-kmd 2.11.0 (ioctl.h:31)

// Mapping tokens returned by QUERY_MAPPINGS (tt-kmd/ioctl.h:33-39). The
// mapping_base values are the Linux mmap-offset-space tokens ((0..5)<<36,
// memory.c:255-274) and are preserved opaquely on Windows (consumed by the
// MAP ioctl from M3 onward).
#define TENSTORRENT_MAPPING_UNUSED       0u
#define TENSTORRENT_MAPPING_RESOURCE0_UC 1u
#define TENSTORRENT_MAPPING_RESOURCE0_WC 2u
#define TENSTORRENT_MAPPING_RESOURCE1_UC 3u
#define TENSTORRENT_MAPPING_RESOURCE1_WC 4u
#define TENSTORRENT_MAPPING_RESOURCE2_UC 5u
#define TENSTORRENT_MAPPING_RESOURCE2_WC 6u

// Fixed-width types: kernel mode must not include the UCRT's <stdint.h>
// (vcruntime.h conflicts with /kernel + km headers), so define the exact-width
// types locally there; user mode uses the real <stdint.h>.
#ifdef _KERNEL_MODE
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef signed char int8_t;
typedef short int16_t;
typedef int int32_t;
typedef long long int64_t;
#else
#include <stdint.h>
#endif

// Struct layouts are byte-identical to tt-kmd/ioctl.h (gcc x86_64 ground truth
// in docs/abi-ground-truth.txt, enforced by ttkmd_abi_check.h). The Linux
// convention is one struct with [in][out] halves; callers pass the same struct
// as both input and output buffer of DeviceIoControl.

#pragma pack(push, 8)

// --- GET_DEVICE_INFO (nr 0), tt-kmd/ioctl.h:46-65 ---
struct tenstorrent_get_device_info_in {
    uint32_t output_size_bytes;
};

struct tenstorrent_get_device_info_out {
    uint32_t output_size_bytes;      // set by driver to sizeof(out) = 20
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint16_t bus_dev_fn;             // [0:2] function, [3:7] device, [8:15] bus
    uint16_t max_dma_buf_size_log2;  // 28 (tt-kmd/memory.h:10)
    uint16_t pci_domain;
    uint16_t reserved;
};

struct tenstorrent_get_device_info {
    struct tenstorrent_get_device_info_in in;
    struct tenstorrent_get_device_info_out out;
};

// --- QUERY_MAPPINGS (nr 2), tt-kmd/ioctl.h:67-86 ---
struct tenstorrent_query_mappings_in {
    uint32_t output_mapping_count;   // count of tenstorrent_mapping slots
    uint32_t reserved;
};

struct tenstorrent_mapping {
    uint32_t mapping_id;             // TENSTORRENT_MAPPING_*
    uint32_t reserved;
    uint64_t mapping_base;           // opaque map token ((0..5)<<36)
    uint64_t mapping_size;
};

// Linux declares out as a flexible array (tenstorrent_mapping mappings[0]);
// MSVC forbids that, so Windows callers place the mapping array immediately
// after tenstorrent_query_mappings_in in the same buffer.
struct tenstorrent_query_mappings {
    struct tenstorrent_query_mappings_in in;
};

// --- GET_DRIVER_INFO (nr 5), tt-kmd/ioctl.h:124-140 ---
struct tenstorrent_get_driver_info_in {
    uint32_t output_size_bytes;
};

struct tenstorrent_get_driver_info_out {
    uint32_t output_size_bytes;      // set by driver to sizeof(out) = 12
    uint32_t driver_version;         // TENSTORRENT_DRIVER_VERSION
    uint8_t driver_version_major;
    uint8_t driver_version_minor;
    uint8_t driver_version_patch;
    uint8_t reserved0;
};

struct tenstorrent_get_driver_info {
    struct tenstorrent_get_driver_info_in in;
    struct tenstorrent_get_driver_info_out out;
};

// --- ALLOCATE_DMA_BUF (nr 3), tt-kmd/ioctl.h:88-111 ---
#define TENSTORRENT_MAX_DMA_BUFS 256
#define TENSTORRENT_ALLOCATE_DMA_BUF_NOC_DMA 2

struct tenstorrent_allocate_dma_buf_in {
    uint32_t requested_size;   // page-multiple, <= 1<<28
    uint8_t buf_index;         // [0, TENSTORRENT_MAX_DMA_BUFS)
    uint8_t flags;
    uint8_t reserved0[2];
    uint64_t reserved1[2];
};

struct tenstorrent_allocate_dma_buf_out {
    uint64_t physical_address; // bus/logical address
    uint64_t mapping_offset;   // MAP token: 0xF00_0000_0000 + idx*4GiB
    uint32_t size;
    uint32_t reserved0;
    uint64_t noc_address;      // valid if NOC_DMA flag set
    uint64_t reserved1;
};

struct tenstorrent_allocate_dma_buf {
    struct tenstorrent_allocate_dma_buf_in in;
    struct tenstorrent_allocate_dma_buf_out out;
};

// --- PIN_PAGES (nr 7) / UNPIN_PAGES (nr 10), tt-kmd/ioctl.h:168-208 ---
#define TENSTORRENT_PIN_PAGES_CONTIGUOUS 1   // vestigial (contiguity always checked)
#define TENSTORRENT_PIN_PAGES_NOC_DMA 2
#define TENSTORRENT_PIN_PAGES_NOC_TOP_DOWN 4 // implies a NOC mapping even alone
#define TENSTORRENT_PIN_PAGES_READ_ONLY 8    // requires IOMMU: STATUS_NOT_SUPPORTED here

struct tenstorrent_pin_pages_in {
    uint32_t output_size_bytes;
    uint32_t flags;
    uint64_t virtual_address;  // page-aligned
    uint64_t size;             // page-multiple, nonzero
};

struct tenstorrent_pin_pages_out {
    uint64_t physical_address;
};

struct tenstorrent_pin_pages_out_extended {
    uint64_t physical_address;
    uint64_t noc_address;
};

struct tenstorrent_pin_pages {
    struct tenstorrent_pin_pages_in in;
    struct tenstorrent_pin_pages_out out;
};

struct tenstorrent_unpin_pages_in {
    uint64_t virtual_address;  // original pin VA
    uint64_t size;             // must match the pin exactly
    uint64_t reserved;         // must be 0
};

struct tenstorrent_unpin_pages {
    struct tenstorrent_unpin_pages_in in;
};

// --- ALLOCATE/FREE/CONFIGURE_TLB (nrs 11-13), tt-kmd/ioctl.h:270-328 ---
#define TENSTORRENT_MAX_INBOUND_TLBS 256

struct tenstorrent_allocate_tlb_in {
    uint64_t size;             // exact pool size: 2 MiB or 4 GiB on Blackhole
    uint64_t reserved;
};

struct tenstorrent_allocate_tlb_out {
    uint32_t id;
    uint32_t reserved0;
    uint64_t mmap_offset_uc;   // MAP token
    uint64_t mmap_offset_wc;   // MAP token
    uint64_t reserved1;
};

struct tenstorrent_allocate_tlb {
    struct tenstorrent_allocate_tlb_in in;
    struct tenstorrent_allocate_tlb_out out;
};

struct tenstorrent_free_tlb_in {
    uint32_t id;
};

struct tenstorrent_free_tlb {
    struct tenstorrent_free_tlb_in in;
};

struct tenstorrent_noc_tlb_config {
    uint64_t addr;             // window-size aligned NOC address
    uint16_t x_end;
    uint16_t y_end;
    uint16_t x_start;
    uint16_t y_start;
    uint8_t noc;
    uint8_t mcast;
    uint8_t ordering;
    uint8_t linked;
    uint8_t static_vc;
    uint8_t reserved0[3];
    uint32_t reserved1[2];
};

struct tenstorrent_configure_tlb_in {
    uint32_t id;
    uint32_t reserved;
    struct tenstorrent_noc_tlb_config config;
};

struct tenstorrent_configure_tlb_out {
    uint64_t reserved;
};

struct tenstorrent_configure_tlb {
    struct tenstorrent_configure_tlb_in in;
    struct tenstorrent_configure_tlb_out out;
};

// --- LOCK_CTL (nr 8), tt-kmd/ioctl.h:210-249 ---
#define TENSTORRENT_LOCK_CTL_ACQUIRE 0          // out.value: 1=acquired, 0=held by another
#define TENSTORRENT_LOCK_CTL_RELEASE 1          // out.value: 1=released, 0=not held by us
#define TENSTORRENT_LOCK_CTL_TEST 2             // out.value: bit0=held by us, bit1=held by any
#define TENSTORRENT_LOCK_CTL_ACQUIRE_BLOCKING 3 // blocks until acquired, out.value: 1
#define TENSTORRENT_RESOURCE_LOCK_COUNT 64

struct tenstorrent_lock_ctl_in {
    uint32_t output_size_bytes;
    uint32_t flags;            // TENSTORRENT_LOCK_CTL_*
    uint8_t index;             // [0, 64)
    uint8_t reserved[3];
};

struct tenstorrent_lock_ctl_out {
    uint8_t value;
    uint8_t reserved[3];
};

struct tenstorrent_lock_ctl {
    struct tenstorrent_lock_ctl_in in;
    struct tenstorrent_lock_ctl_out out;
};

// --- SET_POWER_STATE (nr 15), tt-kmd/ioctl.h:361-411 ---
#define TT_POWER_VALIDITY_FLAGS(n)    (((n) & 0xF) << 0)
#define TT_POWER_VALIDITY_SETTINGS(n) (((n) & 0xF) << 4)
#define TT_POWER_VALIDITY(f, s) (TT_POWER_VALIDITY_FLAGS(f) | TT_POWER_VALIDITY_SETTINGS(s))
#define TT_POWER_FLAG_MAX_AI_CLK       (1u << 0)
#define TT_POWER_FLAG_MRISC_PHY_WAKEUP (1u << 1)
#define TT_POWER_FLAG_TENSIX_ENABLE    (1u << 2)
#define TT_POWER_FLAG_L2CPU_ENABLE     (1u << 3)

struct tenstorrent_power_state {
    uint32_t argsz;            // must == sizeof(struct) = 40
    uint32_t flags;            // must == 0
    uint8_t reserved0;         // must == 0
    uint8_t validity;          // TT_POWER_VALIDITY(flags_count, settings_count)
    uint16_t power_flags;      // TT_POWER_FLAG_*
    uint16_t power_settings[14];
};

// --- SET_NOC_CLEANUP (nr 14), tt-kmd/ioctl.h:349-359 ---
// Registers a NOC write performed at handle close (device-side cleanup if the
// app dies). argsz protocol.
struct tenstorrent_set_noc_cleanup {
    uint32_t argsz;            // must == sizeof(struct) = 32
    uint32_t flags;            // must == 0
    uint8_t enabled;           // 1 to register, 0 to clear
    uint8_t x;
    uint8_t y;
    uint8_t noc;               // 0 or 1
    uint32_t reserved0;
    uint64_t addr;             // 4-byte aligned
    uint64_t data;            // upper 32 bits ignored
};

// --- RESET_DEVICE (nr 6), tt-kmd/ioctl.h:142-166 ---
// flags is an enum value, not a bitmask (chardev.c dispatches with == chains).
#define TENSTORRENT_RESET_DEVICE_RESTORE_STATE 0
#define TENSTORRENT_RESET_DEVICE_RESET_PCIE_LINK 1
#define TENSTORRENT_RESET_DEVICE_CONFIG_WRITE 2
#define TENSTORRENT_RESET_DEVICE_USER_RESET 3
#define TENSTORRENT_RESET_DEVICE_ASIC_RESET 4
#define TENSTORRENT_RESET_DEVICE_ASIC_DMC_RESET 5
#define TENSTORRENT_RESET_DEVICE_POST_RESET 6

struct tenstorrent_reset_device_in {
    uint32_t output_size_bytes;
    uint32_t flags;
};

struct tenstorrent_reset_device_out {
    uint32_t output_size_bytes;
    uint32_t result;           // 0 = success, 1 = failure (out.result = !ok)
};

struct tenstorrent_reset_device {
    struct tenstorrent_reset_device_in in;
    struct tenstorrent_reset_device_out out;
};

// --- EXPORT_TLB_DMABUF (nr 16), tt-kmd/ioctl.h:376-457 ---
// Linux exports a TLB window as a dma-buf fd for RDMA peer-to-peer. There is no
// Windows equivalent; the layout is carried for ABI parity and the driver
// returns STATUS_NOT_SUPPORTED.
struct tenstorrent_export_tlb_dmabuf {
    uint32_t argsz;            // must == sizeof(struct) = 32
    uint32_t flags;            // must == 0
    uint32_t tlb_id;
    int32_t  fd;               // out (Linux only)
    uint64_t offset;
    uint64_t size;
};

// --- SMC_MSG (nr 17), tt-kmd/ioctl.h:459-505 (added in 2.11.0) ---
// Asynchronous per-handle ARC (system management controller) message exchange.
// The driver multiplexes one firmware queue across all handles; each handle may
// have one message outstanding. POST submits, POLL collects (or -EAGAIN while
// pending, -ESRCH with nothing outstanding), ABANDON cancels. Devices without a
// usable queue report -EOPNOTSUPP.
#define TENSTORRENT_SMC_MSG_POST    (1u << 0)
#define TENSTORRENT_SMC_MSG_POLL    (1u << 1)
#define TENSTORRENT_SMC_MSG_ABANDON (1u << 2)

struct tenstorrent_smc_msg {
    uint32_t argsz;            // must == sizeof(struct) = 48
    uint32_t flags;            // exactly one of TENSTORRENT_SMC_MSG_*
    uint32_t queue_index;      // must == 0
    uint32_t reserved0;        // must == 0
    uint32_t message[8];       // request on POST, response on successful POLL
};

// --- Windows extensions: MAP/UNMAP (function codes 0x900+, DD-8) ------------
// Replace mmap/munmap. mmap_offset is the opaque Linux token from
// QUERY_MAPPINGS.mapping_base / ALLOCATE_DMA_BUF.mapping_offset /
// ALLOCATE_TLB.mmap_offset_uc/wc, plus any byte offset within the region
// (tt-umd adds BAR-relative offsets to mapping_base). Mappings are per-handle
// and force-unmapped at handle close.
#define TT_CTL_EXT(nr) \
    CTL_CODE(TT_DEVICE_TYPE, 0x900u + (nr), METHOD_BUFFERED, FILE_ANY_ACCESS)

#define IOCTL_TENSTORRENT_MAP   TT_CTL_EXT(0)
#define IOCTL_TENSTORRENT_UNMAP TT_CTL_EXT(1)

// FREE_DMA_BUF_EX (DD-16): releases one DMA buffer allocated by
// ALLOCATE_DMA_BUF before the handle closes. Linux's FREE_DMA_BUF (nr 4)
// carries an empty payload and still returns -EINVAL upstream, so Windows
// adds a private request that names the slot. Fails with STATUS_DEVICE_BUSY
// while a MAP view or a PIN_PAGES registration still references the buffer.
#define IOCTL_TENSTORRENT_FREE_DMA_BUF_EX TT_CTL_EXT(4)

struct tenstorrent_free_dma_buf_ex_in {
    uint32_t buf_index;        // [0, TENSTORRENT_MAX_DMA_BUFS)
    uint32_t reserved;         // must be 0
};

struct tenstorrent_free_dma_buf_ex {
    struct tenstorrent_free_dma_buf_ex_in in;
};

// Telemetry query (hwmon-equivalent; Windows has no hwmon). Values use the
// EXACT Linux hwmon ABI scaling/units so tooling ports mechanically. Each
// field's `present` bit is set only if the underlying firmware tag exists
// (hwmon is_visible parity — absent tags are hidden on Linux).
#define IOCTL_TENSTORRENT_QUERY_TELEMETRY TT_CTL_EXT(2)

// present-mask bits
#define TT_TELEM_PRESENT_TEMP        (1u << 0)   // temp1_input/_max
#define TT_TELEM_PRESENT_VCORE       (1u << 1)   // in0_input/_max
#define TT_TELEM_PRESENT_CURRENT     (1u << 2)   // curr1_input/_max
#define TT_TELEM_PRESENT_POWER       (1u << 3)   // power1_input/_max
#define TT_TELEM_PRESENT_FAN         (1u << 4)   // fan1_input (BH only)
#define TT_TELEM_PRESENT_AICLK       (1u << 5)   // tt_aiclk
#define TT_TELEM_PRESENT_AXICLK      (1u << 6)   // tt_axiclk
#define TT_TELEM_PRESENT_ARCCLK      (1u << 7)   // tt_arcclk
#define TT_TELEM_PRESENT_HEARTBEAT   (1u << 8)   // tt_heartbeat
#define TT_TELEM_PRESENT_THERMTRIP   (1u << 9)   // tt_therm_trip_count (BH)
#define TT_TELEM_PRESENT_ASIC_ID     (1u << 10)  // tt_asic_id
#define TT_TELEM_PRESENT_SERIAL      (1u << 11)  // tt_serial / board id
#define TT_TELEM_PRESENT_FW_BUNDLE   (1u << 12)  // tt_fw_bundle_ver

struct tenstorrent_query_telemetry_out {
    uint32_t present;          // TT_TELEM_PRESENT_* bitmask
    int32_t  temp_input_mc;    // temp1_input, milli-degC (tag 11, 16.16 fixed)
    int32_t  temp_max_mc;      // temp1_max, milli-degC (tag 56)
    uint32_t vcore_input_mv;   // in0_input, mV (tag 6)
    uint32_t vcore_max_mv;     // in0_max, mV (tag 9 upper 16)
    uint32_t curr_input_ma;    // curr1_input, mA (tag 8)
    uint32_t curr_max_ma;      // curr1_max, mA (tag 55)
    uint32_t power_input_uw;   // power1_input, microwatts (tag 7)
    uint32_t power_max_uw;     // power1_max, microwatts (tag 64)
    uint32_t fan_rpm;          // fan1_input, RPM (tag 41)
    uint32_t aiclk_mhz;        // tt_aiclk (tag 14)
    uint32_t axiclk_mhz;       // tt_axiclk (tag 15)
    uint32_t arcclk_mhz;       // tt_arcclk (tag 16)
    uint32_t heartbeat;        // tt_heartbeat (tag 32)
    uint32_t therm_trip_count; // tt_therm_trip_count (tag 60)
    uint32_t fw_bundle_ver;    // tt_fw_bundle_ver (tag 28)
    uint64_t asic_id;          // tt_asic_id (tags 61<<32 | 62)
    uint64_t serial;           // tt_serial / board id (tags 1<<32 | 2)
};

struct tenstorrent_query_telemetry {
    struct tenstorrent_query_telemetry_out out;
};

struct tenstorrent_map_in {
    uint64_t mmap_offset;      // token + byte offset (page-aligned)
    uint64_t length;           // page-multiple, nonzero
};

struct tenstorrent_map_out {
    uint64_t user_va;
};

struct tenstorrent_map {
    struct tenstorrent_map_in in;
    struct tenstorrent_map_out out;
};

struct tenstorrent_unmap_in {
    uint64_t user_va;          // value returned by MAP
    uint64_t reserved;         // must be 0
};

struct tenstorrent_unmap {
    struct tenstorrent_unmap_in in;
};

// Linux mmap-offset tokens (memory.c:258-274; frozen at the 4K-page values)
#define TT_MMAP_OFFSET_RESOURCE0_UC (0ull << 36)
#define TT_MMAP_OFFSET_RESOURCE0_WC (1ull << 36)
#define TT_MMAP_OFFSET_RESOURCE1_UC (2ull << 36)
#define TT_MMAP_OFFSET_RESOURCE1_WC (3ull << 36)
#define TT_MMAP_OFFSET_RESOURCE2_UC (4ull << 36)
#define TT_MMAP_OFFSET_RESOURCE2_WC (5ull << 36)
#define TT_MMAP_OFFSET_TLB_UC       (6ull << 36)
#define TT_MMAP_OFFSET_TLB_WC       (7ull << 36)
#define TT_MMAP_RESOURCE_SIZE       (1ull << 36)
#define TT_MMAP_OFFSET_DMA_BUF      0xF000000000ull   // (4096-255-1) << 32
#define TT_MMAP_SIZE_DMA_BUF        (1ull << 32)      // 4 GiB per slot

#pragma pack(pop)

#endif // TTKMD_IOCTL_H_INCLUDED
