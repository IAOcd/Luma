// =============================================================================
//  nanosvg_impl.cpp - El código de nanosvg (la biblioteca que dibuja los SVG)
// =============================================================================
//  nanosvg viene "todo en el .h": el archivo que define estas dos palabras
//  ANTES de incluirlo es donde se compila su código, una sola vez en todo el
//  programa. Va en su propio archivo para que los avisos del compilador sobre
//  código ajeno no se mezclen con los nuestros (ver CMakeLists).
// =============================================================================
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#pragma warning(push, 0)
#include <nanosvg.h>
#include <nanosvgrast.h>
#pragma warning(pop)
