// fix compatibility with a weird executable that:
// - dynamically links glibc, so this library will be loaded
// - statically links an ancient version of libc which
//   - doesn't have aligned_alloc, which breaks libc++'s aligned operator new
//   - malloc returns pointers aligned to 8 bytes, which is smaller than
//     __STDCPP_DEFAULT_NEW_ALIGNMENT__ and breaks libc++'s normal operator new

#include <errno.h>
#include <malloc.h>
#include <new>
#include <stdlib.h>

extern "C" {
int posix_memalign(void **memptr, size_t alignment, size_t size) {
  if (alignment % sizeof(void *) != 0 || alignment == 0 ||
      (alignment & (alignment - 1)) != 0) {
    return EINVAL;
  }
  if (size == 0) {
    *memptr = nullptr;
    return 0;
  }
  *memptr = memalign(alignment, size);
  if (*memptr == nullptr) {
    return ENOMEM;
  }
  return 0;
}

void *aligned_alloc(size_t alignment, size_t size) {
  if (size % alignment != 0) {
    return nullptr;
  }
  return memalign(alignment, size);
}
}

void *operator new(std::size_t size) {
  if (size == 0) {
    size = 1;
  }
  void *p = nullptr;
  while ((p = memalign(__STDCPP_DEFAULT_NEW_ALIGNMENT__, size)) == nullptr) {
    auto nh = std::get_new_handler();
    if (nh != nullptr) {
      nh();
    } else {
      throw std::bad_alloc();
    }
  }
  return p;
}

void *operator new[](std::size_t count) { return operator new(count); }

void *operator new(std::size_t count, const std::nothrow_t &tag) noexcept {
  try {
    return operator new(count);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t count, const std::nothrow_t &tag) noexcept {
  try {
    return operator new[](count);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *ptr) noexcept { free(ptr); }

void operator delete[](void *ptr) noexcept { operator delete(ptr); }

void operator delete(void *ptr, const std::nothrow_t &tag) noexcept {
  operator delete(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &tag) noexcept {
  operator delete[](ptr);
}
