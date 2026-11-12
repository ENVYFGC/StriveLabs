#pragma once

#include <atomic>
#include <chrono>

namespace perf {
enum Bucket { REPLAY, MENU, PAUSE_MENU, TRIALS, DAMAGE, COLORS, BAR, GAME, HELD, HUD, BUCKETS };

extern std::atomic<bool> on;
void add(int bucket, double ms);
void tickDone(double whole_ms, bool counted);
void hudDone();
void overlayFrame(double copy_ms, double draw_ms);

inline double since(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

struct Scope
{
    int bucket;
    std::chrono::steady_clock::time_point t0;
    explicit Scope(int b) : bucket(on ? b : -1)
    {
        if (bucket >= 0) t0 = std::chrono::steady_clock::now();
    }
    ~Scope()
    {
        if (bucket >= 0) add(bucket, since(t0));
    }
};
}
