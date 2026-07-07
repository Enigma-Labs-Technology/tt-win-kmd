// Maps to: tt-kmd/module.h + device.h + chardev_private.h (driver-wide declarations)
#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <wdmguid.h>

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
#define TT_BH_KERNEL_TLB_INDEX  201u         // KERNEL_TLB_INDEX (last 2M window)
#define TT_BH_TLB_REG_SIZE      12u          // TLB_REG_SIZE
#define TT_BH_KERNEL_TLB_START  0x19200000u  // window 201 * 2 MiB (KERNEL_TLB_START)
#define TT_BH_KERNEL_TLB_LEN    0x00200000u  // one 2 MiB window
#define TT_BH_NOC2AXI_CFG_START 0x1FD00000u  // NOC2AXI_CFG_START
#define TT_BH_NOC2AXI_CFG_LEN   0x00100000u  // NOC2AXI_CFG_LEN

// Driver version reported by GET_DRIVER_INFO; tracks the upstream baseline tag
// (tt-kmd/module.h:19-21; maintenance guide step 5).
#define TT_VERSION_MAJOR 2
#define TT_VERSION_MINOR 10
#define TT_VERSION_PATCH 1

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

// Pool tags per subsystem
#define TT_TAG_MAPPING 'pMtT'
#define TT_TAG_PINNING 'nPtT'
#define TT_TAG_DMABUF  'bDtT'

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
    ULONG SavedConfigPresent;

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
    PEPROCESS Process;       // creator; needed to unmap from the right address space
} TT_USER_MAPPING, *PTT_USER_MAPPING;

struct tenstorrent_noc_tlb_config;

// One pinned user range from PIN_PAGES (tt-kmd struct pinned_page_range).
typedef struct _TT_PINNING {
    LIST_ENTRY Entry;
    UINT64 VirtualAddress;   // original pin VA (match key for UNPIN)
    UINT64 Size;
    PMDL Mdl;                // probe-and-locked
    LONG IatuRegion;         // -1 = none
} TT_PINNING, *PTT_PINNING;

// One coherent DMA buffer (tt-kmd struct dmabuf).
typedef struct _TT_DMABUF {
    WDFCOMMONBUFFER Buffer;
    UINT32 Size;
    LONG IatuRegion;         // -1 = none
} TT_DMABUF, *PTT_DMABUF;

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

    // M5: resource locks this handle holds (priv->resource_lock parity) and
    // this handle's power-state contribution.
    UINT64 LocksHeld;
    BOOLEAN PowerContributes;   // FALSE until first SET_POWER_STATE / legacy default
    UINT8 PowerValidity;
    UINT16 PowerFlags;
    UINT16 PowerSettings[14];
} TT_FILE_CONTEXT, *PTT_FILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TT_FILE_CONTEXT, TtGetFileContext)

EVT_WDF_DRIVER_DEVICE_ADD TtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE TtEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TtEvtDeviceReleaseHardware;
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
VOID TtMemoryFileCleanup(_In_ PTT_DEVICE_CONTEXT Context,
                         _In_ WDFFILEOBJECT FileObject);
NTSTATUS TtFileContextInitMemory(_In_ WDFFILEOBJECT FileObject);

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

// reset.c (maps to tt-kmd chardev.c reset handler + pcie.c primitives)
NTSTATUS TtIoctlResetDevice(_In_ PTT_DEVICE_CONTEXT Context,
                            _In_ WDFFILEOBJECT FileObject, _In_ WDFREQUEST Request);
VOID TtResetZapMappings(_In_ PTT_DEVICE_CONTEXT Context);
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
