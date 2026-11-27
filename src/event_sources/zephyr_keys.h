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

#pragma once

#include "../top.h"

#if defined(TOIT_ZEPHYR)

#include <zephyr/device.h>

#include "../resource.h"

namespace toit {

// Key changes from Zephyr's input subsystem (gpio-keys). The key table and its
// input callback live in resources/ztoit.cc; a key's resource id is its index
// in that table. Key state is kept under a spinlock even before the VM starts,
// so no press is lost to start-up. Zephyr's input thread is not a Toit thread
// and must not take Toit locks: it marks the key pending and gives a semaphore,
// and this source's own thread dispatches.
class ZephyrKeyEventSource : public EventSource, public Thread {
 public:
  struct Key {
    const char* const* labels;   // Devicetree node labels, null-terminated.
    const struct device* dev;    // The gpio-keys device.
    uint16_t code;               // zephyr,code.
    uint32_t presses;
    bool pressed;
  };

  ZephyrKeyEventSource();
  ~ZephyrKeyEventSource() override;

  static ZephyrKeyEventSource* instance() { return instance_; }
  static Key* key_at(int index);  // null when out of range.
  static const int MAX_KEYS = 32;  // One pending bit each.

  // Called on Zephyr's input thread.
  static void key_changed(int index, bool pressed);
  static void read(int index, uint32_t* presses, bool* pressed);

 private:
  void entry() override;

  bool stop_ = false;
  static ZephyrKeyEventSource* instance_;
};

}  // namespace toit

#endif  // TOIT_ZEPHYR
