// Copyright (C) 2026 Toit contributors.
// Use of this source code is governed by the LGPL-2.1 license in LICENSE.
#include "top.h"
#ifdef TOIT_ZEPHYR
#include "objects_inline.h"
#include "primitive.h"
#include "primitive_zephyr.h"
#include "process.h"
#include <errno.h>

// Only zephyr_error from upstream's primitive_zephyr.cc (bdc3e3fe). The rest
// of that file (reset cause, sleep, the zephyr module) is nRF5340-only.
namespace toit {
Object* zephyr_error(int error, Process* process) {
  ASSERT(error < 0);
  switch (error) {
    case -EINVAL: FAIL(INVALID_ARGUMENT);
    case -EBUSY: FAIL(ALREADY_IN_USE);
    case -EPERM: FAIL(PERMISSION_DENIED);
    case -ENOMEM: FAIL(MALLOC_FAILED);
    case -EMSGSIZE: FAIL(OUT_OF_RANGE);
    default: return Primitive::os_error(-error, process);
  }
}
}  // namespace toit
#endif
