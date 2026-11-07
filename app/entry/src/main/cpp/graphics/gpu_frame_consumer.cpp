#include <cstdio>
#include "graphics/gpu_frame_consumer.h"

#include "compositor/frame/geometry.h"
#include "graphics/graphics_broker.h"
#include "graphics/presenter_common.h"
#include "graphics/shader_utils.h"

#include <GLES2/gl2ext.h>
#include <hilog/log.h>
#include <native_buffer/native_buffer.h>
#include <native_image/native_image.h>
#include <native_window/external_window.h>

#include <algorithm>
#include <cmath>

#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x0000
#define LOG_TAG "WL_GPU_CONSUMER"

namespace ohoswayland {
namespace {
// Samples a sub-rectangle (offset xy, scale zw, in top-left image UVs) of the
// external texture, then applies the NativeImage transform.
const char* kCropExternalFS = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in highp vec2 vUV;
out vec4 oColor;
uniform samplerExternalOES uTex;
uniform highp mat4 uTransform;
uniform highp vec4 uCrop;
// Maps the viewport to the crop, stopping uEdge short of its sides, which
// continues the crop's edge pixels across the insets.
uniform highp vec4 uExtend;
uniform highp vec2 uEdge;
void main() {
    highp vec2 uv = clamp(uExtend.xy + vUV * uExtend.zw, uEdge, 1.0 - uEdge);
    highp vec4 coord = uTransform * vec4(uCrop.xy + uv * uCrop.zw, 0.0, 1.0);
    oColor = texture(uTex, coord.xy);
}
)";

constexpr uint64_t kAttachRetryNs = 100000000;
}

GpuFrameConsumer::~GpuFrameConsumer()
{
    Shutdown();
}

void GpuFrameConsumer::OnFrameAvailable(void* context)
{
    auto* layer = static_cast<Layer*>(context);
    if (layer) layer->frameAvailable.store(true, std::memory_order_release);
}

bool GpuFrameConsumer::Initialize()
{
    const GLuint vertex = winehua::CompileShader(GL_VERTEX_SHADER, winehua::kFullscreenQuadVS);
    const GLuint fragment = winehua::CompileShader(GL_FRAGMENT_SHADER, kCropExternalFS);
    program_ = glCreateProgram();
    glAttachShader(program_, vertex);
    glAttachShader(program_, fragment);
    glLinkProgram(program_);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &linked);
    if (linked != GL_TRUE) {
        char log[1024] = {};
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        OH_LOG_ERROR(LOG_APP, "External OES program link failed: %{public}s", log);
        Shutdown();
        return false;
    }
    transformLocation_ = glGetUniformLocation(program_, "uTransform");
    cropLocation_ = glGetUniformLocation(program_, "uCrop");
    extendLocation_ = glGetUniformLocation(program_, "uExtend");
    edgeLocation_ = glGetUniformLocation(program_, "uEdge");
    const float quad[] = {
        -1,-1, 0,1, 1,-1, 1,1, -1,1, 0,0,
        1,-1, 1,1, 1,1, 1,0, -1,1, 0,0,
    };
    glGenBuffers(1, &vertexBuffer_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    OH_LOG_INFO(LOG_APP, "GPU consumer pipeline initialized");
    return true;
}

bool GpuFrameConsumer::Attach(Layer& layer, const DirectLayer& info, int64_t framePeriodNs)
{
    if (!GraphicsBroker::Instance().HasClient(info.key)) return false;
    if (!layer.texture) {
        glGenTextures(1, &layer.texture);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, layer.texture);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    layer.frameAvailable.store(false, std::memory_order_release);
    layer.image = OH_NativeImage_Create(layer.texture, GL_TEXTURE_EXTERNAL_OES);
    if (!layer.image) {
        OH_LOG_ERROR(LOG_APP, "OH_NativeImage_Create failed");
        return false;
    }
    OH_ConsumerSurface_SetDefaultSize(layer.image, std::max<uint32_t>(info.surfaceW, 1),
                                      std::max<uint32_t>(info.surfaceH, 1));
    OH_ConsumerSurface_SetDefaultUsage(
        layer.image, NATIVEBUFFER_USAGE_HW_RENDER | NATIVEBUFFER_USAGE_HW_TEXTURE);
    OH_NativeImage_SetDropBufferMode(layer.image, true);
    OH_OnFrameAvailableListener listener = {};
    listener.context = &layer;
    listener.onFrameAvailable = &GpuFrameConsumer::OnFrameAvailable;
    layer.listenerSet = OH_NativeImage_SetOnFrameAvailableListener(layer.image, listener) == 0;
    layer.producerWindow = layer.listenerSet ? OH_NativeImage_AcquireNativeWindow(layer.image) : nullptr;
    if (!layer.producerWindow ||
        !GraphicsBroker::Instance().Attach(info.key, layer.producerWindow, framePeriodNs)) {
        OH_LOG_ERROR(LOG_APP, "GPU consumer attach failed key=%{public}llu",
                     static_cast<unsigned long long>(info.key));
        ReleaseLayer(layer);
        return false;
    }
    OH_LOG_INFO(LOG_APP, "GPU consumer attached key=%{public}llu popup=%{public}d",
                static_cast<unsigned long long>(info.key), info.popup);
    return true;
}

void GpuFrameConsumer::SyncLayers(const std::vector<DirectLayer>& scene, int64_t framePeriodNs)
{
    // Surfaces that left the scene release their images.
    for (auto it = layers_.begin(); it != layers_.end();) {
        const uint64_t key = (*it)->key;
        const bool present = std::any_of(scene.begin(), scene.end(),
                                         [key](const DirectLayer& info) { return info.key == key; });
        if (present) {
            ++it;
        } else {
            ReleaseLayer(**it);
            if ((*it)->texture) glDeleteTextures(1, &(*it)->texture);
            it = layers_.erase(it);
        }
    }
    const uint64_t now = winehua::NowNs();
    for (const DirectLayer& info : scene) {
        auto found = std::find_if(layers_.begin(), layers_.end(),
                                  [&info](const auto& layer) { return layer->key == info.key; });
        if (found == layers_.end()) {
            auto layer = std::make_unique<Layer>();
            layer->key = info.key;
            found = layers_.insert(layers_.end(), std::move(layer));
        }
        Layer& layer = **found;
        if (!layer.image && now - layer.lastAttachNs >= kAttachRetryNs) {
            layer.lastAttachNs = now;
            Attach(layer, info, framePeriodNs);
        }
    }
}

bool GpuFrameConsumer::Update(Layer& layer)
{
    if (!layer.image || !layer.frameAvailable.exchange(false, std::memory_order_acq_rel)) return false;
    const int32_t update = OH_NativeImage_UpdateSurfaceImage(layer.image);
    const int32_t matrix = update == 0 ? OH_NativeImage_GetTransformMatrixV2(layer.image, layer.transform) : -1;
    if (update != 0 || matrix != 0) {
        OH_LOG_ERROR(LOG_APP, "NativeImage update failed key=%{public}llu update=%{public}d matrix=%{public}d",
                     static_cast<unsigned long long>(layer.key), update, matrix);
        return false;
    }
    // Vulkan swapchain images are top-down; flip the sampling rows.
    for (int row = 0; row < 4; ++row) {
        const float secondRow = layer.transform[4 + row];
        layer.transform[4 + row] = -secondRow;
        layer.transform[12 + row] = secondRow + layer.transform[12 + row];
    }
    layer.hasFrame = true;
    if (++layer.frames <= 3 || layer.frames % 600 == 0) {
        OH_LOG_INFO(LOG_APP, "GPU_PRESENT key=%{public}llu frame=%{public}llu",
                    static_cast<unsigned long long>(layer.key), static_cast<unsigned long long>(layer.frames));
    }
    return true;
}

void GpuFrameConsumer::Draw(const Layer& layer, int x, int y, int width, int height, int outputHeight,
                            const float crop[4], const float extend[4], const float edge[2])
{
    // Layout uses a top-left origin; GL viewports use bottom-left.
    glViewport(x, outputHeight - (y + height), width, height);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, layer.texture);
    glUniformMatrix4fv(transformLocation_, 1, GL_FALSE, layer.transform);
    glUniform4fv(cropLocation_, 1, crop);
    glUniform4fv(extendLocation_, 1, extend);
    glUniform2fv(edgeLocation_, 1, edge);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

bool GpuFrameConsumer::Render(int outputWidth, int outputHeight, int64_t framePeriodNs, const ContentInsets& insets)
{
    const std::vector<DirectLayer> scene = DirectScene::Instance().Snapshot();
    SyncLayers(scene, framePeriodNs);
    topLatched_ = false;
    for (auto& layer : layers_) {
        if (Update(*layer) && !scene.empty() && layer->key == scene.front().key) topLatched_ = true;
    }

    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (scene.empty() || outputWidth <= 0 || outputHeight <= 0) return false;
    const DirectLayer& top = scene.front();
    auto findLayer = [this](uint64_t key) -> const Layer* {
        for (const auto& layer : layers_) {
            if (layer->key == key) return layer.get();
        }
        return nullptr;
    };
    const Layer* topLayer = findLayer(top.key);
    if (!topLayer || !topLayer->hasFrame || !top.geomW || !top.geomH || !top.surfaceW || !top.surfaceH)
        return false;
    sourceWidth_ = static_cast<int>(top.geomW);
    sourceHeight_ = static_cast<int>(top.geomH);
    FitRect fit;
    if (!ComputeInsetFitRect(outputWidth, outputHeight, sourceWidth_, sourceHeight_, insets, fit)) return false;

    glUseProgram(program_);
    glBindBuffer(GL_ARRAY_BUFFER, vertexBuffer_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, reinterpret_cast<void*>(8));

    // The toplevel shows only its window geometry (CSD shadows are cropped).
    const float topCrop[4] = {
        static_cast<float>(top.geomX) / top.surfaceW, static_cast<float>(top.geomY) / top.surfaceH,
        static_cast<float>(top.geomW) / top.surfaceW, static_cast<float>(top.geomH) / top.surfaceH,
    };
    glDisable(GL_BLEND);
    if (insets.Any()) {
        // Covers the output, so the toplevel's edge pixels continue across the
        // insets. Half a texel keeps filtering off what the crop removes, such
        // as CSD shadows.
        const float extend[4] = {
            -static_cast<float>(fit.offX) / fit.dstW, -static_cast<float>(fit.offY) / fit.dstH,
            static_cast<float>(outputWidth) / fit.dstW, static_cast<float>(outputHeight) / fit.dstH,
        };
        const float edge[2] = {
            0.5f * top.surfaceW / std::max(1.0f, static_cast<float>(top.geomW) * top.bufferW),
            0.5f * top.surfaceH / std::max(1.0f, static_cast<float>(top.geomH) * top.bufferH),
        };
        Draw(*topLayer, 0, 0, outputWidth, outputHeight, outputHeight, topCrop, extend, edge);
    } else {
        Draw(*topLayer, fit.offX, fit.offY, fit.dstW, fit.dstH, outputHeight, topCrop);
    }
    const double clientScale = top.bufferW ? static_cast<double>(top.bufferW) / top.surfaceW : 0;
    topBuffer_ = {PresentPath::GpuDirect, static_cast<int>(top.bufferW),
                  static_cast<int>(top.bufferH), clientScale, clientScale > 0 ? fit.scale / clientScale : 0};

    // Popups draw their whole surface, shadows included, over the toplevel.
    const float fullCrop[4] = {0, 0, 1, 1};
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    for (size_t i = 1; i < scene.size(); ++i) {
        const DirectLayer& popup = scene[i];
        const Layer* layer = findLayer(popup.key);
        if (!layer || !layer->hasFrame || !popup.surfaceW || !popup.surfaceH) continue;
        const int x = fit.offX + static_cast<int>(std::lround((popup.x - popup.geomX) * fit.scale));
        const int y = fit.offY + static_cast<int>(std::lround((popup.y - popup.geomY) * fit.scale));
        Draw(*layer, x, y, static_cast<int>(std::lround(popup.surfaceW * fit.scale)),
             static_cast<int>(std::lround(popup.surfaceH * fit.scale)), outputHeight, fullCrop);
    }
    glDisable(GL_BLEND);
    CaptureFrame(outputWidth, outputHeight);
    return glGetError() == GL_NO_ERROR;
}

// Diagnostic capture (`aa start ... --ps capture 1`): writes the composed
// frame, which holds only this window's pixels, as a binary PPM. It runs once
// per request and is not part of presentation.
void GpuFrameConsumer::RequestCapture(const std::string& path)
{
    std::lock_guard<std::mutex> lock(captureMutex_);
    capturePath_ = path;
}

bool GpuFrameConsumer::CapturePending()
{
    std::lock_guard<std::mutex> lock(captureMutex_);
    return !capturePath_.empty();
}

void GpuFrameConsumer::CaptureFrame(int outputWidth, int outputHeight)
{
    std::string path;
    {
        std::lock_guard<std::mutex> lock(captureMutex_);
        path.swap(capturePath_);
    }
    if (path.empty() || outputWidth <= 0 || outputHeight <= 0) return;
    std::vector<uint8_t> rgba(static_cast<size_t>(outputWidth) * outputHeight * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, outputWidth, outputHeight, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    FILE* file = fopen((path + ".tmp").c_str(), "wb");
    if (!file) {
        OH_LOG_ERROR(LOG_APP, "FRAME_CAPTURE open failed %{public}s", path.c_str());
        return;
    }
    fprintf(file, "P6\n%d %d\n255\n", outputWidth, outputHeight);
    std::vector<uint8_t> row(static_cast<size_t>(outputWidth) * 3);
    // GL rows start at the bottom of the frame.
    for (int y = outputHeight - 1; y >= 0; --y) {
        const uint8_t* source = rgba.data() + static_cast<size_t>(y) * outputWidth * 4;
        for (int x = 0; x < outputWidth; ++x) {
            row[x * 3] = source[x * 4];
            row[x * 3 + 1] = source[x * 4 + 1];
            row[x * 3 + 2] = source[x * 4 + 2];
        }
        fwrite(row.data(), 1, row.size(), file);
    }
    fclose(file);
    rename((path + ".tmp").c_str(), path.c_str());
    OH_LOG_INFO(LOG_APP, "FRAME_CAPTURE %{public}dx%{public}d %{public}s", outputWidth, outputHeight, path.c_str());
}

void GpuFrameConsumer::ReleaseLayer(Layer& layer)
{
    if (layer.image) GraphicsBroker::Instance().Detach(layer.key);
    if (layer.image && layer.listenerSet) OH_NativeImage_UnsetOnFrameAvailableListener(layer.image);
    layer.listenerSet = false;
    layer.producerWindow = nullptr;
    if (layer.image) OH_NativeImage_Destroy(&layer.image);
    layer.frameAvailable.store(false, std::memory_order_release);
    layer.hasFrame = false;
}

void GpuFrameConsumer::Shutdown()
{
    for (auto& layer : layers_) {
        ReleaseLayer(*layer);
        if (layer->texture) glDeleteTextures(1, &layer->texture);
    }
    layers_.clear();
    if (vertexBuffer_) glDeleteBuffers(1, &vertexBuffer_);
    if (program_) glDeleteProgram(program_);
    vertexBuffer_ = 0;
    program_ = 0;
}

} // namespace ohoswayland
