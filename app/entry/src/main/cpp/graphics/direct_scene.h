#pragma once

#include <cstdint>
#include <mutex>
#include <vector>

namespace ohoswayland {

// One direct-GPU surface the XComponent presents. Positions and sizes are in
// logical (surface) coordinates.
struct DirectLayer {
    uint64_t key = 0;
    bool popup = false;
    // Window-geometry origin relative to the toplevel's window geometry.
    int32_t x = 0;
    int32_t y = 0;
    // Window geometry inside the surface, and the full surface size.
    int32_t geomX = 0;
    int32_t geomY = 0;
    uint32_t geomW = 0;
    uint32_t geomH = 0;
    uint32_t surfaceW = 0;
    uint32_t surfaceH = 0;
    // The client's buffer in pixels (0 until its first commit reports it).
    uint32_t bufferW = 0;
    uint32_t bufferH = 0;
};

// The presented scene: the newest toplevel plus its popups, bottom to top.
// Written on the Wayland thread, read by the renderer and input threads.
class DirectScene {
public:
    static DirectScene& Instance();

    void SetToplevel(uint64_t key);
    // x/y place the popup's window geometry relative to its parent's.
    void AddPopup(uint64_t key, uint64_t parentKey, int32_t x, int32_t y, uint32_t width, uint32_t height);
    void MovePopup(uint64_t key, int32_t x, int32_t y, uint32_t width, uint32_t height);
    void Remove(uint64_t key);
    void RemoveClient(uint32_t pid);

    // Commit-time state: window geometry (if the client set one) and the
    // surface size from the client's full-buffer damage (0 if unknown).
    void UpdateSurface(uint64_t key, bool hasGeometry, int32_t geomX, int32_t geomY,
                       uint32_t geomW, uint32_t geomH, uint32_t surfaceW, uint32_t surfaceH,
                       uint32_t bufferW, uint32_t bufferH);
    // Size the compositor configured, used until the client reports one.
    void SetConfiguredSize(uint64_t key, uint32_t width, uint32_t height);

    bool Contains(uint64_t key) const;
    bool IsToplevel(uint64_t key) const;
    // [0] is the toplevel; popups follow bottom to top with absolute positions.
    std::vector<DirectLayer> Snapshot() const;
    // Maps a point in toplevel window-geometry coordinates to the topmost
    // surface under it, in that surface's local coordinates.
    bool HitTest(double x, double y, uint64_t& key, double& localX, double& localY) const;
    bool HasPopups() const;

private:
    struct Entry {
        DirectLayer layer;
        uint64_t parentKey = 0;
        bool hasGeometry = false;
        uint32_t configuredW = 0;
        uint32_t configuredH = 0;
    };
    static void Resolve(Entry& entry);
    std::vector<DirectLayer> SnapshotLocked() const;

    mutable std::mutex mutex_;
    std::vector<Entry> entries_;
};

} // namespace ohoswayland
