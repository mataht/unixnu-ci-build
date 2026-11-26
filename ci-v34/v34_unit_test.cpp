#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>
#include <pthread.h>

extern "C" {
#include "NvtTouchCore.h"
#include "ShengBattmgr.h"
#include "ScreenBlank.h"
}
#include "TouchMouse.h"

// --- 1. V30 resolveUserMetalCacheDir + symlink test ---
static bool ensureCacheDirWritable(const std::string &dir) {
    if (dir.empty() || dir[0] != '/' || dir.rfind("/dev/null", 0) == 0) return false;
    struct stat st = {};
    if (stat(dir.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) && access(dir.c_str(), W_OK) == 0;
    }
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    return access(dir.c_str(), W_OK) == 0;
}

static size_t safeStrlcpy(char *dst, const char *src, size_t cap) {
    const size_t len = std::strlen(src);
    if (cap) {
        const size_t n = len < cap - 1 ? len : cap - 1;
        std::memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}

static bool resolveUserMetalCacheDir(char *buf, size_t cap) {
    if (!buf || !buf[0]) return false;
    if (const char *cdir = std::strstr(buf, "/C/")) {
        const char *after = cdir + 3;
        if (*after != 0 && std::strncmp(after, "com.apple.metal", 15) != 0) {
            const std::string prefix(buf, static_cast<size_t>(after - buf));
            const std::string metalBase = prefix + "com.apple.metal";
            const std::string m2vPath = metalBase + "/m2v";
            if (m2vPath.size() < cap && ensureCacheDirWritable(metalBase) && ensureCacheDirWritable(m2vPath)) {
                safeStrlcpy(buf, m2vPath.c_str(), cap);
                return true;
            }
        }
    }
    std::string base(buf);
    if (!base.empty() && base.back() != '/') base.push_back('/');
    const std::string metalDir = base + "com.apple.metal";
    const std::string m2vDir = metalDir + "/m2v";
    if (m2vDir.size() >= cap || !ensureCacheDirWritable(metalDir) || !ensureCacheDirWritable(m2vDir)) return false;
    safeStrlcpy(buf, m2vDir.c_str(), cap);
    return true;
}

static void test_v30_resolve_user_metal_cache_dir() {
    char tmpl[] = "/tmp/v30_cache_test_XXXXXX";
    char *base = mkdtemp(tmpl);
    assert(base != nullptr);
    std::string realRoot = std::string(base) + "/private_var_folders/ab/cd123/C";
    assert(mkdir((std::string(base) + "/private_var_folders").c_str(), 0700) == 0);
    assert(mkdir((std::string(base) + "/private_var_folders/ab").c_str(), 0700) == 0);
    assert(mkdir((std::string(base) + "/private_var_folders/ab/cd123").c_str(), 0700) == 0);
    assert(mkdir(realRoot.c_str(), 0700) == 0);
    assert(mkdir((realRoot + "/com.apple.iconservicesagent").c_str(), 0700) == 0);

    std::string symVar = std::string(base) + "/var_folders";
    assert(symlink((std::string(base) + "/private_var_folders").c_str(), symVar.c_str()) == 0);

    // Case A: UID=501 iconservicesagent with `/C/com.apple.iconservicesagent/` -> resolves `<prefix>/C/com.apple.metal/m2v`!
    char candA[512] = {};
    safeStrlcpy(candA, (symVar + "/ab/cd123/C/com.apple.iconservicesagent/").c_str(), sizeof candA);
    assert(resolveUserMetalCacheDir(candA, sizeof candA));
    assert(std::string(candA) == symVar + "/ab/cd123/C/com.apple.metal/m2v");

    // Case B: UID=269 _iconservices with `/C/` -> resolves `<prefix>/C/com.apple.metal/m2v`!
    char candB[512] = {};
    safeStrlcpy(candB, (symVar + "/ab/cd123/C/").c_str(), sizeof candB);
    assert(resolveUserMetalCacheDir(candB, sizeof candB));
    assert(std::string(candB) == symVar + "/ab/cd123/C/com.apple.metal/m2v");
}

// --- 2. V30 absentd fast-exit classifier test ---
static bool should_exit_immediately(const char *arg0, const char *arg0_base, const char *label) {
    if (arg0_base && !std::strcmp(arg0_base, "Install-in-Progress")) return true;
    if (label && (!std::strncmp(label, "application.", 12) ||
                  !std::strncmp(label, "com.apple.PackageUIKit.Install-in-Progress", 42))) return true;
    if (arg0 && (std::strstr(arg0, ".app/Contents/MacOS/") != nullptr ||
                 std::strstr(arg0, ".xpc/Contents/MacOS/") != nullptr)) return true;
    return false;
}

static void test_v30_absentd_classifier() {
    assert(should_exit_immediately(
        "/System/Library/CoreServices/Install in Progress.app/Contents/MacOS/Install-in-Progress",
        "Install-in-Progress",
        "application.com.apple.PackageUIKit.Install-in-Progress.1152921500311996182"));
    assert(should_exit_immediately(
        "/System/Library/PrivateFrameworks/WorkflowKit.framework/XPCServices/BackgroundShortcutRunner.xpc/Contents/MacOS/BackgroundShortcutRunner",
        "BackgroundShortcutRunner",
        "com.apple.siri.BackgroundShortcutRunner"));
    assert(should_exit_immediately(
        "/System/Library/PrivateFrameworks/StreamingExtractor.framework/XPCServices/STExtractionService.privileged.xpc/Contents/MacOS/STExtractionService.privileged",
        "STExtractionService.privileged",
        "com.apple.STExtractionService.privileged"));
    // Persistent LaunchDaemons MUST stay alive in dispatch_main() to avoid 10s KeepAlive respawn loops
    assert(!should_exit_immediately("/usr/sbin/spindump", "spindump", "com.apple.spindump"));
    assert(!should_exit_immediately("/usr/libexec/lskdd", "lskdd", "com.apple.lskdd"));
    assert(!should_exit_immediately(
        "/System/Library/CoreServices/Software Update.app/Contents/Resources/softwareupdated",
        "softwareupdated",
        "com.apple.softwareupdated"));
    assert(!should_exit_immediately("/usr/libexec/mobileassetd", "mobileassetd", "com.apple.mobileassetd"));
}

// --- 3. V30 ScreenBlank 4.5s force-halt state machine test ---
static void test_v30_power_force_halt_state_machine() {
    screen_blank_power st = SCREEN_BLANK_POWER_INIT;
    uint32_t wait_ms = 0;

    assert(screen_blank_power_step(&st, true, 1000, false, false, &wait_ms) == ScreenPowerSwallowWait);
    assert(wait_ms == 1200);
    assert(screen_blank_power_step(&st, true, 2200, false, false, &wait_ms) == ScreenPowerForwardHold);
    assert(wait_ms == 3300);
    assert(screen_blank_power_step(&st, true, 4000, false, false, &wait_ms) == ScreenPowerPassThrough);
    assert(wait_ms == 1500);
    assert(screen_blank_power_step(&st, true, 5500, false, false, &wait_ms) == ScreenPowerForceHalt);
    assert(wait_ms == 0);
    // Must NOT fire twice during the same hold
    assert(screen_blank_power_step(&st, true, 6000, false, false, &wait_ms) == ScreenPowerPassThrough);
    assert(wait_ms == 0);
    // Release resets state cleanly
    assert(screen_blank_power_step(&st, false, 6100, false, false, &wait_ms) == ScreenPowerPassThrough);
    assert(!st.held && !st.forwarded && !st.force_halt_fired);
}

// --- 4. V30 TouchProbe unblank finger-delta guard test ---
static void fill_finger(int16_t m[NVT_ROWS * NVT_COLUMNS], int r0, int c0, int16_t peak) {
    for (int dr = -2; dr <= 2; ++dr) {
        for (int dc = -2; dc <= 2; ++dc) {
            int r = r0 + dr, c = c0 + dc;
            if (r >= 0 && r < NVT_ROWS && c >= 0 && c < NVT_COLUMNS) {
                int dist = std::abs(dr) + std::abs(dc);
                int16_t v = (dist == 0) ? peak : (dist == 1) ? (int16_t)(peak * 2 / 3) : (int16_t)(peak / 3);
                m[r * NVT_COLUMNS + c] = (int16_t)(m[r * NVT_COLUMNS + c] + v);
            }
        }
    }
}

static void test_v30_touch_unblank_defers_rebaseline_while_finger_touching() {
    auto *core = static_cast<nvt_touch_core *>(std::calloc(1, sizeof(nvt_touch_core)));
    assert(core != nullptr);
    nvt_touch_reset(core);
    int16_t matrix[NVT_NODES];
    nvt_contact out[NVT_SLOTS];

    for (unsigned f = 0; f < NVT_STARTUP_FRAMES; ++f) {
        std::fill_n(matrix, NVT_NODES, (int16_t)5000);
        assert(nvt_touch_process(core, matrix, 4, out, NVT_SLOTS) == -1);
    }
    assert(core->have_reference == 1 && core->rebaselines == 0);

    bool wasClosedOrBlank = true, pendingUnblankRebaseline = false;
    auto stepUnblank = [&](bool closedNow) {
        if (closedNow) {
            pendingUnblankRebaseline = false;
        } else if (wasClosedOrBlank) {
            pendingUnblankRebaseline = true;
        }
        if (pendingUnblankRebaseline) {
            bool fingerPresent = false;
            if (core->have_reference) {
                for (unsigned q = 0; q < NVT_NODES; ++q) {
                    if (matrix[q] - core->reference[q] >= NVT_PEAK_THRESHOLD) {
                        fingerPresent = true;
                        break;
                    }
                }
            }
            if (!fingerPresent) {
                pendingUnblankRebaseline = false;
                nvt_touch_rebaseline(core);
            }
        }
        wasClosedOrBlank = closedNow;
    };

    // Unblank while finger is resting on glass: rebaseline MUST be deferred so finger isn't absorbed!
    std::fill_n(matrix, NVT_NODES, (int16_t)5000);
    fill_finger(matrix, 15, 22, 850);
    stepUnblank(false);
    assert(pendingUnblankRebaseline == true);
    assert(core->rebaselines == 0);
    assert(nvt_touch_process(core, matrix, 4, out, NVT_SLOTS) == 0); // frame 0 onset debounce
    stepUnblank(false);
    assert(pendingUnblankRebaseline == true);
    assert(core->rebaselines == 0);
    assert(nvt_touch_process(core, matrix, 4, out, NVT_SLOTS) == 1); // frame 1 confirmed touch!

    // Next frame after finger lifts: pending rebaseline fires cleanly!
    std::fill_n(matrix, NVT_NODES, (int16_t)5000);
    stepUnblank(false);
    assert(pendingUnblankRebaseline == false);
    assert(core->rebaselines == 1);
    std::free(core);
}

// --- 5. Preserved V29 EncodeBatch + NBPipelineWorkerState tests ---
struct EncodeBatchTest {
    static constexpr size_t kAlign = alignof(std::max_align_t);
    static constexpr size_t kChunk = 64u << 10;
    static constexpr size_t rounded(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }
    struct Header { uint64_t pacCanary; void (*run)(void *); size_t size; };
    struct Chunk { std::unique_ptr<uint8_t[]> bytes; size_t capacity = 0; size_t used = 0; };
    std::vector<Chunk> chunks;
    size_t at = 0;

    template <typename F>
    void add(F &&work) {
        using Closure = typename std::decay<F>::type;
        const size_t need = rounded(sizeof(Header)) + rounded(sizeof(Closure));
        while (at < chunks.size() && chunks[at].used + need > chunks[at].capacity) at++;
        if (at == chunks.size()) {
            Chunk chunk;
            chunk.capacity = std::max(kChunk, need);
            chunk.bytes.reset(new uint8_t[chunk.capacity]);
            chunks.push_back(std::move(chunk));
        }
        Chunk &chunk = chunks[at];
        uint8_t *slot = chunk.bytes.get() + chunk.used;
        chunk.used += need;
        new (slot) Header{
            0xA11CE55E50ACE001ULL,
            [](void *closure) {
                Closure &call = *static_cast<Closure *>(closure);
                call();
                call.~Closure();
            },
            need,
        };
        new (slot + rounded(sizeof(Header))) Closure(std::forward<F>(work));
    }

    size_t run() {
        size_t executed = 0;
        for (Chunk &chunk : chunks) {
            for (size_t offset = 0; offset < chunk.used;) {
                const Header &header = *reinterpret_cast<const Header *>(chunk.bytes.get() + offset);
                assert(header.pacCanary == 0xA11CE55E50ACE001ULL);
                header.run(chunk.bytes.get() + offset + rounded(sizeof(Header)));
                offset += header.size;
                ++executed;
            }
            chunk.used = 0;
        }
        at = 0;
        return executed;
    }
};

static void test_v29_encode_batch_preserved() {
    EncodeBatchTest batch;
    int count = 0;
    for (int i = 0; i < 512; ++i) {
        uint32_t raw[8] = {0xd63f091fu, (uint32_t)i};
        const std::vector<uint8_t> copy(reinterpret_cast<const uint8_t *>(raw), reinterpret_cast<const uint8_t *>(raw) + sizeof(raw));
        batch.add([copy, &count]() {
            uint32_t w0 = 0;
            std::memcpy(&w0, copy.data(), 4);
            assert(w0 == 0xd63f091fu);
            ++count;
        });
    }
    assert(batch.run() == 512 && count == 512);
}


// --- V32 Additional Host Unit Tests ---
static void test_v32_weak_usb_sdp_net_discharge_and_family_code() {
    ShengBattmgr b;
    ShengBatterySnapshot s;
    sheng_battmgr_init(&b);

    // Populate valid readings matching BootLog-27 overnight weak 500mA USB SDP drain:
    // usb online = 1, status = 1 (PMIC says charging), gauges = 53 / 8959 mAh (1%), BmCurrent = +713000 uA -> -713 mA net drain!
    b.valid = (1u << BmStatus) | (1u << BmPresent) | (1u << BmCapacity) | (1u << BmVoltage) |
              (1u << BmCurrent) | (1u << BmUsbOnline) | (1u << BmUsbType) |
              (1u << BmGauge1Remaining) | (1u << BmGauge1Full) | (1u << BmGauge2Remaining) | (1u << BmGauge2Full);
    b.charge_known = (1u << BmStatus) | (1u << BmUsbOnline);
    b.value[BmStatus] = 1;
    b.value[BmPresent] = 1;
    b.value[BmCapacity] = 100;
    b.value[BmVoltage] = 3574000;
    b.value[BmCurrent] = 713000; // positive raw uA = discharging (-713 mA)
    b.value[BmUsbOnline] = 1;
    b.value[BmUsbType] = 1; // SDP -> 0xe0004000
    b.value[BmGauge1Remaining] = 44;
    b.value[BmGauge1Full] = 4423;
    b.value[BmGauge2Remaining] = 9;
    b.value[BmGauge2Full] = 4536;

    sheng_battmgr_snapshot(&b, &s);
    assert(s.present == 1 && s.external == 1);
    assert(s.percent == 1);
    assert(s.current_ma == -713);
    assert(s.charging == 0); // MUST NOT report charging during -713 mA net drain!
    assert(s.critical == 1); // MUST report critical at <=2% under >100 mA net drain even on weak USB!

    // Unsigned 32-bit FamilyCode cast check (fixes 0xffffffffe0004000 sign-extension bug)
    const unsigned long long fam_u64 = (unsigned long long)(uint32_t)s.adapter_family;
    assert(fam_u64 == 0xe0004000ULL);

    // Normal PD charging (+2400 mA) at 1% -> charging=1, critical=0
    b.value[BmCurrent] = -2400000;
    b.value[BmUsbType] = 6;
    sheng_battmgr_snapshot(&b, &s);
    assert(s.current_ma == 2400);
    assert(s.charging == 1);
    assert(s.critical == 0);
    assert((unsigned long long)(uint32_t)s.adapter_family == 0xe000400aULL);

    // Near-zero trickle (-20 mA) at 80% -> charging=1, critical=0
    b.value[BmCurrent] = 20000;
    b.value[BmGauge1Remaining] = 3600;
    b.value[BmGauge2Remaining] = 3600;
    sheng_battmgr_snapshot(&b, &s);
    assert(s.current_ma == -20);
    assert(s.charging == 1);
    assert(s.critical == 0);
}

static bool v32_gpubundles_readonly_match(const char *gp_path) {
    return std::strncmp(gp_path, "/Library/GPUBundles/", sizeof("/Library/GPUBundles/") - 1) == 0 ||
           std::strncmp(gp_path, "/System/Volumes/Data/Library/GPUBundles/", sizeof("/System/Volumes/Data/Library/GPUBundles/") - 1) == 0 ||
           std::strncmp(gp_path, "/private/var/ngpu/GPUBundles/", sizeof("/private/var/ngpu/GPUBundles/") - 1) == 0 ||
           std::strncmp(gp_path, "/private/var/tmp/NativeAdrenoMetal.cache/", sizeof("/private/var/tmp/NativeAdrenoMetal.cache/") - 1) == 0 ||
           std::strncmp(gp_path, "/System/Volumes/Data/private/var/tmp/NativeAdrenoMetal.cache/", sizeof("/System/Volumes/Data/private/var/tmp/NativeAdrenoMetal.cache/") - 1) == 0 ||
           std::strcmp(gp_path, "/Library/GPUBundles") == 0 ||
           std::strcmp(gp_path, "/System/Volumes/Data/Library/GPUBundles") == 0 ||
           std::strcmp(gp_path, "/private/var/ngpu/GPUBundles") == 0 ||
           std::strcmp(gp_path, "/private/var/tmp/NativeAdrenoMetal.cache") == 0 ||
           std::strcmp(gp_path, "/System/Volumes/Data/private/var/tmp/NativeAdrenoMetal.cache") == 0;
}

static bool v32_launchd_override_match(int pid, const char *lp_path) {
    if (pid != 1 || !lp_path) return false;
    return std::strncmp(lp_path, "/private/var/db/com.apple.xpc.launchd/", sizeof("/private/var/db/com.apple.xpc.launchd/") - 1) == 0 ||
           std::strcmp(lp_path, "/private/var/db/com.apple.xpc.launchd") == 0 ||
           std::strncmp(lp_path, "/System/Volumes/Data/private/var/db/com.apple.xpc.launchd/", sizeof("/System/Volumes/Data/private/var/db/com.apple.xpc.launchd/") - 1) == 0 ||
           std::strcmp(lp_path, "/System/Volumes/Data/private/var/db/com.apple.xpc.launchd") == 0;
}

static void test_v32_sandbox_and_launchd_path_grants() {
    // talagentd needs libvulkan_freedreno.dylib inside NativeAdrenoMetal.bundle AND any direct GPUBundles dylib/cache
    assert(v32_gpubundles_readonly_match("/Library/GPUBundles/NativeAdrenoMetal.bundle/Contents/MacOS/libvulkan_freedreno.dylib"));
    assert(v32_gpubundles_readonly_match("/Library/GPUBundles/libvulkan_freedreno.dylib"));
    assert(v32_gpubundles_readonly_match("/private/var/tmp/NativeAdrenoMetal.cache/ir3/abc"));
    assert(!v32_gpubundles_readonly_match("/private/var/tmp/other"));
    assert(!v32_gpubundles_readonly_match("/Library/Preferences/com.apple.plist"));

    // Only PID 1 (launchd) gets override write grant on /private/var/db/com.apple.xpc.launchd/disabled.plist
    assert(v32_launchd_override_match(1, "/private/var/db/com.apple.xpc.launchd/disabled.plist"));
    assert(v32_launchd_override_match(1, "/System/Volumes/Data/private/var/db/com.apple.xpc.launchd/disabled.501.plist"));
    assert(!v32_launchd_override_match(501, "/private/var/db/com.apple.xpc.launchd/disabled.plist"));
    assert(!v32_launchd_override_match(1, "/private/var/db/other.plist"));
}


#include "ShengSmcKeys.h"

static uint64_t v33_pad_auto_heap(uint32_t heap_type, uint64_t size) {
    uint64_t heapBytes = size;
    if (heap_type == 0 /* MTLHeapTypeAutomatic */ && heapBytes > 0 && heapBytes < (16ULL << 20)) {
        const uint64_t extra = (heapBytes >> 2) > 0x20000ULL ? (heapBytes >> 2) : 0x20000ULL;
        heapBytes = (heapBytes + extra + 0xffffULL) & ~0xffffULL;
    }
    return heapBytes;
}

static int64_t v33_absentd_delay_ms(const char *name) {
    if (std::strncmp(name, "com.apple.assistant", 19) == 0 ||
        std::strncmp(name, "com.apple.siri", 14) == 0 ||
        std::strncmp(name, "com.apple.Siri", 14) == 0 ||
        std::strncmp(name, "com.apple.softwareupdated", 25) == 0) {
        return 5000;
    }
    return 250;
}

static int v33_not_charging_reason(bool charging, bool fully_charged, bool external) {
    return charging ? 0 : (fully_charged ? 1 : (external ? 2 : 0));
}

static void test_v33_battery_smc_heap_and_absentd_policies() {
    // 1. 64-bit FamilyCode avoids SInt32 -> SInt64 sign extension of 0xe0004000
    const int32_t raw_fam = static_cast<int32_t>(0xe0004000u);
    const int64_t broken_s32_to_s64 = static_cast<int64_t>(raw_fam);
    const int64_t fixed_u32_in_64bit_osnumber = static_cast<int64_t>(static_cast<uint64_t>(static_cast<uint32_t>(raw_fam)));
    assert(broken_s32_to_s64 < 0);
    assert(fixed_u32_in_64bit_osnumber == 0xe0004000LL && fixed_u32_in_64bit_osnumber > 0);

    // 2. NotChargingReason states
    assert(v33_not_charging_reason(true, false, true) == 0);   // actively charging
    assert(v33_not_charging_reason(false, true, true) == 1);   // full on external power
    assert(v33_not_charging_reason(false, false, true) == 2);  // weak-USB SDP net discharge
    assert(v33_not_charging_reason(false, false, false) == 0); // unplugged on battery

    // 3. SMC 'RBID' key present and readable as ui8 = 0
    ShengSmcSnapshot snap = {};
    ShengSmcParam in_p = {}, out_p = {};
    in_p.key = SHENG_SMC_FOURCC('R', 'B', 'I', 'D');
    in_p.data8 = SHENG_SMC_CMD_READ_KEY;
    uint32_t reads = 0, misses = 0;
    sheng_smc_handle_call(&in_p, &out_p, &snap, &reads, nullptr, nullptr, nullptr, &misses, nullptr);
    assert(out_p.result == SHENG_SMC_RESULT_OK);
    assert(out_p.dataSize == 1 && out_p.dataType == SHENG_SMC_TYPE_UI8 && out_p.bytes[0] == 0);
    assert(reads == 1 && misses == 0);

    // 4. MTLHeapTypeAutomatic UBWC alignment headroom:
    // dock.extra requests 0x70000 for two 212x217 RGBA8 textures whose Turnip UBWC size is 2 * 0x40800 = 0x81000
    const uint64_t padded_dock = v33_pad_auto_heap(0, 0x70000ULL);
    assert(padded_dock == 0x90000ULL && padded_dock >= 2ULL * 0x40800ULL);
    assert(v33_pad_auto_heap(1 /* MTLHeapTypePlacement */, 0x70000ULL) == 0x70000ULL);
    assert(v33_pad_auto_heap(0, 32ULL << 20) == (32ULL << 20));

    // 5. absentd bounded per-service hold delay (no sync-XPC deadlock!)
    assert(v33_absentd_delay_ms("com.apple.assistant.settings") == 5000);
    assert(v33_absentd_delay_ms("com.apple.assistant.analytics") == 5000);
    assert(v33_absentd_delay_ms("com.apple.siri.invoke") == 5000);
    assert(v33_absentd_delay_ms("com.apple.softwareupdated") == 5000);
    assert(v33_absentd_delay_ms("com.apple.mobileactivationd") == 250);
    assert(v33_absentd_delay_ms("com.apple.fairplayd") == 250);
}


#include "ThermalCapPolicy.h"
#include <vector>

struct V34Span {
    uint64_t offset;
    uint64_t physical;
    uint64_t bytes;
};

static bool v34_coalesce_segments(
    const std::vector<std::pair<uint64_t, uint64_t>> &hw_runs,
    size_t bytes,
    uint64_t limit,
    std::vector<V34Span> *out_spans,
    size_t *out_queries
) {
    out_spans->clear();
    *out_queries = 0;
    auto get_seg = [&](size_t offset, uint64_t *len_out) -> uint64_t {
        ++(*out_queries);
        size_t cur = 0;
        for (const auto &r : hw_runs) {
            if (offset >= cur && offset < cur + r.second) {
                const size_t delta = offset - cur;
                *len_out = r.second - delta;
                return r.first + delta;
            }
            cur += r.second;
        }
        *len_out = 0;
        return 0;
    };
    for (size_t offset = 0; offset < bytes;) {
        uint64_t length = 0;
        const uint64_t physical = get_seg(offset, &length);
        size_t step = static_cast<size_t>(length & ~uint64_t(4095));
        if (step > bytes - offset) step = bytes - offset;
        if (!physical || (physical & 4095) || step < 4096 || physical >= limit || step > limit - physical) return false;
        if (!out_spans->empty() && physical == out_spans->back().physical + out_spans->back().bytes)
            out_spans->back().bytes += step;
        else
            out_spans->push_back({offset, physical, step});
        offset += step;
    }
    return true;
}

static bool v34_is_third_party_mac_gpu_caller(bool enabled, const char *prog, const char *fname) {
    if (!enabled) return false;
    if (prog && (!std::strcmp(prog, "WindowServer") || !std::strcmp(prog, "backboardd") ||
                 !std::strcmp(prog, "iconservicesagent") || !std::strcmp(prog, "iconservicesd") ||
                 !std::strcmp(prog, "Finder") || !std::strcmp(prog, "Dock") ||
                 !std::strcmp(prog, "SystemUIServer") || !std::strcmp(prog, "ControlCenter") ||
                 !std::strcmp(prog, "loginwindow") || !std::strcmp(prog, "System Settings"))) {
        return false;
    }
    if (!fname || !fname[0]) return false;
    if (!std::strncmp(fname, "/System/Library/", 16) ||
        !std::strncmp(fname, "/System/Applications/", 21) ||
        !std::strncmp(fname, "/System/Volumes/Data/System/", 28) ||
        !std::strncmp(fname, "/usr/lib/", 9) ||
        !std::strncmp(fname, "/usr/libexec/", 13) ||
        !std::strncmp(fname, "/Library/GPUBundles/", 20)) {
        return false;
    }
    if (std::strstr(fname, "Electron Framework") ||
        std::strstr(fname, "Chromium Framework") ||
        std::strstr(fname, "libGLESv2") ||
        std::strstr(fname, "libdawn") ||
        std::strstr(fname, "/Applications/") ||
        std::strstr(fname, "/Users/") ||
        !std::strncmp(fname, "/opt/", 5)) {
        return true;
    }
    return false;
}

static bool v34_should_skip_appex_metal(bool appex_metal_bootarg, const char *fsr, const char *exe_path) {
    if (appex_metal_bootarg) return false;
    if (!fsr || std::strcmp(fsr, "1") != 0) return false;
    return exe_path && std::strstr(exe_path, ".appex/Contents/MacOS/") != nullptr;
}

static void test_v34_coalescing_thermal_chromium_and_appex() {
    // 1. 48 MiB IOSurface (12,288 pages) in 3 contiguous 16 MiB runs -> 3 queries instead of 12,288!
    const size_t total_48m = 48ULL << 20;
    std::vector<std::pair<uint64_t, uint64_t>> runs = {
        {0x100000000ULL, 16ULL << 20},
        {0x101000000ULL, 16ULL << 20}, // adjacent to first -> merges into 32 MiB span!
        {0x180000000ULL, 16ULL << 20},
    };
    std::vector<V34Span> spans;
    size_t queries = 0;
    assert(v34_coalesce_segments(runs, total_48m, 1ULL << 40, &spans, &queries));
    assert(queries == 3);
    assert(spans.size() == 2);
    assert(spans[0].offset == 0 && spans[0].physical == 0x100000000ULL && spans[0].bytes == (32ULL << 20));
    assert(spans[1].offset == (32ULL << 20) && spans[1].physical == 0x180000000ULL && spans[1].bytes == (16ULL << 20));

    // Boundary check: PA exceeding limit (0x180000000 + 16 MiB = 0x181000000 > 0x180800000) is rejected cleanly
    assert(!v34_coalesce_segments(runs, total_48m, 0x180800000ULL, &spans, &queries));

    // 2. ThermalCapPolicy v5: 3-round upward debounce on L1..L3 + immediate 2-round safety on L4 (102C) & L5 (108C)
    static_assert(sheng_thermal_cap::kTableVersion == 5, "Expected ThermalCap table v5");
    sheng_thermal_cap::ThermalState st = {};
    sheng_thermal_cap::ThermalInput in = {{50000, 52000, 76000}, 55000, 76000, 50000, false, 0, 8};

    // 2 transient rounds at 76C (< 3 rounds) do NOT clamp!
    auto c1 = sheng_thermal_cap::thermalStep(st, in, 1000);
    assert(c1.level == 0 && c1.cpuPercent[2] == 100 && c1.gpuLevel == 8);
    auto c2 = sheng_thermal_cap::thermalStep(st, in, 2000);
    assert(c2.level == 0 && c2.cpuPercent[2] == 100 && c2.gpuLevel == 8);

    // Dropping back to 65C resets upRounds to 0
    in.tjMax = 65000;
    auto c_cool = sheng_thermal_cap::thermalStep(st, in, 3000);
    assert(c_cool.level == 0 && st.upRounds == 0);

    // 3 sustained rounds at 83C enter L2 ({88, 78, 72}, GPU 7)
    in.tjMax = 83000;
    assert(sheng_thermal_cap::thermalStep(st, in, 4000).level == 0);
    assert(sheng_thermal_cap::thermalStep(st, in, 5000).level == 0);
    auto c_l2 = sheng_thermal_cap::thermalStep(st, in, 6000);
    assert(c_l2.level == 2 && c_l2.cpuPercent[0] == 88 && c_l2.cpuPercent[1] == 78 && c_l2.cpuPercent[2] == 72 && c_l2.gpuLevel == 7);

    // Emergency L4 (102C) and L5 (108C) STILL fire on round #2 without waiting for kUpRounds=3!
    sheng_thermal_cap::ThermalState st_emg = {};
    sheng_thermal_cap::ThermalInput in_halt = {{90000, 95000, 103000}, 80000, 103000, 60000, false, 0, 8};
    assert(sheng_thermal_cap::thermalStep(st_emg, in_halt, 1000).action == 0);
    auto c_halt = sheng_thermal_cap::thermalStep(st_emg, in_halt, 2000);
    assert(c_halt.level == 4 && c_halt.action == 1);

    // 3. Scoped Chromium / Electron / ANGLE / Dawn caller filter
    assert(v34_is_third_party_mac_gpu_caller(
        true, "Helper (GPU)",
        "/Applications/ExampleApp.app/Contents/Frameworks/Electron Framework.framework/Versions/A/Libraries/libGLESv2.dylib"));
    assert(v34_is_third_party_mac_gpu_caller(
        true, "Helper (GPU)",
        "/System/Volumes/Data/Applications/ExampleApp.app/Contents/Frameworks/Electron Framework.framework/Electron Framework"));
    // Even INSIDE an Electron GPU helper, Apple's own QuartzCore / RenderBox / CoreImage must still see Mac2=NO!
    assert(!v34_is_third_party_mac_gpu_caller(
        true, "Helper (GPU)",
        "/System/Library/Frameworks/QuartzCore.framework/Versions/A/QuartzCore"));
    assert(!v34_is_third_party_mac_gpu_caller(
        true, "Helper (GPU)",
        "/System/Library/PrivateFrameworks/RenderBox.framework/Versions/A/RenderBox"));
    // Apple system apps & daemons always false
    assert(!v34_is_third_party_mac_gpu_caller(true, "WindowServer", "/Applications/Anything"));
    assert(!v34_is_third_party_mac_gpu_caller(true, "iconservicesagent", "/Applications/Anything"));
    assert(!v34_is_third_party_mac_gpu_caller(true, "Weather", "/System/Applications/Weather.app/Contents/MacOS/Weather"));
    assert(!v34_is_third_party_mac_gpu_caller(false, "Helper (GPU)", "/Applications/ExampleApp.app/libGLESv2.dylib"));

    // 4. Settings .appex fast skip
    assert(v34_should_skip_appex_metal(
        false, "1",
        "/System/Library/ExtensionKit/Extensions/BatterySettings.appex/Contents/MacOS/BatterySettings"));
    assert(!v34_should_skip_appex_metal(
        false, "1",
        "/System/Applications/System Settings.app/Contents/MacOS/System Settings"));
    assert(!v34_should_skip_appex_metal(
        false, "1",
        "/System/Library/CoreServices/iconservicesagent"));
    assert(!v34_should_skip_appex_metal(
        true, "1",
        "/System/Library/ExtensionKit/Extensions/BatterySettings.appex/Contents/MacOS/BatterySettings"));
}

int main() {
    test_v30_resolve_user_metal_cache_dir();
    test_v30_absentd_classifier();
    test_v30_power_force_halt_state_machine();
    test_v30_touch_unblank_defers_rebaseline_while_finger_touching();
    test_v29_encode_batch_preserved();
    test_v32_weak_usb_sdp_net_discharge_and_family_code();
    test_v32_sandbox_and_launchd_path_grants();
    test_v33_battery_smc_heap_and_absentd_policies();
    test_v34_coalescing_thermal_chromium_and_appex();
    std::printf("ALL_V34_CPP_UNIT_TESTS_PASSED\n");
    return 0;
}
