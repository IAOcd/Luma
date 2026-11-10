// =============================================================================
//  post_processing.cpp - Implementación del post-procesado
// =============================================================================
#include "modules/visual/post_processing.h"

#include <imgui.h>

#include <algorithm>

#include "render/postfx.h"
#include "render/ui.h"

namespace luma::modules::visual {

namespace ui = render::ui;
using render::gpu::PostParams;

namespace {

// Estilos listos. Son solo puntos de partida: después se ajusta cada valor.
struct Preset {
    const char* name;
    PostParams params;
};
PostParams Make(float exposure, float brightness, float contrast, float saturation, float vibrance, float gamma,
                float temperature, float tint, float vignette, float sharpen) {
    PostParams p;
    p.exposure = exposure;
    p.brightness = brightness;
    p.contrast = contrast;
    p.saturation = saturation;
    p.vibrance = vibrance;
    p.gamma = gamma;
    p.temperature = temperature;
    p.tint = tint;
    p.vignette = vignette;
    p.sharpen = sharpen;
    return p;
}
const Preset kPresets[] = {
    {"Neutral", Make(0, 0, 1.0f, 1.0f, 0, 1.0f, 0, 0, 0, 0)},
    {"Vívido", Make(0, 0.02f, 1.12f, 1.25f, 0.35f, 1.0f, 0.05f, 0, 0.15f, 0.25f)},
    {"Cálido", Make(0, 0.03f, 1.05f, 1.08f, 0.1f, 1.05f, 0.6f, 0.05f, 0.2f, 0)},
    {"Frío", Make(0, 0, 1.08f, 0.95f, 0.1f, 1.0f, -0.6f, 0, 0.15f, 0)},
    {"Cine", Make(-0.1f, -0.02f, 1.2f, 0.85f, 0, 0.95f, 0.15f, -0.05f, 0.45f, 0.15f)},
    {"Pastel", Make(0.15f, 0.08f, 0.85f, 0.8f, 0.2f, 1.15f, 0.1f, 0.05f, 0, 0)},
    {"Blanco y negro", Make(0, 0, 1.15f, 0.0f, 0, 1.0f, 0, 0, 0.3f, 0.2f)},
    {"Más claro", Make(0.35f, 0.05f, 1.0f, 1.0f, 0, 1.25f, 0, 0, 0, 0)},
};

}  // namespace

PostProcessing::PostProcessing()
    : Module("post_processing", "Post-procesado",
             "Cambia cómo se ve el juego, como un editor de fotos: brillo, contraste, saturación, temperatura, "
             "viñeta y nitidez, con estilos listos.",
             Category::Visual) {}

const char* PostProcessing::IconSvg() const {
    return "M4 6h16v12H4z M4 15l4-4 3 3 4-5 5 6 M9 9.5C9 10.33 8.33 11 7.5 11C6.67 11 6 10.33 6 9.5C6 8.67 6.67 8 7.5 8C8.33 8 9 8.67 9 9.5Z";
}

void PostProcessing::Push() const {
    render::postfx::Set(params_);
}

void PostProcessing::OnEnable() {
    Push();
    render::postfx::SetActive(true);
}

void PostProcessing::OnDisable() {
    render::postfx::SetActive(false);
}

bool PostProcessing::DrawSettings() {
    bool changed = false;
    ui::Section("Estilo");
    int preset = preset_;
    if (ui::Choice("Empezar desde", &preset,
                   {"Neutral", "Vívido", "Cálido", "Frío", "Cine", "Pastel", "Blanco y negro", "Más claro"})) {
        preset_ = preset;
        params_ = kPresets[preset].params;
        changed = true;
    }
    PostParams& p = params_;
    ui::Section("Luz");
    changed |= ui::Slider("Exposición", &p.exposure, -2.0f, 2.0f, "%+.2f");
    changed |= ui::Slider("Brillo", &p.brightness, -1.0f, 1.0f, "%+.2f");
    changed |= ui::Slider("Contraste", &p.contrast, 0.0f, 2.0f, "%.2f");
    changed |= ui::Slider("Gamma", &p.gamma, 0.5f, 2.0f, "%.2f");
    ui::Section("Color");
    changed |= ui::Slider("Saturación", &p.saturation, 0.0f, 2.0f, "%.2f");
    changed |= ui::Slider("Intensidad", &p.vibrance, -1.0f, 1.0f, "%+.2f");
    changed |= ui::Slider("Temperatura", &p.temperature, -1.0f, 1.0f, "%+.2f");
    changed |= ui::Slider("Tinte", &p.tint, -1.0f, 1.0f, "%+.2f");
    ui::Section("Efectos");
    changed |= ui::Slider("Viñeta", &p.vignette, 0.0f, 1.0f, "%.2f");
    changed |= ui::Slider("Nitidez", &p.sharpen, 0.0f, 1.0f, "%.2f");
    if (ui::Button("Volver a neutral", "Restablecer")) {
        p = PostParams{};
        preset_ = 0;
        changed = true;
    }
    ui::Hint("También cambia la interfaz del juego (barra, chat). El menú y el HUD de Luma no.");
    if (changed) {
        Push();
    }
    return changed;
}

void PostProcessing::SaveSettings(nlohmann::json& out) const {
    const PostParams& p = params_;
    out["preset"] = preset_;
    out["exposure"] = p.exposure;
    out["brightness"] = p.brightness;
    out["contrast"] = p.contrast;
    out["saturation"] = p.saturation;
    out["vibrance"] = p.vibrance;
    out["gamma"] = p.gamma;
    out["temperature"] = p.temperature;
    out["tint"] = p.tint;
    out["vignette"] = p.vignette;
    out["sharpen"] = p.sharpen;
}

void PostProcessing::LoadSettings(const nlohmann::json& in) {
    const PostParams d;
    PostParams& p = params_;
    preset_ = std::clamp(in.value("preset", 0), 0, 7);
    p.exposure = std::clamp(in.value("exposure", d.exposure), -2.0f, 2.0f);
    p.brightness = std::clamp(in.value("brightness", d.brightness), -1.0f, 1.0f);
    p.contrast = std::clamp(in.value("contrast", d.contrast), 0.0f, 2.0f);
    p.saturation = std::clamp(in.value("saturation", d.saturation), 0.0f, 2.0f);
    p.vibrance = std::clamp(in.value("vibrance", d.vibrance), -1.0f, 1.0f);
    p.gamma = std::clamp(in.value("gamma", d.gamma), 0.5f, 2.0f);
    p.temperature = std::clamp(in.value("temperature", d.temperature), -1.0f, 1.0f);
    p.tint = std::clamp(in.value("tint", d.tint), -1.0f, 1.0f);
    p.vignette = std::clamp(in.value("vignette", d.vignette), 0.0f, 1.0f);
    p.sharpen = std::clamp(in.value("sharpen", d.sharpen), 0.0f, 1.0f);
    if (IsEnabled()) {
        Push();
    }
}

}  // namespace luma::modules::visual
