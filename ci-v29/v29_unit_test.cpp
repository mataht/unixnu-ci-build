#include <algorithm>
#include <atomic>
#include <cassert>
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
}
#include "TouchMouse.h"

// --- 1. V29 WindowServer EncodeBatch + setBytes closure stream integrity test ---
struct EncodeBatchTest {
    static constexpr size_t kAlign = alignof(std::max_align_t);
    static constexpr size_t kChunk = 64u << 10;
    static constexpr size_t rounded(size_t n) { return (n + kAlign - 1) & ~(kAlign - 1); }

    struct Header {
        uint64_t pacCanary;
        void (*run)(void *);
        size_t size;
    };
    struct Chunk {
        std::unique_ptr<uint8_t[]> bytes;
        size_t capacity = 0;
        size_t used = 0;
    };
    std::vector<Chunk> chunks;
    size_t at = 0;

    template <typename F>
    void add(F &&work) {
        using Closure = typename std::decay<F>::type;
        static_assert(alignof(Closure) <= kAlign, "closure alignment");
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
                assert(header.run != nullptr);
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

static void test_v29_encode_batch_set_bytes_never_corrupts_header_stream() {
    EncodeBatchTest batch;
    std::vector<uint32_t> recordedWords;
    int drawCalls = 0;

    // Interleave 1,024 setBytes uniform blocks (carrying 0xd63f091f & float matrices) + draw closures across >64 KiB
    for (uint32_t i = 0; i < 1024; ++i) {
        uint32_t rawUniforms[16];
        rawUniforms[0] = 0xd63f091fu; // exact `blraaz x8` opcode that crashed V28 if placed untagged in chunks!
        for (uint32_t k = 1; k < 16; ++k) rawUniforms[k] = (i << 8) ^ (0x3f800000u + k);

        // V29 restored setBytes path: owns uniform bytes inside the Header-tagged std::vector<uint8_t> closure
        const uint8_t *bytes = reinterpret_cast<const uint8_t *>(rawUniforms);
        const size_t length = sizeof(rawUniforms);
        const std::vector<uint8_t> copy(bytes, bytes + length);
        batch.add([copy, &recordedWords]() {
            assert(copy.size() == 64u);
            uint32_t w0 = 0, wLast = 0;
            std::memcpy(&w0, copy.data(), 4);
            std::memcpy(&wLast, copy.data() + 60, 4);
            assert(w0 == 0xd63f091fu);
            recordedWords.push_back(wLast);
        });
        batch.add([&drawCalls]() { ++drawCalls; });
    }

    const size_t ran = batch.run();
    assert(ran == 2048);
    assert(drawCalls == 1024);
    assert(recordedWords.size() == 1024);
    for (uint32_t i = 0; i < 1024; ++i) {
        assert(recordedWords[i] == ((i << 8) ^ (0x3f800000u + 15u)));
    }
}

// --- 2. V28/V29 NBPipelineWorkerState concurrency, re-entrancy, & TLS error test ---
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
    tPipelineStackError = nullptr;
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

static void test_v29_persistent_pipeline_worker_32_thread_burst_and_reentrancy() {
    std::mutex seenLock;
    std::set<pthread_t> workerThreadsSeen;
    std::atomic<int> activeInWorker{0};
    std::atomic<int> maxConcurrentInWorker{0};
    std::atomic<int> completedJobs{0};

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

// --- 3. ensureCacheDirWritable symlink + null reject test ---
static bool ensureCacheDirWritable(const std::string &dir) {
    if (dir.empty() || dir[0] != '/' || dir.rfind("/dev/null", 0) == 0) return false;
    struct stat st = {};
    if (stat(dir.c_str(), &st) == 0) {
        return S_ISDIR(st.st_mode) && access(dir.c_str(), W_OK) == 0;
    }
    if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) return false;
    return access(dir.c_str(), W_OK) == 0;
}

static void test_v29_cache_dir_symlink_and_null_reject() {
    assert(!ensureCacheDirWritable(""));
    assert(!ensureCacheDirWritable("/dev/null/m2v"));
    char tmpl[] = "/tmp/v29_cache_test_XXXXXX";
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
    assert(ensureCacheDirWritable(m2vDir));
    assert(ensureCacheDirWritable(ir3Dir));
}

// --- 4. Preserved Novatek thermal baseline recovery ---
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

static void test_v29_novatek_preserved() {
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

    for (int step = 0; step < 300; ++step) {
        int16_t bg = (int16_t)(5000 + (step * 180) / 300);
        std::fill_n(frame, NVT_NODES, bg);
        fill_finger(frame, 18, 28, 900);
        int n = nvt_touch_process(core, frame, 4, out, NVT_SLOTS);
        if (step == 0) assert(n == 0);
        else assert(n == 1);
    }

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

int main() {
    test_v29_encode_batch_set_bytes_never_corrupts_header_stream();
    test_v29_persistent_pipeline_worker_32_thread_burst_and_reentrancy();
    test_v29_cache_dir_symlink_and_null_reject();
    test_v29_novatek_preserved();
    std::printf("ALL_V29_CPP_UNIT_TESTS_PASSED\n");
    return 0;
}
