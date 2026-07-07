// Maps to: tt-kmd/module.c (tenstorrent_init / tenstorrent_exit, module.c:76-108)
#include "ttkmd.h"

TRACELOGGING_DEFINE_PROVIDER(
    g_TtTraceProvider,
    "Tenstorrent.TtKmd",
    (0xdd99fa3c, 0x98a4, 0x479d, 0x91, 0xd2, 0x5b, 0x43, 0xef, 0x8f, 0x5c, 0x22));

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP TtEvtDriverContextCleanup;

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    TraceLoggingRegister(g_TtTraceProvider);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = TtEvtDriverContextCleanup;

    WDF_DRIVER_CONFIG_INIT(&config, TtEvtDeviceAdd);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             &attributes,
                             &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(g_TtTraceProvider, "DriverEntryFailed",
                          TraceLoggingNTStatus(status, "status"));
        TraceLoggingUnregister(g_TtTraceProvider);
        return status;
    }

    TraceLoggingWrite(g_TtTraceProvider, "DriverLoaded");
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
static VOID
TtEvtDriverContextCleanup(
    WDFOBJECT DriverObject
    )
{
    UNREFERENCED_PARAMETER(DriverObject);

    TraceLoggingWrite(g_TtTraceProvider, "DriverUnloaded");
    TraceLoggingUnregister(g_TtTraceProvider);
}
