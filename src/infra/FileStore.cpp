#include "infra/FileStore.h"

#include <filesystem>
#include <fstream>

namespace tg::infra {

std::optional<std::string> ReadFileBytes(const std::string& path,
                                         std::size_t max_bytes) {
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return std::nullopt;
  }
  if (size > max_bytes) {
    return std::nullopt;
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }
  std::string bytes;
  bytes.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    in.read(bytes.data(), static_cast<std::streamsize>(size));
    if (!in) {
      return std::nullopt;
    }
  }
  return bytes;
}

bool FileExists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

}  // namespace tg::infra
