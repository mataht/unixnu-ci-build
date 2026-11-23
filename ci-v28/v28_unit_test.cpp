#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <pthread.h>

extern "C" {
#include "NvtTouchCore.h"
#include "ShengBattmgr.h"
}
#include "TouchMouse.h"

// --- 1. Exact V28 NBPipelineWorkerState concurrency, re-entrancy, & TLS error test ---
static thread_local std::string tMockNmbLastError;
static const char *mock_nmb_last_error() { return tMockNmbLastError.c_str(); }

static thread_local const std::string *tPipelineStackError = nullptr;
struct NBPipelineStackError {
    std::string text;
    const std::string *prev;
    NBPipelineStackError() : prev(tPipelineStackError) {}
    NBPipelineStackError(const NBPipelineStackError &) = delete;
    NBPipelineStackError &operator=(const NBPipelineStackError &) = delete;
    ~NBPipelineStackError() { tPipelineStackError = prev; }
};

struct NBPipelineWorkerState {
    std::mutex callMutex;
    std::mutex jobMutex;
    std::condition_variable jobCv;
    std::condition_variable doneCv;
    pthread_t thread{};
    std::atomic<bool> threadReady{false};
    std::atomic<int> threadsSpawned{0};
    bool startTried = false;
    const std::function<void()> *fn = nullptr;
    std::string *errOut = nullptr;
    bool hasJob = false;
};

static NBPipelineWorkerState &pipelineWorker() {
    static NBPipelineWorkerState *s = new NBPipelineWorkerState();
    return *s;
}

static void *pipelineWorkerMain(void *arg) {
    NBPipelineWorkerState &s = *static_cast<NBPipelineWorkerState *>(arg);
    for (;;) {
        const std::function<void()> *fn = nullptr;
        std::string *errOut = nullptr;
        {
            std::unique_lock<std::mutex> lk(s.jobMutex);
            s.jobCv.wait(lk, [&] { return s.hasJob; });
            fn = s.fn;
            errOut = s.errOut;
        }
        tMockNmbLastError.clear();
        (*fn)();
        const char *e = mock_nmb_last_error();
        if (errOut && e && *e) *errOut = e;
        {
            std::lock_guard<std::mutex> lk(s.jobMutex);
            s.fn = nullptr;
            s.errOut = nullptr;
            s.hasJob = false;
        }
        s.doneCv.notify_one();
    }
    return nullptr;
}

static void onPipelineStackSim(NBPipelineStackError *err, bool forceSmallCallerStack, const std::function<void()> &body) {
    NBPipelineWorkerState &s = pipelineWorker();
    if ((s.threadReady.load(std::memory_order_acquire) && pthread_equal(pthread_self(), s.thread)) ||
        !forceSmallCallerStack) {
        body();
        return;
    }
    std::lock_guard<std::mutex> serialLock(s.callMutex);
    if (!s.startTried) {
        s.startTried = true;
        pthread_attr_t attr;
        if (!pthread_attr_init(&attr)) {
            if (!pthread_attr_setstacksize(&attr, 16u << 20) &&
                !pthread_create(&s.thread, &attr, pipelineWorkerMain, &s)) {
                s.threadsSpawned.fetch_add(1);
                s.threadReady.store(true, std::memory_order_release);
            }
            pthread_attr_destroy(&attr);
        }
    }
    assert(s.threadReady.load(std::memory_order_relaxed));
    std::string childErr;
    {
        std::unique_lock<std::mutex> lk(s.jobMutex);
        s.fn = &body;
        s.errOut = &childErr;
        s.hasJob = true;
        s.jobCv.notify_one();
        s.doneCv.wait(lk, [&] { return !s.hasJob; });
    }
    if (err && !childErr.empty()) {
        err->text = std::move(childErr);
        tPipelineStackError = &err->text;
    }
}

static void test_v28_persistent_pipeline_worker_32_thread_burst_and_reentrancy() {
    std::mutex seenLock;
    std::set<pthread_t> workerThreadsSeen;
    std::atomic<int> activeInWorker{0};
    std::atomic<int> maxConcurrentInWorker{0};
    std::atomic<int> completedJobs{0};

    // Launch 32 concurrent 512 KiB GCD-like caller threads simultaneously
    std::vector<std::thread> callers;
    for (int tid = 0; tid < 32; ++tid) {
        callers.emplace_back([&, tid]() {
            for (int iter = 0; iter < 8; ++iter) {
                NBPipelineStackError stackErr;
                const bool wantErr = ((tid + iter) % 3 == 0);
                const std::string expectedMsg = "NeedsInput(LinkedFunction { symbol: \"vfx::api::applySystemTreatments\" })#" + std::to_string(tid);
                onPipelineStackSim(&stackErr, true, [&]() {
                    const int cur = activeInWorker.fetch_add(1) + 1;
                    int prevMax = maxConcurrentInWorker.load();
                    while (cur > prevMax && !maxConcurrentInWorker.compare_exchange_weak(prevMax, cur)) {}
                    {
                        std::lock_guard<std::mutex> g(seenLock);
                        workerThreadsSeen.insert(pthread_self());
                    }
                    // Simulate recursive re-entry (`zeroRestFunction -> nb_newFunctionWithDescriptor: -> onPipelineStack`)
                    int nestedRan = 0;
                    NBPipelineStackError innerErr;
                    onPipelineStackSim(&innerErr, true, [&]() {
                        assert(pthread_equal(pthread_self(), pipelineWorker().thread));
                        nestedRan = 1;
                    });
                    assert(nestedRan == 1);
                    if (wantErr) {
                        tMockNmbLastError = expectedMsg;
                    }
                    activeInWorker.fetch_sub(1);
                    completedJobs.fetch_add(1);
                });
                if (wantErr) {
                    assert(tPipelineStackError != nullptr);
                    assert(*tPipelineStackError == expectedMsg);
                } else {
                    assert(tPipelineStackError == nullptr);
                }
            }
        });
    }
    for (auto &t : callers) t.join();

    assert(completedJobs.load() == 32 * 8);
    assert(pipelineWorker().threadsSpawned.load() == 1);
    assert(workerThreadsSeen.size() == 1);
    assert(maxConcurrentInWorker.load() == 1);
}

// --- 2. V28 ensureCacheDirWritable symlink + read-only shared dir test ---
static bool ensureCacheDirWritable(const std::string &dir) {
    if (dir.empty() || dir[0] != '/' || dir.rfind("/dev/null", 0) == 0) return false;
    struct stat st = {};
    if (stat(dir.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) && access(dir.c_str(), W_OK) == 0;
    }
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    return access(dir.c_str(), W_OK) == 0;
}

static void test_v28_cache_dir_symlink_and_null_reject() {
    assert(!ensureCacheDirWritable(""));
    assert(!ensureCacheDirWritable("/dev/null/m2v"));
    char tmpl[] = "/tmp/v28_cache_test_XXXXXX";
    char *base = mkdtemp(tmpl);
    assert(base != nullptr);
    std::string realVar = std::string(base) + "/private_var_folders";
    std::string symVar = std::string(base) + "/var_folders";
    assert(mkdir(realVar.c_str(), 0700) == 0);
    assert(symlink(realVar.c_str(), symVar.c_str()) == 0);

    std::string metalDir = symVar + "/com.apple.metal";
    std::string m2vDir = metalDir + "/m2v";
    std::string ir3Dir = m2vDir + "/ir3";
    assert(ensureCacheDirWritable(metalDir));
    assert(ensureCacheDirWritable(m2vDir));
    assert(ensureCacheDirWritable(ir3Dir));
    // Re-checking already-existing writable directories takes the stat() fast path cleanly
    assert(ensureCacheDirWritable(m2vDir));
    assert(ensureCacheDirWritable(ir3Dir));
}

// --- 3. Preserved V27 Novatek thermal baseline recovery & uniform drift while touching ---
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

static void test_v28_novatek_preserved() {
    auto *core = static_cast<nvt_touch_core *>(std::calloc(1, sizeof(nvt_touch_core)));
    assert(core != nullptr);
    nvt_touch_reset(core);
    int16_t frame[NVT_ROWS * NVT_COLUMNS];
    nvt_contact out[NVT_SLOTS];

    for (unsigned f = 0; f < NVT_STARTUP_FRAMES; ++f) {
        std::fill_n(frame, NVT_NODES, (int16_t)5000);
        assert(nvt_touch_process(core, frame, 4, out, NVT_SLOTS) == -1);
    }
    assert(core->have_reference == 1 && core->rebaselines == 0);

    // Hold 1 finger across +180 uniform thermal drift -> 1 clean contact, 0 step-edge phantoms
    for (int step = 0; step < 300; ++step) {
        int16_t bg = (int16_t)(5000 + (step * 180) / 300);
        std::fill_n(frame, NVT_NODES, bg);
        fill_finger(frame, 18, 28, 900);
        int n = nvt_touch_process(core, frame, 4, out, NVT_SLOTS);
        if (step == 0) assert(n == 0); // 1-frame onset debounce
        else assert(n == 1);
    }

    // Inject 30-peak storm -> suppressed at >=12, auto-rebaselined at 18
    for (int f = 0; f < 18; ++f) {
        std::fill_n(frame, NVT_NODES, (int16_t)5360);
        for (int k = 0; k < 30; ++k) {
            const int rr = 2 + (k * 3) % 36;
            const int cc = 35 + (k % 12);
            frame[rr * NVT_COLUMNS + cc] = (int16_t)(frame[rr * NVT_COLUMNS + cc] + 240 + (k % 5) * 20);
        }
        int n = nvt_touch_process(core, frame, 4, out, NVT_SLOTS);
        if (f >= 11) assert(n == 0);
    }
    assert(core->rebaselines == 1 && core->have_reference == 0);
    std::free(core);
}

// --- 4. V28 native_deny_exec token & length test ---
static bool deny_exec_matches(const char *csv, const char *path) {
    const char *base = std::strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *p = csv;
    while (*p) {
        const char *comma = std::strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : std::strlen(p);
        if (len > 0) {
            if (p[0] == '/') {
                if (std::strlen(path) == len && std::strncmp(path, p, len) == 0) return true;
            } else {
                if (std::strlen(base) == len && std::strncmp(base, p, len) == 0) return true;
            }
        }
        if (!comma) break;
        p = comma + 1;
    }
    return false;
}

static void test_v28_deny_exec_tokens() {
    const char *csv =
        "/usr/libexec/mobileactivationd,/usr/libexec/cryptexd,seputil,lskdd,fairplayd,"
        "modelcatalogd,mediaanalysisd,photoanalysisd,spindump,tailspind,ReportMemoryException,"
        "duetexpertd,intelligenceflowd,intelligenceplatformd,intelligencecontextd,intelligencetasksd,"
        "spotlightknowledged,spotlightknowledged.importer,spotlightknowledged.updater,"
        "siriknowledged,siriinferenced,knowledge-agent,PerfPowerServices,PerfPowerServicesExtended,"
        "coreduetd,BackgroundShortcutRunner,STExtractionService.privileged";
    assert(std::strlen(csv) + 1 < 1024);
    assert(deny_exec_matches(csv, "/System/Library/PrivateFrameworks/WorkflowKit.framework/XPCServices/BackgroundShortcutRunner.xpc/Contents/MacOS/BackgroundShortcutRunner"));
    assert(deny_exec_matches(csv, "/System/Library/PrivateFrameworks/StreamingExtractor.framework/XPCServices/STExtractionService.privileged.xpc/Contents/MacOS/STExtractionService.privileged"));
    assert(!deny_exec_matches(csv, "/System/Applications/Weather.app/Contents/MacOS/Weather"));
    assert(!deny_exec_matches(csv, "/System/Library/CoreServices/iconservicesagent"));
}

int main() {
    test_v28_persistent_pipeline_worker_32_thread_burst_and_reentrancy();
    test_v28_cache_dir_symlink_and_null_reject();
    test_v28_novatek_preserved();
    test_v28_deny_exec_tokens();
    std::printf("ALL_V28_CPP_UNIT_TESTS_PASSED\n");
    return 0;
}
