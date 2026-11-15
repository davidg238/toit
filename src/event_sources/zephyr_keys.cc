// Copyright (C) 2026 Toit contributors.
//
// This library is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; version
// 2.1 only.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.
//
// The license can be found in the file `LICENSE` in the top level
// directory of this repository.

#include "../top.h"

#if defined(TOIT_ZEPHYR)

#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>

#include "../objects_inline.h"
#include "zephyr_keys.h"

namespace toit {

ZephyrKeyEventSource* ZephyrKeyEventSource::instance_ = null;
static struct k_spinlock key_lock;
static atomic_t pending;  // Bit per key index: changed since the last dispatch.
K_SEM_DEFINE(key_sem, 0, 1);

ZephyrKeyEventSource::ZephyrKeyEventSource() : EventSource("ZephyrKeys"), Thread("ZephyrKeys") {
  ASSERT(instance_ == null);
  instance_ = this;
  if (!spawn(4096)) FATAL("key event source: no thread");
}

ZephyrKeyEventSource::~ZephyrKeyEventSource() {
  { Locker locker(mutex()); stop_ = true; }
  k_sem_give(&key_sem);
  join();
  instance_ = null;
}

void ZephyrKeyEventSource::entry() {
  while (true) {
    k_sem_take(&key_sem, K_FOREVER);
    Locker locker(mutex());
    if (stop_) return;
    uint32_t bits = static_cast<uint32_t>(atomic_clear(&pending));
    for (int index = 0; bits != 0; index++, bits >>= 1) {
      if ((bits & 1) == 0) continue;
      IntResource* resource = find_resource_by_id(locker, index);
      if (resource != null) dispatch(locker, resource, 1);
    }
  }
}

void ZephyrKeyEventSource::key_changed(int index, bool pressed) {
  Key* key = key_at(index);
  if (key == null || index >= MAX_KEYS) return;
  k_spinlock_key_t k = k_spin_lock(&key_lock);
  if (pressed && !key->pressed) key->presses++;
  key->pressed = pressed;
  k_spin_unlock(&key_lock, k);
  atomic_or(&pending, BIT(index));
  k_sem_give(&key_sem);
}

void ZephyrKeyEventSource::read(int index, uint32_t* presses, bool* pressed) {
  Key* key = key_at(index);
  k_spinlock_key_t k = k_spin_lock(&key_lock);
  *presses = key->presses;
  *pressed = key->pressed;
  k_spin_unlock(&key_lock, k);
}

}  // namespace toit

#endif  // TOIT_ZEPHYR
