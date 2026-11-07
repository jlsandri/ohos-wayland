#include "graphics/present_stats.h"

#include <algorithm>
#include <chrono>

namespace ohoswayland {
namespace {
constexpr int64_t kWindowNs = 1000000000;
// An input with no frame within this time changed nothing on screen (a key
// at the end of a line); its stamp is dropped rather than charged to an
// unrelated later frame.
constexpr int64_t kInputTimeoutNs = 1000000000;
}

int64_t SteadyNowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

PresentStats& PresentStats::Instance()
{
    static PresentStats stats;
    return stats;
}

void PresentStats::TrimLocked(int64_t nowNs)
{
    while (!frames_.empty() && frames_.front() <= nowNs - kWindowNs) frames_.pop_front();
    if (pendingInputNs_ && nowNs - pendingInputNs_ > kInputTimeoutNs) pendingInputNs_ = 0;
}

void PresentStats::OnClientFrame(int64_t nowNs, const PresentedBuffer& buffer)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TrimLocked(nowNs);
    frames_.push_back(nowNs);
    ++presented_;
    buffer_ = buffer;
    if (pendingInputNs_) {
        inputToFrameMs_ = (nowNs - pendingInputNs_) / 1e6;
        pendingInputNs_ = 0;
    }
}

void PresentStats::OnDisplayPeriod(int64_t periodNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    periodNs_ = periodNs;
}

void PresentStats::OnInput(int64_t nowNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TrimLocked(nowNs);
    // The oldest unanswered input is the one the user has waited longest on.
    if (!pendingInputNs_) pendingInputNs_ = nowNs;
}

int PresentStats::FramesInLastSecond(int64_t nowNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TrimLocked(nowNs);
    return static_cast<int>(frames_.size());
}

PresentSnapshot PresentStats::Snapshot(int64_t nowNs)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TrimLocked(nowNs);
    PresentSnapshot snapshot;
    snapshot.frames = static_cast<int>(frames_.size());
    snapshot.presented = presented_;
    if (frames_.size() >= 2) {
        snapshot.meanIntervalMs = (frames_.back() - frames_.front()) / 1e6 / (frames_.size() - 1);
        for (size_t i = 1; i < frames_.size(); ++i) {
            snapshot.intervalsMs.push_back((frames_[i] - frames_[i - 1]) / 1e6);
            snapshot.worstIntervalMs = std::max(snapshot.worstIntervalMs, snapshot.intervalsMs.back());
        }
    }
    snapshot.refreshHz = periodNs_ > 0 ? 1e9 / periodNs_ : 0;
    snapshot.inputToFrameMs = inputToFrameMs_;
    snapshot.buffer = buffer_;
    return snapshot;
}

} // namespace ohoswayland
