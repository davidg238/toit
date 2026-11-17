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

#if defined(CONFIG_WIFI)
#include <zephyr/net/net_if.h>
#include <zephyr/net/wifi_mgmt.h>
#include <zephyr/net/dhcpv4.h>
#endif

#if defined(CONFIG_NET_L2_OPENTHREAD)
#include <zephyr/net/net_if.h>
#include <zephyr/net/openthread.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#endif

// Called before the link is taken down (wifi_down, a detaching thread_down),
// so the firmware can finish what it is doing on it first: ztoit's agent
// waits for a check-in that has begun (rete #521). Weak, so a firmware
// without one links.
extern "C" __attribute__((weak)) void ztoit_link_going_down(void) {}

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
// thread_role.
//
// The node is always a sleepy end device (rx-off), so up and down change only
// the poll period, which OpenThread keeps in RAM. Flipping rx-on/off instead
// stores network info to flash on every change, and the first switch to
// sleepy forces a re-attach (OpenThread mle.cpp SetDeviceMode). "Down" polls at
// half the child timeout (the parent drops a child silent for the whole
// timeout), or disables Thread with CONFIG_TOIT_THREAD_DOWN_DETACH (ztoit
// design §7; that symbol is defined by ztoit's modules/toit/Kconfig).
#if defined(CONFIG_NET_L2_OPENTHREAD)
static const uint32_t THREAD_UP_POLL_MS = 100;  // Not tuned: measured by ztoit's soak.
static bool thread_started = false;

// Under openthread_mutex. Stores to flash only if the mode differs: once per node.
static otError thread_set_sleepy(otInstance* instance) {
  otLinkModeConfig mode = otThreadGetLinkMode(instance);
  if (!mode.mRxOnWhenIdle) return OT_ERROR_NONE;
  mode.mRxOnWhenIdle = false;
  return otThreadSetLinkMode(instance, mode);
}
#endif

PRIMITIVE(thread_up) {
#if defined(CONFIG_NET_L2_OPENTHREAD)
  if (!thread_started) {
    // openthread_init restores the stored network info (instance.cpp
    // AfterInit), so the mode is set after it and before openthread_run attaches.
    if (openthread_init() != 0) FAIL(HARDWARE_ERROR);
    otInstance* instance = openthread_get_default_instance();
    openthread_mutex_lock();
    otError error = thread_set_sleepy(instance);
    if (error == OT_ERROR_NONE) error = otLinkSetPollPeriod(instance, THREAD_UP_POLL_MS);
    openthread_mutex_unlock();
    if (error != OT_ERROR_NONE) FAIL(HARDWARE_ERROR);
    if (openthread_run() != 0) FAIL(HARDWARE_ERROR);
    // openthread_run() does not always admin-up the net_if (sockets then fail
    // with ENETDOWN); when it has, net_if_up returns -EALREADY.
    struct openthread_context* context = openthread_get_default_context();
    struct net_if* iface = context ? context->iface : net_if_get_default();
    int up = iface ? net_if_up(iface) : -ENODEV;
    if (up < 0 && up != -EALREADY) FAIL(HARDWARE_ERROR);
    thread_started = true;
    return process->null_object();
  }
  otInstance* instance = openthread_get_default_instance();
  openthread_mutex_lock();
  otError error = otLinkSetPollPeriod(instance, THREAD_UP_POLL_MS);
  if (error == OT_ERROR_NONE && otThreadGetDeviceRole(instance) == OT_DEVICE_ROLE_DISABLED) {
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

// Returns true if Thread is now disabled: always with `disable` (an attach that
// failed must not keep the radio retrying), else per CONFIG_TOIT_THREAD_DOWN_DETACH.
PRIMITIVE(thread_down) {
  ARGS(bool, disable);
#if defined(CONFIG_NET_L2_OPENTHREAD)
#if defined(CONFIG_TOIT_THREAD_DOWN_DETACH)
  disable = true;
#endif
  // Never started (thread_up failed): already down. Throwing here would hide
  // thread_up's error, since NetworkState.up disconnects after a failed connect.
  if (!thread_started) return BOOL(disable);
  if (disable) ztoit_link_going_down();
  otInstance* instance = openthread_get_default_instance();
  openthread_mutex_lock();
  otError error = disable
      ? otThreadSetEnabled(instance, false)
      : otLinkSetPollPeriod(instance, otThreadGetChildTimeout(instance) * 1000 / 2);
  openthread_mutex_unlock();
  if (error != OT_ERROR_NONE) FAIL(HARDWARE_ERROR);
  return BOOL(disable);
#else
  FAIL(UNSUPPORTED);
#endif
}

// Which link the build carries, for the system program's network provider:
// 0 none, 1 Thread, 2 WiFi.
PRIMITIVE(link_kind) {
#if defined(CONFIG_NET_L2_OPENTHREAD)
  return Smi::from(1);
#elif defined(CONFIG_WIFI)
  return Smi::from(2);
#else
  return Smi::from(0);
#endif
}

// WiFi station (rete's radio_wifi_connect is the model): up starts the
// association with the build's credentials (CONFIG_TOIT_WIFI_SSID/_PSK, from a
// git-ignored local conf), starts DHCPv4 and returns. Toit polls
// wifi_ready, which is true once the interface holds an IPv4 address. Down
// disconnects. A WiFi build is IPv4-only: sockets are AF_INET
// (socket_address_zephyr.h).
#if defined(CONFIG_WIFI)
// The association's outcome on the console: a refused or lost association
// otherwise reads only as Toit's connect timeout.
static void wifi_event(struct net_mgmt_event_callback* cb, uint64_t event, struct net_if* iface) {
  const struct wifi_status* status = static_cast<const struct wifi_status*>(cb->info);
  printk("wifi: %s status=%d\n",
         event == NET_EVENT_WIFI_CONNECT_RESULT ? "connect" : "disconnect",
         status ? status->status : -1);
}
static struct net_mgmt_event_callback wifi_callback;
static bool wifi_callback_added = false;

// Associated and leased. The address alone is not enough: it outlives a
// disconnect until DHCP is stopped.
static bool wifi_is_ready(struct net_if* iface) {
  struct wifi_iface_status status = {};
  if (net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &status, sizeof(status)) != 0) return false;
  return status.state == WIFI_STATE_COMPLETED &&
         net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED) != null;
}
#endif

PRIMITIVE(wifi_up) {
#if defined(CONFIG_WIFI)
  struct net_if* iface = net_if_get_first_wifi();
  if (iface == null) FAIL(HARDWARE_ERROR);
  if (!wifi_callback_added) {
    net_mgmt_init_event_callback(&wifi_callback, wifi_event,
                                 NET_EVENT_WIFI_CONNECT_RESULT | NET_EVENT_WIFI_DISCONNECT_RESULT);
    net_mgmt_add_event_callback(&wifi_callback);
    wifi_callback_added = true;
  }
  if (wifi_is_ready(iface)) return process->null_object();
  struct wifi_connect_req_params params;
  memset(&params, 0, sizeof(params));
  params.ssid = reinterpret_cast<const uint8_t*>(CONFIG_TOIT_WIFI_SSID);
  params.ssid_length = strlen(CONFIG_TOIT_WIFI_SSID);
  params.psk = reinterpret_cast<const uint8_t*>(CONFIG_TOIT_WIFI_PSK);
  params.psk_length = strlen(CONFIG_TOIT_WIFI_PSK);
  params.security = WIFI_SECURITY_TYPE_PSK;
  params.channel = WIFI_CHANNEL_ANY;
  params.band = WIFI_FREQ_BAND_2_4_GHZ;
  params.mfp = WIFI_MFP_OPTIONAL;
  int result = net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &params, sizeof(params));
  // -EALREADY: an association is already in progress.
  if (result != 0 && result != -EALREADY) FAIL(HARDWARE_ERROR);
  net_dhcpv4_start(iface);  // As rete's wifi_bringup: the lease is not automatic.
  return process->null_object();
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(wifi_ready) {
#if defined(CONFIG_WIFI)
  struct net_if* iface = net_if_get_first_wifi();
  if (iface == null) return BOOL(false);
  return BOOL(net_if_is_up(iface) && wifi_is_ready(iface));
#else
  FAIL(UNSUPPORTED);
#endif
}

PRIMITIVE(wifi_down) {
#if defined(CONFIG_WIFI)
  struct net_if* iface = net_if_get_first_wifi();
  if (iface == null) return process->null_object();
  ztoit_link_going_down();
  net_dhcpv4_stop(iface);  // Drops the lease, so a stale address cannot read as up.
  (void)net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, null, 0);
  return process->null_object();
#else
  FAIL(UNSUPPORTED);
#endif
}

}  // namespace toit

#endif  // TOIT_ZEPHYR
