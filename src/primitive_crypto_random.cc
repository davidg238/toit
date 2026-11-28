// Copyright (C) 2018 Toitware ApS.
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

#if defined(TOIT_ESP32)
#include <esp_random.h>
#elif defined(TOIT_EC618)
// The hardware TRNG, through the same entry the mbedtls entropy source uses
// (os_ec618.cc, backed by rngGenRandom).
extern "C" int mbedtls_hardware_poll(void* data, unsigned char* output,
                                     size_t len, size_t* olen);
#elif defined(TOIT_ZEPHYR)
#include <zephyr/random/random.h>
#else
#include <random>
#endif

#include "objects.h"
#include "objects_inline.h"
#include "primitive.h"
#include "process.h"
#include "tags.h"


namespace toit {

MODULE_IMPLEMENTATION(crypto_random, MODULE_CRYPTO_RANDOM)

PRIMITIVE(random) {
  ARGS(int, size);

  if (size < 0) FAIL(INVALID_ARGUMENT);

  ByteArray* result = process->allocate_byte_array(size);
  if (result == null) FAIL(ALLOCATION_FAILED);

  ByteArray::Bytes bytes(result);

#if defined(TOIT_ESP32)
  // We should eventually try to use std::random_device here too.
  // https://github.com/espressif/esp-idf/issues/11398
  esp_fill_random(bytes.address(), size);
#elif defined(TOIT_EC618)
  // The hardware TRNG. std::random_device is NOT an option here: on newlib
  // it degenerates to a deterministically seeded PRNG.
  size_t filled = 0;
  if (mbedtls_hardware_poll(null, bytes.address(), size, &filled) != 0 ||
      filled != static_cast<size_t>(size)) {
    FAIL(ERROR);
  }
#elif defined(TOIT_ZEPHYR)
  // Zephyr's CSPRNG, backed by the board's entropy driver. std::random_device
  // is not an option: on Zephyr's libc it gives the same bytes every time.
  // A build without a secure source (no entropy driver, or the test CSPRNG)
  // fails here rather than hand out predictable bytes.
#if defined(CONFIG_CSPRNG_ENABLED) && !defined(CONFIG_TEST_CSPRNG_GENERATOR)
  if (sys_csrand_get(bytes.address(), size) != 0) FAIL(ERROR);
#else
  FAIL(UNSUPPORTED);
#endif
#else
  // The std::random_device is mapped to /dev/urandom on Linux/macOS and to
  // a cryptographic API on Windows.
  std::random_device device;
  std::uniform_int_distribution<> distribution(0, 255);
  auto address = bytes.address();
  for (int i = 0; i < size; i++) {
    address[i] = distribution(device);
  }
#endif

  return result;
}

}
