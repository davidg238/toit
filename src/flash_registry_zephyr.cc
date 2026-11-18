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
// flash_registry_esp32.cc with esp_partition_* replaced by Zephyr's flash API.
// Espressif flash sits behind an MMU and must be mapped (spi_flash_mmap); on
// SoCs with memory-mapped internal flash (nRF) the partition is read at the
// flash node's base address plus its offset.
#if DT_NODE_EXISTS(DT_NODELABEL(toit_partition)) && defined(CONFIG_FLASH)

#ifdef CONFIG_SOC_FAMILY_ESPRESSIF_ESP32
#include <spi_flash_mmap.h>
#define TOIT_FLASH_MMAP 1
#endif

namespace toit {

static const struct device* const flash_device = FIXED_PARTITION_DEVICE(toit_partition);
static const off_t partition_offset = FIXED_PARTITION_OFFSET(toit_partition);
static const size_t partition_size = FIXED_PARTITION_SIZE(toit_partition);

#ifdef TOIT_FLASH_MMAP
static spi_flash_mmap_handle_t allocations_handle;
#endif
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
#ifdef TOIT_FLASH_MMAP
  const void* memory = null;
  esp_err_t result = spi_flash_mmap(partition_offset, partition_size, SPI_FLASH_MMAP_DATA,
                                    &memory, &allocations_handle);
  if (result != ESP_OK || memory == null) FATAL("cannot map toit_partition (%d)", result);
  allocations_memory_ = reinterpret_cast<uint8*>(const_cast<void*>(memory));
#else
  // The partition's grandparent is the flash node (flash0), whose reg is its bus address.
  allocations_memory_ = reinterpret_cast<uint8*>(
      DT_REG_ADDR(DT_GPARENT(DT_NODELABEL(toit_partition))) + partition_offset);
#endif
}

void FlashRegistry::tear_down() {
  allocations_memory_ = null;
#ifdef TOIT_FLASH_MMAP
  spi_flash_munmap(allocations_handle);
#endif
}

// The ESP32's mapped view is read through the flash cache, and the HAL's write
// does not flush it for this mapping: esp_mmu_paddr_find_caps() answers
// ESP_ERR_NOT_FOUND for the registry's addresses under Zephyr 4.2, so
// spi_flash_check_and_flush_cache() skips them. A page read while erased and
// then written kept reading erased (32 of 32 pages, ztoit#31), which failed
// FlashAllocation::commit's validity check. cache_sync() flushes the whole
// cache; with it the same 32 pages read back written.
// nRF's NVMC writes through to the array that the bus reads directly.
#if defined(CONFIG_SOC_SERIES_ESP32)
extern "C" void cache_sync(void);  // hal_espressif, components/esp_mm/cache_esp32.c
void FlashRegistry::flush() { cache_sync(); }
#else
void FlashRegistry::flush() {}
#endif

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

#else  // No toit_partition or no flash driver (RP2350: none for its QMI in Zephyr 4.2).

// The registry is a static RAM array: programs are installed into RAM at boot
// and run from there (no XIP, nothing survives a reset). 128 KB holds the
// largest program so far (soak.toit, 64 KB).
namespace toit {

static const int RAM_REGISTRY_SIZE = 128 * KB;
alignas(FLASH_PAGE_SIZE) static uint8 ram_registry[RAM_REGISTRY_SIZE];
uint8* FlashRegistry::allocations_memory_ = null;

void FlashRegistry::set_up() {
  ASSERT(allocations_memory() == null);
  allocations_memory_ = ram_registry;
  memset(ram_registry, 0xff, RAM_REGISTRY_SIZE);
}

void FlashRegistry::tear_down() {
  allocations_memory_ = null;
}

void FlashRegistry::flush() {}

int FlashRegistry::allocations_size() {
  return RAM_REGISTRY_SIZE;
}

int FlashRegistry::erase_chunk(word offset, word size) {
  ASSERT(Utils::is_aligned(offset, FLASH_PAGE_SIZE));
  size = Utils::round_up(size, FLASH_PAGE_SIZE);
  memset(ram_registry + offset, 0xff, size);
  return size;
}

bool FlashRegistry::write_chunk(const void* chunk, word offset, word size) {
  memcpy(ram_registry + offset, chunk, size);
  return true;
}

bool FlashRegistry::erase_flash_registry() {
  memset(ram_registry, 0xff, RAM_REGISTRY_SIZE);
  return true;
}

}  // namespace toit

#endif  // toit_partition

#endif  // TOIT_ZEPHYR
