// SPDX-License-Identifier: GPL-2.0-only
// Teardown/configuration exclusion contracts, using actual handler bodies.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define VOID void
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER -1
#define STATUS_ACCESS_DENIED -2
#define STATUS_DEVICE_REMOVED -3
#define STATUS_DEVICE_NOT_READY -4
#define ARRAYSIZE(a) (sizeof(a)/sizeof((a)[0]))
#define UNREFERENCED_PARAMETER(v) ((void)(v))
#define RtlZeroMemory(p,n) memset(p,0,n)
#define TENSTORRENT_MAX_INBOUND_TLBS 256
#define TT_TLB_TOTAL 210
#define PAGE_SIZE 4096
typedef int NTSTATUS;
typedef unsigned long ULONG;
typedef uint64_t UINT64;
typedef uintptr_t ULONG_PTR;
typedef struct link { struct link *Flink, *Blink; } LIST_ENTRY, *PLIST_ENTRY;
#define CONTAINING_RECORD(p,t,m) ((t *)((char *)(p)-offsetof(t,m)))
static void init_list(PLIST_ENTRY l) { l->Flink=l->Blink=l; }
static void insert(PLIST_ENTRY l,PLIST_ENTRY e) { e->Flink=l; e->Blink=l->Blink; l->Blink->Flink=e; l->Blink=e; }
static void RemoveEntryList(PLIST_ENTRY e) { e->Blink->Flink=e->Flink; e->Flink->Blink=e->Blink; }
typedef struct mapping { LIST_ENTRY Entry; void *UserVa; } TT_USER_MAPPING, *PTT_USER_MAPPING;
typedef struct pin { LIST_ENTRY Entry; UINT64 VirtualAddress, Size; long IatuRegion; } TT_PINNING, *PTT_PINNING;
typedef struct dmabuf { long IatuRegion; } *PTT_DMABUF;
typedef struct file {
 pthread_mutex_t *Lock; LIST_ENTRY Mappings, Pinnings, DeviceLink;
 long long OpenResetGen; struct file *FileObject; PTT_DMABUF DmaBufs[256];
} TT_FILE_CONTEXT, *PTT_FILE_CONTEXT, *WDFFILEOBJECT;
typedef struct context {
 pthread_mutex_t *TlbLock; int TlbUsed[210]; WDFFILEOBJECT TlbOwner[210];
 pthread_mutex_t *FileListLock; LIST_ENTRY FileList; long long ResetGen;
 int Active[16], HardwareReady, Detached;
} *PTT_DEVICE_CONTEXT;
typedef void *WDFREQUEST;
struct tenstorrent_unmap_in { UINT64 user_va, reserved; };
struct tenstorrent_unpin_pages_in { UINT64 virtual_address, size; uint32_t reserved; };
struct tenstorrent_configure_tlb_in { unsigned id; int config; };
struct tenstorrent_configure_tlb_out { unsigned reserved; };
struct tenstorrent_configure_tlb { struct tenstorrent_configure_tlb_in in; struct tenstorrent_configure_tlb_out out; };
static PTT_FILE_CONTEXT TtGetFileContext(WDFFILEOBJECT f) { return f; }
static NTSTATUS TtCopyInBuffer(WDFREQUEST r,void *p,size_t n) { memcpy(p,r,n); return 0; }
static void WdfWaitLockAcquire(pthread_mutex_t *m,void *t) { (void)t; assert(!pthread_mutex_lock(m)); }
static void WdfWaitLockRelease(pthread_mutex_t *m) { assert(!pthread_mutex_unlock(m)); }
static pthread_mutex_t file_lock=PTHREAD_MUTEX_INITIALIZER, tlb_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned unmapped, unpinned, configured;
static void TtDestroyUserMapping(PTT_USER_MAPPING m) {
 (void)m; assert(pthread_mutex_trylock(&file_lock)==EBUSY); ++unmapped;
}
static void TtDestroyPinning(PTT_DEVICE_CONTEXT c,PTT_PINNING p) {
 (void)c; (void)p; assert(pthread_mutex_trylock(&file_lock)==EBUSY); ++unpinned;
}
static NTSTATUS TtBhConfigureUserTlb(PTT_DEVICE_CONTEXT c,unsigned id,const int *cfg) {
 (void)c; (void)id; (void)cfg; assert(pthread_mutex_trylock(&tlb_lock)==EBUSY); ++configured; return 0;
}
static NTSTATUS TtCompleteSizedOutBuffer(WDFREQUEST r,size_t o,const void *v,size_t n,unsigned d) {
 (void)r; (void)o; (void)v; (void)n; (void)d; return 0;
}
static long long ReadAcquire64(long long *value) { return *value; }
static void TtTeardownNocDma(PTT_DEVICE_CONTEXT c,long region) {
 if(region>=0 && region<16) c->Active[region]=0;
}
/* DRIVER_FUNCTIONS */
int main(void) {
 struct file file={.Lock=&file_lock}; struct context context={.TlbLock=&tlb_lock};
 struct mapping mapping={.UserVa=(void *)0x1000}; struct pin pin={.VirtualAddress=0x1000,.Size=4096};
 struct tenstorrent_unmap_in unmap={.user_va=0x1000};
 struct tenstorrent_unpin_pages_in unpin={.virtual_address=0x1000,.size=4096};
 struct tenstorrent_configure_tlb_in configure={.id=17};
 init_list(&file.Mappings); init_list(&file.Pinnings);
 insert(&file.Mappings,&mapping.Entry); insert(&file.Pinnings,&pin.Entry);
 assert(TtIoctlUnmap(&context,&file,&unmap)==0 && unmapped==1);
 assert(TtIoctlUnmap(&context,&file,&unmap)==STATUS_INVALID_PARAMETER);
 assert(TtIoctlUnpinPages(&context,&file,&unpin)==0 && unpinned==1);
 assert(TtIoctlUnpinPages(&context,&file,&unpin)==STATUS_INVALID_PARAMETER);
 assert(TtIoctlConfigureTlb(&context,&file,&configure)==STATUS_ACCESS_DENIED && configured==0);
 context.TlbUsed[17]=1; context.TlbOwner[17]=&file;
 assert(TtIoctlConfigureTlb(&context,&file,&configure)==0 && configured==1);
 pthread_mutex_t list_lock=PTHREAD_MUTEX_INITIALIZER;
 struct file current={.Lock=&file_lock,.OpenResetGen=1};
 struct dmabuf old_buffer={.IatuRegion=0}, current_buffer={.IatuRegion=2};
 context.FileListLock=&list_lock; context.ResetGen=1;
 init_list(&context.FileList); init_list(&current.Pinnings);
 file.FileObject=&file; current.FileObject=&current;
 file.DmaBufs[1]=&old_buffer; current.DmaBufs[1]=&current_buffer;
 pin.IatuRegion=1; insert(&file.Pinnings,&pin.Entry);
 insert(&context.FileList,&file.DeviceLink); insert(&context.FileList,&current.DeviceLink);
 context.Active[0]=context.Active[1]=context.Active[2]=1;
 context.TlbUsed[18]=1; context.TlbOwner[18]=&current;
 TtMemoryReclaimStale(&context);
 assert(old_buffer.IatuRegion==-1 && pin.IatuRegion==-1);
 assert(!context.Active[0] && !context.Active[1] && context.Active[2]);
 assert(context.TlbOwner[17]==NULL && !context.TlbUsed[17]);
 assert(context.TlbOwner[18]==&current && context.TlbUsed[18]);
 context.Active[0]=context.Active[1]=1; // slots now assigned to new buffers
 TtMemoryReclaimStale(&context);
 assert(context.Active[0] && context.Active[1]); // old references cannot disable reuse
 assert(TtCheckIoGates(&context,&current)==STATUS_DEVICE_NOT_READY);
 context.HardwareReady=1;
 assert(TtCheckIoGates(&context,&current)==0);
 assert(TtCheckIoGates(&context,&file)==STATUS_DEVICE_REMOVED);
 context.Detached=1;
 assert(TtCheckIoGates(&context,&current)==STATUS_DEVICE_REMOVED);

}
