// SPDX-License-Identifier: BSD-2-Clause-Patent
// Qualcomm Hexagon HTP V73 (CDSP / NSP, 35 TOPS INT8) bring-up & Apple ANE compute counterpart for SM8550 (sheng).
//
// Devicetree facts (sm8550.dtsi / sm8550-xiaomi-sheng.dts):
//   remoteproc@32300000 ("qcom,sm8550-cdsp-pas"):
//   - cdsp_mem: 0x9c900000 + 0x2000000 (32 MiB), PAS id 18 ("modem:cdsp" / cdsp.mbn)
//   - q6_cdsp_dtb_mem: 0x9e900000 + 0x80000 (512 KiB), PAS id 37 ("modem:cdsp_dtb" / cdsp_dtb.mbn)
//   - proxy power domains: cx.lvl, mxc.lvl, nsp.lvl (voted via active TCS 2 under AppsSmmuLock, dropped on handover)
//   - AOSS QMP load_state "cdsp" (serialized through AdspProbe's open QMP channel)
//   - SMP2P items 94 (in) and 432 (out), local pid 0, remote pid 5 ("master-kernel" / "slave-kernel")
//   - IPCC client CDSP (6), signals SMP2P (2) and GLINK_QMP (0), dispatched via shared APPS IPCC FIFO
//   - SMEM GLINK items 478 (desc), 479 (tx 16 KiB), 480 (rx) in partition (APPS=0, CDSP=5), channel "fastrpcglink-apps-dsp"
//
// Non-fatal: runs on its own "cdsp" kernel thread (cdsp=2 starts, cdsp=1 surveys, cdsp=0 off) after AdspProbe completes boot.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/kmod.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
#include "AppsSmmuLock.h"

extern kmod_info_t g_kernel_kmod_info;

extern "C" {
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmemGlobal.h"
#include "QcomSmp2p.h"

int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
extern volatile int32_t native_adsp_stats[16] __attribute__((weak_import));
int sheng_adsp_qmp_cdsp_load_state(int on) __attribute__((weak_import));

void sheng_cdsp_static_init(void) __asm__("__GLOBAL__sub_I_CdspProbe.cpp");
// [0] stage (negative: failed step), [1] last error, [2] IPCC irqs, [3] slave-kernel bits,
// [4] SMEM/PAS survey bits, [5] QMP rc, [6] dtb present (0/1), [7] cdsp.mbn KiB,
// [8] ready ms, [9] GLINK rx, [10] GLINK tx, [11] glink state (bit0 fastrpc up, bit14 agreed, bit15 broken),
// [12] unknown opens, [13] last SCM x0, [14] last SCM x1, [15] rail vote mask
volatile int32_t native_cdsp_stats[16];
char native_cdsp_text[96];

void sheng_cdsp_ipcc_irq(uint32_t signal);
void sheng_cdsp_probe(void);
unsigned sheng_cdsp_row(char *row, unsigned size);
}

class ShengCdspNpu : public IOService {
    OSDeclareDefaultStructors(ShengCdspNpu)
public:
    bool publish(IOService *provider)
    {
        if (!init() || !attach(provider) || !start(provider)) return false;
        setName("ShengCdspNpu");
        setProperty("model", "Qualcomm Hexagon HTP V73 (SM8550 CDSP)");
        setProperty("compatible", "qcom,sm8550-cdsp-pas");
        setProperty("role", "AppleANE-Counterpart");
        setProperty("tops-int8", 35ull, 32);
        setProperty("stage", 0ull, 32);
        registerService();
        return true;
    }

    void updateTelemetry()
    {
        setProperty("stage", (uint64_t)(int64_t)native_cdsp_stats[0], 64);
        setProperty("firmware-kib", (uint64_t)(uint32_t)native_cdsp_stats[7], 32);
        setProperty("ready-ms", (uint64_t)(uint32_t)native_cdsp_stats[8], 32);
        setProperty("glink-version-agreed", ((native_cdsp_stats[11] >> 14) & 1) ? kOSBooleanTrue : kOSBooleanFalse);
        setProperty("fastrpc-channel-up", (native_cdsp_stats[11] & 1) ? kOSBooleanTrue : kOSBooleanFalse);
    }
};
OSDefineMetaClassAndStructors(ShengCdspNpu, IOService)

namespace {

enum Stage {
    StMapped = 1, StSurveyed = 2, StFirmware = 3, StSmp2p = 4, StLoadState = 6,
    StRails = 7, StDtb = 8, StLoaded = 9, StStarted = 10, StReady = 11, StGlink = 12, StChannel = 13,
};

constexpr uint64_t kSmem = 0x81d00000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kIpcc = 0x408000;
constexpr uint64_t kRscDrv = 0x17a20000, kRscTcs = 0xd00, kCmdDb = 0x81c60000, kCmdDbSize = 0x20000;
constexpr uint64_t kCdspBase = 0x9c900000, kCdspSize = 0x2000000;
constexpr uint64_t kDtbBase  = 0x9e900000, kDtbSize  = 0x80000;
constexpr uint32_t kPasCdsp = 18, kPasDtb = 37, kRemotePid = 5, kItemIn = 94, kItemOut = 432, kOwnedTcs = 2;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2;

struct Window { IOMemoryMap *map; volatile uint8_t *va; };
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache)
{
    IODeviceMemory *mem = IODeviceMemory::withRange(base, size);
    w.map = mem ? mem->map(kIOMapAnywhere | cache) : nullptr;
    if (mem) mem->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) { if (w.map) w.map->release(); w = {}; }
uint32_t rd(const Window &w, uint64_t at) { return *reinterpret_cast<volatile uint32_t *>(w.va + at); }
void wr(const Window &w, uint64_t at, uint32_t v)
{
    *reinterpret_cast<volatile uint32_t *>(w.va + at) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

Window gSmemW, gMutex, gIpcc, gRsc;
QcomSmem gSmem;
QcomSmemPartition gPart, gGlobal;
bool gHasPart, gHasGlobal;
QcomSmp2p gP2p;
QcomGlink gGlink;
QcomGlinkChannel *gFastRpcChan;
uint8_t *gFastRpcIntents;
bool gGlinkStarted, gHandedOver, gRailsVoted;
uint32_t gSlave;
volatile uint32_t gPending;
ShengCdspNpu *gNpuService;

void setStage(int s)
{
    native_cdsp_stats[0] = s;
    if (gNpuService) gNpuService->updateTelemetry();
}
void fail(int step, int err)
{
    native_cdsp_stats[1] = err;
    native_cdsp_stats[0] = -step;
    if (gNpuService) gNpuService->updateTelemetry();
}
void note(const char *prefix, const char *text)
{
    size_t at = 0;
    for (; prefix[at] && at < sizeof(native_cdsp_text) - 1; ++at) native_cdsp_text[at] = prefix[at];
    for (size_t i = 0; text[i] && at < sizeof(native_cdsp_text) - 1; ++i) native_cdsp_text[at++] = text[i];
    native_cdsp_text[at] = 0;
}

void wake(uint32_t bits)
{
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}
uint32_t waitEvents(uint32_t ms)
{
    uint32_t bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms, kMillisecondScale);
    bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) { clear_wait(current_thread(), THREAD_AWAKENED); return bits; }
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
}

void ringCdsp(uint32_t sig) { if (gIpcc.va) wr(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(QCOM_IPCC_CLIENT_CDSP, sig)); }
void kickGlink(void *) { ringCdsp(QCOM_IPCC_SIGNAL_GLINK_QMP); }
void kickSmp2p() { ringCdsp(QCOM_IPCC_SIGNAL_SMP2P); }

bool lockSmem()
{
    for (unsigned i = 0; i < 1000; ++i) {
        wr(gMutex, 0, 1);
        if (rd(gMutex, 0) == 1) return true;
        IODelay(10);
    }
    return false;
}
void unlockSmem() { wr(gMutex, 0, 0); }
int allocItem(uint16_t id, uint32_t size)
{
    if (!lockSmem()) return -16;
    const int rc = qcom_smem_alloc(&gSmem, &gPart, id, size);
    unlockSmem();
    return rc == QcomSmemExists ? 0 : rc;
}
volatile uint8_t *findItemLoc(uint16_t id, uint32_t *size, int *loc_out)
{
    uint32_t at = 0, bytes = 0;
    const int loc = qcom_smem_find_any(&gSmem, (uint32_t)kSmem, &gPart, gHasPart, &gGlobal, gHasGlobal, id, &at, &bytes);
    if (loc_out) *loc_out = loc;
    if (loc == QcomSmemLocNone) return nullptr;
    if (size) *size = bytes;
    return gSmemW.va + at;
}
volatile uint8_t *findItem(uint16_t id, uint32_t *size)
{
    return findItemLoc(id, size, nullptr);
}
void readCrashReason()
{
    // Qualcomm Subsystem Failure Reason (SFR) SMEM item IDs: CDSP is 425 (or 601/424); check cleanly without mutating SMEM.
    static const uint16_t kCrashIds[3] = {425u, 601u, 424u};
    for (uint16_t cid : kCrashIds) {
        uint32_t sz = 0;
        const volatile uint8_t *p = findItem(cid, &sz);
        if (!p || !sz) continue;
        char text[72];
        size_t n = 0;
        for (; n < sizeof(text) - 1 && n < sz; ++n) {
            const char c = (char)p[n];
            if (!c) break;
            text[n] = (c >= 32 && c < 127) ? c : '.';
        }
        text[n] = 0;
        if (n) {
            note("sfr: ", text);
            return;
        }
    }
}

struct ScmResult { int64_t x0; uint64_t x1; };
ScmResult scm(uint32_t command, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c)
{
    ScmResult result = {-1, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t fn = qcom_scm_fn(QCOM_SCM_SVC_PIL, command), session = 0;
        unsigned resumes = 0;
        for (;;) {
            register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a,
                              x3 __asm__("x3") = b, x4 __asm__("x4") = c, x5 __asm__("x5") = 0,
                              x6 __asm__("x6") = session, x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            result = {(int64_t)x0, x1};
            if ((int64_t)x0 != QCOM_SCM_INTERRUPTED || ++resumes > 100000) break;
            fn = QCOM_SCM_INTERRUPTED;
            session = x6;
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    native_cdsp_stats[13] = (int32_t)result.x0;
    native_cdsp_stats[14] = (int32_t)result.x1;
    return result;
}
bool scmOk(const ScmResult &r) { return r.x0 == 0 && r.x1 == 0; }

void cleanToPoc(const void *start, size_t length)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line) {
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

struct Image {
    void *file;
    uint64_t size;
    QcomMdtImage mdt;
    IOBufferMemoryDescriptor *meta;
};
Image gDtbImg, gCdspImg;
int fileRead(void *ctx, uint64_t off, void *to, size_t len) { return sheng_fw_read(ctx, off, to, len); }
void releaseImg(Image &img)
{
    if (img.file) sheng_fw_close(img.file);
    if (img.meta) img.meta->release();
    img.file = nullptr; img.meta = nullptr;
}
int openImg(Image &img, const char *path)
{
    int rc = sheng_fw_open(path, &img.file, &img.size);
    if (rc) { img.file = nullptr; return rc; }
    rc = qcom_mdt_parse(&img.mdt, img.size, fileRead, img.file);
    return rc ? 1000 - rc : 0;
}
int authHeader(Image &img, uint32_t pas, uint64_t base)
{
    const uint64_t bytes = qcom_mdt_metadata_size(&img.mdt);
    img.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes, 0xfffffffffffff000ull);
    if (!img.meta || img.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(img.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&img.mdt, fileRead, img.file, va, bytes)) return -5;
    cleanToPoc(va, bytes);
    const uint64_t phys = img.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (!scmOk(scm(QCOM_SCM_PAS_INIT_IMAGE, qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), pas, phys, 0))) return -1;
    if (img.mdt.relocatable &&
        !scmOk(scm(QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), pas, base, qcom_mdt_span(&img.mdt)))) {
        return -2;
    }
    return 0;
}
int loadSegs(Image &img, uint64_t base, uint64_t size)
{
    Window reg = {};
    if (!mapWindow(reg, base, size, kIOMapWriteCombineCache)) return -12;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -12;
    for (unsigned i = 0; !rc && i < img.mdt.count; ++i) {
        const QcomMdtSegment &s = img.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(&img.mdt, i, base, size, &at)) { rc = -7; break; }
        uint8_t *to = const_cast<uint8_t *>(reg.va) + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(img.file, s.file_offset + done, bounce, n)) { rc = -5; break; }
            memcpy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) bzero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(reg);
    return rc;
}

// Vote cx.lvl, mxc.lvl, nsp.lvl via shared active TCS 2 under AppsSmmuLock (same lock discipline as VideoProbe.cpp).
bool voteRails(bool on)
{
    uint8_t *db = static_cast<uint8_t *>(IOMallocData(kCmdDbSize));
    Window dbw = {};
    if (!db || !mapWindow(dbw, kCmdDb, kCmdDbSize)) {
        unmap(dbw);
        if (db) IOFreeData(db, kCmdDbSize);
        return false;
    }
    for (uint32_t i = 0; i < kCmdDbSize; i += 4) {
        const uint32_t w = rd(dbw, i);
        memcpy(db + i, &w, 4);
    }
    unmap(dbw);

    const QcomRscLayout layout = qcom_rsc_layout(rd(gRsc, 0));
    static const char *const kNames[3] = {"cx.lvl", "mxc.lvl", "nsp.lvl"};
    uint32_t mask = 0;
    {
        AppsSmmuLock guard;
        for (unsigned r = 0; r < 3; ++r) {
            QcomRpmhResource res = {};
            uint32_t idx = 0;
            if (qcom_cmd_db_find(db, kCmdDbSize, kNames[r], &res) != 0) continue;
            if (on) {
                if (qcom_rpmh_level_index(&res, 0xffffffffu, &idx) != 0) continue;
            } else {
                idx = 0;
            }
            QcomRpmhWrite writes[16];
            const unsigned n = qcom_rpmh_active_request(&layout, kOwnedTcs, &res.address, &idx, 1, writes, 16);
            for (unsigned i = 0; i < n; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
            IOSleep(2);
            const unsigned m = qcom_rpmh_finish(&layout, kOwnedTcs, writes, 16);
            for (unsigned i = 0; i < m; ++i) wr(gRsc, kRscTcs + writes[i].offset, writes[i].value);
            mask |= (1u << r) | ((idx & 0xfu) << (4 + r * 4));
        }
    }
    IOFreeData(db, kCmdDbSize);
    if (on) native_cdsp_stats[15] = (int32_t)mask;
    gRailsVoted = on && (mask & 0x7u) != 0;
    return !on || (mask & 0x4u) != 0; // nsp.lvl (bit 2) or cx/mxc present
}

void serviceSmp2p()
{
    if (!gP2p.in) {
        if (volatile uint8_t *in = findItem(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gSlave = value;
    native_cdsp_stats[3] = (int32_t)value;
    if ((value & kHandover) && !gHandedOver) {
        gHandedOver = true;
        if (gRailsVoted) voteRails(false);
    }
}

void fastRpcState(void *, QcomGlinkChannel *, int up)
{
    if (up) note("chan: ", "fastrpcglink-apps-dsp up");
}
void fastRpcRecv(void *, QcomGlinkChannel *, const uint8_t *, uint32_t)
{
}

void serviceGlink()
{
    if (!gGlinkStarted) return;
    if (!gGlink.rx.data) {
        uint32_t sz = 0;
        if (volatile uint8_t *rx = findItem(kItemGlinkRx, &sz)) qcom_glink_set_rx(&gGlink, rx, sz);
    }
    qcom_glink_poll(&gGlink);
    const uint32_t up = (gFastRpcChan && gFastRpcChan->up) ? 1u : 0u;
    native_cdsp_stats[9]  = (int32_t)gGlink.rx_messages;
    native_cdsp_stats[10] = (int32_t)gGlink.tx_messages;
    native_cdsp_stats[11] = (int32_t)(up | ((uint32_t)gGlink.version_agreed << 14) | ((uint32_t)gGlink.broken << 15));
    native_cdsp_stats[12] = (int32_t)gGlink.unknown_opens;
    if (gGlink.version_agreed && native_cdsp_stats[0] >= StReady && native_cdsp_stats[0] < StGlink) setStage(StGlink);
    if (up && native_cdsp_stats[0] >= StGlink && native_cdsp_stats[0] < StChannel) setStage(StChannel);
    if (gNpuService) gNpuService->updateTelemetry();
}

void service(uint32_t bits)
{
    if (bits & (kEvSmp2p | kEvGlink)) serviceSmp2p();
    if (bits & kEvGlink) serviceGlink();
}

void survey()
{
    int32_t bits = 0;
    gHasPart = (qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_APPS, kRemotePid, &gPart) == QcomSmemOk);
    gHasGlobal = (qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &gGlobal) == QcomSmemOk);
    if (gHasPart) bits |= 1 << 8;
    if (gHasGlobal) bits |= 1 << 9;
    {
        int inLoc = 0;
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) {
            int loc = 0;
            if (findItemLoc(ids[i], nullptr, &loc)) {
                bits |= 1 << i;
                if (i == 0) inLoc = loc;
            }
        }
        bits |= (inLoc & 0x3) << 10;
    }
    const ScmResult sCdsp = scm(QCOM_SCM_PAS_IS_SUPPORTED, qcom_scm_args(1, QCOM_SCM_VAL, 0, 0), kPasCdsp, 0, 0);
    const ScmResult sDtb  = scm(QCOM_SCM_PAS_IS_SUPPORTED, qcom_scm_args(1, QCOM_SCM_VAL, 0, 0), kPasDtb, 0, 0);
    if (sCdsp.x0 == 0 && sCdsp.x1 == 1) bits |= 1 << 12;
    if (sDtb.x0 == 0 && sDtb.x1 == 1)  bits |= 1 << 13;
    native_cdsp_stats[4] = bits;
    native_cdsp_stats[3] = -1;
    if (volatile uint8_t *in = findItem(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int idx = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (idx >= 0) native_cdsp_stats[3] = (int32_t)qcom_smp2p_in_value(&view, idx);
    }
}

void shutdownCdsp(bool dtb, bool cdsp)
{
    if (cdsp) scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasCdsp, 0, 0);
    if (dtb)  scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasDtb, 0, 0);
}

bool startCdsp()
{
    // Wait up to 15 s for AdspProbe to finish its boot phase so UFS firmware reads, PAS SMC, and QMP have zero contention.
    for (unsigned i = 0; i < 60; ++i) {
        const int32_t a = &native_adsp_stats[0] ? native_adsp_stats[0] : -1;
        if (a >= 11 || a < 0) break;
        IOSleep(250);
    }

    static const char *const kSources[2] = {"modem:", "/private/var/firmware/qcom/sm8550/sheng"};
    bool haveDtb = false, haveCdsp = false;
    int lastRc = ENOENT;
    char path[112] = "";
    for (unsigned attempt = 0; attempt < 4 && !haveCdsp; ++attempt) {
        for (const char *src : kSources) {
            const bool modem = !strcmp(src, "modem:");
            snprintf(path, sizeof(path), modem ? "%scdsp_dtb" : "%s/cdsp_dtb.mbn", src);
            if (openImg(gDtbImg, path) == 0) haveDtb = true;
            snprintf(path, sizeof(path), modem ? "%scdsp" : "%s/cdsp.mbn", src);
            lastRc = openImg(gCdspImg, path);
            if (lastRc == 0) { haveCdsp = true; break; }
            if (haveDtb) { releaseImg(gDtbImg); haveDtb = false; }
        }
        if (!haveCdsp) IOSleep(1000);
    }
    if (!haveCdsp) {
        note("fw: ", path);
        fail(StFirmware, lastRc);
        return false;
    }
    native_cdsp_stats[6] = haveDtb ? 1 : 0;
    native_cdsp_stats[7] = (int32_t)(gCdspImg.size >> 10);
    setStage(StFirmware);

    if (native_cdsp_stats[3] > 0) {
        note("state: ", "already running");
        releaseImg(gDtbImg);
        releaseImg(gCdspImg);
        setStage(StReady);
        return true;
    }

    int rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES);
    if (rc) { fail(StSmp2p, rc); releaseImg(gDtbImg); releaseImg(gCdspImg); return false; }
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(findItem(kItemOut, nullptr)), 0, kRemotePid);
    qcom_smp2p_add_entry(&gP2p, "master-kernel");
    wr(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(QCOM_IPCC_CLIENT_CDSP, QCOM_IPCC_SIGNAL_SMP2P));
    wr(gIpcc, QCOM_IPCC_RECV_ENABLE, qcom_ipcc_id(QCOM_IPCC_CLIENT_CDSP, QCOM_IPCC_SIGNAL_GLINK_QMP));
    kickSmp2p();
    setStage(StSmp2p);

    if (sheng_adsp_qmp_cdsp_load_state) {
        const int qmpRc = sheng_adsp_qmp_cdsp_load_state(1);
        native_cdsp_stats[5] = qmpRc;
        if (qmpRc != 0) {
            note("qmp: ", "cdsp load_state failed");
            fail(StLoadState, qmpRc);
            releaseImg(gDtbImg);
            releaseImg(gCdspImg);
            return false;
        }
    }
    setStage(StLoadState);

    if (!voteRails(true)) {
        note("rails: ", "cmd-db vote failed");
        fail(StRails, -2);
        if (sheng_adsp_qmp_cdsp_load_state) sheng_adsp_qmp_cdsp_load_state(0);
        releaseImg(gDtbImg);
        releaseImg(gCdspImg);
        return false;
    }
    setStage(StRails);

    if (haveDtb) {
        if ((rc = authHeader(gDtbImg, kPasDtb, kDtbBase)) ||
            (rc = loadSegs(gDtbImg, kDtbBase, kDtbSize)) ||
            (rc = scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasDtb, 0, 0)) ? 0 : -3)) {
            note("pas: ", "cdsp_dtb auth failed");
            fail(StDtb, rc);
            voteRails(false);
            if (sheng_adsp_qmp_cdsp_load_state) sheng_adsp_qmp_cdsp_load_state(0);
            releaseImg(gDtbImg);
            releaseImg(gCdspImg);
            return false;
        }
    }
    setStage(StDtb);

    if ((rc = authHeader(gCdspImg, kPasCdsp, kCdspBase)) || (rc = loadSegs(gCdspImg, kCdspBase, kCdspSize))) {
        note("pas: ", "cdsp load failed");
        fail(StLoaded, rc);
        shutdownCdsp(haveDtb, false);
        voteRails(false);
        if (sheng_adsp_qmp_cdsp_load_state) sheng_adsp_qmp_cdsp_load_state(0);
        releaseImg(gDtbImg);
        releaseImg(gCdspImg);
        return false;
    }
    setStage(StLoaded);

    const uint64_t began = mach_absolute_time();
    if (!scmOk(scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasCdsp, 0, 0))) {
        note("pas: ", "cdsp reset failed");
        fail(StStarted, -3);
        shutdownCdsp(haveDtb, false);
        voteRails(false);
        if (sheng_adsp_qmp_cdsp_load_state) sheng_adsp_qmp_cdsp_load_state(0);
        releaseImg(gDtbImg);
        releaseImg(gCdspImg);
        return false;
    }
    setStage(StStarted);

    uint64_t deadline = 0;
    clock_interval_to_deadline(5000, kMillisecondScale, &deadline);
    while ((gSlave & (kReady | kFatal)) == 0 && mach_absolute_time() < deadline) {
        service(waitEvents(50) | kEvSmp2p);
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_cdsp_stats[8] = (int32_t)(ns / 1000000ull);
    releaseImg(gDtbImg);
    releaseImg(gCdspImg);

    if ((gSlave & kReady) == 0 || (gSlave & kFatal) != 0) {
        int inLoc = 0;
        if (findItemLoc(kItemIn, nullptr, &inLoc)) native_cdsp_stats[4] |= (1 << 0) | ((inLoc & 0x3) << 10);
        note("smp2p: ", (gSlave & kFatal) ? "cdsp fatal" : "cdsp ready timeout");
        readCrashReason();
        fail(StReady, (gSlave & kFatal) ? -5 : -60);
        shutdownCdsp(haveDtb, true);
        if (!gHandedOver) voteRails(false);
        if (sheng_adsp_qmp_cdsp_load_state) sheng_adsp_qmp_cdsp_load_state(0);
        return false;
    }
    note("ready: ", "Hexagon HTP V73 online");
    setStage(StReady);
    IOLog("ShengCdspNpu: Hexagon HTP V73 (CDSP) online in %d ms (%d KiB)\n", native_cdsp_stats[8], native_cdsp_stats[7]);
    return true;
}

bool startGlink()
{
    int rc = allocItem(kItemGlinkDesc, 32);
    if (!rc) rc = allocItem(kItemGlinkTx, kGlinkFifo);
    uint32_t sz = 0;
    volatile uint8_t *desc = findItem(kItemGlinkDesc, nullptr), *txf = findItem(kItemGlinkTx, &sz);
    if (rc || !desc || !txf) { fail(StGlink, rc ? rc : -2); return false; }
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(desc);
    const QcomGlinkFifo tx = {&d[1], &d[0], txf, sz}, rx = {&d[3], &d[2], nullptr, 0};
    qcom_glink_init(&gGlink, tx, rx, kickGlink, nullptr);
    gGlinkStarted = true;
    gFastRpcIntents = static_cast<uint8_t *>(IOMallocData(4096 * 4));
    if (gFastRpcIntents) {
        static const QcomGlinkClient kGlue = {fastRpcState, fastRpcRecv};
        gFastRpcChan = qcom_glink_channel(&gGlink, "fastrpcglink-apps-dsp", 4096, 4, gFastRpcIntents, &kGlue, nullptr, 1);
    }
    serviceGlink();
    return true;
}

void cdspThread(void *, wait_result_t)
{
    thread_set_thread_name(current_thread(), "cdsp");
    unsigned mode = 0;
    PE_parse_boot_argn("cdsp", &mode, sizeof mode);
    if (!mapWindow(gSmemW, kSmem, kSmemSize) || !mapWindow(gMutex, kMutex, 0x1000) ||
        !mapWindow(gIpcc, kIpcc, 0x1000) || !mapWindow(gRsc, kRscDrv, 0x10000)) {
        fail(StMapped, -12);
        return;
    }
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    setStage(StMapped);
    survey();
    setStage(StSurveyed);
    if (mode < 2 || !startCdsp() || !startGlink()) return;

    for (;;) {
        service(waitEvents(3600u * 1000u));
    }
}

} // namespace

extern "C" void sheng_cdsp_ipcc_irq(uint32_t signal)
{
    __atomic_fetch_add(&native_cdsp_stats[2], 1, __ATOMIC_RELAXED);
    if (signal == QCOM_IPCC_SIGNAL_SMP2P) wake(kEvSmp2p);
    else if (signal == QCOM_IPCC_SIGNAL_GLINK_QMP) wake(kEvGlink);
}

extern "C" void sheng_cdsp_probe(void)
{
    unsigned mode = 0;
    if (!PE_parse_boot_argn("cdsp", &mode, sizeof mode) || !mode || native_cdsp_stats[0]) return;

    const OSSymbol *sym = OSSymbol::withCString("ShengCdspNpu");
    const bool taken = sym && OSMetaClass::getMetaClassWithName(sym);
    OSSafeReleaseNULL(sym);
    if (!taken) {
        if (void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name)) {
            sheng_cdsp_static_init();
            if (OSMetaClass::postModLoad(handle) == kIOReturnSuccess) {
                IOService *platform = IOService::getPlatform();
                if (!platform) platform = IOService::getResourceService();
                if (platform) {
                    ShengCdspNpu *npu = OSTypeAlloc(ShengCdspNpu);
                    if (npu && npu->publish(platform)) gNpuService = npu;
                    else OSSafeReleaseNULL(npu);
                }
            }
        }
    }

    thread_t thread = nullptr;
    if (kernel_thread_start(cdspThread, nullptr, &thread) != KERN_SUCCESS) {
        fail(StMapped, -7);
        return;
    }
    thread_deallocate(thread);
}

extern "C" unsigned sheng_cdsp_row(char *row, unsigned size)
{
    if (!native_cdsp_stats[0] || !row || size < 2) return 0;
    const volatile int32_t *c = native_cdsp_stats;
    const int n = snprintf(
        row, size,
        "[cdsp] st=%d err=%d irq=%d slv=0x%x smem=0x%x qmp=%d dtb=%d fw=%dKiB ms=%d glink=%d/%d/0x%x scm=%d/%d rails=0x%x | %.48s",
        c[0], c[1], c[2], (unsigned)c[3], (unsigned)c[4], c[5], c[6], c[7], c[8],
        c[9], c[10], (unsigned)c[11], c[13], c[14], (unsigned)c[15], native_cdsp_text);
    if (n <= 0) return 0;
    return (unsigned)n < size ? (unsigned)n : size - 1;
}
