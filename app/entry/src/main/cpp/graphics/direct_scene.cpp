#include "graphics/direct_scene.h"

#include <algorithm>

namespace ohoswayland {

DirectScene& DirectScene::Instance()
{
    static DirectScene scene;
    return scene;
}

// Fills in sizes the client has not reported: no geometry means the whole
// surface, and no surface size means the configured or geometry size.
void DirectScene::Resolve(Entry& entry)
{
    DirectLayer& layer = entry.layer;
    if (!layer.surfaceW || !layer.surfaceH) {
        layer.surfaceW = entry.hasGeometry ? layer.geomX + layer.geomW : entry.configuredW;
        layer.surfaceH = entry.hasGeometry ? layer.geomY + layer.geomH : entry.configuredH;
    }
    if (!entry.hasGeometry) {
        layer.geomX = 0;
        layer.geomY = 0;
        layer.geomW = layer.surfaceW;
        layer.geomH = layer.surfaceH;
    }
}

void DirectScene::SetToplevel(uint64_t key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    Entry entry;
    entry.layer.key = key;
    entry.configuredW = 640;
    entry.configuredH = 360;
    Resolve(entry);
    entries_.push_back(entry);
}

void DirectScene::AddPopup(uint64_t key, uint64_t parentKey, int32_t x, int32_t y,
                           uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    Entry entry;
    entry.layer.key = key;
    entry.layer.popup = true;
    entry.layer.x = x;
    entry.layer.y = y;
    entry.parentKey = parentKey;
    entry.configuredW = width;
    entry.configuredH = height;
    Resolve(entry);
    entries_.push_back(entry);
}

void DirectScene::MovePopup(uint64_t key, int32_t x, int32_t y, uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
        if (entry.layer.key != key) continue;
        entry.layer.x = x;
        entry.layer.y = y;
        entry.configuredW = width;
        entry.configuredH = height;
        Resolve(entry);
    }
}

void DirectScene::Remove(uint64_t key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entries_.empty() && entries_.front().layer.key == key) {
        entries_.clear();
        return;
    }
    // Removing a popup also removes popups stacked on it.
    std::vector<uint64_t> removed = {key};
    for (size_t i = 0; i < entries_.size();) {
        const bool gone = std::find(removed.begin(), removed.end(), entries_[i].layer.key) != removed.end() ||
            std::find(removed.begin(), removed.end(), entries_[i].parentKey) != removed.end();
        if (gone) {
            removed.push_back(entries_[i].layer.key);
            entries_.erase(entries_.begin() + i);
        } else {
            ++i;
        }
    }
}

void DirectScene::RemoveClient(uint32_t pid)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!entries_.empty() && static_cast<uint32_t>(entries_.front().layer.key >> 32) == pid) {
        entries_.clear();
        return;
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [pid](const Entry& entry) {
        return static_cast<uint32_t>(entry.layer.key >> 32) == pid;
    }), entries_.end());
}

void DirectScene::UpdateSurface(uint64_t key, bool hasGeometry, int32_t geomX, int32_t geomY,
                                uint32_t geomW, uint32_t geomH, uint32_t surfaceW, uint32_t surfaceH,
                                uint32_t bufferW, uint32_t bufferH)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
        if (entry.layer.key != key) continue;
        entry.hasGeometry = hasGeometry && geomW && geomH;
        if (entry.hasGeometry) {
            entry.layer.geomX = geomX;
            entry.layer.geomY = geomY;
            entry.layer.geomW = geomW;
            entry.layer.geomH = geomH;
        }
        entry.layer.surfaceW = surfaceW;
        entry.layer.surfaceH = surfaceH;
        entry.layer.bufferW = bufferW;
        entry.layer.bufferH = bufferH;
        Resolve(entry);
    }
}

void DirectScene::SetConfiguredSize(uint64_t key, uint32_t width, uint32_t height)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (Entry& entry : entries_) {
        if (entry.layer.key != key || !width || !height) continue;
        entry.configuredW = width;
        entry.configuredH = height;
        if (!entry.hasGeometry) {
            entry.layer.surfaceW = 0;
            entry.layer.surfaceH = 0;
        }
        Resolve(entry);
    }
}

bool DirectScene::Contains(uint64_t key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return key && std::any_of(entries_.begin(), entries_.end(),
                              [key](const Entry& entry) { return entry.layer.key == key; });
}

bool DirectScene::IsToplevel(uint64_t key) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return key && !entries_.empty() && entries_.front().layer.key == key;
}

bool DirectScene::HasPopups() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size() > 1;
}

std::vector<DirectLayer> DirectScene::SnapshotLocked() const
{
    std::vector<DirectLayer> layers;
    for (const Entry& entry : entries_) {
        DirectLayer layer = entry.layer;
        // Popup positions are relative to their parent's window geometry.
        for (const DirectLayer& parent : layers) {
            if (parent.key == entry.parentKey) {
                layer.x += parent.x;
                layer.y += parent.y;
            }
        }
        layers.push_back(layer);
    }
    return layers;
}

std::vector<DirectLayer> DirectScene::Snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return SnapshotLocked();
}

bool DirectScene::HitTest(double x, double y, uint64_t& key, double& localX, double& localY) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const std::vector<DirectLayer> layers = SnapshotLocked();
    for (auto it = layers.rbegin(); it != layers.rend(); ++it) {
        // Popups receive input over their window geometry; the toplevel takes the rest.
        const bool inside = it->popup
            ? x >= it->x && y >= it->y && x < it->x + static_cast<double>(it->geomW) &&
                y < it->y + static_cast<double>(it->geomH)
            : true;
        if (!inside) continue;
        key = it->key;
        localX = x - it->x + it->geomX;
        localY = y - it->y + it->geomY;
        return true;
    }
    return false;
}

} // namespace ohoswayland
