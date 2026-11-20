#pragma once

namespace Loader {
struct Program;
// Demon's Souls 01.005.000's resource loader waits out a load request that another thread is withdrawing instead of
// handling it with no request (demonsSoulsLoaderGuard.cpp).
namespace DemonsSoulsLoaderGuard {
void Install(Program* program);
} // namespace DemonsSoulsLoaderGuard
} // namespace Loader
