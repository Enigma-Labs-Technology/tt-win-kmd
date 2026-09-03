// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd/module.h + device.h + chardev_private.h (driver-wide declarations)
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdmguid.h>

#include "trace.h"

// KeStackAttachProcess/KeUnstackDetachProcess and KAPC_STATE live in ntifs.h,
// which conflicts with the ntddk.h WDF already includes. Declare the functions
// with an opaque APC-state pointer; callers pass a suitably-sized aligned
// buffer (KAPC_STATE is 48 bytes on x64). Used to unmap a mapping from a
// foreign process on reset (DD-9).
NTKERNELAPI VOID KeStackAttachProcess(_Inout_ PEPROCESS Process,
                                      _Out_ PVOID ApcState);
NTKERNELAPI VOID KeUnstackDetachProcess(_In_ PVOID ApcState);

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
#define TT_BH_KERNEL_TLB_INDEX  201u         // KERNEL_TLB_INDEX (last 2M window)
#define TT_BH_TLB_REG_SIZE      12u          // TLB_REG_SIZE
#define TT_BH_KERNEL_TLB_START  0x19200000u  // window 201 * 2 MiB (KERNEL_TLB_START)
#define TT_BH_KERNEL_TLB_LEN    0x00200000u  // one 2 MiB window
#define TT_BH_NOC2AXI_CFG_START 0x1FD00000u  // NOC2AXI_CFG_START
#define TT_BH_NOC2AXI_CFG_LEN   0x00100000u  // NOC2AXI_CFG_LEN

// Driver version reported by GET_DRIVER_INFO; tracks the upstream baseline tag
// (tt-kmd/module.h:19-21; maintenance guide step 5). The numbers come from
// ttkmd-version.props through the project's preprocessor definitions so the
// driver, its VERSIONINFO resource and the INF DriverVer cannot disagree.
#if !defined(TT_VERSION_MAJOR) || !defined(TT_VERSION_MINOR) || !defined(TT_VERSION_PATCH)
#error "TT_VERSION_* must come from ttkmd-version.props (build with ttkmd.vcxproj)"
#endif

// TLB pool geometry (tt-kmd/blackhole.c:20-28): 202 2M windows (id 201 kernel-
// reserved) + up to 8 4G windows, clamped by BAR4 length at init.
#define TT_TLB_2M_COUNT 202u
#define TT_TLB_4G_MAX 8u
#define TT_TLB_TOTAL (TT_TLB_2M_COUNT + TT_TLB_4G_MAX)

// Outbound iATU regions (tt-kmd/memory.h:65-71, blackhole.c:78)
#define TT_IATU_REGIONS 16u
// Blackhole NOC-DMA address space (blackhole.c:817-818)
#define TT_BH_NOC_DMA_LIMIT ((1ull << 58) - 1)
#define TT_BH_NOC_PCIE_OFFSET (4ull << 58)

#define TT_MAX_DMA_BUF_SIZE (1ull << 28)   // MAX_DMA_BUF_SIZE_LOG2=28, memory.h:10

// DD-17: per-device ceiling on outstanding DMA-buffer memory across all
// handles. Linux has no equivalent; on Windows the buffers are nonpaged,
// physically contiguous common buffers that any device user can request, so
// an ordinary user must not be able to exhaust the kernel. Overridable via
// the service's Parameters\DmaBufferLimitMiB registry value (0 = unlimited).
#define TT_DMA_BUF_DEFAULT_LIMIT_MIB 4096u
#define TT_DMA_BUF_LIMIT_VALUE_NAME  L"DmaBufferLimitMiB"

// Storage for a KAPC_STATE used by cross-process unmapping (KeStackAttachProcess).
#define TT_KAPC_STATE_QWORDS 8   // >= sizeof(KAPC_STATE), 8-byte aligned

// Pool tags per subsystem
#define TT_TAG_MAPPING 'pMtT'
#define TT_TAG_PINNING 'nPtT'
#define TT_TAG_DMABUF  'bDtT'

// Per-device context. Maps to: tt-kmd struct tenstorrent_device (device.h:21).
typedef struct _TT_DEVICE_CONTEXT {
    WDFDEVICE Device;

    // DD-15: membership in the driver-global device list (g_TtDeviceList),
    // walked by the process-exit callback to release a dying process's
    // mappings and pins in its own context. Guarded by g_TtDeviceListLock.
    LIST_ENTRY DriverLink;
    BOOLEAN OnDriverList;

    // DD-17: DMA-buffer accounting. DmaBufBytes is updated with interlocked
    // operations; DmaBufLimitBytes is fixed at DeviceAdd (0 = unlimited).
    volatile LONG64 DmaBufBytes;
    UINT64 DmaBufLimitBytes;

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
    BOOLEAN Detached;
    BOOLEAN NeedsHwInit;
    volatile LONG64 ResetGen;

    // reset_rwsem parity (DD-9): RESET_DEVICE exclusive, all else shared.
    ERESOURCE ResetResource;
    BOOLEAN ResetResourceInit;

    // Device-global list of open file objects, for reset-time mapping zap
    // (tenstorrent_vma_zap walks every fd). Guarded by FileListLock; the reset
    // resource held exclusive gives a stable snapshot during zap.
    WDFWAITLOCK FileListLock;
    LIST_ENTRY FileList;

    // PCI config-space access for reset primitives (marker, timer interrupt),
    // referenced across PrepareHardware..ReleaseHardware.
    BUS_INTERFACE_STANDARD BusInterface;
    BOOLEAN BusInterfaceValid;

    // OQ-5/DD-12: PCI config snapshot for restore after chip-internal resets
    // (pci_save_state parity, enumerate.c:372 + pcie.c:43-59). Header dwords
    // indexed by dword number; only the writable subset is restored. SavedMps
    // is the DBI-view Max Payload Size field (blackhole.c:304-330) — the chip
    // reset wipes it and the host snapshot cannot recover it.
    UINT32 SavedHeaderDword[16];
    USHORT SavedPcieDevCtl;
    USHORT SavedPcieLnkCtl;
    ULONG PcieCapOffset;          // 0 = PCIe capability not found
    UINT8 SavedMps;
    BOOLEAN SavedMpsValid;
    BOOLEAN SavedStateValid;
    BOOLEAN HardwareInitDone;     // probe-time init ran; D0Entry uses this to
                                  // distinguish resume from initial start

    // OQ-4/DD-11: pci.sys device-reset interface for RESET_PCIE_LINK (PLDR).
    // The reset fires from a work item after the ioctl completes, because
    // PLDR surprise-removes this very device stack.
    DEVICE_RESET_INTERFACE_STANDARD ResetInterface;
    BOOLEAN ResetInterfaceValid;
    // Snapshot captured at enqueue time, holding its own extra
    // InterfaceReference: the work item can outlive ReleaseHardware's
    // dereference of ResetInterface (WDF flushes device work items at object
    // cleanup, which is after ReleaseHardware).
    DEVICE_RESET_INTERFACE_STANDARD PldrSnapshot;
    WDFWORKITEM PldrWorkItem;
    volatile LONG PldrQueued;

    // DD-8: PIN_PAGES hands raw physical addresses to the iATU, valid only in
    // an identity (untranslated) DMA domain. Probed once at PrepareHardware.
    BOOLEAN DmaIdentityKnown;
    BOOLEAN DmaIdentityMapped;

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

    // M2: kernel-TLB serialization (kernel_tlb_mutex parity, blackhole.c:243)
    // and ARC message transaction lock (documented superset of Linux).
    WDFWAITLOCK KernelTlbLock;
    WDFWAITLOCK ArcMsgLock;

    // Telemetry tag cache (tt-kmd device.h telemetry_tag_cache parity):
    // per-tag absolute CSM value address, 0 = tag absent.
    UINT64 TelemetryTagCache[128];
    BOOLEAN TelemetryValid;

    // M3: TLB pool (tt-kmd tt_dev->tlbs bitmap parity, single-owner model
    // per DD-8). Owner file object, NULL = free; id 201 reserved at init with
    // KernelReserved. Guarded by TlbLock.
    WDFWAITLOCK TlbLock;
    WDFFILEOBJECT TlbOwner[TT_TLB_TOTAL];
    BOOLEAN TlbUsed[TT_TLB_TOTAL];
    UINT32 Tlb4gCount;             // clamped by BAR4 length (blackhole_init)

    // M3: outbound iATU region table (tt-kmd tt_dev->outbound_iatus parity),
    // guarded by IatuLock.
    WDFWAITLOCK IatuLock;
    struct {
        BOOLEAN Used;
        WDFFILEOBJECT Owner;
        UINT64 Base;
        UINT64 Limit;
        UINT64 Target;
    } Iatu[TT_IATU_REGIONS];

    // M3: DMA enabler for coherent (common) buffers.
    WDFDMAENABLER DmaEnabler;

    // M5: resource locks (tt_dev->resource_lock parity, device.h:50). 64-bit
    // "held" bitmap + a manual queue of pended ACQUIRE_BLOCKING requests, both
    // guarded by LockLock.
    WDFWAITLOCK LockLock;
    UINT64 ResourceLockHeld;
    WDFQUEUE LockWaitQueue;

    // M5: aggregated power state (last value sent to firmware), guarded by
    // PowerLock which also serializes aggregation over the FileList.
    WDFWAITLOCK PowerLock;
    BOOLEAN PowerAggValid;
    UINT16 PowerAggFlags;
    UINT16 PowerAggSettings[14];
} TT_DEVICE_CONTEXT, *PTT_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TT_DEVICE_CONTEXT, TtGetDeviceContext)

// One coherent DMA buffer (tt-kmd struct dmabuf).
typedef struct _TT_DMABUF {
    WDFCOMMONBUFFER Buffer;
    UINT32 Size;
    UINT32 Index;            // buf_index slot in the owning file context
    UINT64 Logical;          // device (bus) address, valid in any DMA domain
    LONG IatuRegion;         // -1 = none
} TT_DMABUF, *PTT_DMABUF;

// One user-mode mapping created by IOCTL_TENSTORRENT_MAP (DD-8). The MDL is a
// physical-page MDL (device BAR pages or a common buffer's contiguous pages),
// not a pool MDL — MmBuildMdlForNonPagedPool cannot describe either.
typedef struct _TT_USER_MAPPING {
    LIST_ENTRY Entry;
    PVOID UserVa;
    PMDL Mdl;
    SIZE_T Length;
    LONG TlbId;              // -1 unless this maps a TLB window (FREE_TLB -EBUSY)
    BOOLEAN IsDmaBuf;        // DMA-buffer maps are NOT zapped on reset (Linux parity)
    PTT_DMABUF DmaBuf;       // the buffer behind a DMA-buffer view (FREE_DMA_BUF_EX -EBUSY)
    PEPROCESS Process;       // creator; needed to unmap from the right address space
} TT_USER_MAPPING, *PTT_USER_MAPPING;

struct tenstorrent_noc_tlb_config;

// One pinned user range from PIN_PAGES (tt-kmd struct pinned_page_range).
// DD-16: a range inside a MAP view of one of the handle's own DMA buffers is
// "backed": no pages are probed or locked and the device address comes from
// the common buffer, so it works in translated DMA domains and leaves nothing
// locked in the process.
typedef struct _TT_PINNING {
    LIST_ENTRY Entry;
    UINT64 VirtualAddress;   // original pin VA (match key for UNPIN)
    UINT64 Size;
    PMDL Mdl;                // probe-and-locked; NULL when BackingDmaBuf != NULL
    PTT_DMABUF BackingDmaBuf;
    LONG IatuRegion;         // -1 = none
} TT_PINNING, *PTT_PINNING;

// Per-open-handle context. Maps to: tt-kmd struct chardev_private
// (chardev_private.h): reset-generation latch (chardev.c:812), dmabufs,
// pinnings, and user mappings, all torn down at handle cleanup.
typedef struct _TT_FILE_CONTEXT {
    LONG64 OpenResetGen;
    WDFWAITLOCK Lock;        // priv->mutex parity; NULL until first use path
    LIST_ENTRY Mappings;     // TT_USER_MAPPING
    LIST_ENTRY Pinnings;     // TT_PINNING
    PTT_DMABUF DmaBufs[256]; // by buf_index (TENSTORRENT_MAX_DMA_BUFS)
    LIST_ENTRY DeviceLink;   // membership in TT_DEVICE_CONTEXT.FileList
    WDFFILEOBJECT FileObject;
    BOOLEAN OnDeviceList;

    // DD-15: the process that opened the handle (referenced). MAP/UNMAP/
    // PIN_PAGES/UNPIN_PAGES are only honoured from this process, and the
    // process-exit callback releases whatever it still owns.
    PEPROCESS CreatorProcess;

    // M5: resource locks this handle holds (priv->resource_lock parity) and
    // this handle's power-state contribution.
    UINT64 LocksHeld;
    BOOLEAN PowerContributes;   // FALSE until first SET_POWER_STATE / legacy default
    UINT8 PowerValidity;
    UINT16 PowerFlags;
    UINT16 PowerSettings[14];

    // SET_NOC_CLEANUP registration (priv->noc_cleanup parity): a NOC write
    // performed at handle close for device-side cleanup.
    BOOLEAN NocCleanupEnabled;
    UINT8 NocCleanupX;
    UINT8 NocCleanupY;
    UINT8 NocCleanupNoc;
    UINT64 NocCleanupAddr;
    UINT64 NocCleanupData;
} TT_FILE_CONTEXT, *PTT_FILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TT_FILE_CONTEXT, TtGetFileContext)

EVT_WDF_DRIVER_DEVICE_ADD TtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE TtEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TtEvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY TtEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT TtEvtDeviceD0Exit;
EVT_WDF_DEVICE_SURPRISE_REMOVAL TtEvtDeviceSurpriseRemoval;
EVT_WDF_OBJECT_CONTEXT_CLEANUP TtEvtDeviceContextCleanup;
EVT_WDF_DEVICE_FILE_CREATE TtEvtDeviceFileCreate;
EVT_WDF_FILE_CLEANUP TtEvtFileCleanup;
EVT_WDF_IO_IN_CALLER_CONTEXT TtEvtIoInCallerContext;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL TtEvtIoDeviceControl;

// Shared gate check (chardev.c:591-624 parity); used by both the queue path
// and the in-caller-context path.
NTSTATUS TtCheckIoGates(_In_ PTT_DEVICE_CONTEXT Context,
                        _In_opt_ WDFFILEOBJECT FileObject, _In_ UINT32 Nr);

// memory.c (maps to tt-kmd memory.c + tlb.c)
NTSTATUS TtIoctlAllocateDmaBuf(_In_ PTT_DEVICE_CONTEXT Context,
                               _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlAllocateTlb(_In_ PTT_DEVICE_CONTEXT Context,
                            _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlFreeTlb(_In_ PTT_DEVICE_CONTEXT Context,
                        _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlConfigureTlb(_In_ PTT_DEVICE_CONTEXT Context,
                             _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlMap(_In_ PTT_DEVICE_CONTEXT Context,
                    _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlUnmap(_In_ PTT_DEVICE_CONTEXT Context,
                      _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlPinPages(_In_ PTT_DEVICE_CONTEXT Context,
                         _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlUnpinPages(_In_ PTT_DEVICE_CONTEXT Context,
                           _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
NTSTATUS TtIoctlFreeDmaBufEx(_In_ PTT_DEVICE_CONTEXT Context,
                             _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
VOID TtMemoryFileCleanup(_In_ PTT_DEVICE_CONTEXT Context,
                         _In_ WDFFILEOBJECT FileObject);
// DD-15: releases a process's mappings and pins from inside that process
// (process-exit callback). Caller holds Context->FileListLock.
VOID TtMemoryProcessExit(_In_ PTT_DEVICE_CONTEXT Context,
                         _In_ PTT_FILE_CONTEXT FileContext);
// Unmaps and frees one user mapping, attaching to the creating process when
// the caller runs elsewhere (reset zap, cleanup through a duplicated handle).
VOID TtDestroyUserMapping(_In_ PTT_USER_MAPPING Mapping);
NTSTATUS TtFileContextInitMemory(_In_ WDFFILEOBJECT FileObject);

// driver.c: driver-global device list for the process-exit callback (DD-15).
extern LIST_ENTRY g_TtDeviceList;
extern WDFWAITLOCK g_TtDeviceListLock;
VOID TtDeviceListRegister(_In_ PTT_DEVICE_CONTEXT Context);
VOID TtDeviceListUnregister(_In_ PTT_DEVICE_CONTEXT Context);

// ioctl.c helpers shared with memory.c
NTSTATUS TtCopyInBuffer(_In_ WDFREQUEST Request,
                        _Out_writes_bytes_(InSize) VOID *In, _In_ size_t InSize);
NTSTATUS TtCompleteSizedOutBuffer(_In_ WDFREQUEST Request, _In_ size_t OutOffset,
                                  _In_reads_bytes_(OutSize) const VOID *Out,
                                  _In_ size_t OutSize, _In_ UINT32 Declared);

// blackhole.c M3 surface
NTSTATUS TtBhConfigureUserTlb(_In_ PTT_DEVICE_CONTEXT Context, _In_ UINT32 Id,
                              _In_ const struct tenstorrent_noc_tlb_config *Config);
NTSTATUS TtBhConfigureOutboundAtu(_In_ PTT_DEVICE_CONTEXT Context, _In_ UINT32 Region,
                                  _In_ UINT64 Base, _In_ UINT64 Limit, _In_ UINT64 Target);
BOOLEAN TtBhReset(_In_ PTT_DEVICE_CONTEXT Context, _In_ UINT32 Flags);
BOOLEAN TtBhInitHardware(_In_ PTT_DEVICE_CONTEXT Context);
VOID TtBhNocWrite(_In_ PTT_DEVICE_CONTEXT Context, _In_ UINT32 X, _In_ UINT32 Y,
                  _In_ UINT64 Addr, _In_ UINT32 Data, _In_ UINT32 Noc);
NTSTATUS TtIoctlSetNocCleanup(_In_ PTT_DEVICE_CONTEXT Context,
                              _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);

// reset.c (maps to tt-kmd chardev.c reset handler + pcie.c primitives)
NTSTATUS TtIoctlResetDevice(_In_ PTT_DEVICE_CONTEXT Context,
                            _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
VOID TtResetZapMappings(_In_ PTT_DEVICE_CONTEXT Context);
VOID TtPciSaveState(_In_ PTT_DEVICE_CONTEXT Context);
VOID TtDiscoverPcieCap(_In_ PTT_DEVICE_CONTEXT Context);
EVT_WDF_WORKITEM TtPldrWorkItem;
BOOLEAN TtCfgReadWord(_In_ PTT_DEVICE_CONTEXT Context, _In_ ULONG Offset, _Out_ USHORT *Value);
VOID TtCfgWriteWord(_In_ PTT_DEVICE_CONTEXT Context, _In_ ULONG Offset, _In_ USHORT Value);
VOID TtCfgWriteDword(_In_ PTT_DEVICE_CONTEXT Context, _In_ ULONG Offset, _In_ ULONG Value);

// Reset resource helpers (DD-9). KeEnterCriticalRegion is taken inside.
VOID TtResetAcquireShared(_In_ PTT_DEVICE_CONTEXT Context);
VOID TtResetAcquireExclusive(_In_ PTT_DEVICE_CONTEXT Context);
VOID TtResetRelease(_In_ PTT_DEVICE_CONTEXT Context);

// locks.c (LOCK_CTL, tt-kmd chardev.c:323-430)
NTSTATUS TtIoctlLockCtl(_In_ PTT_DEVICE_CONTEXT Context,
                        _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request,
                        _Out_ BOOLEAN *Pended);
VOID TtLocksWakeWaiters(_In_ PTT_DEVICE_CONTEXT Context);
VOID TtLocksReleaseAll(_In_ PTT_DEVICE_CONTEXT Context, _In_ WDFFILEOBJECT FileObject);
NTSTATUS TtLocksInit(_In_ PTT_DEVICE_CONTEXT Context);

// power.c (SET_POWER_STATE aggregation, tt-kmd chardev.c:478-589)
NTSTATUS TtIoctlSetPowerState(_In_ PTT_DEVICE_CONTEXT Context,
                              _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
VOID TtPowerFileDefault(_In_ WDFFILEOBJECT FileObject);
VOID TtPowerAggregate(_In_ PTT_DEVICE_CONTEXT Context);

// telemetry query (memory-free; blackhole.c telemetry tags)
NTSTATUS TtIoctlQueryTelemetry(_In_ PTT_DEVICE_CONTEXT Context, _In_ WDFREQUEST Request);
