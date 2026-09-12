#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "splat/core/Result.h"

namespace splat {

// A whole file mapped read only, the way a decoder wants its input: no copy through the
// host's heap, and the pages leave with the object. Movable, not copyable.
class MappedFile {
 public:
  // Fails as `unreadable` for a file that cannot be opened, is empty, or cannot be
  // mapped; the message names the path.
  static Result<MappedFile> open(const std::string& path);

  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;
  ~MappedFile();

  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  const std::uint8_t* data() const { return static_cast<const std::uint8_t*>(mapping_); }
  std::size_t size() const { return size_; }

 private:
  MappedFile(void* mapping, std::size_t size) : mapping_(mapping), size_(size) {}
  void release();

  void* mapping_ = nullptr;
  std::size_t size_ = 0;
};

}  // namespace splat
