#ifndef KYTY_LOADER_GUEST_OODLE_H_
#define KYTY_LOADER_GUEST_OODLE_H_

#include <cstdint>
#include <span>
#include <stop_token>

// The Oodle decoder a game links in its own code (Unreal Engine 5 keeps its shader libraries
// compressed with it), for the emulator's search of the game's files: the emulator carries no
// decoder of its own.
namespace Loader {
struct Program;
namespace GuestOodle {

// Once the programs loaded at start are relocated, before the guest runs: Decompress() looks
// for the decoder in them.
void SetPrograms(std::span<Program* const> programs);

// Decompresses an Oodle stream of the modern codecs (Kraken, Mermaid, Selkie, Leviathan) into
// `dst`, with the game's decoder, on the calling thread. Waits for SetPrograms(); false when the
// game has no decoder, `stop` is requested meanwhile, or the stream does not decode to exactly
// dst.size() bytes.
bool Decompress(std::span<const uint8_t> src, std::span<uint8_t> dst, const std::stop_token& stop);

} // namespace GuestOodle
} // namespace Loader

#endif /* KYTY_LOADER_GUEST_OODLE_H_ */
