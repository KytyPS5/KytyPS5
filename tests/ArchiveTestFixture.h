#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <zarchive/zarchivewriter.h>

namespace ArchiveTests {

inline constexpr std::string_view Eboot = "ELF fixture";
inline constexpr std::string_view Param =
    R"({"titleId":"TEST00001","titleName":"Archive Test"})";
inline constexpr std::string_view UnicodeFilename =
    "asset-\xc3\xa9-\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e.bin";
inline const std::string LongFilename = std::string(160, 'a') + ".bin";

// Both Common::File and guest filesystem tests read the same archive layout.
inline bool CreateArchive(const std::filesystem::path &path,
                          std::span<const uint8_t> payload) {
  struct Output {
    std::filesystem::path path;
    std::ofstream stream;
    bool good = true;
  } output{.path = path};
  {
    ZArchiveWriter writer(
        [](int32_t, void *opaque) {
          auto &out = *static_cast<Output *>(opaque);
          out.stream.open(out.path, std::ios::binary | std::ios::trunc);
          out.good = out.stream.is_open();
        },
        [](const void *data, size_t size, void *opaque) {
          auto &out = *static_cast<Output *>(opaque);
          out.stream.write(static_cast<const char *>(data),
                           static_cast<std::streamsize>(size));
          out.good = out.good && out.stream.good();
        },
        &output);
    if (!output.good || !writer.MakeDir("sce_sys", true) ||
        !writer.MakeDir("assets/subdir", true)) {
      return false;
    }
    const auto add = [&](std::string_view name, const void *data, size_t size) {
      if (!writer.StartNewFile(name.data())) {
        return false;
      }
      writer.AppendData(data, size);
      return true;
    };
    if (!add("eboot.bin", Eboot.data(), Eboot.size()) ||
        !add("sce_sys/param.json", Param.data(), Param.size()) ||
        !add("assets/subdir/data.bin", payload.data(), payload.size()) ||
        !add(UnicodeFilename, Eboot.data(), Eboot.size()) ||
        !add(LongFilename, Eboot.data(), Eboot.size())) {
      return false;
    }
    writer.Finalize();
  }
  output.stream.close();
  return output.good && !output.stream.fail();
}

} // namespace ArchiveTests
