// SPDX-License-Identifier: GPL-2.0-only
// A refused asynchronous reset must not reopen admission after D0 resume.
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define VOID void
#define FALSE 0
#define TRUE 1
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define NT_SUCCESS(s) ((s) >= 0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define TraceLoggingWrite(provider, event, field) ((void)(field))
#define TraceLoggingNTStatus(status, name) (status)
typedef int NTSTATUS;
typedef int WDF_POWER_DEVICE_STATE;
enum { PlatformLevelDeviceReset = 1 };
typedef struct {
    void *Context;
    NTSTATUS (*DeviceReset)(void *, int, unsigned, void *);
    void (*InterfaceDereference)(void *);
} DEVICE_RESET_INTERFACE_STANDARD;
typedef struct context {
    int Detached, IsBlackhole, HardwarePrepared, HardwareReady, PowerAggValid;
    long PldrQueued;
    DEVICE_RESET_INTERFACE_STANDARD PldrSnapshot;
} *PTT_DEVICE_CONTEXT, *WDFDEVICE, *WDFWORKITEM;
static unsigned hardware_calls, references_released;
static PTT_DEVICE_CONTEXT TtGetDeviceContext(WDFDEVICE device) { return device; }
static WDFDEVICE WdfWorkItemGetParentObject(WDFWORKITEM work) { return work; }
static void TtResetAcquireExclusive(PTT_DEVICE_CONTEXT c) { (void)c; }
static void TtResetRelease(PTT_DEVICE_CONTEXT c) { (void)c; }
#define InterlockedExchange(p, v) (*(p) = (v))
static int TtBhInitHardware(PTT_DEVICE_CONTEXT c) {
    (void)c; ++hardware_calls; return TRUE;
}
static NTSTATUS TtBhTelemetryProbe(PTT_DEVICE_CONTEXT c) { (void)c; return 0; }
static void TtPowerAggregate(PTT_DEVICE_CONTEXT c) { c->PowerAggValid = TRUE; }
static NTSTATUS refuse_reset(void *context, int type, unsigned flags, void *parameters) {
    (void)context; (void)type; (void)flags; (void)parameters; return -1;
}
static void dereference(void *context) { (void)context; ++references_released; }

/* DRIVER_FUNCTIONS */

int main(void) {
    struct context device = {.IsBlackhole = TRUE, .HardwarePrepared = TRUE, .PldrQueued = 1};
    device.PldrSnapshot.DeviceReset = refuse_reset;
    device.PldrSnapshot.InterfaceDereference = dereference;
    TtPldrWorkItem(&device);
    assert(references_released == 1);
    assert(device.PldrQueued == 1);
    assert(TtEvtDeviceD0Entry(&device, 0) == STATUS_DEVICE_NOT_READY);
    assert(!device.HardwareReady && hardware_calls == 0);

    struct context fresh = {.IsBlackhole = TRUE, .HardwarePrepared = TRUE};
    assert(TtEvtDeviceD0Entry(&fresh, 0) == STATUS_SUCCESS);
    assert(fresh.HardwareReady && hardware_calls == 1);
    fresh.Detached = TRUE;
    assert(TtEvtDeviceD0Entry(&fresh, 0) == STATUS_DEVICE_NOT_READY);
    assert(!fresh.HardwareReady && hardware_calls == 1);
}
