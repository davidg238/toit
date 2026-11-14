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

#include "top.h"

#ifdef TOIT_ZEPHYR

#include "flash_registry.h"

namespace toit {

// An empty registry: no flash is mapped yet, so allocations_size() is zero,
// find_next() finds nothing, and every write or erase fails closed. Mapping a
// flash partition is the program-loading step of ztoit #1.

uint8* FlashRegistry::allocations_memory_ = null;

void FlashRegistry::set_up() {}
void FlashRegistry::tear_down() {}
void FlashRegistry::flush() {}
int FlashRegistry::allocations_size() { return 0; }
int FlashRegistry::erase_chunk(word offset, word size) { return 0; }
bool FlashRegistry::write_chunk(const void* chunk, word offset, word size) { return false; }
bool FlashRegistry::erase_flash_registry() { return false; }

}  // namespace toit

#endif  // TOIT_ZEPHYR
