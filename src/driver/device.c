// Maps to: tt-kmd/enumerate.c (tenstorrent_pci_probe / tenstorrent_pci_remove).
// M1 scope: device creation, interface publication with per-device reference
// string, PCI identity capture, BAR inventory by PCI BAR index, and the
// Blackhole kernel mappings (blackhole.c:587-590). Hardware registers are not
// touched beyond mapping; a resource-less soft device (ROOT\TTKMD_SOFT) must
// still start.
#include "ttkmd.h"

#include <ntstrsafe.h>
#include <initguid.h>
#include <wdmguid.h>
#include "ttkmd_ioctl.h"
#include "ttkmd_abi_check.h"
#include "blackhole.h"

// Ordinal parity with Linux's device numbering (enumerate.c ordinal XArray):
// monotonically increasing per driver load, used as the interface reference
// string. Stable ASIC-ID-based identity is a later milestone (spec mapping).
static volatile LONG g_TtNextOrdinal = -1;

_Use_decl_annotations_
NTSTATUS
TtEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    PTT_DEVICE_CONTEXT context;
    DECLARE_UNICODE_STRING_SIZE(refString, 16);
    LONG ordinal;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    // MAP/UNMAP/PIN/UNPIN need the calling process context (DD-8).
    WdfDeviceInitSetIoInCallerContextCallback(DeviceInit,
                                              TtEvtIoInCallerContext);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = TtEvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = TtEvtDeviceReleaseHardware;
    pnpCallbacks.EvtDeviceSurpriseRemoval = TtEvtDeviceSurpriseRemoval;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    // Per-handle context: reset-generation latch (chardev.c open parity) and
    // memory state torn down in Cleanup (IRP_MJ_CLEANUP runs in the closing
    // process, where user unmaps are legal).
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               TtEvtDeviceFileCreate,
                               WDF_NO_EVENT_CALLBACK,
                               TtEvtFileCleanup);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, TT_FILE_CONTEXT);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, &attributes);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, TT_DEVICE_CONTEXT);
    attributes.EvtCleanupCallback = TtEvtDeviceContextCleanup;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(g_TtTraceProvider, "DeviceCreateFailed",
                          TraceLoggingNTStatus(status, "status"));
        return status;
    }

    context = TtGetDeviceContext(device);
    context->Device = device;
    InitializeListHead(&context->FileList);

    // reset_rwsem parity (DD-9): serializes RESET_DEVICE (exclusive) against
    // all other ioctls and mapping ops (shared).
    status = ExInitializeResourceLite(&context->ResetResource);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    context->ResetResourceInit = TRUE;

    // M2 locks: kernel-TLB access serialization (kernel_tlb_mutex parity)
    // and the ARC message transaction lock.
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &context->KernelTlbLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfWaitLockCreate(&attributes, &context->ArcMsgLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfWaitLockCreate(&attributes, &context->TlbLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfWaitLockCreate(&attributes, &context->IatuLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfWaitLockCreate(&attributes, &context->FileListLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = WdfWaitLockCreate(&attributes, &context->PowerLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    status = TtLocksInit(context);   // M5 lock bitmap + wait queue
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Replaces /dev/tenstorrent/N: interface instance per device with the
    // ordinal as reference string (DD-2).
    ordinal = InterlockedIncrement(&g_TtNextOrdinal);
    status = RtlUnicodeStringPrintf(&refString, L"TT%d", ordinal);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_TENSTORRENT,
                                            &refString);
    if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(g_TtTraceProvider, "DeviceInterfaceCreateFailed",
                          TraceLoggingNTStatus(status, "status"));
        return status;
    }

    // All ioctls arrive on the default queue; dispatch mirrors
    // tt_cdev_ioctl (chardev.c:591-706) in ioctl.c.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                           WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = TtEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES,
                              WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(g_TtTraceProvider, "QueueCreateFailed",
                          TraceLoggingNTStatus(status, "status"));
        return status;
    }

    TraceLoggingWrite(g_TtTraceProvider, "DeviceAdded",
                      TraceLoggingInt32(ordinal, "ordinal"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
TtEvtDeviceFileCreate(
    WDFDEVICE Device,
    WDFREQUEST Request,
    WDFFILEOBJECT FileObject
    )
{
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(Device);
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);
    NTSTATUS status;

    // Latch the reset generation (chardev.c:812 parity). Linux open() does not
    // check detached (analysis §03 open question) — a create during removal
    // succeeds and every subsequent operation fails; we mirror that.
    fileContext->OpenResetGen = ReadAcquire64(&context->ResetGen);

    status = TtFileContextInitMemory(FileObject);

    if (NT_SUCCESS(status)) {
        // Join the device-global file list so a reset can reach this handle's
        // mappings (VMA-zap parity, DD-9) and power aggregation can see it.
        fileContext->FileObject = FileObject;

        // Legacy power default (chardev.c:821-823): contribute all flags on
        // except MAX_AI_CLK. A power-aware client opts out via SET_POWER_STATE.
        TtPowerFileDefault(FileObject);

        WdfWaitLockAcquire(context->FileListLock, NULL);
        InsertTailList(&context->FileList, &fileContext->DeviceLink);
        fileContext->OnDeviceList = TRUE;
        WdfWaitLockRelease(context->FileListLock);

        // Aggregate now that this handle contributes (chardev.c:852-856).
        TtPowerAggregate(context);
    }

    TraceLoggingWrite(g_TtTraceProvider, "FileCreate",
                      TraceLoggingNTStatus(status, "status"));
    WdfRequestComplete(Request, status);
}

_Use_decl_annotations_
VOID
TtEvtFileCleanup(
    WDFFILEOBJECT FileObject
    )
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(device);
    PTT_FILE_CONTEXT fileContext = TtGetFileContext(FileObject);

    // Take the reset resource shared so cleanup does not race a reset's zap
    // (release takes reset_rwsem shared, chardev.c:929).
    TtResetAcquireShared(context);

    // Release all resource locks this handle held; wakes blocking waiters
    // (chardev.c:877-885). Before delisting so waiter gen checks are correct.
    TtLocksReleaseAll(context, FileObject);

    if (fileContext->OnDeviceList) {
        WdfWaitLockAcquire(context->FileListLock, NULL);
        RemoveEntryList(&fileContext->DeviceLink);
        fileContext->OnDeviceList = FALSE;
        WdfWaitLockRelease(context->FileListLock);
    }

    TtMemoryFileCleanup(context, FileObject);

    TtResetRelease(context);

    // Re-aggregate power now this handle no longer contributes (chardev.c:938).
    if (fileContext->PowerContributes) {
        fileContext->PowerContributes = FALSE;
        TtPowerAggregate(context);
    }
}

// PCI segment (GET_DEVICE_INFO pci_domain parity). Multi-segment systems are
// not supported yet; Linux reports pci_domain_nr which is 0 on single-segment
// hosts. Isolated here so multi-segment support is a one-function change.
static UINT16
TtQueryPciSegment(
    _In_ WDFDEVICE Device
    )
{
    UNREFERENCED_PARAMETER(Device);
    return 0;
}

// Reads the six BAR registers from config space and returns per-BAR-index
// assigned base addresses (0 = unimplemented). 64-bit BARs consume two slots.
static VOID
TtReadConfigBars(
    _In_ const BUS_INTERFACE_STANDARD *BusIf,
    _Out_writes_all_(TT_MAX_BARS) UINT64 *BarValue
    )
{
    UINT32 raw[TT_MAX_BARS];
    ULONG i;

    RtlZeroMemory(raw, sizeof(raw));
    RtlZeroMemory(BarValue, TT_MAX_BARS * sizeof(BarValue[0]));

    if (BusIf->GetBusData(BusIf->Context, PCI_WHICHSPACE_CONFIG, raw,
                          0x10, sizeof(raw)) != sizeof(raw)) {
        return;
    }

    for (i = 0; i < TT_MAX_BARS; i++) {
        UINT64 value = raw[i] & ~0xFull;

        if ((raw[i] & 0x1) != 0) {
            continue;   // I/O BAR: not used by this device family
        }
        if ((raw[i] & 0x6) == 0x4) {
            if (i + 1 >= TT_MAX_BARS) {
                break;
            }
            value |= ((UINT64)raw[i + 1]) << 32;
            BarValue[i] = value;
            i++;        // consumed the upper half
        } else {
            BarValue[i] = value;
        }
    }
}

// Decodes small and large memory descriptors. BAR sizes on Blackhole reach
// 4 GiB+ (BAR4; analysis §07), arriving as CmResourceTypeMemoryLarge.
static ULONGLONG
TtDecodeMemoryLength(
    _In_ const CM_PARTIAL_RESOURCE_DESCRIPTOR *Descriptor
    )
{
    if (Descriptor->Type == CmResourceTypeMemory) {
        return Descriptor->u.Memory.Length;
    }

    if ((Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_40) != 0) {
        return ((ULONGLONG)Descriptor->u.Memory40.Length40) << 8;
    }
    if ((Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_48) != 0) {
        return ((ULONGLONG)Descriptor->u.Memory48.Length48) << 16;
    }
    if ((Descriptor->Flags & CM_RESOURCE_MEMORY_LARGE_64) != 0) {
        return ((ULONGLONG)Descriptor->u.Memory64.Length64) << 32;
    }

    return 0;
}

static NTSTATUS
TtMapBlackholeBars(
    _Inout_ PTT_DEVICE_CONTEXT Context
    )
{
    // Mirrors blackhole_init's mapping set (blackhole.c:587-592) with
    // MmMapIoSpaceEx. BAR2 failure is fatal here by design (DD note in
    // ttkmd.h): Linux omits the NULL check and would fault later.
    struct {
        volatile UCHAR **slot;
        ULONG bar;
        ULONGLONG offset;
        SIZE_T length;
    } maps[] = {
        { &Context->TlbRegs,     0, TT_BH_TLB_REGS_START,    TT_BH_TLB_REGS_LEN },
        { &Context->KernelTlb,   0, TT_BH_KERNEL_TLB_START,  TT_BH_KERNEL_TLB_LEN },
        { &Context->Noc2AxiCfg,  0, TT_BH_NOC2AXI_CFG_START, TT_BH_NOC2AXI_CFG_LEN },
        { &Context->Bar2Mapping, 2, 0,                       0 /* whole BAR2 */ },
    };
    ULONG i;

    if (Context->BarLength[0] < TT_BH_NOC2AXI_CFG_START + TT_BH_NOC2AXI_CFG_LEN ||
        Context->BarLength[2] == 0) {
        TraceLoggingWrite(g_TtTraceProvider, "BlackholeBarLayoutUnexpected",
                          TraceLoggingUInt64(Context->BarLength[0], "bar0Len"),
                          TraceLoggingUInt64(Context->BarLength[2], "bar2Len"));
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    for (i = 0; i < ARRAYSIZE(maps); i++) {
        PHYSICAL_ADDRESS pa;
        SIZE_T length = maps[i].length;

        if (length == 0) {
            length = (SIZE_T)Context->BarLength[maps[i].bar];
        }
        pa.QuadPart = Context->BarBase[maps[i].bar].QuadPart +
                      (LONGLONG)maps[i].offset;

        *maps[i].slot = (volatile UCHAR *)MmMapIoSpaceEx(pa, length,
                                                         PAGE_READWRITE |
                                                         PAGE_NOCACHE);
        if (*maps[i].slot == NULL) {
            TraceLoggingWrite(g_TtTraceProvider, "BarMapFailed",
                              TraceLoggingUInt32(maps[i].bar, "bar"),
                              TraceLoggingUInt64(maps[i].offset, "offset"));
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    return STATUS_SUCCESS;
}

static VOID
TtUnmapBlackholeBars(
    _Inout_ PTT_DEVICE_CONTEXT Context
    )
{
    struct {
        volatile UCHAR **slot;
        SIZE_T length;
    } maps[] = {
        { &Context->TlbRegs,     TT_BH_TLB_REGS_LEN },
        { &Context->KernelTlb,   TT_BH_KERNEL_TLB_LEN },
        { &Context->Noc2AxiCfg,  TT_BH_NOC2AXI_CFG_LEN },
        { &Context->Bar2Mapping, (SIZE_T)Context->BarLength[2] },
    };
    ULONG i;

    for (i = 0; i < ARRAYSIZE(maps); i++) {
        if (*maps[i].slot != NULL) {
            MmUnmapIoSpace((PVOID)*maps[i].slot, maps[i].length);
            *maps[i].slot = NULL;
        }
    }
}

_Use_decl_annotations_
NTSTATUS
TtEvtDevicePrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(Device);
    BUS_INTERFACE_STANDARD busIf;
    UINT64 barValue[TT_MAX_BARS];
    BOOLEAN haveBusIf = FALSE;
    ULONG resourceCount;
    ULONG i;
    NTSTATUS status;

    RtlZeroMemory(context->BarLength, sizeof(context->BarLength));
    RtlZeroMemory(context->BarBase, sizeof(context->BarBase));
    RtlZeroMemory(barValue, sizeof(barValue));

    // PCI identity + config access via the bus interface. Kept referenced for
    // the driver lifetime (reset primitives use it); released in ReleaseHardware.
    // Absent on the ROOT-enumerated soft device.
    status = WdfFdoQueryForInterface(Device, &GUID_BUS_INTERFACE_STANDARD,
                                     (PINTERFACE)&busIf, sizeof(busIf), 1,
                                     NULL);
    if (NT_SUCCESS(status)) {
        PCI_COMMON_HEADER header;

        haveBusIf = TRUE;
        context->BusInterface = busIf;
        context->BusInterfaceValid = TRUE;
        if (busIf.GetBusData(busIf.Context, PCI_WHICHSPACE_CONFIG, &header, 0,
                             sizeof(header)) == sizeof(header)) {
            ULONG busNumber = 0;
            ULONG address = 0;
            ULONG resultLength;

            context->VendorId = header.VendorID;
            context->DeviceId = header.DeviceID;
            context->SubsysVendorId = header.u.type0.SubVendorID;
            context->SubsysId = header.u.type0.SubSystemID;
            context->IsBlackhole =
                (header.VendorID == TT_PCI_VENDOR_ID &&
                 header.DeviceID == TT_PCI_DEVICE_ID_BLACKHOLE);

            // bus_dev_fn = bus<<8 | dev<<3 | fn (PCI_DEVID parity, ioctl.h:56)
            (VOID)IoGetDeviceProperty(
                WdfDeviceWdmGetPhysicalDevice(Device),
                DevicePropertyBusNumber, sizeof(busNumber), &busNumber,
                &resultLength);
            (VOID)IoGetDeviceProperty(
                WdfDeviceWdmGetPhysicalDevice(Device),
                DevicePropertyAddress, sizeof(address), &address,
                &resultLength);
            context->BusDevFn = (UINT16)(((busNumber & 0xFF) << 8) |
                                         ((address >> 16) & 0x1F) << 3 |
                                         (address & 0x7));
            context->PciDomain = TtQueryPciSegment(Device);
        }

        TtReadConfigBars(&busIf, barValue);
    }

    // Correlate translated memory resources with PCI BAR indices by matching
    // the raw (bus-relative) base against the BAR register values.
    resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < resourceCount; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR raw =
            WdfCmResourceListGetDescriptor(ResourcesRaw, i);
        PCM_PARTIAL_RESOURCE_DESCRIPTOR xlated =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        ULONG bar;

        if (raw == NULL || xlated == NULL) {
            continue;
        }
        if (xlated->Type != CmResourceTypeMemory &&
            xlated->Type != CmResourceTypeMemoryLarge) {
            continue;
        }

        for (bar = 0; bar < TT_MAX_BARS; bar++) {
            if (barValue[bar] != 0 &&
                barValue[bar] == (UINT64)raw->u.Memory.Start.QuadPart) {
                context->BarBase[bar] = xlated->u.Memory.Start;
                context->BarLength[bar] = TtDecodeMemoryLength(xlated);
                TraceLoggingWrite(g_TtTraceProvider, "BarDiscovered",
                                  TraceLoggingUInt32(bar, "bar"),
                                  TraceLoggingUInt64((ULONGLONG)context->BarBase[bar].QuadPart, "base"),
                                  TraceLoggingUInt64(context->BarLength[bar], "length"));
                break;
            }
        }
    }

    if (context->IsBlackhole) {
        status = TtMapBlackholeBars(context);
        if (!NT_SUCCESS(status)) {
            TtUnmapBlackholeBars(context);
            return status;
        }

        // TLB pool init (blackhole_init parity): 4G count clamped by BAR4
        // length; kernel window 201 claimed for the driver.
        RtlZeroMemory(context->TlbUsed, sizeof(context->TlbUsed));
        RtlZeroMemory(context->TlbOwner, sizeof(context->TlbOwner));
        context->Tlb4gCount = (UINT32)min(context->BarLength[4] >> 32,
                                          (UINT64)TT_TLB_4G_MAX);
        context->TlbUsed[TT_BH_KERNEL_TLB_INDEX] = TRUE;

        // DMA enabler for coherent buffers (dma_set_mask(58) parity via the
        // 64-bit S/G profile; common buffers only in M3).
        if (context->DmaEnabler == NULL) {
            WDF_DMA_ENABLER_CONFIG dmaConfig;

            WDF_DMA_ENABLER_CONFIG_INIT(&dmaConfig,
                                        WdfDmaProfileScatterGather64,
                                        TT_MAX_DMA_BUF_SIZE);
            status = WdfDmaEnablerCreate(Device, &dmaConfig,
                                         WDF_NO_OBJECT_ATTRIBUTES,
                                         &context->DmaEnabler);
            if (!NT_SUCCESS(status)) {
                TraceLoggingWrite(g_TtTraceProvider, "DmaEnablerCreateFailed",
                                  TraceLoggingNTStatus(status, "status"));
                context->DmaEnabler = NULL;   // !dma_capable path (-EINVAL)
            }
        }

        // Telemetry probe is non-fatal, like Linux blackhole_init_hardware:
        // a device without telemetry still enumerates (blackhole.c:652-653).
        (VOID)TtBhTelemetryProbe(context);
    }

    TraceLoggingWrite(g_TtTraceProvider, "PrepareHardware",
                      TraceLoggingBoolean(haveBusIf, "pciDevice"),
                      TraceLoggingUInt16(context->DeviceId, "deviceId"),
                      TraceLoggingUInt16(context->BusDevFn, "busDevFn"));

    // busIf reference is retained in context (released in ReleaseHardware).
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
TtEvtDeviceReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    TtUnmapBlackholeBars(context);
    RtlZeroMemory(context->BarLength, sizeof(context->BarLength));
    RtlZeroMemory(context->BarBase, sizeof(context->BarBase));

    if (context->BusInterfaceValid) {
        if (context->BusInterface.InterfaceDereference != NULL) {
            context->BusInterface.InterfaceDereference(context->BusInterface.Context);
        }
        context->BusInterfaceValid = FALSE;
    }

    TraceLoggingWrite(g_TtTraceProvider, "ReleaseHardware");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
TtEvtDeviceSurpriseRemoval(
    WDFDEVICE Device
    )
{
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(Device);

    // Surprise removal: mark detached (gate returns STATUS_DEVICE_REMOVED for
    // everything) and revoke user mappings so stale accesses fault rather than
    // dangle. Under the reset resource exclusive for a stable snapshot.
    TtResetAcquireExclusive(context);
    context->Detached = TRUE;
    TtResetZapMappings(context);
    TtResetRelease(context);

    // Wake blocking lock waiters so they observe detached and fail -ENODEV
    // (enumerate.c:461-463 parity).
    WdfWaitLockAcquire(context->LockLock, NULL);
    TtLocksWakeWaiters(context);
    WdfWaitLockRelease(context->LockLock);

    TraceLoggingWrite(g_TtTraceProvider, "SurpriseRemoval");
}

_Use_decl_annotations_
VOID
TtEvtDeviceContextCleanup(
    WDFOBJECT Device
    )
{
    PTT_DEVICE_CONTEXT context = TtGetDeviceContext(Device);

    if (context->ResetResourceInit) {
        ExDeleteResourceLite(&context->ResetResource);
        context->ResetResourceInit = FALSE;
    }
}
