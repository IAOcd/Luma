// =============================================================================
//  keys.h - Key names
// =============================================================================
//  Windows identifies every key with a "virtual key code" (VK): a number.
//  For example 'L' = 0x4C, VK_END = 0x23. Here we turn them into a readable
//  name to show in the menu.
//
//  The configuration stores the number (not the name): that way it works the
//  same no matter the keyboard language.
// =============================================================================
#pragma once

#include <string>

namespace luma::keys {

// 0 means "no key assigned".
constexpr int kNone = 0;

// The key's name, for example "L", "F5", "Insert", "End".
std::string Name(int virtualKey);

// Can this key be assigned? (Esc can't: it's used to cancel.)
bool IsBindable(int virtualKey);

}  // namespace luma::keys
