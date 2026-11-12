// =============================================================================
//  version.h - Basic Luma data
// =============================================================================
//  A ".h" file (header) holds declarations other files can include with
//  #include. Here we keep the name and the version so we don't type them by
//  hand in several places.
// =============================================================================

// "#pragma once" stops this file from being included twice in the same
// compilation (which would cause "defined twice" errors).
#pragma once

// A "namespace" groups names under a prefix. Everything of ours lives inside
// "luma", so this is used as luma::kName and never clashes with names from
// Windows or other libraries.
namespace luma {

// "inline constexpr" = a constant known at compile time, with no runtime
// cost. The leading "k" is a convention for constants.
inline constexpr const char* kName = "Luma";
inline constexpr const char* kVersion = "0.4.0";

}  // namespace luma
