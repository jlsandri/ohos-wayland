#include <wayland-server-core.h>
#include "protocols/xdg-shell-server-protocol.h"
#include "wayland_server.h"
#include "xdg_shell.h"
#include "xdg_configure.h"
#include "compositor/xdg_decoration.h"
#include "graphics/direct_scene.h"
#include "graphics/graphics_broker.h"
#include <vector>
#include <algorithm>
#include <cstring>
#include <string>
#include <cstdio>

#undef LOG_TAG
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_Xdg"
#include <hilog/log.h>

namespace {

// 装配注入 (重构第 6A 步): toplevel 状态查询/ID 取号/几何快照经注入的
// ToplevelManager 直调 — 替代 WaylandServer::IsToplevel*/GetToplevelW/H/
// NextToplevelId 一行转发 (6A 删除)。装配在 RegisterXdgShell
// (WaylandServer::Start, wl 事件循环启动前), 之后协议回调只在 Wayland
// 线程读 → 无新锁 (与 4C1 warpSink 装配同模式)。保留的 xdg 状态转换
// 方法 (SetToplevel*/PostToplevelEvent 等) 仍经 WaylandServer 门面 —
// 它们是会话状态/事件职责, 非转发 (见 wayland_server.h)。
ToplevelManager* gTmgr = nullptr;

// -- xdg_toplevel 实现 (最小: 记录 title, 其余空) --
static void tl_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void tl_resource_destroy(wl_resource* r) {
    // 使用 ToplevelData 独立的 toplevelId，不依赖 XdgSurface
    // 避免 wl_client_destroy 时 xs_resource_destroy 先释放 XdgSurface 导致野指针
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(r));
    if (td && td->toplevelId) {
        WaylandServer::GetInstance()->UnregisterToplevelResource(td->toplevelId);
        ForgetToplevelDecoration(td->toplevelId);
    }
    delete td;
}
static void tl_set_parent(wl_client*, wl_resource*, wl_resource*) {}
static void tl_set_title(wl_client*, wl_resource* tlRes, const char* title) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    OH_LOG_INFO(LOG_APP, "[XDG] title tl=%{public}u %{public}s",
                sd->toplevelId, title ? title : "(null)");
    sd->title = title ? title : "";
    WaylandServer::GetInstance()->PostToplevelEvent(
        sd->toplevelId, ToplevelEventType::Title,
        ToplevelEventBus::JsonTitle(sd->title));
}
static void tl_set_app_id(wl_client*, wl_resource* tlRes, const char* appId) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    OH_LOG_INFO(LOG_APP, "[XDG] app_id tl=%{public}u %{public}s",
                sd->toplevelId, appId ? appId : "(null)");
    sd->appId = appId ? appId : "";
}
static void tl_show_window_menu(wl_client*, wl_resource*, wl_resource*, uint32_t, int32_t, int32_t) {}
static void tl_move(wl_client*, wl_resource* tlRes, wl_resource* /*seat*/, uint32_t serial) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || td->toplevelId == 0) return;
    WaylandServer::GetInstance()->StartMoveGrab(td->toplevelId, serial);
}
static void tl_resize(wl_client*, wl_resource*, wl_resource*, uint32_t, uint32_t) {}
static void fire_limits_event(SurfaceData* sd) {
    if (!sd || sd->toplevelId == 0) return;
    std::string json =
        ToplevelEventBus::JsonLimits(sd->minWidth, sd->minHeight, sd->maxWidth, sd->maxHeight);
    OH_LOG_INFO(LOG_APP, "[XDG] fire_limits tl=%{public}u %{public}s maxState=%{public}s",
                sd->toplevelId, json.c_str(),
                gTmgr->IsToplevelMaximized(sd->toplevelId) ? "yes" : "no");
    WaylandServer::GetInstance()->PostToplevelEvent(sd->toplevelId, ToplevelEventType::Limits,
                                                    json);
}

static void tl_set_min_size(wl_client*, wl_resource* tlRes, int32_t w, int32_t h) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    sd->minWidth = w;
    sd->minHeight = h;
    sd->hasSizeLimits = true;
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_min_size toplevel=%{public}u %{public}dx%{public}d",
                sd->toplevelId, w, h);
    fire_limits_event(sd);
}

static void tl_set_max_size(wl_client* client, wl_resource* tlRes, int32_t w, int32_t h) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    sd->maxWidth = w;
    sd->maxHeight = h;
    sd->hasSizeLimits = true;

    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_max_size toplevel=%{public}u %{public}dx%{public}d",
                sd->toplevelId, w, h);
    fire_limits_event(sd);
}
static void tl_set_maximized(wl_client* client, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    auto* ws = WaylandServer::GetInstance();
    // 全屏窗口不接受最大化: MAXIMIZED configure 会与 FULLSCREEN 打架
    if (gTmgr->IsToplevelFullscreen(sd->toplevelId)) {
        OH_LOG_INFO(LOG_APP, "[XDG] tl_set_maximized tl=%{public}u ignored (fullscreen)", sd->toplevelId);
        return;
    }
    if (gTmgr->IsToplevelMinimized(sd->toplevelId)) {
        ws->SetToplevelRestored(sd->toplevelId);
    }
    if (!gTmgr->IsToplevelMaximized(sd->toplevelId)) {  // 权威在 ToplevelState (重构第 5C 步)
        sd->preMaxW = gTmgr->GetToplevelW(sd->toplevelId);
        sd->preMaxH = gTmgr->GetToplevelH(sd->toplevelId);
        ws->SetToplevelMaximizedState(sd->toplevelId, true);
        ws->SetToplevelMaximized(sd->toplevelId);
    }
    // 发 configure 让 Wine 渲染到工作区尺寸 (排除任务栏)
    int32_t mw = ws->OutputWidth(), mh = ws->GetWorkAreaHeight();
    XdgConfigureSend(tlRes, xdg->xdgSurface, mw, mh,
                     {XDG_TOPLEVEL_STATE_MAXIMIZED, XDG_TOPLEVEL_STATE_ACTIVATED});
    ws->PostToplevelEvent(sd->toplevelId, ToplevelEventType::Maximized);
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_maximized tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, mw, mh);
}
static void tl_unset_maximized(wl_client* client, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    // 全屏窗口不接受 unmaximize: 否则会发出不带 FULLSCREEN 状态的 configure,
    // 把 Wine 的窗口状态机打出全屏
    if (gTmgr->IsToplevelFullscreen(sd->toplevelId)) {
        OH_LOG_INFO(LOG_APP, "[XDG] tl_unset_maximized tl=%{public}u ignored (fullscreen)", sd->toplevelId);
        return;
    }
    // maximized 状态位清 (权威在 ToplevelState, 重构第 5C 步): 裸状态写无
    // dirty — 还原到 preMax 尺寸后位置回归正常浮动, 合成更新由 Wine 随
    // configure 后的新帧 commit 触发 (与旧 sd->maximized = false 等价)
    WaylandServer::GetInstance()->SetToplevelMaximizedState(sd->toplevelId, false);
    // 发 configure 用最大化前尺寸, 不能用 0,0 (Wine 0,0+state → SWP_NOSIZE → 不resize)
    int32_t w = sd->preMaxW > 0 ? sd->preMaxW : 0;
    int32_t h = sd->preMaxH > 0 ? sd->preMaxH : 0;
    XdgConfigureSend(tlRes, xdg->xdgSurface, w, h, {XDG_TOPLEVEL_STATE_ACTIVATED});
    WaylandServer::GetInstance()->PostToplevelEvent(sd->toplevelId,
                                                    ToplevelEventType::Unmaximized);
    OH_LOG_INFO(LOG_APP, "[XDG] tl_unset_maximized tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, w, h);
}
static void tl_set_fullscreen(wl_client* client, wl_resource* tlRes, wl_resource*) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    auto* ws = WaylandServer::GetInstance();

    if (gTmgr->IsToplevelMinimized(sd->toplevelId)) {
        ws->SetToplevelRestored(sd->toplevelId);
    }
    if (!gTmgr->IsToplevelFullscreen(sd->toplevelId)) {
        sd->preFsW = gTmgr->GetToplevelW(sd->toplevelId);
        sd->preFsH = gTmgr->GetToplevelH(sd->toplevelId);
        // 全屏 configure 不含 MAXIMIZED, Wine 会据此清掉 WS_MAXIMIZE;
        // 合成器侧的标志位必须同步清, 否则后续 configure 会持续误带 MAXIMIZED。
        // 重构第 5C 步: maximized 权威迁入 ToplevelState (本行原清
        // SurfaceData::maximized, 现清 ToplevelState — 唯一权威, 全屏生效
        // 状态 SetToplevelFullscreen 与 maximized 不再分存两处, 失步窗口消除)
        ws->SetToplevelMaximizedState(sd->toplevelId, false);
        ws->SetToplevelFullscreen(sd->toplevelId, true);
        // 全屏置顶 (RaiseToplevel 对全屏窗口跳过任务栏 pin)。
        // 注意走默认 userInitiated=false: 显示模式切换时 Wine 会批量连带
        // 标记旧窗口 fullscreen, 此处若取号, 全屏优先级就退回请求到达
        // 顺序决定论 (旧窗口压游戏, 见 ToplevelState::fsPriority 注释)
        ws->RaiseToplevel(sd->toplevelId);
    }
    // 按 xdg-shell 协议回 configure: FULLSCREEN 状态 + 整个输出尺寸
    // (含任务栏区, 全屏应覆盖)。Wine 可保持自己的分辨率不变 (fullscreen
    // 对任意尺寸兼容, 见 winewayland wayland_surface_config_is_compatible),
    // 缩放由合成器完成
    int32_t fw = ws->OutputWidth(), fh = ws->OutputHeight();
    XdgConfigureSend(tlRes, xdg->xdgSurface, fw, fh,
                     {XDG_TOPLEVEL_STATE_FULLSCREEN, XDG_TOPLEVEL_STATE_ACTIVATED});
    ws->PostToplevelEvent(sd->toplevelId, ToplevelEventType::Fullscreen);
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_fullscreen tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, fw, fh);
}
static void tl_unset_fullscreen(wl_client* client, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;
    auto* ws = WaylandServer::GetInstance();
    if (!gTmgr->IsToplevelFullscreen(sd->toplevelId)) return;  // 非全屏: 幂等忽略

    ws->SetToplevelFullscreen(sd->toplevelId, false);
    // 恢复全屏前尺寸, 不能用 0,0 (Wine 0,0+state → SWP_NOSIZE → 不resize)
    int32_t w = sd->preFsW > 0 ? sd->preFsW : 0;
    int32_t h = sd->preFsH > 0 ? sd->preFsH : 0;
    XdgConfigureSend(tlRes, xdg->xdgSurface, w, h, {XDG_TOPLEVEL_STATE_ACTIVATED});
    ws->PostToplevelEvent(sd->toplevelId, ToplevelEventType::Unfullscreen);
    OH_LOG_INFO(LOG_APP, "[XDG] tl_unset_fullscreen tl=%{public}u → configure(%{public}d,%{public}d)",
                sd->toplevelId, w, h);
}
static void tl_set_minimized(wl_client*, wl_resource* tlRes) {
    auto* td = static_cast<ToplevelData*>(wl_resource_get_user_data(tlRes));
    if (!td || !td->xdgSurface) return;
    auto* xdg = static_cast<XdgSurface*>(wl_resource_get_user_data(td->xdgSurface));
    if (!xdg || !xdg->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(xdg->wlSurface));
    if (!sd) return;

    // 生效状态唯一权威是 ToplevelState.minimized
    // (SetToplevelMinimized 写入 — 6A 删兼容别名 NotifyToplevelMinimized 后
    // 直调语义方法, 与旧调用同实现同值, 本处不再存协议侧副本)
    // 几何参数为历史遗留 (旧读 sd->geoX/geoY, 两参数在下方签名中未命名未消费);
    // 重构第 5A2 步 geo 字段消亡后改读快照 contentRect (窗口几何即时值, 语义
    // 与旧值同源) — 值仍未被消费, 仅保持调用形态并见证字段迁移
    WaylandServer::GetInstance()->SetToplevelMinimized(sd->toplevelId);
    WaylandServer::GetInstance()->PostToplevelEvent(sd->toplevelId,
                                                    ToplevelEventType::Minimized);
    OH_LOG_INFO(LOG_APP, "[XDG] tl_set_minimized tl=%{public}u", sd->toplevelId);
}

static const struct xdg_toplevel_interface kToplevelImpl = {
    .destroy          = tl_destroy,
    .set_parent       = tl_set_parent,
    .set_title        = tl_set_title,
    .set_app_id       = tl_set_app_id,
    .show_window_menu = tl_show_window_menu,
    .move             = tl_move,
    .resize           = tl_resize,
    .set_max_size     = tl_set_max_size,
    .set_min_size     = tl_set_min_size,
    .set_maximized    = tl_set_maximized,
    .unset_maximized  = tl_unset_maximized,
    .set_fullscreen   = tl_set_fullscreen,
    .unset_fullscreen = tl_unset_fullscreen,
    .set_minimized    = tl_set_minimized,
};

// -- xdg_surface 实现 --
static void xs_destroy(wl_client*, wl_resource* r) {
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(r));
    OH_LOG_INFO(LOG_APP, "[MW-Life] xs_destroy xdg=%{public}p wlSurf=%{public}p",
                r, d ? d->wlSurface : nullptr);
    if (d && d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && sd->hasToplevel) {
            OH_LOG_INFO(LOG_APP, "[MW-Life] xs_destroy → OnToplevelDestroyed tl=%{public}u", sd->toplevelId);
            WaylandServer::GetInstance()->OnToplevelDestroyed(sd->toplevelId);
            WaylandServer::GetInstance()->PostToplevelEvent(sd->toplevelId,
                                                            ToplevelEventType::Destroyed);
        }
    }
    wl_resource_destroy(r);
}

static void xs_get_toplevel(wl_client* client, wl_resource* xsRes, uint32_t id) {
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(xsRes));

    wl_resource* tl = wl_resource_create(client, &xdg_toplevel_interface,
                                          wl_resource_get_version(xsRes), id);
    d->xdgToplevel = tl;

    // 为 toplevel 创建独立的 user_data（ToplevelData），不再共享 XdgSurface*
    // 避免 wl_client_destroy 时 xs_resource_destroy 先释放 XdgSurface 导致 use-after-free
    auto* td = new ToplevelData();
    td->xdgSurface = xsRes;
    wl_resource_set_implementation(tl, &kToplevelImpl, td, tl_resource_destroy);

    // 关联 SurfaceData: 分配 toplevelId, 发送 created 事件到 ArkTS
    if (d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && !sd->hasToplevel) {
            sd->hasToplevel = true;
            // CommittedSurface.role 即时同步 (重构第 5A2 步): 角色判定单点 =
            // RoleFor (committed_surface.h), 取代旧"hasToplevel 猜义分流"
            sd->committed.role = RoleFor(sd->hasToplevel, sd->isSubsurface);
            sd->toplevelId = gTmgr->AllocateToplevelId();
            d->toplevelId = sd->toplevelId;
            td->toplevelId = sd->toplevelId;
            WaylandServer::GetInstance()->RegisterToplevelResource(sd->toplevelId, tl);
            ohoswayland::GraphicsBroker::PublishPresentSurface(sd->clientPid, sd->protocolId);
            // PC 模式: created 延迟到首帧 commit (此时才知 wl_shm 格式,
            // ARGB 异型窗口需走子窗口路线而非 ability, 见 surface_commit)
        }
    }

    // The first configure carries the window's logical size. With 0x0 the
    // client picks its own size (gpui chose 4096x1324 logical, an 8192-wide
    // first buffer) until the bind-time resize arrives.
    auto* server = WaylandServer::GetInstance();
    const int32_t scale = std::max(1, server->OutputScale());
    XdgConfigureSend(tl, xsRes, server->OutputWidth() / scale, server->OutputHeight() / scale,
                     {XDG_TOPLEVEL_STATE_ACTIVATED});
}

// -- xdg_positioner / xdg_popup --
struct PositionerData {
    int32_t width = 0, height = 0;
    int32_t anchorX = 0, anchorY = 0, anchorW = 0, anchorH = 0;
    uint32_t anchor = XDG_POSITIONER_ANCHOR_NONE;
    uint32_t gravity = XDG_POSITIONER_GRAVITY_NONE;
    uint32_t constraint = 0;
    int32_t offsetX = 0, offsetY = 0;
};

static PositionerData* Positioner(wl_resource* r) {
    return static_cast<PositionerData*>(wl_resource_get_user_data(r));
}
static void pos_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void pos_set_size(wl_client*, wl_resource* r, int32_t w, int32_t h) {
    Positioner(r)->width = w;
    Positioner(r)->height = h;
}
static void pos_set_anchor_rect(wl_client*, wl_resource* r, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* p = Positioner(r);
    p->anchorX = x; p->anchorY = y; p->anchorW = w; p->anchorH = h;
}
static void pos_set_anchor(wl_client*, wl_resource* r, uint32_t anchor) { Positioner(r)->anchor = anchor; }
static void pos_set_gravity(wl_client*, wl_resource* r, uint32_t gravity) { Positioner(r)->gravity = gravity; }
static void pos_set_constraint(wl_client*, wl_resource* r, uint32_t c) { Positioner(r)->constraint = c; }
static void pos_set_offset(wl_client*, wl_resource* r, int32_t x, int32_t y) {
    Positioner(r)->offsetX = x;
    Positioner(r)->offsetY = y;
}
static void pos_set_reactive(wl_client*, wl_resource*) {}
static void pos_set_parent_size(wl_client*, wl_resource*, int32_t, int32_t) {}
static void pos_set_parent_configure(wl_client*, wl_resource*, uint32_t) {}

static const struct xdg_positioner_interface kPositionerImpl = {
    .destroy = pos_destroy,
    .set_size = pos_set_size,
    .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_anchor,
    .set_gravity = pos_set_gravity,
    .set_constraint_adjustment = pos_set_constraint,
    .set_offset = pos_set_offset,
    .set_reactive = pos_set_reactive,
    .set_parent_size = pos_set_parent_size,
    .set_parent_configure = pos_set_parent_configure,
};

// Places the popup's window geometry relative to the parent's per
// xdg-shell: the anchor point on the anchor rect, the popup extended from it
// in the gravity direction, then the offset. Popups that would leave the
// toplevel slide back inside it.
static void PlacePopup(const PositionerData& p, int32_t parentX, int32_t parentY,
                       int32_t boundsW, int32_t boundsH, int32_t& x, int32_t& y) {
    int32_t ax = p.anchorX + p.anchorW / 2, ay = p.anchorY + p.anchorH / 2;
    switch (p.anchor) {
    case XDG_POSITIONER_ANCHOR_TOP: ay = p.anchorY; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM: ay = p.anchorY + p.anchorH; break;
    case XDG_POSITIONER_ANCHOR_LEFT: ax = p.anchorX; break;
    case XDG_POSITIONER_ANCHOR_RIGHT: ax = p.anchorX + p.anchorW; break;
    case XDG_POSITIONER_ANCHOR_TOP_LEFT: ax = p.anchorX; ay = p.anchorY; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT: ax = p.anchorX; ay = p.anchorY + p.anchorH; break;
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT: ax = p.anchorX + p.anchorW; ay = p.anchorY; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT: ax = p.anchorX + p.anchorW; ay = p.anchorY + p.anchorH; break;
    default: break;
    }
    x = ax - p.width / 2;
    y = ay - p.height / 2;
    switch (p.gravity) {
    case XDG_POSITIONER_GRAVITY_TOP: y = ay - p.height; break;
    case XDG_POSITIONER_GRAVITY_BOTTOM: y = ay; break;
    case XDG_POSITIONER_GRAVITY_LEFT: x = ax - p.width; break;
    case XDG_POSITIONER_GRAVITY_RIGHT: x = ax; break;
    case XDG_POSITIONER_GRAVITY_TOP_LEFT: x = ax - p.width; y = ay - p.height; break;
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT: x = ax - p.width; y = ay; break;
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT: x = ax; y = ay - p.height; break;
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT: x = ax; y = ay; break;
    default: break;
    }
    x += p.offsetX;
    y += p.offsetY;
    if (boundsW > 0 && boundsH > 0) {
        const int32_t absX = std::clamp(parentX + x, 0, std::max(0, boundsW - p.width));
        const int32_t absY = std::clamp(parentY + y, 0, std::max(0, boundsH - p.height));
        x = absX - parentX;
        y = absY - parentY;
    }
}

struct PopupData {
    wl_resource* popup = nullptr;
    wl_resource* xdgSurface = nullptr;
    uint64_t key = 0;
    uint64_t parentKey = 0;
    bool grab = false;
};
std::vector<PopupData*> gPopups;

static uint64_t SurfaceKeyOf(wl_resource* xdgSurfaceRes) {
    if (!xdgSurfaceRes) return 0;
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(xdgSurfaceRes));
    if (!d || !d->wlSurface) return 0;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
    return sd ? sd->surfaceKey : 0;
}

// Computes the popup position and, for direct-GPU parents, adds it to the scene.
static void ConfigurePopup(PopupData* data, const PositionerData& pos, bool add) {
    int32_t parentX = 0, parentY = 0, boundsW = 0, boundsH = 0;
    const auto layers = ohoswayland::DirectScene::Instance().Snapshot();
    for (const auto& layer : layers) {
        if (layer.key == data->parentKey) { parentX = layer.x; parentY = layer.y; }
    }
    if (!layers.empty()) {
        boundsW = static_cast<int32_t>(layers.front().geomW);
        boundsH = static_cast<int32_t>(layers.front().geomH);
    }
    int32_t x = 0, y = 0;
    PlacePopup(pos, parentX, parentY, boundsW, boundsH, x, y);
    auto& scene = ohoswayland::DirectScene::Instance();
    if (scene.Contains(data->parentKey)) {
        if (add) {
            scene.AddPopup(data->key, data->parentKey, x, y, pos.width, pos.height);
        } else {
            scene.MovePopup(data->key, x, y, pos.width, pos.height);
        }
    }
    xdg_popup_send_configure(data->popup, x, y, pos.width, pos.height);
    wl_display* dpy = wl_client_get_display(wl_resource_get_client(data->popup));
    xdg_surface_send_configure(data->xdgSurface, wl_display_next_serial(dpy));
    OH_LOG_INFO(LOG_APP, "[XDG] popup key=%{public}llu at (%{public}d,%{public}d) %{public}dx%{public}d",
                static_cast<unsigned long long>(data->key), x, y, pos.width, pos.height);
}

static void popup_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
static void popup_grab(wl_client*, wl_resource* r, wl_resource*, uint32_t) {
    static_cast<PopupData*>(wl_resource_get_user_data(r))->grab = true;
}
static void popup_reposition(wl_client*, wl_resource* r, wl_resource* positioner, uint32_t token) {
    auto* data = static_cast<PopupData*>(wl_resource_get_user_data(r));
    xdg_popup_send_repositioned(r, token);
    ConfigurePopup(data, *Positioner(positioner), false);
}

static const struct xdg_popup_interface kPopupImpl = {
    .destroy = popup_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

static void popup_resource_destroy(wl_resource* r) {
    auto* data = static_cast<PopupData*>(wl_resource_get_user_data(r));
    gPopups.erase(std::remove(gPopups.begin(), gPopups.end(), data), gPopups.end());
    ohoswayland::DirectScene::Instance().Remove(data->key);
    delete data;
}

static void xs_get_popup(wl_client* client, wl_resource* xsRes, uint32_t id,
                         wl_resource* parentXs, wl_resource* positionerRes) {
    wl_resource* popup = wl_resource_create(client, &xdg_popup_interface,
                                            wl_resource_get_version(xsRes), id);
    auto* data = new PopupData();
    data->popup = popup;
    data->xdgSurface = xsRes;
    data->key = SurfaceKeyOf(xsRes);
    data->parentKey = SurfaceKeyOf(parentXs);
    wl_resource_set_implementation(popup, &kPopupImpl, data, popup_resource_destroy);
    gPopups.push_back(data);
    ConfigurePopup(data, *Positioner(positionerRes), true);
}

static void xs_set_window_geometry(wl_client*, wl_resource* xsRes, int32_t x, int32_t y, int32_t w, int32_t h) {
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(xsRes));
    if (!d || !d->wlSurface) return;
    auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
    if (!sd) return;
    // CommittedSurface 几何写点直写 (重构第 5A2 步): 旧字段 geoX/geoY/geoW/geoH
    // (三义字段, PLAN §2.4) 已删除, window_geometry 原值直写内容矩形 —
    // 语义 (contentRect=buffer 内内容偏移, toplevel 的"桌面屏幕位置"义另经
    // compute 派生存入 screenPos) 与取值时机与旧"写 geo 字段→commit 时读"
    // 逐点等价, 含"写后未 commit 即被读"窗口期。
    sd->committed.hasWindowGeometry = true;
    sd->committed.contentRect.x = x;
    sd->committed.contentRect.y = y;
    sd->committed.contentRect.w = w;
    sd->committed.contentRect.h = h;
    OH_LOG_INFO(LOG_APP, "[MW-GEO] window_geometry for surface -> toplevel #%{public}u: (%{public}d,%{public}d %{public}dx%{public}d)",
                sd->toplevelId, x, y, w, h);
}
static void xs_ack_configure(wl_client*, wl_resource*, uint32_t) {}

static const struct xdg_surface_interface kSurfaceImpl = {
    .destroy             = xs_destroy,
    .get_toplevel        = xs_get_toplevel,
    .get_popup           = xs_get_popup,
    .set_window_geometry = xs_set_window_geometry,
    .ack_configure       = xs_ack_configure,
};

static void xs_resource_destroy(wl_resource* r) {
    // client 断开时 libwayland-server 走此路径, 不会触发 xs_destroy。
    // 必须在此处也做 compositor 清理 (等价于 xs_destroy 的逻辑),
    // 否则已断开进程的窗口像素永久滞留。
    auto* d = static_cast<XdgSurface*>(wl_resource_get_user_data(r));
    if (d && d->wlSurface) {
        auto* sd = static_cast<SurfaceData*>(wl_resource_get_user_data(d->wlSurface));
        if (sd && sd->hasToplevel) {
            OH_LOG_INFO(LOG_APP, "[MW-Life] xs_resource_destroy → OnToplevelDestroyed tl=%{public}u (client disconnect)", sd->toplevelId);
            WaylandServer::GetInstance()->OnToplevelDestroyed(sd->toplevelId);
            WaylandServer::GetInstance()->PostToplevelEvent(sd->toplevelId,
                                                            ToplevelEventType::Destroyed);
        }
    }
    delete static_cast<XdgSurface*>(wl_resource_get_user_data(r));
}

// -- xdg_wm_base 实现 --
static void wm_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }

static void wm_create_positioner(wl_client* c, wl_resource* wmRes, uint32_t id) {
    wl_resource* p = wl_resource_create(c, &xdg_positioner_interface, wl_resource_get_version(wmRes), id);
    wl_resource_set_implementation(p, &kPositionerImpl, new PositionerData(), [](wl_resource* r) {
        delete static_cast<PositionerData*>(wl_resource_get_user_data(r));
    });
}

static void wm_get_xdg_surface(wl_client* client, wl_resource* wmRes,
                                uint32_t id, wl_resource* surfaceRes) {
    wl_resource* xs = wl_resource_create(client, &xdg_surface_interface,
                                          wl_resource_get_version(wmRes), id);
    auto* d = new XdgSurface();
    d->wlSurface = surfaceRes;
    d->xdgSurface = xs;
    wl_resource_set_implementation(xs, &kSurfaceImpl, d, xs_resource_destroy);
}

static void wm_pong(wl_client*, wl_resource*, uint32_t) {}

static const struct xdg_wm_base_interface kWmBaseImpl = {
    .destroy           = wm_destroy,
    .create_positioner = wm_create_positioner,
    .get_xdg_surface   = wm_get_xdg_surface,
    .pong              = wm_pong,
};

static void wm_bind(wl_client* client, void*, uint32_t version, uint32_t id) {
    uint32_t v = std::min(version, 3u);
    wl_resource* r = wl_resource_create(client, &xdg_wm_base_interface, v, id);
    wl_resource_set_implementation(r, &kWmBaseImpl, nullptr, nullptr);
}

} // namespace

extern "C" void RegisterXdgShell(wl_display* display) {
    // 装配注入 (重构第 6A 步): toplevel 状态查询/取号经 ToplevelManager 引用
    // 直调 (见 gTmgr 注释)。装配在 Server Start 阶段 (wl 事件循环启动前),
    // 之后协议回调只在 Wayland 线程读 → 无锁; 与 Policy getter 同源引用。
    gTmgr = &WaylandServer::GetInstance()->GetToplevelManager();
    wl_global_create(display, &xdg_wm_base_interface, 3, nullptr, wm_bind);
    OH_LOG_INFO(LOG_APP, "[XDG] wm_base global registered");
}

// Closes every popup, topmost first. Runs on the Wayland thread.
void XdgPopupDismissAll() {
    const std::vector<PopupData*> popups = gPopups;
    for (auto it = popups.rbegin(); it != popups.rend(); ++it) xdg_popup_send_popup_done((*it)->popup);
}
