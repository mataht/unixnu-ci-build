/* SPDX-License-Identifier: MIT */
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoPool.h"
/* Where a call that never returns is stuck: the free/unmap/release path stores a stage number here, the board page shows it. */
extern "C" {volatile uint32_t native_adreno_trace_stage;}
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <kern/task.h>
extern "C" boolean_t PE_parse_boot_argn(const char *,void *,int);

static bool outputWidth(unsigned bits)
{return bits==32 || bits==36 || bits==40 || bits==42 || bits==44 || bits==48;}
/* Where the last import stopped (10..19, 19 = done), and for a refused page the physical address and offset: the board log shows them. */
enum : IOReturn {kNativeAdrenoProtectionFailure=2};   /* KERN_PROTECTION_FAILURE, as prepare() reports a range that cannot be wired for writing */
extern "C" {volatile uint32_t native_adreno_import_stage;volatile uint64_t native_adreno_import_fail_pa,native_adreno_import_fail_offset;}
/* BO memory for the board log: [0] user BOs from the system (allocateSystem), [1] their spans, [2] refused, [3] the last refusal's status,
 * [4] MiB of them live, [5] the peak, [6] kernel-mapped BOs (allocate), [7] import spans. */
extern "C" {volatile uint32_t native_adreno_bo_memory[8];}
/* Contiguous BO runs: [0] made, [1] refused by the free memory (those BOs fell back to pageable pages). */
extern "C" {volatile uint32_t native_adreno_bo_contig[2];}
/* Who holds the system BOs, for the board log: a row a process {pid, live KiB, live BOs, peak KiB}, the 24 processes that hold the
 * most kept (a process that released everything gives its row up); and the live BOs by size, {count, KiB} for sizes up to 64 KiB,
 * 1 MiB, 4 MiB, 16 MiB, 64 MiB and above. Both change under the driver's lock, as systemBytes does. */
extern "C" {volatile uint32_t native_adreno_bo_owners[24][4];volatile uint32_t native_adreno_bo_sizes[6][2];int proc_selfpid(void);}
/* Imported memory the driver keeps wired: an import pins another mapping's pages (prepare()) from importTask until release(), whether
 * or not the GPU maps them. [0] imports alive, [1] KiB of them, [2] the peak KiB, [3] imports made so far. native_adreno_import_owners
 * holds the importing processes as native_adreno_bo_owners holds the allocating ones: {pid, live KiB, live imports, peak KiB}. A range
 * imported twice counts twice, although its pages are wired once. Both change under the driver's lock. */
extern "C" {volatile uint32_t native_adreno_import_memory[4];volatile uint32_t native_adreno_import_owners[24][4];}
namespace {
uint64_t systemBytes,systemPeak;
bool pressure;
unsigned sizeClass(uint64_t bytes)
{
    static const uint64_t limit[5]={UINT64_C(64)<<10,UINT64_C(1)<<20,UINT64_C(4)<<20,UINT64_C(16)<<20,UINT64_C(64)<<20};
    unsigned c=0;while(c<5 && bytes>limit[c])++c;
    return c;
}
}
void NativeAdrenoNoteHolder(volatile uint32_t (*table)[4],int32_t pid,int64_t bytes)
{
    const uint32_t kib=(uint32_t)((uint64_t)(bytes<0?-bytes:bytes)>>10);
    unsigned row=24,spare=24,least=0;
    for(unsigned i=0;i<24;++i) {
        if(table[i][2] && table[i][0]==(uint32_t)pid) {row=i;break;}
        if(!table[i][2] && spare==24)spare=i;
        if(table[i][1]<table[least][1])least=i;
    }
    if(bytes<0) {
        if(row==24)return;
        table[row][1]=table[row][1]>kib?table[row][1]-kib:0;
        table[row][2]=table[row][2]-1;
        return;
    }
    if(row==24) {
        /* A new holder takes a free row, else the row of the smallest holder once it holds more than that one. */
        row=spare!=24?spare:least;
        if(spare==24 && table[least][1]>=kib)return;
        table[row][0]=(uint32_t)pid;table[row][1]=0;table[row][2]=0;table[row][3]=0;
    }
    table[row][1]=table[row][1]+kib;
    table[row][2]=table[row][2]+1;
    if(table[row][1]>table[row][3])table[row][3]=table[row][1];
}
namespace {
void noteSystem(int64_t bytes,int32_t pid)
{
    systemBytes+=bytes;if(systemBytes>systemPeak)systemPeak=systemBytes;
    native_adreno_bo_memory[4]=(uint32_t)(systemBytes>>20);native_adreno_bo_memory[5]=(uint32_t)(systemPeak>>20);
    const unsigned c=sizeClass((uint64_t)(bytes<0?-bytes:bytes));
    const uint32_t kib=(uint32_t)((uint64_t)(bytes<0?-bytes:bytes)>>10);
    if(bytes>0) {native_adreno_bo_sizes[c][0]=native_adreno_bo_sizes[c][0]+1;native_adreno_bo_sizes[c][1]=native_adreno_bo_sizes[c][1]+kib;}
    else {
        native_adreno_bo_sizes[c][0]=native_adreno_bo_sizes[c][0]?native_adreno_bo_sizes[c][0]-1:0;
        native_adreno_bo_sizes[c][1]=native_adreno_bo_sizes[c][1]>kib?native_adreno_bo_sizes[c][1]-kib:0;
    }
    NativeAdrenoNoteHolder(native_adreno_bo_owners,pid,bytes);
}
struct ImportPageRef { uint64_t pa; uint32_t refs; };
static ImportPageRef *gImportPageRefs;
static ImportPageRef *importPageRefMap()
{
    static bool tried;
    if (!gImportPageRefs && !tried) {
        tried = true;
        gImportPageRefs = static_cast<ImportPageRef *>(IOMallocZero(4096u * sizeof(ImportPageRef)));
    }
    return gImportPageRefs;
}
static void bumpImportPageRef(uint64_t pa, int delta)
{
    ImportPageRef *table = pa ? importPageRefMap() : nullptr;
    if (!table) return;
    const uint32_t mask = 4095u;
    uint32_t h = (uint32_t)((pa >> 12) * 2654435761ull) & mask;
    uint32_t first_free = 4096u;
    for (uint32_t step = 0; step < 4096u; ++step) {
        ImportPageRef &slot = table[(h + step) & mask];
        if (slot.refs && slot.pa == pa) {
            if (delta > 0) {
                if (slot.refs < UINT32_MAX) ++slot.refs;
            } else if (slot.refs > 1) {
                --slot.refs;
            } else {
                slot = {0, 0};
            }
            return;
        }
        if (!slot.refs && first_free == 4096u) first_free = (h + step) & mask;
    }
    if (delta > 0 && first_free < 4096u) {
        table[first_free] = {pa, 1u};
    }
}
static uint32_t lookupImportPageRef(uint64_t pa)
{
    const ImportPageRef *table = pa ? gImportPageRefs : nullptr;
    if (!table) return 0;
    const uint32_t mask = 4095u;
    uint32_t h = (uint32_t)((pa >> 12) * 2654435761ull) & mask;
    for (uint32_t step = 0; step < 4096u; ++step) {
        const ImportPageRef &slot = table[(h + step) & mask];
        if (slot.refs && slot.pa == pa) return slot.refs;
    }
    return 0;
}
void noteImport(int64_t bytes,int32_t pid)
{
    static uint64_t importBytes,importPeak;
    importBytes+=bytes;if(importBytes>importPeak)importPeak=importBytes;
    if(bytes>0) {native_adreno_import_memory[0]=native_adreno_import_memory[0]+1;native_adreno_import_memory[3]=native_adreno_import_memory[3]+1;}
    else native_adreno_import_memory[0]=native_adreno_import_memory[0]?native_adreno_import_memory[0]-1:0;
    native_adreno_import_memory[1]=(uint32_t)(importBytes>>10);native_adreno_import_memory[2]=(uint32_t)(importPeak>>10);
    NativeAdrenoNoteHolder(native_adreno_import_owners,pid,bytes);
}
}
bool NativeAdrenoMemory::memoryPressure() {return pressure;}
/* Which DRAM pages this driver's own BOs hold, a bit a page over the kernel's physical range: an import (another task's pages) aliases
 * one of them only where a bit is set, so proving an import exclusive costs its own pages, not the ranges mapped so far (every BO is
 * scattered pages: a pairwise check over them ran out of budget on each import). Made on first use; without it every page counts as
 * owned (the full alias check). */
extern "C" uint64_t gPhysBase,gPhysSize;
namespace {
uint64_t *ownedBits;
/* The bits of the kernel's own range, then those of the first 4 GiB below it (DRAM a kernel adopts outside its range). */
const uint64_t kOwnedLowPages=UINT64_C(1)<<20;
uint64_t *ownedMap()
{
    static bool tried;
    if(!ownedBits && !tried) {tried=true;ownedBits=static_cast<uint64_t *>(IOMallocZero((size_t)((gPhysSize/4096+kOwnedLowPages+63)/64*8)));}
    return ownedBits;
}
/* The page's bit; false for a page that has none (neither in the kernel's range nor below it within 4 GiB). */
bool ownedIndex(uint64_t page,uint64_t *at)
{
    if(page>=gPhysBase) {
        if(page-gPhysBase>=gPhysSize)return false;
        *at=(page-gPhysBase)/4096;
        return true;
    }
    if(page>>32)return false;
    *at=gPhysSize/4096+page/4096;
    return true;
}
void noteOwned(const NativeAdrenoPhysicalSpan *spans,size_t count,bool owned)
{
    uint64_t *bits=ownedMap();
    if(!bits)return;
    for(size_t i=0;i<count;++i)for(uint64_t page=spans[i].physical;page<spans[i].physical+spans[i].bytes;page+=4096) {
        uint64_t at=0;
        if(!ownedIndex(page,&at))continue;
        const uint64_t bit=UINT64_C(1)<<(at%64);
        if(owned)__atomic_fetch_or(&bits[at/64],bit,__ATOMIC_RELAXED);else __atomic_fetch_and(&bits[at/64],~bit,__ATOMIC_RELAXED);
    }
}
}
bool NativeAdrenoMemory::ownsAnyPage(const NativeAdrenoPhysicalSpan *spans,size_t count)
{
    const uint64_t *bits=ownedMap();
    if(!bits)return true;
    for(size_t i=0;i<count;++i)for(uint64_t page=spans[i].physical;page<spans[i].physical+spans[i].bytes;page+=4096) {
        uint64_t at=0;
        if(!ownedIndex(page,&at))continue;
        if(__atomic_load_n(&bits[at/64],__ATOMIC_RELAXED)>>(at%64)&1)return true;
    }
    return false;
}
/* The descriptor's pages as spans (neighbours merged), each below `limit` and, for a contiguous allocation, following the first. */
IOReturn NativeAdrenoMemory::takeSegments(IOBufferMemoryDescriptor *descriptor,size_t bytes,uint64_t limit,bool contiguous)
{
    span_capacity_=bytes/4096;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_)return kIOReturnNoMemory;
    for(size_t offset=0;offset<bytes;) {
        IOByteCount length=0;
        addr64_t physical=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        size_t step=(size_t)(length&~IOByteCount(4095));
        if(step>bytes-offset)step=bytes-offset;
        if(!physical || (physical&4095) || step<4096 || physical>=limit || step>limit-physical)return kIOReturnUnsupported;
        if(contiguous && offset && physical!=spans_[0].physical+offset)return kIOReturnUnsupported;
        if(span_count_ && physical==spans_[span_count_-1].physical+spans_[span_count_-1].bytes)
            spans_[span_count_-1].bytes+=step;
        else spans_[span_count_++]={offset,physical,step};
        offset+=step;
    }
    return kIOReturnSuccess;
}
/* The grant's runs as spans, each below `limit`. */
IOReturn NativeAdrenoMemory::takeRuns(const NativeAdrenoPoolGrant *grant,size_t bytes,uint64_t limit)
{
    if(!grant->count || grant->bytes!=bytes)return kIOReturnUnsupported;
    span_capacity_=grant->count;
    spans_=IONewData(NativeAdrenoPhysicalSpan,span_capacity_);
    if(!spans_)return kIOReturnNoMemory;
    uint64_t offset=0;
    for(uint32_t i=0;i<grant->count;++i) {
        const NativeAdrenoPoolRun &run=grant->runs[i];
        if(!run.physical || !run.bytes || ((run.physical|run.bytes)&4095) || run.bytes>bytes-offset || run.physical>=limit ||
            run.bytes>limit-run.physical)return kIOReturnUnsupported;
        spans_[span_count_++]={offset,run.physical,run.bytes};
        offset+=run.bytes;
    }
    return offset==bytes?kIOReturnSuccess:kIOReturnUnsupported;
}
/* The first kernel allocation that failed: [0] where (1 no descriptor, 2 prepare, 3 after it), [1] status, [2] bytes, [3] address
 * bits, [4] contiguous, [5] failures so far. */
extern "C" {volatile uint32_t native_adreno_kernel_alloc_fail[6];}
static void noteKernelFailure(uint32_t where,IOReturn status,size_t bytes,unsigned bits,bool contiguous)
{
    volatile uint32_t *f=native_adreno_kernel_alloc_fail;
    if(!f[5]) {f[0]=where;f[1]=(uint32_t)status;f[2]=(uint32_t)bytes;f[3]=bits;f[4]=contiguous;}
    f[5]=f[5]+1;
}
IOReturn NativeAdrenoMemory::allocate(size_t bytes,unsigned bits,bool contiguous,size_t budget)
{
    if(descriptor_ || grant_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget)
        return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    uint64_t limit=UINT64_C(1)<<bits;
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if(contiguous)options|=kIOMemoryPhysicallyContiguous;
    descriptor_=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,options,bytes,(limit-1)&~UINT64_C(4095));
    if(!descriptor_) {noteKernelFailure(1,kIOReturnNoMemory,bytes,bits,contiguous);return kIOReturnNoMemory;}
    status=descriptor_->prepare();
    if(status!=kIOReturnSuccess) {noteKernelFailure(2,status,bytes,bits,contiguous);descriptor_->release();descriptor_=nullptr;return status;}
    prepared_=true;bytes_=bytes;mapping_=descriptor_->getBytesNoCopy();
    status=kIOReturnUnsupported;
    if(!mapping_ || ((uintptr_t)mapping_&4095) || bytes>UINTPTR_MAX-(uintptr_t)mapping_ ||
        descriptor_->getLength()!=bytes)goto failed;
    status=takeSegments(descriptor_,bytes,limit,contiguous);
    if(status!=kIOReturnSuccess)goto failed;
    bzero(mapping_,bytes);
    status=NativeAdrenoMaintainOwnedPages(mapping_,bytes,NativeAdrenoClean);
    if(status!=kIOReturnSuccess)goto failed;
    NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes);
    noteOwned(spans_,span_count_,true);owned_=true;
    native_adreno_bo_memory[6]=native_adreno_bo_memory[6]+1;
    return kIOReturnSuccess;
failed:
    noteKernelFailure(3,status,bytes,bits,contiguous);
    /* release() deliberately retains descriptor/metadata if complete fails.
     * Preserve the allocation's original failure instead of reporting cleanup
     * as success. No object has been exposed to a GPU or userspace yet. */
    (void)release();return status;
}
/* No task: the memory is mapped nowhere until a client view maps it. The cache mode is the memory entry's, so every mapping of it (the
 * client views) and the pages themselves take it. */
static IOBufferMemoryDescriptor *systemDescriptor(size_t bytes,bool write_combined,uint64_t *made=nullptr,uint64_t *wired=nullptr)
{
    IOOptionBits options=static_cast<IOOptionBits>(kIODirectionInOut)|static_cast<IOOptionBits>(kIOMemoryMapperNone)|
        static_cast<IOOptionBits>(kIOMemoryPageable);
    if(write_combined)options|=kIOMapWriteCombineCache;
    /* Pageable pages arrive one by one (a BO averaged 69 spans); from `gpu-bo-contig` KiB up to 128 KiB a BO first asks for one
     * physically contiguous run (one span, one table walk), and a request the free memory cannot serve falls back to pageable pages.
     * A contiguous request the kernel cannot serve scans every free page before it gives up: in ibb10331f24 124 such searches (0.1 to
     * 64 MiB) took 163 ms each at the median and 21.7 s in all, synchronously in the caller (the render server's and the widget
     * renderer's allocations, the stalls at an app launch and on the widget page), while every success was 64 to 128 KiB. So larger
     * BOs never ask, and after a failure no request of that size or larger asks again for 5 s. */
    static uint32_t contigKiB=UINT32_MAX;
    if(contigKiB==UINT32_MAX) {contigKiB=0;PE_parse_boot_argn("gpu-bo-contig",&contigKiB,sizeof contigKiB);}
    static const size_t contigMax=size_t(128)*1024;
    static size_t failedBytes=0;
    static uint64_t failedAt=0,backoff=0;
    if(!backoff)nanoseconds_to_absolutetime(UINT64_C(5000000000),&backoff);
    const uint64_t now=mach_absolute_time();
    if(failedBytes && now-failedAt>=backoff)failedBytes=0;
    if(contigKiB && bytes>=size_t(contigKiB)*1024 && bytes<=contigMax && !(failedBytes && bytes>=failedBytes)) {
        IOBufferMemoryDescriptor *run=IOBufferMemoryDescriptor::inTaskWithPhysicalMask(nullptr,
            (options&~static_cast<IOOptionBits>(kIOMemoryPageable))|static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),bytes,UINT64_C(0xfffffffff000));
        if(run && run->prepare()==kIOReturnSuccess) {native_adreno_bo_contig[0]=native_adreno_bo_contig[0]+1;return run;}
        if(run)run->release();
        native_adreno_bo_contig[1]=native_adreno_bo_contig[1]+1;
        if(!failedBytes || bytes<failedBytes)failedBytes=bytes;
        failedAt=now;
    }
    IOBufferMemoryDescriptor *descriptor=IOBufferMemoryDescriptor::inTaskWithOptions(nullptr,options,bytes,4096);
    if(made)*made=mach_absolute_time();
    if(descriptor && descriptor->prepare()!=kIOReturnSuccess) {descriptor->release();descriptor=nullptr;}
    if(wired)*wired=mach_absolute_time();
    return descriptor;
}
IOReturn NativeAdrenoMemory::prepareSystem(size_t bytes,bool write_combined,NativeAdrenoSystemPages *out,unsigned bits)
{
    if(!out || out->held() || !bytes || (bytes&4095) || (bits && !outputWidth(bits)))return kIOReturnBadArgument;
    /* Pool pages first: taken and zero-filled here, outside the driver's lock. Nothing to clean, no cache holds them. */
    if(bits) {
        NativeAdrenoPoolGrant *grant=nullptr;
        if(NativeAdrenoPoolAllocate(bytes,UINT64_C(1)<<bits,write_combined,&grant)==kIOReturnSuccess) {
            out->grant=grant;out->bytes=bytes;out->write_combined=write_combined;out->cleaned=true;
            out->made=out->wired=mach_absolute_time();
            return kIOReturnSuccess;
        }
    }
    uint64_t made=0,wired=0;
    IOBufferMemoryDescriptor *descriptor=systemDescriptor(bytes,write_combined,&made,&wired);
    if(!descriptor)return kIOReturnNoMemory;
    if(!made)made=wired=mach_absolute_time();   /* a contiguous run: made and wired in one step */
    if(descriptor->getLength()!=bytes) {descriptor->complete();descriptor->release();return kIOReturnUnsupported;}
    NativeAdrenoPhysicalSpan *pre_spans=nullptr;
    size_t pre_count=0,pre_cap=0;
    if(bits) {
        const uint64_t limit=UINT64_C(1)<<bits;
        pre_cap=bytes/4096;
        pre_spans=IONewData(NativeAdrenoPhysicalSpan,pre_cap);
        if(pre_spans) {
            bool ok=true;
            for(size_t offset=0;offset<bytes;) {
                IOByteCount length=0;
                const addr64_t physical=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
                size_t step=(size_t)(length&~IOByteCount(4095));
                if(step>bytes-offset)step=bytes-offset;
                if(!physical || (physical&4095) || step<4096 || physical>=limit || step>limit-physical) {ok=false;break;}
                if(pre_count && physical==pre_spans[pre_count-1].physical+pre_spans[pre_count-1].bytes)
                    pre_spans[pre_count-1].bytes+=step;
                else pre_spans[pre_count++]={offset,physical,step};
                offset+=step;
            }
            if(!ok) {IODeleteData(pre_spans,NativeAdrenoPhysicalSpan,pre_cap);pre_spans=nullptr;pre_count=pre_cap=0;}
        } else pre_cap=0;
    }
    bool cleaned=false;
    /* Default-attribute pages may still hold the zero fill in a CPU's cache: written back here, by physical runs, as allocateSystem would. */
    if(!write_combined) {
        cleaned=true;
        if(pre_spans) {
            for(size_t i=0;i<pre_count && cleaned;++i)
                if(NativeAdrenoMaintainPhysicalPages(pre_spans[i].physical,pre_spans[i].bytes,NativeAdrenoClean)!=kIOReturnSuccess)cleaned=false;
        } else {
            for(size_t offset=0;offset<bytes && cleaned;) {
                IOByteCount length=0;
                const addr64_t physical=descriptor->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
                if(!physical || (physical&4095) || length<4096) {cleaned=false;break;}
                if(length>bytes-offset)length=bytes-offset;
                length&=~IOByteCount(4095);
                if(NativeAdrenoMaintainPhysicalPages(physical,length,NativeAdrenoClean)!=kIOReturnSuccess)cleaned=false;
                offset+=length;
            }
        }
        if(cleaned)NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes);
    }
    out->descriptor=descriptor;out->spans=pre_spans;out->bytes=bytes;out->span_count=pre_count;out->span_capacity=pre_cap;
    out->output_bits=pre_spans?bits:0;out->write_combined=write_combined;out->cleaned=cleaned;out->made=made;out->wired=wired;
    return kIOReturnSuccess;
}
void NativeAdrenoMemory::discardSystem(NativeAdrenoSystemPages *pages)
{
    if(!pages || !pages->held())return;
    if(pages->imported) {pages->imported->complete();pages->imported->release();}
    if(pages->descriptor) {pages->descriptor->complete();pages->descriptor->release();}
    /* The BO's reference to its pool pages: they go back once no client view holds them either. */
    if(pages->grant)NativeAdrenoPoolRelease(pages->grant);
    if(pages->spans)IODeleteData(pages->spans,NativeAdrenoPhysicalSpan,pages->span_capacity);
    *pages=NativeAdrenoSystemPages{};
}
IOReturn NativeAdrenoMemory::allocateSystem(size_t bytes,unsigned bits,size_t budget,bool write_combined,NativeAdrenoSystemPages *prepared)
{
    if(descriptor_ || grant_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!bytes || (bytes&4095) || !outputWidth(bits) || !budget)return kIOReturnBadArgument;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    /* Pages prepared outside the lock for this very request are taken over; any other request makes its own. */
    bool cleaned=false,prebuilt_spans=false;
    if(prepared && prepared->held() && !prepared->imported && prepared->bytes==bytes && prepared->write_combined==write_combined) {
        descriptor_=prepared->descriptor;grant_=prepared->grant;cleaned=prepared->cleaned;
        if(descriptor_ && prepared->spans && prepared->output_bits==bits) {
            spans_=prepared->spans;span_count_=prepared->span_count;span_capacity_=prepared->span_capacity;
            prepared->spans=nullptr;prepared->span_count=prepared->span_capacity=0;
            prebuilt_spans=true;
        } else if(prepared->spans) {
            IODeleteData(prepared->spans,NativeAdrenoPhysicalSpan,prepared->span_capacity);
            prepared->spans=nullptr;prepared->span_count=prepared->span_capacity=0;
        }
        *prepared=NativeAdrenoSystemPages{};
    } else if(prepared || NativeAdrenoPoolAllocate(bytes,UINT64_C(1)<<bits,write_combined,&grant_)!=kIOReturnSuccess) {
        /* A caller that offers prepared pages holds the driver's lock: the pool's zero fill maps its run in the kernel map and is not
         * done under it (prepareSystem already asked the pool, outside the lock). */
        grant_=nullptr;descriptor_=systemDescriptor(bytes,write_combined);
    }
    if(grant_) {
        /* Pool pages: the grant's runs are the spans. A grant that does not fit this client's physical limit after all (it was
         * prepared for another width) goes back and the VM's pages take its place. */
        const IOReturn taken=takeRuns(grant_,bytes,UINT64_C(1)<<bits);
        if(taken!=kIOReturnSuccess) {
            if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
            spans_=nullptr;span_count_=span_capacity_=0;
            NativeAdrenoPoolRelease(grant_);grant_=nullptr;cleaned=false;
            descriptor_=systemDescriptor(bytes,write_combined);
        }
    }
    status=descriptor_ || grant_?kIOReturnSuccess:kIOReturnNoMemory;
    if(status==kIOReturnSuccess) {
        system_=true;prepared_=true;bytes_=bytes;write_combined_=write_combined;owner_pid_=proc_selfpid();noteSystem((int64_t)bytes,owner_pid_);
        if(descriptor_) {
            if(descriptor_->getLength()!=bytes)status=kIOReturnUnsupported;
            else if(!prebuilt_spans)status=takeSegments(descriptor_,bytes,UINT64_C(1)<<bits,false);
        }
        /* Default-attribute pages may still hold the zero fill in this CPU's cache: written back before the GPU reads them. */
        if(status==kIOReturnSuccess && descriptor_ && !write_combined && !cleaned) {
            status=maintainImported(0,bytes,NativeAdrenoClean);
            NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes);
        }
        if(status!=kIOReturnSuccess) {write_combined_=false;(void)release();}
    }
    if(status!=kIOReturnSuccess) {
        pressure=true;
        native_adreno_bo_memory[2]=native_adreno_bo_memory[2]+1;native_adreno_bo_memory[3]=(uint32_t)status;
        return status;
    }
    pressure=false;
    noteOwned(spans_,span_count_,true);owned_=true;
    native_adreno_bo_memory[0]=native_adreno_bo_memory[0]+1;native_adreno_bo_memory[1]=native_adreno_bo_memory[1]+(uint32_t)span_count_;
    return kIOReturnSuccess;
}
/* An import holds the pages through a memory entry of its own (kIOMemoryPersistent), which references the VM object: the owner may unmap
 * its range while the GPU still uses it (Core Animation frees an image's Shmem once its no-copy buffer is released, before the runtime frees
 * the import), and the object then lives on until the import completes instead of being torn down under a wired UPL (int-233..235: the render
 * thread stuck in vm_deallocate). A range no memory entry can be made of is imported as before; [0] counts the persistent imports, [1] those. */
extern "C" {volatile uint32_t native_adreno_import_persistent[2];}
/* Imports whose pages were all non-cacheable ([1]: no CPU maintenance for them) or not ([0]). */
extern "C" {volatile uint32_t native_adreno_pool_stats_uncached[2];}
static IOMemoryDescriptor *importRange(uint64_t address,size_t bytes,IODirection direction,void *task)
{
    const IOOptionBits options=static_cast<IOOptionBits>(direction)|static_cast<IOOptionBits>(kIOMemoryMapperNone);
    IOMemoryDescriptor *range=IOMemoryDescriptor::withAddressRange(address,bytes,options|kIOMemoryPersistent,static_cast<task_t>(task));
    if(range) {__atomic_fetch_add(&native_adreno_import_persistent[0],1u,__ATOMIC_RELAXED);return range;}
    range=IOMemoryDescriptor::withAddressRange(address,bytes,options,static_cast<task_t>(task));
    if(range)__atomic_fetch_add(&native_adreno_import_persistent[1],1u,__ATOMIC_RELAXED);
    return range;
}
IOReturn NativeAdrenoMemory::prepareImport(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget,
    NativeAdrenoImportedPages *out)
{
    if(!out || out->held())return kIOReturnBadArgument;
    out->stage=11;
    if(!task || !address || (address&4095) || !bytes || (bytes&4095) || address>UINT64_MAX-bytes || !outputWidth(bits) ||
        !budget)return kIOReturnBadArgument;
    out->stage=12;
    if(bytes>budget)return kIOReturnNoResources;
    NativeAdrenoCpuState cpu{};
    out->stage=13;
    IOReturn status=NativeAdrenoQueryCpu4K(&cpu);if(status!=kIOReturnSuccess)return status;
    const uint64_t limit=UINT64_C(1)<<bits;
    IOMemoryDescriptor *imported=importRange(address,bytes,kIODirectionInOut,task);
    out->stage=14;
    if(!imported)return kIOReturnNoMemory;
    out->stage=15;
    bool read_only=false;
    status=imported->prepare();
    if(status==kNativeAdrenoProtectionFailure) {
        imported->release();imported=nullptr;
        imported=importRange(address,bytes,kIODirectionOut,task);
        if(!imported)return kIOReturnNoMemory;
        status=imported->prepare();
        if(status==kIOReturnSuccess)read_only=true;
    }
    if(status!=kIOReturnSuccess) {imported->release();return status;}
    out->stage=16;
    if(imported->getLength()!=bytes) {imported->complete();imported->release();return kIOReturnUnsupported;}
    out->stage=17;
    const size_t cap=bytes/4096;
    NativeAdrenoPhysicalSpan *spans=IONewData(NativeAdrenoPhysicalSpan,cap);
    if(!spans) {imported->complete();imported->release();return kIOReturnNoMemory;}
    size_t sc=0;
    for(size_t offset=0;offset<bytes;) {
        IOByteCount length=0;
        const addr64_t physical=imported->getPhysicalSegment(offset,&length,kIOMemoryMapperNone);
        size_t step=(size_t)(length&~IOByteCount(4095));
        if(step>bytes-offset)step=bytes-offset;
        if(!physical || (physical&4095) || step<4096 || physical>=limit || step>limit-physical) {
            out->stage=18;out->fail_pa=physical;out->fail_offset=offset;
            IODeleteData(spans,NativeAdrenoPhysicalSpan,cap);imported->complete();imported->release();
            return kIOReturnUnsupported;
        }
        if(NativeAdrenoPoolContains(physical,step)) {
            out->stage=18;out->fail_pa=physical;out->fail_offset=offset;
            IODeleteData(spans,NativeAdrenoPhysicalSpan,cap);imported->complete();imported->release();
            return kIOReturnNotPermitted;
        }
        if(sc && physical==spans[sc-1].physical+spans[sc-1].bytes)spans[sc-1].bytes+=step;
        else spans[sc++]={offset,physical,step};
        offset+=step;
    }
    out->stage=19;
    const bool uncached=NativeAdrenoPagesUncached(spans,sc);
    bool cleaned=false;
    if(!uncached) {
        cleaned=true;
        const NativeAdrenoCacheOperation op=read_only?NativeAdrenoClean:NativeAdrenoCleanInvalidate;
        for(size_t i=0;i<sc && cleaned;++i)
            if(NativeAdrenoMaintainPhysicalPages(spans[i].physical,spans[i].bytes,op)!=kIOReturnSuccess)cleaned=false;
    }
    out->imported=imported;out->spans=spans;out->bytes=bytes;out->span_count=sc;out->span_capacity=cap;
    out->output_bits=bits;out->read_only=read_only;out->owner_uncached=uncached;out->cleaned=cleaned;
    return kIOReturnSuccess;
}
void NativeAdrenoMemory::discardImport(NativeAdrenoImportedPages *pages)
{
    if(!pages || !pages->held())return;
    if(pages->imported) {pages->imported->complete();pages->imported->release();}
    if(pages->spans)IODeleteData(pages->spans,NativeAdrenoPhysicalSpan,pages->span_capacity);
    *pages=NativeAdrenoImportedPages{};
}
IOReturn NativeAdrenoMemory::adoptImport(NativeAdrenoImportedPages *prepared)
{
    native_adreno_import_stage=10;
    if(descriptor_ || grant_ || imported_ || spans_ || quarantined_)return kIOReturnBusy;
    if(!prepared || !prepared->imported || !prepared->spans || !prepared->span_count || !prepared->bytes)return kIOReturnBadArgument;
    imported_=prepared->imported;spans_=prepared->spans;bytes_=prepared->bytes;
    span_count_=prepared->span_count;span_capacity_=prepared->span_capacity;
    read_only_=prepared->read_only;owner_uncached_=prepared->owner_uncached;import_cleaned_=prepared->cleaned;
    prepared_=true;
    *prepared=NativeAdrenoImportedPages{};
    native_adreno_import_stage=19;
    if(import_cleaned_)NativeAdrenoNotePhase(NativeAdrenoPhaseMap,bytes_);
    native_adreno_pool_stats_uncached[owner_uncached_]=native_adreno_pool_stats_uncached[owner_uncached_]+1;
    native_adreno_bo_memory[7]=native_adreno_bo_memory[7]+(uint32_t)span_count_;
    owner_pid_=proc_selfpid();import_noted_=true;noteImport((int64_t)bytes_,owner_pid_);
    if(span_count_)bumpImportPageRef(spans_[0].physical,1);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::importTask(void *task,uint64_t address,size_t bytes,unsigned bits,size_t budget)
{
    native_adreno_import_stage=10;
    if(descriptor_ || grant_ || imported_ || spans_ || quarantined_)return kIOReturnBusy;
    NativeAdrenoImportedPages imp{};
    const IOReturn status=prepareImport(task,address,bytes,bits,budget,&imp);
    native_adreno_import_stage=imp.stage;
    if(status!=kIOReturnSuccess) {
        if(imp.stage==18) {native_adreno_import_fail_pa=imp.fail_pa;native_adreno_import_fail_offset=imp.fail_offset;}
        discardImport(&imp);
        return status;
    }
    const IOReturn adopted=adoptImport(&imp);
    if(adopted!=kIOReturnSuccess)discardImport(&imp);
    return adopted;
}
IOReturn NativeAdrenoMemory::borrow(void *mapping,uint64_t physical,size_t bytes)
{
    if(descriptor_ || grant_ || imported_ || spans_ || quarantined_ || borrowed_)return kIOReturnBusy;
    if(!mapping || ((uintptr_t)mapping&4095) || !physical || (physical&4095) || !bytes || (bytes&4095))return kIOReturnBadArgument;
    spans_=IONewData(NativeAdrenoPhysicalSpan,1);
    if(!spans_)return kIOReturnNoMemory;
    span_capacity_=1;span_count_=1;spans_[0]={0,physical,bytes};
    mapping_=mapping;bytes_=bytes;prepared_=true;borrowed_=true;
    bzero(mapping_,bytes);
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::makeWriteCombined()
{
    if(!descriptor_ || system_ || imported_ || !prepared_ || quarantined_ || device_references_ || write_combined_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoCleanInvalidate);
    if(status)return status;
    NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes_);
    status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,true);
    if(status)return status;
    write_combined_=true;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::declareSynchronized()
{
    if(!descriptor_ || system_ || imported_ || borrowed_ || !prepared_ || quarantined_ || device_references_ || !mapping_ || !span_count_)
        return kIOReturnNotReady;
    const IOReturn status=NativeAdrenoMaintainOwnedPages(mapping_,bytes_,NativeAdrenoCleanInvalidate);
    if(status)return status;
    NativeAdrenoNotePhase(NativeAdrenoPhaseAllocate,bytes_);
    synchronized_=true;return kIOReturnSuccess;
}
/* Frees of an owned BO whose first pages a user task still maps: [0] count, [1] the last mapping's page number. */
extern "C" {volatile uint32_t native_adreno_free_user_mapped[2];}
extern "C" unsigned int native_pmap_user_mappings(ppnum_t,void **,uint64_t *,char *) __attribute__((weak_import));
IOReturn NativeAdrenoMemory::release()
{
    native_adreno_trace_stage=30;
    if(device_references_ || quarantined_)return kIOReturnBusy;
    /* System memory's pages go back to the VM with the default attribute (vm_object_reap), as they came. */
    if(system_)write_combined_=false;
    if(write_combined_) {
        /* The pages go back to the VM cacheable, as they came. */
        const IOReturn status=NativeAdrenoSetPagesCacheMode(spans_,span_count_,false);
        if(status) {quarantined_=true;return status;}
        write_combined_=false;
    }
    if(imported_) {
        if(prepared_) {
            IOReturn status=imported_->complete();
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        imported_->release();imported_=nullptr;
    }
    if(descriptor_ && !system_ && native_pmap_user_mappings && spans_) {
        /* A page a user task still maps (a BO view over physical pages holds no reference to them) is counted before the pages are freed:
         * the first page of up to 64 spans, then which process maps it. XNU frees such a page under the mapping (pmap_assert_free). A view
         * of system memory maps its VM object, which keeps the pages until the view goes. */
        for(size_t i=0;i<span_count_ && i<64;++i) {
            char name[17]={};void *pmap=nullptr;uint64_t va=0;
            if(native_pmap_user_mappings((ppnum_t)(spans_[i].physical>>12),&pmap,&va,name)) {
                native_adreno_free_user_mapped[0]=native_adreno_free_user_mapped[0]+1;
                native_adreno_free_user_mapped[1]=(uint32_t)(va>>12);
                break;
            }
        }
    }
    if(descriptor_) {
        if(prepared_) {
            native_adreno_trace_stage=31;
            IOReturn status=descriptor_->complete();
            native_adreno_trace_stage=32;
            if(status!=kIOReturnSuccess) {quarantined_=true;return status;}
            prepared_=false;
        }
        native_adreno_trace_stage=33;
        descriptor_->release();descriptor_=nullptr;
        native_adreno_trace_stage=34;
    }
    /* Pool pages: the BO's reference goes; the pages return to the pool once no client view holds the grant. */
    if(grant_) {NativeAdrenoPoolRelease(grant_);grant_=nullptr;prepared_=false;}
    if(owned_) {noteOwned(spans_,span_count_,false);owned_=false;}
    if(system_) {noteSystem(-(int64_t)bytes_,owner_pid_);system_=false;}
    if(import_noted_) {if(span_count_)bumpImportPageRef(spans_[0].physical,-1);noteImport(-(int64_t)bytes_,owner_pid_);import_noted_=false;}
    if(borrowed_) {borrowed_=false;prepared_=false;}
    if(spans_)IODeleteData(spans_,NativeAdrenoPhysicalSpan,span_capacity_);
    mapping_=nullptr;spans_=nullptr;bytes_=span_count_=span_capacity_=0;read_only_=false;owner_uncached_=false;import_cleaned_=false;synchronized_=false;
    return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::releaseSystemInto(NativeAdrenoSystemPages *out)
{
    if(!out || out->held() || !prepared_ || device_references_ || quarantined_)return release();
    if(imported_ && !system_ && !descriptor_ && !grant_) {
        out->imported=imported_;out->bytes=bytes_;
        imported_=nullptr;prepared_=false;
        return release();
    }
    if(!system_ || !(descriptor_ || grant_) || imported_)return release();
    /* The GPU no longer maps the pages (the free unmapped them first) and every client view holds the descriptor itself, or its own
     * reference to the pool grant. */
    out->descriptor=descriptor_;out->grant=grant_;out->bytes=bytes_;out->write_combined=write_combined_;out->cleaned=false;
    descriptor_=nullptr;grant_=nullptr;prepared_=false;
    return release();
}
IOReturn NativeAdrenoMemory::holdForDevice()
{
    if(!backed() || !prepared_ || !span_count_ || quarantined_)return kIOReturnNotReady;
    if(device_references_==UINT32_MAX)return kIOReturnNoResources;
    ++device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::retireAfterQuiescence()
{
    if(!device_references_)return kIOReturnBadArgument;
    if(quarantined_)return kIOReturnNotReady;
    --device_references_;return kIOReturnSuccess;
}
IOReturn NativeAdrenoMemory::publishForDevice(size_t offset,size_t bytes,bool writes,bool include_write_combined,unsigned phase)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    /* Pages every CPU mapping holds non-cacheable carry no line to clean. */
    if(!holdsCpuLines(include_write_combined))return kIOReturnSuccess;
    if(imported_) {NativeAdrenoNotePhase(phase,bytes);return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);}
    if(write_combined_) {  /* no CPU mapping should cache these pages; if one does, reach its lines through the physical pages */
        NativeAdrenoNotePhase(phase,bytes);
        return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
    }
    NativeAdrenoNotePhase(phase,bytes);
    if(!mapping_)return maintainImported(offset,bytes,writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);   /* system memory */
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,
        writes?NativeAdrenoCleanInvalidate:NativeAdrenoClean);
}
IOReturn NativeAdrenoMemory::acquireDeviceWrites(size_t offset,size_t bytes,unsigned phase)
{
    if(!backed() || !prepared_ || quarantined_)return kIOReturnNotReady;
    if(!bytes || ((offset|bytes)&4095) || offset>bytes_ || bytes>bytes_-offset)return kIOReturnBadArgument;
    if(grant_ || (imported_ && owner_uncached_))return kIOReturnSuccess;
    /* Imported pages may hold the owner's unrelated dirty lines next to the GPU's writes: clean as well as invalidate. */
    if(imported_) {NativeAdrenoNotePhase(phase,bytes);return maintainImported(offset,bytes,NativeAdrenoCleanInvalidate);}
    if(write_combined_)return kIOReturnSuccess;
    NativeAdrenoNotePhase(phase,bytes);
    if(!mapping_)return maintainImported(offset,bytes,NativeAdrenoInvalidate);   /* system memory */
    return NativeAdrenoMaintainOwnedPages(static_cast<unsigned char *>(mapping_)+offset,bytes,NativeAdrenoInvalidate);
}
/* Imported pages have no kernel mapping here (and write-combined ones only a non-cacheable one); maintain each span's part of
 * [offset, offset+bytes) by physical address. */
IOReturn NativeAdrenoMemory::maintainImported(size_t offset,size_t bytes,NativeAdrenoCacheOperation op)
{
    for(size_t i=0;i<span_count_;++i) {
        const NativeAdrenoPhysicalSpan &span=spans_[i];
        const size_t begin=offset>span.offset?offset:span.offset;
        const size_t end=offset+bytes<span.offset+span.bytes?offset+bytes:span.offset+span.bytes;
        if(begin>=end)continue;
        const IOReturn status=NativeAdrenoMaintainPhysicalPages(span.physical+(begin-span.offset),end-begin,op);
        if(status)return status;
    }
    return kIOReturnSuccess;
}

uint32_t NativeAdrenoMemory::importRefCount() const
{
    if(!imported_ || !span_count_ || !spans_)return 0;
    return lookupImportPageRef(spans_[0].physical);
}
