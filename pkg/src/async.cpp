#include "async.h"
#include "log.h"

#include <orbis/libkernel.h>
#include <deque>

namespace {

struct Task {
    std::function<void()> work;
    std::function<void()> done;
};

struct Lane {
    std::deque<Task> queue;       // ждут выполнения
    int running = 0;              // выполняются прямо сейчас (0 или 1)
    OrbisPthread th;
    OrbisPthreadMutex mu;         // защищает queue/running
};

Lane g_lane[ASYNC_LANES];
std::deque<std::function<void()>> g_doneQ;   // готовые done-обработчики (ждут главный поток)
OrbisPthreadMutex g_doneMu;
bool g_inited = false;

void* lane_main(void* arg)
{
    Lane* L = (Lane*)arg;
    for (;;) {
        Task t;
        bool have = false;
        scePthreadMutexLock(&L->mu);
        if (!L->queue.empty()) {
            t = L->queue.front();
            L->queue.pop_front();
            L->running = 1;
            have = true;
        }
        scePthreadMutexUnlock(&L->mu);
        if (!have) { sceKernelUsleep(8000); continue; }

        if (t.work) t.work();

        scePthreadMutexLock(&g_doneMu);
        if (t.done) g_doneQ.push_back(t.done);
        scePthreadMutexUnlock(&g_doneMu);

        scePthreadMutexLock(&L->mu);
        L->running = 0;
        scePthreadMutexUnlock(&L->mu);
    }
    return NULL;
}

} // namespace

void async_init()
{
    if (g_inited) return;
    g_inited = true;
    scePthreadMutexInit(&g_doneMu, NULL, "pc_done");
    for (int i = 0; i < ASYNC_LANES; i++) {
        scePthreadMutexInit(&g_lane[i].mu, NULL, "pc_lane");
        OrbisPthreadAttr attr;
        scePthreadAttrInit(&attr);
        scePthreadAttrSetstacksize(&attr, 1024 * 1024);
        int r = scePthreadCreate(&g_lane[i].th, &attr, lane_main, &g_lane[i], "pc_worker");
        if (r != 0) logf_("async: lane %d create failed 0x%08X", i, (unsigned)r);
        scePthreadAttrDestroy(&attr);
    }
}

void async_run(int lane, std::function<void()> work, std::function<void()> done)
{
    if (lane < 0 || lane >= ASYNC_LANES) lane = ASYNC_ACT;
    Lane* L = &g_lane[lane];
    Task t; t.work = work; t.done = done;
    scePthreadMutexLock(&L->mu);
    L->queue.push_back(t);
    scePthreadMutexUnlock(&L->mu);
}

bool async_pump()
{
    bool any = false;
    for (;;) {
        std::function<void()> f;
        scePthreadMutexLock(&g_doneMu);
        if (!g_doneQ.empty()) { f = g_doneQ.front(); g_doneQ.pop_front(); }
        scePthreadMutexUnlock(&g_doneMu);
        if (!f) break;
        f();
        any = true;
    }
    return any;
}

bool async_pending(int lane)
{
    if (lane < 0 || lane >= ASYNC_LANES) return false;
    Lane* L = &g_lane[lane];
    scePthreadMutexLock(&L->mu);
    bool p = L->running != 0 || !L->queue.empty();
    scePthreadMutexUnlock(&L->mu);
    return p;
}
