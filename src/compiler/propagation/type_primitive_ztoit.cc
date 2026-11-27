// Copyright (C) 2022 Toitware ApS.
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

#include "type_primitive.h"

namespace toit {
namespace compiler {

MODULE_TYPES(ztoit, MODULE_ZTOIT)

TYPE_PRIMITIVE_SMI(device_find)
TYPE_PRIMITIVE_NULL(sensor_fetch)
TYPE_PRIMITIVE_FLOAT(sensor_read)
TYPE_PRIMITIVE_NULL(strip_update)
TYPE_PRIMITIVE_SMI(led_find)
TYPE_PRIMITIVE_NULL(led_set)
TYPE_PRIMITIVE_SMI(key_find)
TYPE_PRIMITIVE_BYTE_ARRAY(key_init)
TYPE_PRIMITIVE_BYTE_ARRAY(key_open)
TYPE_PRIMITIVE_SMI(key_state)
TYPE_PRIMITIVE_NULL(thread_up)
TYPE_PRIMITIVE_SMI(thread_role)
TYPE_PRIMITIVE_BOOL(thread_down)
TYPE_PRIMITIVE_SMI(link_kind)
TYPE_PRIMITIVE_BOOL(ble_ready)
TYPE_PRIMITIVE_NULL(wifi_up)
TYPE_PRIMITIVE_BOOL(wifi_ready)
TYPE_PRIMITIVE_NULL(wifi_down)

TYPE_PRIMITIVE(i2c_transfer) {
  result.add_byte_array(program);
  result.add_null(program);
  failure.add_string(program);
}

TYPE_PRIMITIVE_BOOL(mic_start)

TYPE_PRIMITIVE(mic_read) {
  result.add_byte_array(program);
  result.add_null(program);
  failure.add_string(program);
}

TYPE_PRIMITIVE_NULL(mic_stop)
TYPE_PRIMITIVE_SMI(lc3_setup)
TYPE_PRIMITIVE_BYTE_ARRAY(lc3_encode)
TYPE_PRIMITIVE_NULL(uart_open)

TYPE_PRIMITIVE(uart_read) {
  result.add_byte_array(program);
  result.add_null(program);
  failure.add_string(program);
}

TYPE_PRIMITIVE_SMI(uart_close)
TYPE_PRIMITIVE_BOOL(speech_say)
TYPE_PRIMITIVE_BOOL(speech_busy)
TYPE_PRIMITIVE_NULL(observe)

}  // namespace toit::compiler
}  // namespace toit
