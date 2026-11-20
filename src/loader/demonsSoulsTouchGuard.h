#pragma once

namespace Loader {
struct Program;
// Demon's Souls 01.005.000's TouchManager skips a node that is in no swept list instead of unlinking it, as
// 01.007.000 does (demonsSoulsTouchGuard.cpp).
namespace DemonsSoulsTouchGuard {
void Install(Program* program);
} // namespace DemonsSoulsTouchGuard
} // namespace Loader
