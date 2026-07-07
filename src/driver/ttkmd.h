// Maps to: tt-kmd/module.h + device.h + chardev_private.h (driver-wide declarations)
#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "trace.h"

// Kernel pool tags, unique per subsystem for leak triage (spec: engineering
// standards). Tags appear reversed in debugger output: 'vDtT' shows as "TtDv".
#define TT_TAG_DEVICE 'vDtT'

// PCI identity (tt-kmd/enumerate.h:15-18)
#define TT_PCI_VENDOR_ID 0x1E52
#define TT_PCI_DEVICE_ID_WORMHOLE 0x401E
#define TT_PCI_DEVICE_ID_BLACKHOLE 0xB140

// Maximum PCI BARs a device can present (BAR0..BAR5).
#define TT_MAX_BARS 6

// Blackhole BAR0 kernel-mapping geometry (tt-kmd/blackhole.c:20-46)
#define TT_BH_TLB_REGS_START    0x1FC00000u  // TLB_REGS_START
#define TT_BH_TLB_REGS_LEN      0x00001000u  // TLB_REGS_LEN
#define TT_BH_KERNEL_TLB_START  0x19200000u  // window 201 * 2 MiB (KERNEL_TLB_START)
#define TT_BH_KERNEL_TLB_LEN    0x00200000u  // one 2 MiB window
#define TT_BH_NOC2AXI_CFG_START 0x1FD00000u  // NOC2AXI_CFG_START
#define TT_BH_NOC2AXI_CFG_LEN   0x00100000u  // NOC2AXI_CFG_LEN

// Driver version reported by GET_DRIVER_INFO; tracks the upstream baseline tag
// (tt-kmd/module.h:19-21; maintenance guide step 5).
#define TT_VERSION_MAJOR 2
#define TT_VERSION_MINOR 10
#define TT_VERSION_PATCH 1

// Per-device context. Maps to: tt-kmd struct tenstorrent_device (device.h:21).
typedef struct _TT_DEVICE_CONTEXT {
    WDFDEVICE Device;

    // PCI identity captured in EvtDevicePrepareHardware (enumerate.c probe
    // parity). All zero for the ROOT\TTKMD_SOFT test device.
    UINT16 VendorId;
    UINT16 DeviceId;
    UINT16 SubsysVendorId;
    UINT16 SubsysId;
    UINT16 BusDevFn;    // bus<<8 | dev<<3 | fn (tt-kmd/ioctl.h:56)
    UINT16 PciDomain;   // PCI segment; see TtQueryPciSegment
    BOOLEAN IsBlackhole;

    // fd-gating state, semantics per analysis §04.2 (chardev.c:591-706).
    // M1 wires the checks; M4 (reset paths) makes them fire.
    BOOLEAN Detached;
    BOOLEAN NeedsHwInit;
    volatile LONG64 ResetGen;

    // BAR inventory indexed by PCI BAR number (pci_resource_len parity).
    ULONGLONG BarLength[TT_MAX_BARS];
    PHYSICAL_ADDRESS BarBase[TT_MAX_BARS];

    // Blackhole kernel mappings (maps to blackhole.c:587-590). BAR2 mapping
    // failure is fatal by design decision (analysis §07 open question, resolved
    // conservatively).
    volatile UCHAR *TlbRegs;      // BAR0 + TLB_REGS_START
    volatile UCHAR *KernelTlb;    // BAR0 + KERNEL_TLB_START
    volatile UCHAR *Noc2AxiCfg;   // BAR0 + NOC2AXI_CFG_START
    volatile UCHAR *Bar2Mapping;  // whole BAR2
} TT_DEVICE_CONTEXT, *PTT_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TT_DEVICE_CONTEXT, TtGetDeviceContext)

// Per-open-handle context. Maps to: tt-kmd struct chardev_private
// (chardev_private.h) — M1 carries only the reset-generation latch
// (chardev.c:812 open_reset_gen parity).
typedef struct _TT_FILE_CONTEXT {
    LONG64 OpenResetGen;
} TT_FILE_CONTEXT, *PTT_FILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TT_FILE_CONTEXT, TtGetFileContext)

EVT_WDF_DRIVER_DEVICE_ADD TtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE TtEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TtEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_FILE_CREATE TtEvtDeviceFileCreate;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TtEvtIoDeviceControl;
