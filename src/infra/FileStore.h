#ifndef TG_INFRA_FILESTORE_H_
#define TG_INFRA_FILESTORE_H_

// Small file helpers used by the media pipeline (v5 §2.2 step [4]: read the
// downloaded thumbnail once into a std::string, never re-read).

#include <optional>
#include <string>

namespace tg::infra {

// Reads the whole file into a string; nullopt on open/read failure or when
// the file exceeds max_bytes (guard against pathological "thumbnails").
std::optional<std::string> ReadFileBytes(const std::string& path,
                                         std::size_t max_bytes);

bool FileExists(const std::string& path);

}  // namespace tg::infra

#endif  // TG_INFRA_FILESTORE_H_
