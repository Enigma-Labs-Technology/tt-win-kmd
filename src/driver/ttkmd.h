// Maps to: tt-kmd/module.h + device.h (driver-wide declarations — M0 scaffold subset)
#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "trace.h"

// Kernel pool tags, unique per subsystem for leak triage (spec: engineering
// standards). Tags appear reversed in debugger output: 'vDtT' shows as "TtDv".
#define TT_TAG_DEVICE 'vDtT'

// Maximum PCI BARs a device can present (BAR0..BAR5).
#define TT_MAX_BARS 6

// Per-device context. Maps to: tt-kmd struct tenstorrent_device (device.h:21) —
// M0 carries only the BAR inventory discovered in EvtDevicePrepareHardware.
typedef struct _TT_DEVICE_CONTEXT {
    WDFDEVICE Device;
    ULONG MemBarCount;                       // memory BARs seen in PrepareHardware
    PHYSICAL_ADDRESS BarBase[TT_MAX_BARS];   // translated base per discovered BAR
    ULONGLONG BarLength[TT_MAX_BARS];
} TT_DEVICE_CONTEXT, *PTT_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(TT_DEVICE_CONTEXT, TtGetDeviceContext)

EVT_WDF_DRIVER_DEVICE_ADD TtEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE TtEvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE TtEvtDeviceReleaseHardware;
