// SPDX-FileCopyrightText: 2026 tt-win-kmd contributors
// SPDX-License-Identifier: GPL-2.0-only
//
// Maps to: tt-kmd/module.c (tenstorrent_init / tenstorrent_exit, module.c:76-108)
#include "ttkmd.h"

TRACELOGGING_DEFINE_PROVIDER(
    g_TtTraceProvider,
    "Tenstorrent.TtKmd",
    (0xdd99fa3c, 0x98a4, 0x479d, 0x91, 0xd2, 0x5b, 0x43, 0xef, 0x8f, 0x5c, 0x22));

DRIVER_INITIALIZE DriverEntry;
static EVT_WDF_OBJECT_CONTEXT_CLEANUP TtEvtDriverContextCleanup;

// DD-15: driver-global device list, walked by the process-exit callback.
LIST_ENTRY g_TtDeviceList;
WDFWAITLOCK g_TtDeviceListLock;
static BOOLEAN g_TtProcessNotifyRegistered;

_Use_decl_annotations_
VOID
TtDeviceListRegister(
    PTT_DEVICE_CONTEXT Context
    )
{
    WdfWaitLockAcquire(g_TtDeviceListLock, NULL);
    InsertTailList(&g_TtDeviceList, &Context->DriverLink);
    Context->OnDriverList = TRUE;
    WdfWaitLockRelease(g_TtDeviceListLock);
}

_Use_decl_annotations_
VOID
TtDeviceListUnregister(
    PTT_DEVICE_CONTEXT Context
    )
{
    WdfWaitLockAcquire(g_TtDeviceListLock, NULL);
    if (Context->OnDriverList) {
        RemoveEntryList(&Context->DriverLink);
        Context->OnDriverList = FALSE;
    }
    WdfWaitLockRelease(g_TtDeviceListLock);
}

// Process-exit callback (DD-15). Runs in the context of the exiting process's
// last thread, before its address space is torn down, so user mappings can be
// unmapped and locked pages unlocked from the one context where that is
// legal. Handles duplicated into other processes keep the file object alive
// past this point, which is exactly the case IRP_MJ_CLEANUP cannot cover
// (DRIVER_LEFT_LOCKED_PAGES_IN_PROCESS, wrong-process MmUnmapLockedPages).
//
// Lock order matches cleanup and the reset zap: g_TtDeviceListLock ->
// ResetResource (shared) -> FileListLock -> file Lock.
static VOID
TtProcessNotify(
    _Inout_ PEPROCESS Process,
    _In_ HANDLE ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    PLIST_ENTRY deviceEntry;

    UNREFERENCED_PARAMETER(ProcessId);

    if (CreateInfo != NULL) {
        return;   // creation; only exits matter here
    }

    WdfWaitLockAcquire(g_TtDeviceListLock, NULL);
    for (deviceEntry = g_TtDeviceList.Flink; deviceEntry != &g_TtDeviceList;
         deviceEntry = deviceEntry->Flink) {
        PTT_DEVICE_CONTEXT context =
            CONTAINING_RECORD(deviceEntry, TT_DEVICE_CONTEXT, DriverLink);
        PLIST_ENTRY fileEntry;

        TtResetAcquireShared(context);
        WdfWaitLockAcquire(context->FileListLock, NULL);
        for (fileEntry = context->FileList.Flink; fileEntry != &context->FileList;
             fileEntry = fileEntry->Flink) {
            PTT_FILE_CONTEXT fileContext =
                CONTAINING_RECORD(fileEntry, TT_FILE_CONTEXT, DeviceLink);

            if (fileContext->CreatorProcess == Process) {
                TtMemoryProcessExit(context, fileContext);
            }
        }
        WdfWaitLockRelease(context->FileListLock);
        TtResetRelease(context);
    }
    WdfWaitLockRelease(g_TtDeviceListLock);
}

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

    // DD-15: device list + process-exit callback. The lock is parented to the
    // driver object so it outlives every device.
    InitializeListHead(&g_TtDeviceList);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = WdfGetDriver();
    status = WdfWaitLockCreate(&attributes, &g_TtDeviceListLock);
    if (!NT_SUCCESS(status)) {
        TraceLoggingWrite(g_TtTraceProvider, "DriverEntryFailed",
                          TraceLoggingNTStatus(status, "status"));
        // WDF deletes the driver object on a failed DriverEntry; the context
        // cleanup callback unregisters the trace provider.
        return status;
    }

    // Process-bound mappings require this callback, including when a handle
    // is duplicated into a process that outlives its creator (DD-19).
    status = PsSetCreateProcessNotifyRoutineEx(TtProcessNotify, FALSE);
    if (NT_SUCCESS(status)) {
        g_TtProcessNotifyRegistered = TRUE;
    } else {
        TraceLoggingWrite(g_TtTraceProvider, "ProcessNotifyRegisterFailed",
                          TraceLoggingNTStatus(status, "status"));
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

    if (g_TtProcessNotifyRegistered) {
        (VOID)PsSetCreateProcessNotifyRoutineEx(TtProcessNotify, TRUE);
        g_TtProcessNotifyRegistered = FALSE;
    }

    TraceLoggingWrite(g_TtTraceProvider, "DriverUnloaded");
    TraceLoggingUnregister(g_TtTraceProvider);
}
