#include "splat/io/MappedFile.h"

#include <cstdio>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

namespace splat {
namespace {

// A file under the test's temp dir with `content`, removed when the test ends.
class TempFile {
 public:
  explicit TempFile(const std::string& content) {
    path_ = testing::TempDir() + "/mapped-file-test-" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this));
    std::ofstream out(path_, std::ios::binary);
    out << content;
  }
  ~TempFile() { std::remove(path_.c_str()); }
  TempFile(const TempFile&) = delete;
  TempFile& operator=(const TempFile&) = delete;
  const std::string& path() const { return path_; }

 private:
  std::string path_;
};

TEST(MappedFile, MapsTheWholeFile) {
  const TempFile file("hello, splats");
  auto mapped = MappedFile::open(file.path());
  ASSERT_TRUE(mapped.ok()) << mapped.error().message;
  ASSERT_EQ(mapped.value().size(), 13u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(mapped.value().data()), 13), "hello, splats");
}

TEST(MappedFile, MissingFileIsUnreadableAndNamesThePath) {
  auto mapped = MappedFile::open("/nonexistent/world.spz");
  ASSERT_FALSE(mapped.ok());
  EXPECT_EQ(mapped.error().code, ErrorCode::unreadable);
  EXPECT_NE(mapped.error().message.find("/nonexistent/world.spz"), std::string::npos);
}

TEST(MappedFile, EmptyFileIsUnreadable) {
  const TempFile file("");
  auto mapped = MappedFile::open(file.path());
  ASSERT_FALSE(mapped.ok());
  EXPECT_EQ(mapped.error().code, ErrorCode::unreadable);
}

TEST(MappedFile, MoveHandsOverTheMapping) {
  const TempFile file("abc");
  auto opened = MappedFile::open(file.path());
  ASSERT_TRUE(opened.ok());
  MappedFile moved = std::move(opened.value());
  EXPECT_EQ(moved.size(), 3u);
  EXPECT_EQ(opened.value().size(),
            0u);  // NOLINT(bugprone-use-after-move): the hollow state is the point
  EXPECT_EQ(opened.value().data(), nullptr);
  const MappedFile assigned = std::move(moved);
  EXPECT_EQ(assigned.size(), 3u);
  EXPECT_EQ(assigned.data()[2], 'c');
}

}  // namespace
}  // namespace splat
