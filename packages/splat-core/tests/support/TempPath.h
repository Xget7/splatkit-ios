#pragma once

#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

namespace splat {

enum class TempKind { file, directory };

// A path under the test temp dir, removed with whatever the test put there. ctest runs each
// test in a process of its own, where a fixture's address repeats, so the name is random.
class TempPath {
 public:
  explicit TempPath(const std::string& name, TempKind kind = TempKind::file)
      : path_(std::filesystem::path(testing::TempDir()) / (uniquePrefix() + name)) {
    if (kind == TempKind::directory) std::filesystem::create_directories(path_);
  }
  TempPath(const TempPath&) = delete;
  TempPath& operator=(const TempPath&) = delete;
  ~TempPath() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  const std::filesystem::path& path() const { return path_; }
  std::string string() const { return path_.string(); }

 private:
  static std::string uniquePrefix() {
    std::random_device random;
    const std::uint64_t bits = (std::uint64_t{random()} << 32) | random();
    return std::to_string(bits) + "-";
  }

  std::filesystem::path path_;
};

}  // namespace splat
