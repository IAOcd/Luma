// =============================================================================
//  postfx.cpp - Estado del post-procesado (ver postfx.h)
// =============================================================================
#include "render/postfx.h"

#include <atomic>

namespace luma::render::postfx {

namespace {
std::atomic<bool> g_active{false};
gpu::PostParams g_params;
}  // namespace

void SetActive(bool active) {
    g_active.store(active);
}

bool Active() {
    return g_active.load(std::memory_order_relaxed);
}

void Set(const gpu::PostParams& params) {
    g_params = params;
}

const gpu::PostParams& Get() {
    return g_params;
}

}  // namespace luma::render::postfx
