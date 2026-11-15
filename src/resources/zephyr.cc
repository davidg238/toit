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

// Zephyr devices by devicetree node label, generic over Zephyr's device
// classes: any sensor driver works through sensor_*, any LED strip through
// strip_update, so no driver needs primitives of its own.
//
// A device is an index into Zephyr's static device array. `gpio-leds` and
// `gpio-keys` children are not devices, so they are looked up in tables built
// from the devicetree at compile time. Key changes arrive from Zephyr's input
// subsystem (its thread) and are dispatched through ZephyrKeyEventSource.

#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/led.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/input/input.h>

#include "../objects.h"
#include "../objects_inline.h"
#include "../primitive.h"
#include "../process.h"
#include "../resource.h"
#include "../vm.h"

#include "../event_sources/zephyr_keys.h"

#if defined(CONFIG_NET_L2_OPENTHREAD)
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#endif

namespace toit {

// A devicetree node's labels, as `static const char* const name[]`, null-terminated.
#define ZEPHYR_LABEL_STRING(label) STRINGIFY(label),
#define ZEPHYR_LABELS(prefix, node) \
  static const char* const _CONCAT(prefix, DT_DEP_ORD(node))[] = { \
    DT_FOREACH_NODELABEL(node, ZEPHYR_LABEL_STRING) null };

static bool has_label(const char* const* labels, const char* label) {
  for (; *labels != null; labels++) if (strcmp(*labels, label) == 0) return true;
  return false;
}

static const struct device* device_at(int index) {
  const struct device* devices;
  size_t count = z_device_get_all_static(&devices);
  if (index < 0 || static_cast<size_t>(index) >= count) return null;
  return &devices[index];
}

// --- gpio-leds children -------------------------------------------------------

struct LedEntry {
  const char* const* labels;
  const struct device* dev;
  uint32_t index;
};

#if defined(CONFIG_LED) && DT_HAS_COMPAT_STATUS_OKAY(gpio_leds)
#define LED_LABELS(child) ZEPHYR_LABELS(led_labels_, child)
#define LED_LABELS_OF(node) DT_FOREACH_CHILD(node, LED_LABELS)
DT_FOREACH_STATUS_OKAY(gpio_leds, LED_LABELS_OF)
#define LED_ENTRY(child) \
  { _CONCAT(led_labels_, DT_DEP_ORD(child)), DEVICE_DT_GET(DT_PARENT(child)), DT_NODE_CHILD_IDX(child) },
#define LED_ENTRIES_OF(node) DT_FOREACH_CHILD(node, LED_ENTRY)
static const LedEntry leds[] = { DT_FOREACH_STATUS_OKAY(gpio_leds, LED_ENTRIES_OF) };
static const int LED_COUNT = ARRAY_SIZE(leds);
#else
static const LedEntry* leds = null;
static const int LED_COUNT = 0;
#endif

// --- gpio-keys children -------------------------------------------------------

#if defined(CONFIG_INPUT) && DT_HAS_COMPAT_STATUS_OKAY(gpio_keys)
#define KEY_LABELS(child) ZEPHYR_LABELS(key_labels_, child)
#define KEY_LABELS_OF(node) DT_FOREACH_CHILD(node, KEY_LABELS)
DT_FOREACH_STATUS_OKAY(gpio_keys, KEY_LABELS_OF)
#define KEY_ENTRY(child) \
  { _CONCAT(key_labels_, DT_DEP_ORD(child)), DEVICE_DT_GET(DT_PARENT(child)), DT_PROP(child, zephyr_code), 0, 0 },
#define KEY_ENTRIES_OF(node) DT_FOREACH_CHILD(node, KEY_ENTRY)
static ZephyrKeyEventSource::Key keys[] = { DT_FOREACH_STATUS_OKAY(gpio_keys, KEY_ENTRIES_OF) };
static const int KEY_COUNT = ARRAY_SIZE(keys);
#else
static ZephyrKeyEventSource::Key* keys = null;
static const int KEY_COUNT = 0;
#endif

#if defined(CONFIG_INPUT)
// Runs on Zephyr's input thread for every input event of every device.
static void on_input(struct input_event* event, void* user_data) {
  if (event->type != INPUT_EV_KEY) return;
  for (int i = 0; i < KEY_COUNT; i++) {
    if (keys[i].dev == event->dev && keys[i].code == event->code) {
      ZephyrKeyEventSource::key_changed(i, event->value != 0);
    }
  }
}
INPUT_CALLBACK_DEFINE(NULL, on_input, NULL);
#endif

ZephyrKeyEventSource::Key* ZephyrKeyEventSource::key_at(int index) {
  return (index >= 0 && index < KEY_COUNT) ? &keys[index] : null;
}

class KeyResourceGroup : public ResourceGroup {
 public:
  TAG(KeyResourceGroup);
  KeyResourceGroup(Process* process, EventSource* event_source) : ResourceGroup(process, event_source) {}

 private:
  uint32_t on_event(Resource* resource, word data, uint32_t state) override {
    return state | 1;  // A key changed; Toit reads key_state.
  }
};

MODULE_IMPLEMENTATION(zephyr, MODULE_ZEPHYR)

PRIMITIVE(device_find) {
  ARGS(cstring, label);
#if defined(CONFIG_DEVICE_DT_METADATA)
  const struct device* dev = device_get_by_dt_nodelabel(label);
  if (dev == null) FAIL(FILE_NOT_FOUND);
  if (!device_is_ready(dev)) FAIL(HARDWARE_ERROR);
  const struct device* devices;
  z_device_get_all_static(&devices);
  return Smi::from(dev - devices);
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(sensor_fetch) {
  ARGS(int, device);
#if defined(CONFIG_SENSOR)
  const struct device* dev = device_at(device);
  if (dev == null) FAIL(INVALID_ARGUMENT);
  int ret = sensor_sample_fetch(dev);
  if (ret != 0) return Primitive::os_error(-ret, process);
  return process->null_object();
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(sensor_read) {
  ARGS(int, device, int, channel);
#if defined(CONFIG_SENSOR)
  const struct device* dev = device_at(device);
  if (dev == null) FAIL(INVALID_ARGUMENT);
  struct sensor_value value;
  int ret = sensor_channel_get(dev, static_cast<enum sensor_channel>(channel), &value);
  if (ret != 0) return Primitive::os_error(-ret, process);
  return Primitive::allocate_double(sensor_value_to_double(&value), process);
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(strip_update) {
  ARGS(int, device, Blob, rgb);
#if defined(CONFIG_LED_STRIP)
  static const int MAX_PIXELS = 64;
  const struct device* dev = device_at(device);
  if (dev == null) FAIL(INVALID_ARGUMENT);
  int count = rgb.length() / 3;
  if (rgb.length() % 3 != 0 || count > MAX_PIXELS || static_cast<size_t>(count) > led_strip_length(dev)) {
    FAIL(OUT_OF_RANGE);
  }
  struct led_rgb pixels[MAX_PIXELS] = {};
  for (int i = 0; i < count; i++) {
    pixels[i].r = rgb.address()[3 * i];
    pixels[i].g = rgb.address()[3 * i + 1];
    pixels[i].b = rgb.address()[3 * i + 2];
  }
  int ret = led_strip_update_rgb(dev, pixels, count);
  if (ret != 0) return Primitive::os_error(-ret, process);
  return process->null_object();
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(led_find) {
  ARGS(cstring, label);
  for (int i = 0; i < LED_COUNT; i++) {
    if (has_label(leds[i].labels, label)) {
      if (!device_is_ready(leds[i].dev)) FAIL(HARDWARE_ERROR);
      return Smi::from(i);
    }
  }
  FAIL(FILE_NOT_FOUND);
}

PRIMITIVE(led_set) {
  ARGS(int, led, bool, on);
#if defined(CONFIG_LED)
  if (led < 0 || led >= LED_COUNT) FAIL(INVALID_ARGUMENT);
  int ret = on ? led_on(leds[led].dev, leds[led].index) : led_off(leds[led].dev, leds[led].index);
  if (ret != 0) return Primitive::os_error(-ret, process);
  return process->null_object();
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(key_find) {
  ARGS(cstring, label);
  for (int i = 0; i < KEY_COUNT; i++) {
    if (has_label(keys[i].labels, label)) {
      if (!device_is_ready(keys[i].dev)) FAIL(HARDWARE_ERROR);
      return Smi::from(i);
    }
  }
  FAIL(FILE_NOT_FOUND);
}

PRIMITIVE(key_init) {
  if (ZephyrKeyEventSource::instance() == null) FAIL(UNSUPPORTED);  // No CONFIG_INPUT.
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (proxy == null) FAIL(ALLOCATION_FAILED);
  KeyResourceGroup* group = _new KeyResourceGroup(process, ZephyrKeyEventSource::instance());
  if (group == null) FAIL(MALLOC_FAILED);
  proxy->set_external_address(group);
  return proxy;
}

// A resource whose id is the key's index: its state gets bit 0 on every change.
PRIMITIVE(key_open) {
  ARGS(KeyResourceGroup, group, int, key);
  if (ZephyrKeyEventSource::key_at(key) == null) FAIL(INVALID_ARGUMENT);
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (proxy == null) FAIL(ALLOCATION_FAILED);
  IntResource* resource = group->register_id(key);
  if (resource == null) FAIL(MALLOC_FAILED);
  proxy->set_external_address(resource);
  return proxy;
}

// The key's press count times 2, plus 1 while it is pressed: a press shorter
// than Toit's wakeup still shows as a count change.
PRIMITIVE(key_state) {
  ARGS(int, key);
  ZephyrKeyEventSource::Key* k = ZephyrKeyEventSource::key_at(key);
  if (k == null) FAIL(INVALID_ARGUMENT);
  uint32_t presses;
  bool pressed;
  ZephyrKeyEventSource::read(key, &presses, &pressed);
  return Smi::from(static_cast<word>(presses & 0x3fffffff) * 2 + (pressed ? 1 : 0));
}

// Thread, driven by the system program's network provider (system/extensions/
// zephyr/network.toit): up on the first net.open, down after the last close.
// Attaching takes seconds, so thread_up only starts it and Toit polls
// thread_role. "Down" is a sleepy end device unless
// CONFIG_TOIT_THREAD_DOWN_DETACH disables Thread (ztoit design §7).
#if defined(CONFIG_NET_L2_OPENTHREAD)
static bool thread_started = false;

static void thread_set_rx_on(otInstance* instance, bool rx_on) {
  otLinkModeConfig mode = otThreadGetLinkMode(instance);
  mode.mRxOnWhenIdle = rx_on;
  otThreadSetLinkMode(instance, mode);
}
#endif

PRIMITIVE(thread_up) {
#if defined(CONFIG_NET_L2_OPENTHREAD)
  if (!thread_started) {
    // openthread_run() does not admin-up the net_if; sockets then fail with ENETDOWN.
    openthread_init();
    if (openthread_run() != 0) FAIL(HARDWARE_ERROR);
    struct openthread_context* context = openthread_get_default_context();
    struct net_if* iface = context ? context->iface : net_if_get_default();
    // openthread_run() may already have brought it up: -EALREADY is success.
    int up = iface ? net_if_up(iface) : -ENODEV;
    if (up < 0 && up != -EALREADY) FAIL(HARDWARE_ERROR);
    thread_started = true;
  }
  otInstance* instance = openthread_get_default_instance();
  openthread_mutex_lock();
  thread_set_rx_on(instance, true);
  otError error = OT_ERROR_NONE;
  if (otThreadGetDeviceRole(instance) == OT_DEVICE_ROLE_DISABLED) {
    error = otThreadSetEnabled(instance, true);
  }
  openthread_mutex_unlock();
  if (error != OT_ERROR_NONE) FAIL(HARDWARE_ERROR);
  return process->null_object();
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(thread_role) {
#if defined(CONFIG_NET_L2_OPENTHREAD)
  if (!thread_started) return Smi::from(OT_DEVICE_ROLE_DISABLED);
  otInstance* instance = openthread_get_default_instance();
  openthread_mutex_lock();
  otDeviceRole role = otThreadGetDeviceRole(instance);
  openthread_mutex_unlock();
  return Smi::from(role);
#else
  FAIL(UNSUPPORTED);
#endif
}

// Returns true if Thread was disabled, false if it stays attached as a sleepy
// end device polling at half the child timeout (the parent drops a child that
// is silent for the whole timeout).
PRIMITIVE(thread_down) {
#if defined(CONFIG_NET_L2_OPENTHREAD)
#if defined(CONFIG_TOIT_THREAD_DOWN_DETACH)
  const bool detach = true;
#else
  const bool detach = false;
#endif
  // Never started (thread_up failed): already down. Throwing here would hide
  // thread_up's error, since NetworkState.up disconnects after a failed connect.
  if (!thread_started) return BOOL(detach);
  otInstance* instance = openthread_get_default_instance();
  openthread_mutex_lock();
#if defined(CONFIG_TOIT_THREAD_DOWN_DETACH)
  otError error = otThreadSetEnabled(instance, false);
#else
  thread_set_rx_on(instance, false);
  otError error = otLinkSetPollPeriod(instance, otThreadGetChildTimeout(instance) * 1000 / 2);
#endif
  openthread_mutex_unlock();
  if (error != OT_ERROR_NONE) FAIL(HARDWARE_ERROR);
  return BOOL(detach);
#else
  FAIL(UNSUPPORTED);
#endif
}

}  // namespace toit

#endif  // TOIT_ZEPHYR
