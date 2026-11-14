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

#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include "flash_registry.h"

// The registry is the devicetree partition labelled `toit_partition`, mapped
// into the data address space so programs run from flash in place. This is
// flash_registry_esp32.cc with esp_partition_* replaced by Zephyr's flash API;
// the memory map itself is Espressif-specific until another SoC needs it.
#if !DT_NODE_EXISTS(DT_NODELABEL(toit_partition))
#error "the board devicetree needs a toit_partition (see ztoit's board overlay)"
#endif

#include <spi_flash_mmap.h>

namespace toit {

static const struct device* const flash_device = FIXED_PARTITION_DEVICE(toit_partition);
static const off_t partition_offset = FIXED_PARTITION_OFFSET(toit_partition);
static const size_t partition_size = FIXED_PARTITION_SIZE(toit_partition);

static spi_flash_mmap_handle_t allocations_handle;
uint8* FlashRegistry::allocations_memory_ = null;

static bool erase_range(word offset, word size) {
  return flash_erase(flash_device, partition_offset + offset, size) == 0;
}

static bool is_erased_page(word offset) {
  ASSERT(Utils::is_aligned(offset, FLASH_PAGE_SIZE));
  return FlashRegistry::is_erased(offset, FLASH_PAGE_SIZE);
}

// Erase only the dirty pages of [offset, offset + size): erasing is slow and
// wears the flash.
static bool ensure_erased(word offset, word size) {
  FlashRegistry::flush();
  ASSERT(Utils::is_aligned(offset, FLASH_PAGE_SIZE));
  ASSERT(Utils::is_aligned(size, FLASH_PAGE_SIZE));
  word to = offset + size;
  for (word cursor = offset; cursor < to; cursor += FLASH_PAGE_SIZE) {
    if (!is_erased_page(cursor)) {
      word dirty_to = cursor + FLASH_PAGE_SIZE;
      while (dirty_to < to && !is_erased_page(dirty_to)) dirty_to += FLASH_PAGE_SIZE;
      if (!erase_range(cursor, dirty_to - cursor)) return false;
      cursor = dirty_to;  // Continues at dirty_to + FLASH_PAGE_SIZE.
    }
  }
  return true;
}

void FlashRegistry::set_up() {
  ASSERT(allocations_memory() == null);
  if (!device_is_ready(flash_device)) FATAL("flash device not ready");
  const void* memory = null;
  esp_err_t result = spi_flash_mmap(partition_offset, partition_size, SPI_FLASH_MMAP_DATA,
                                    &memory, &allocations_handle);
  if (result != ESP_OK || memory == null) FATAL("cannot map toit_partition (%d)", result);
  allocations_memory_ = reinterpret_cast<uint8*>(const_cast<void*>(memory));
}

void FlashRegistry::tear_down() {
  allocations_memory_ = null;
  spi_flash_munmap(allocations_handle);
}

// Espressif's flash write and erase invalidate the cache over the range they
// touch, so the mapped view is coherent without an explicit flush (as on ESP32).
void FlashRegistry::flush() {}

int FlashRegistry::allocations_size() {
  return static_cast<int>(partition_size);
}

int FlashRegistry::erase_chunk(word offset, word size) {
  ASSERT(Utils::is_aligned(offset, FLASH_PAGE_SIZE));
  size = Utils::round_up(size, FLASH_PAGE_SIZE);
  if (!ensure_erased(offset, size)) return 0;
  FlashRegistry::flush();
  return size;
}

bool FlashRegistry::write_chunk(const void* chunk, word offset, word size) {
  return flash_write(flash_device, partition_offset + offset, chunk, size) == 0;
}

bool FlashRegistry::erase_flash_registry() {
  return erase_range(0, partition_size);
}

}  // namespace toit

#endif  // TOIT_ZEPHYR
