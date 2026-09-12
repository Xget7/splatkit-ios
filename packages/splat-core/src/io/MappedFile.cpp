#include "splat/io/MappedFile.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace splat {

Result<MappedFile> MappedFile::open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return Error{ErrorCode::unreadable, "cannot open file: " + path};
  struct stat info {};
  if (fstat(fd, &info) != 0 || info.st_size <= 0) {
    close(fd);
    return Error{ErrorCode::unreadable, "empty file: " + path};
  }
  const auto size = static_cast<std::size_t>(info.st_size);
  void* mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (mapping == MAP_FAILED) return Error{ErrorCode::unreadable, "cannot map file: " + path};
  madvise(mapping, size, MADV_SEQUENTIAL);
  return MappedFile(mapping, size);
}

MappedFile::MappedFile(MappedFile&& other) noexcept : mapping_(other.mapping_), size_(other.size_) {
  other.mapping_ = nullptr;
  other.size_ = 0;
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    release();
    mapping_ = other.mapping_;
    size_ = other.size_;
    other.mapping_ = nullptr;
    other.size_ = 0;
  }
  return *this;
}

MappedFile::~MappedFile() {
  release();
}

void MappedFile::release() {
  if (mapping_ != nullptr) munmap(mapping_, size_);
  mapping_ = nullptr;
  size_ = 0;
}

}  // namespace splat
