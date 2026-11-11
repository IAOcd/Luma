// =============================================================================
//  post_processing.cpp - Implementación del post-procesado
// =============================================================================
#include "modules/visual/post_processing.h"

#include "render/icons.h"

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
    {"Vivid", Make(0, 0.02f, 1.12f, 1.25f, 0.35f, 1.0f, 0.05f, 0, 0.15f, 0.25f)},
    {"Warm", Make(0, 0.03f, 1.05f, 1.08f, 0.1f, 1.05f, 0.6f, 0.05f, 0.2f, 0)},
    {"Cold", Make(0, 0, 1.08f, 0.95f, 0.1f, 1.0f, -0.6f, 0, 0.15f, 0)},
    {"Cinema", Make(-0.1f, -0.02f, 1.2f, 0.85f, 0, 0.95f, 0.15f, -0.05f, 0.45f, 0.15f)},
    {"Pastel", Make(0.15f, 0.08f, 0.85f, 0.8f, 0.2f, 1.15f, 0.1f, 0.05f, 0, 0)},
    {"Black and white", Make(0, 0, 1.15f, 0.0f, 0, 1.0f, 0, 0, 0.3f, 0.2f)},
    {"Brighter", Make(0.35f, 0.05f, 1.0f, 1.0f, 0, 1.25f, 0, 0, 0, 0)},
};

}  // namespace

PostProcessing::PostProcessing()
    : Module("post_processing", "Post-processing",
             "Cambia cómo se ve el juego, como un editor de fotos: brillo, contraste, saturación, temperatura, "
             "viñeta y nitidez, con estilos listos.",
             Category::Visual) {}

const char* PostProcessing::IconSvg() const {
    return render::icons::kWand;
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
    ui::Section("Style");
    int preset = preset_;
    if (ui::Choice("Start from", &preset,
                   {"Neutral", "Vivid", "Warm", "Cold", "Cinema", "Pastel", "Black and white", "Brighter"})) {
        preset_ = preset;
        params_ = kPresets[preset].params;
        changed = true;
    }
    PostParams& p = params_;
    ui::Section("Luz");
    changed |= ui::Slider("Exposure", &p.exposure, -2.0f, 2.0f, "%+.2f");
    changed |= ui::Slider("Brightness", &p.brightness, -1.0f, 1.0f, "%+.2f");
    changed |= ui::Slider("Contrast", &p.contrast, 0.0f, 2.0f, "%.2f");
    changed |= ui::Slider("Gamma", &p.gamma, 0.5f, 2.0f, "%.2f");
    ui::Section("Color");
    changed |= ui::Slider("Saturation", &p.saturation, 0.0f, 2.0f, "%.2f");
    changed |= ui::Slider("Intensity", &p.vibrance, -1.0f, 1.0f, "%+.2f");
    changed |= ui::Slider("Temperature", &p.temperature, -1.0f, 1.0f, "%+.2f");
    changed |= ui::Slider("Tint", &p.tint, -1.0f, 1.0f, "%+.2f");
    ui::Section("Effects");
    changed |= ui::Slider("Vignette", &p.vignette, 0.0f, 1.0f, "%.2f");
    changed |= ui::Slider("Sharpness", &p.sharpen, 0.0f, 1.0f, "%.2f");
    if (ui::Button("Back to neutral", "Reset")) {
        p = PostParams{};
        preset_ = 0;
        changed = true;
    }
    ui::Hint("It also changes the game's UI (hotbar, chat). Luma's menu and HUD don't change.");
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
