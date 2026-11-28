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

#include "vm.h"
#include "event_sources/timer.h"
#include "event_sources/zsock_zephyr.h"
#include "event_sources/zephyr_keys.h"
#include "event_sources/ble_udp_zephyr.h"

#include <zephyr/devicetree.h>
#if DT_HAS_COMPAT_STATUS_OKAY(toit_platform)
#include "event_sources/zephyr.h"
#endif

namespace toit {

// Timers, key changes (Zephyr input), socket readiness when the build has
// a network stack, and GPIO edges when the board has a toit,platform node
// (resources/gpio_zephyr.cc, ztoit#62).
void VM::load_platform_event_sources() {
  event_manager()->add_event_source(_new TimerEventSource());
#if DT_HAS_COMPAT_STATUS_OKAY(toit_platform)
  event_manager()->add_event_source(_new ZephyrEventSource());
#endif
#ifdef CONFIG_INPUT
  event_manager()->add_event_source(_new ZephyrKeyEventSource());
#endif
#ifdef CONFIG_NET_SOCKETS
  event_manager()->add_event_source(_new ZsockEventSource());
#elif defined(CONFIG_BT)
  event_manager()->add_event_source(_new BleUdpEventSource());  // UDP over the BLE relay.
#endif
}

}  // namespace toit

#endif  // TOIT_ZEPHYR
