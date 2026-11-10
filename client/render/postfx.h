// =============================================================================
//  postfx.h - Estado del post-procesado (lo activa el módulo, lo dibuja el overlay)
// =============================================================================
//  El módulo "Post-procesado" (modules/visual/post_processing) guarda aquí
//  los ajustes; el overlay, en cada frame, aplica el shader con ellos
//  (render/gpu.h, RecordPost).
//
//  HILOS: Set/Get con luma::StateMutex() tomado; Active() sin candado (es la
//  pregunta del "camino rápido" del overlay).
// =============================================================================
#pragma once

#include "render/gpu.h"

namespace luma::render::postfx {

void SetActive(bool active);
bool Active();

void Set(const gpu::PostParams& params);
const gpu::PostParams& Get();

}  // namespace luma::render::postfx
