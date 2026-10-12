#include "support/AllocationTracker.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace splat::allocations {
namespace {

std::atomic<std::uint64_t> gLive{0};
std::atomic<std::uint64_t> gPeak{0};

// Each block keeps the size asked for just before the bytes handed out: what malloc rounds
// a block up to is address space the caller never writes, so it is not counted.
constexpr std::size_t kMinHeaderBytes = alignof(std::max_align_t);

std::size_t headerBytes(std::size_t alignment) {
  return std::max(alignment, kMinHeaderBytes);
}

void* allocate(std::size_t size, std::size_t alignment) {
  const std::size_t header = headerBytes(alignment);
  void* block = nullptr;
  if (posix_memalign(&block, header, header + size) != 0) return nullptr;
  auto* user = static_cast<std::byte*>(block) + header;
  std::memcpy(user - sizeof(size), &size, sizeof(size));
  const std::uint64_t live = gLive.fetch_add(size) + size;
  std::uint64_t peak = gPeak.load();
  while (live > peak && !gPeak.compare_exchange_weak(peak, live)) {
  }
  return user;
}

void release(void* pointer, std::size_t alignment) {
  if (pointer == nullptr) return;
  auto* user = static_cast<std::byte*>(pointer);
  std::size_t size = 0;
  std::memcpy(&size, user - sizeof(size), sizeof(size));
  gLive.fetch_sub(size);
  // What operator new is built on cannot be RAII over operator new.
  std::free(user - headerBytes(alignment));  // NOLINT(cppcoreguidelines-no-malloc)
}

void* orThrow(void* pointer) {
  if (pointer == nullptr) throw std::bad_alloc();
  return pointer;
}

std::size_t unaligned() {
  return kMinHeaderBytes;
}

std::size_t aligned(std::align_val_t alignment) {
  return static_cast<std::size_t>(alignment);
}

}  // namespace

std::uint64_t liveBytes() {
  return gLive.load();
}

std::uint64_t peakBytes() {
  return gPeak.load();
}

void resetPeak() {
  gPeak.store(gLive.load());
}

}  // namespace splat::allocations

using splat::allocations::aligned;
using splat::allocations::allocate;
using splat::allocations::orThrow;
using splat::allocations::release;
using splat::allocations::unaligned;

void* operator new(std::size_t size) {
  return orThrow(allocate(size, unaligned()));
}
void* operator new[](std::size_t size) {
  return orThrow(allocate(size, unaligned()));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  return allocate(size, unaligned());
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
  return allocate(size, unaligned());
}
void* operator new(std::size_t size, std::align_val_t alignment) {
  return orThrow(allocate(size, aligned(alignment)));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return orThrow(allocate(size, aligned(alignment)));
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return allocate(size, aligned(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  return allocate(size, aligned(alignment));
}
void operator delete(void* pointer) noexcept {
  release(pointer, unaligned());
}
void operator delete[](void* pointer) noexcept {
  release(pointer, unaligned());
}
void operator delete(void* pointer, std::size_t) noexcept {
  release(pointer, unaligned());
}
void operator delete[](void* pointer, std::size_t) noexcept {
  release(pointer, unaligned());
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  release(pointer, unaligned());
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  release(pointer, unaligned());
}
void operator delete(void* pointer, std::align_val_t alignment) noexcept {
  release(pointer, aligned(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
  release(pointer, aligned(alignment));
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
  release(pointer, aligned(alignment));
}
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
  release(pointer, aligned(alignment));
}
void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  release(pointer, aligned(alignment));
}
void operator delete[](void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  release(pointer, aligned(alignment));
}
