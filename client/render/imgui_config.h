// =============================================================================
//  imgui_config.h - Configuración de ImGui para Luma
// =============================================================================
//  ImGui permite cambiar parte de su comportamiento con un archivo de
//  configuración propio (CMake se lo pasa con IMGUI_USER_CONFIG).
//
//  IM_ASSERT: ImGui revisa internamente que se use bien ("asserts"). Por
//  defecto, en Debug, un assert que falla CIERRA EL PROGRAMA... que es el
//  juego. Lo cambiamos para que solo lo anote en el log de Luma.
//  En Release los asserts quedan desactivados, como en ImGui normal.
// =============================================================================
#pragma once

#ifndef NDEBUG
void LumaImGuiAssert(const char* expression, const char* file, int line);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : LumaImGuiAssert(#_EXPR, __FILE__, __LINE__))
#else
#define IM_ASSERT(_EXPR) ((void)0)
#endif
