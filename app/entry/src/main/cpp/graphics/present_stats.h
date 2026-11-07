#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace ohoswayland {

enum class PresentPath : uint8_t { None, GpuDirect, Shm };

// The bound toplevel's client buffer as the renderer last drew it.
struct PresentedBuffer {
    PresentPath path = PresentPath::None;
    int bufferW = 0;
    int bufferH = 0;
    // Client buffer pixels per logical pixel (wl_surface buffer scale or viewport).
    double clientScale = 0;
    // View pixels per client buffer pixel: 1 is pixel exact.
    double pixelRatio = 0;
};

struct PresentSnapshot {
    // Client frames presented in the last second.
    int frames = 0;
    // Client frames presented since the stats began, so a chart can tell how
    // many arrived between two snapshots.
    uint64_t presented = 0;
    // Mean and longest gap between those frames; 0 with fewer than two.
    double meanIntervalMs = 0;
    double worstIntervalMs = 0;
    // Every gap between those frames, oldest first.
    std::vector<double> intervalsMs;
    double refreshHz = 0;
    // Last key, button, or scroll to the next presented client frame; -1 if none.
    double inputToFrameMs = -1;
    PresentedBuffer buffer;
};

// What the diagnostics overlay reads. The render thread reports presented
// client frames, the input path reports input, and NAPI takes snapshots.
class PresentStats {
public:
    static PresentStats& Instance();

    void OnClientFrame(int64_t nowNs, const PresentedBuffer& buffer);
    void OnDisplayPeriod(int64_t periodNs);
    void OnInput(int64_t nowNs);
    PresentSnapshot Snapshot(int64_t nowNs);
    // Client frames presented in the last second, cheap enough to read every display frame.
    int FramesInLastSecond(int64_t nowNs);

private:
    void TrimLocked(int64_t nowNs);

    std::mutex mutex_;
    std::deque<int64_t> frames_;
    uint64_t presented_ = 0;
    PresentedBuffer buffer_;
    int64_t periodNs_ = 0;
    int64_t pendingInputNs_ = 0;
    double inputToFrameMs_ = -1;
};

int64_t SteadyNowNs();

} // namespace ohoswayland
