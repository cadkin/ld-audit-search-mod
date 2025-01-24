#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// a dirty fix for https://sourceware.org/bugzilla/show_bug.cgi?id=32412
// replace the dtv possibly from __minimal_malloc with memory allocated by
// malloc, so that realloc called by _dl_dtv_resize won't crash

// the fix should be compatible with all existing versions of glibc, though a
// version check should be added once a version with this bug fixed is released

// from sysdeps/generic/dl-dtv.h
struct dtv_pointer {
  void *val;
  void *to_free;
};
typedef union dtv {
  size_t counter;
  struct dtv_pointer pointer;
} dtv_t;

// TODO: implement the fix for aarch64

#ifdef __x86_64__

// from sysdeps/x86_64/nptl/tls.h
typedef struct {
  void *tcb;
  dtv_t *dtv;
} tcbhead_t;

void fix_dtv_realloc() {
  dtv_t *old_dtv;
  asm volatile("movq %%fs:%P1,%q0"
               : "=r"(old_dtv)
               : "i"(offsetof(tcbhead_t, dtv)));
  size_t dtv_size = sizeof(dtv_t) * (old_dtv[-1].counter + 2);
  dtv_t *new_dtv = (dtv_t *)malloc(dtv_size);
  if (new_dtv) {
    memcpy(new_dtv, &old_dtv[-1], dtv_size);
    asm volatile("movq %q0,%%fs:%P1"
                 :
                 : "er"((uintptr_t)&new_dtv[1]), "i"(offsetof(tcbhead_t, dtv)));
  }
}

#else

void fix_dtv_realloc() {}

#endif
