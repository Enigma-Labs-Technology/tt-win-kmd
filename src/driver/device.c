// Maps to: tt-kmd/enumerate.c (tenstorrent_pci_probe / tenstorrent_pci_remove) —
// M0 scaffold subset: device creation, interface publication, BAR inventory.
// Hardware is never touched in M0; a resource-less soft device (ROOT\TTKMD_SOFT)
// must start successfully.
#include "ttkmd.h"

#include <initguid.h>
#include "ttkmd_ioctl.h"

_Use_decl_annotations_
NTSTATUS
TtEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
    )
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFDEVICE device;
    PTT_DEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    // Parity note: Linux ioctls are all copy_from_user/copy_to_user (buffered);
    // METHOD_BUFFERED ioctls arrive that way regardless, this sets the default
    // for read/write paths.
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = TtEvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = TtEvtDeviceReleaseHardware;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, TT_DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(g_TtTraceProvider, "DeviceCreateFailed",
                          TraceLoggingNTStatus(status, "status"));
        return status;
    }

    context = TtGetDeviceContext(device);
    context->Device = device;

    // Replaces /dev/tenstorrent/N: consumers enumerate this interface (DD-2).
    // Per-device ordinal/stable-ID reference strings arrive with M1.
    status = WdfDeviceCreateDeviceInterface(device,
                                            &GUID_DEVINTERFACE_TENSTORRENT,
                                            NULL);
    if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(g_TtTraceProvider, "DeviceInterfaceCreateFailed",
                          TraceLoggingNTStatus(status, "status"));
        return status;
    }

    TraceLoggingWrite(g_TtTraceProvider, "DeviceAdded");
    return STATUS_SUCCESS;
}

// Decodes small and large memory descriptors. BAR sizes on Blackhole reach 4 GiB
// (BAR4; analysis section 07), which arrive as CmResourceTypeMemoryLarge.
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

_Use_decl_annotations_
NTSTATUS
TtEvtDevicePrepareHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesRaw,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PTT_DEVICE_CONTEXT context;
    ULONG resourceCount;
    ULONG i;

    UNREFERENCED_PARAMETER(ResourcesRaw);

    context = TtGetDeviceContext(Device);
    context->MemBarCount = 0;

    resourceCount = WdfCmResourceListGetCount(ResourcesTranslated);

    for (i = 0; i < resourceCount; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor;

        descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (descriptor == NULL) {
            continue;
        }

        if (descriptor->Type != CmResourceTypeMemory &&
            descriptor->Type != CmResourceTypeMemoryLarge) {
            continue;
        }

        if (context->MemBarCount < TT_MAX_BARS) {
            ULONG bar = context->MemBarCount;

            context->BarBase[bar] = descriptor->u.Memory.Start;
            context->BarLength[bar] = TtDecodeMemoryLength(descriptor);

            TraceLoggingWrite(g_TtTraceProvider, "BarDiscovered",
                              TraceLoggingUInt32(bar, "index"),
                              TraceLoggingUInt64((ULONGLONG)context->BarBase[bar].QuadPart, "base"),
                              TraceLoggingUInt64(context->BarLength[bar], "length"));
        }

        context->MemBarCount++;
    }

    // Zero resources is valid: the M0 soft device (ROOT\TTKMD_SOFT) has none.
    TraceLoggingWrite(g_TtTraceProvider, "PrepareHardware",
                      TraceLoggingUInt32(context->MemBarCount, "memBarCount"));
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS
TtEvtDeviceReleaseHardware(
    WDFDEVICE Device,
    WDFCMRESLIST ResourcesTranslated
    )
{
    PTT_DEVICE_CONTEXT context;

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    context = TtGetDeviceContext(Device);
    context->MemBarCount = 0;
    RtlZeroMemory(context->BarBase, sizeof(context->BarBase));
    RtlZeroMemory(context->BarLength, sizeof(context->BarLength));

    TraceLoggingWrite(g_TtTraceProvider, "ReleaseHardware");
    return STATUS_SUCCESS;
}
