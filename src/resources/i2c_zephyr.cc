// Copyright (C) 2026 Toit contributors.
// Use of this source code is governed by the LGPL-2.1 license in LICENSE.

// Toit's standard i2c library, controller role, on the I2C controllers a
// board's ztoit,buses devicetree node lists (ztoit#62). Not upstream's
// i2c_zephyr.cc (bdc3e3fe), which re-routes pins through Nordic pinctrl and
// runs each bus on a worker thread: here pins are devicetree's, and a
// transfer runs on the VM thread inside device_transfer_start.
#include "../top.h"
#ifdef TOIT_ZEPHYR
#include <zephyr/devicetree.h>
#if DT_HAS_COMPAT_STATUS_OKAY(ztoit_buses) && DT_HAS_COMPAT_STATUS_OKAY(toit_platform)

#define ZTOIT_BUSES DT_COMPAT_GET_ANY_STATUS_OKAY(ztoit_buses)
// A child that names sda is an I2C controller.
#define ZTOIT_I2C_CHILD(node) DT_NODE_HAS_PROP(node, sda) ||
#if DT_FOREACH_CHILD_STATUS_OKAY(ZTOIT_BUSES, ZTOIT_I2C_CHILD) 0
#ifndef CONFIG_I2C
#error "the ztoit,buses node has an I2C child: set CONFIG_I2C"
#endif

#include "../primitive_zephyr.h"
#include "../event_sources/zephyr.h"
#include "../objects_inline.h"
#include "../primitive.h"
#include "../process.h"
#include "pins_zephyr.h"
#include <zephyr/drivers/i2c.h>

namespace toit {

struct I2cController {
  const device* dev;
  int sda, scl;
  bool open;
};

#define ZTOIT_I2C_ENTRY(node) IF_ENABLED(DT_NODE_HAS_PROP(node, sda), \
    ({DEVICE_DT_GET(DT_PHANDLE(node, controller)), DT_PROP(node, sda), DT_PROP(node, scl), false},))
static I2cController controllers[] = {
  DT_FOREACH_CHILD_STATUS_OKAY(ZTOIT_BUSES, ZTOIT_I2C_ENTRY)
};

static const uint32_t DONE = 1;

class I2cDeviceResource;

// One open controller. A transfer runs to completion in start(); its result
// and received bytes wait here for finish().
class I2cBusResource : public ZephyrResource {
 public:
  TAG(I2cBusResource);
  explicit I2cBusResource(ResourceGroup* group) : ZephyrResource(group) {}
  ~I2cBusResource() override {
    ASSERT(device_count_ == 0);
    // Unregistered from the event source already: no lock needed.
    free(buffer_);
    // Bus pins are released without reconfiguring them: the controller's
    // pinctrl keeps routing them, for the next open and for a devicetree
    // device on the same controller.
    for (int i = 0; i < claimed_; i++) zephyr_pin_unclaim(pins_[i]);
    if (controller_) controller_->open = false;
  }

  // Finds the controller the board gave these pins and claims them.
  int open(int sda, int scl) {
    I2cController* match = null;
    for (auto& c : controllers) if (c.sda == sda && c.scl == scl) match = &c;
    if (!match) return -EINVAL;
    if (match->open) return -EBUSY;
    if (!device_is_ready(match->dev)) return -ENODEV;
    pins_[0] = sda;
    pins_[1] = scl;
    for (; claimed_ < 2; claimed_++) {
      int result = zephyr_pin_claim(pins_[claimed_]);
      if (result != 0) return result;
    }
    controller_ = match;
    controller_->open = true;
    return 0;
  }

  // owner is null for a probe. tx_length == rx_length == 0 is an address-only
  // write, the probe.
  int start(I2cDeviceResource* owner, int address, uint32_t speed,
            const uint8_t* tx, int tx_length, int rx_length) {
    if (active_) return -EBUSY;
    buffer_ = static_cast<uint8_t*>(malloc(Utils::max(1, tx_length + rx_length)));
    if (!buffer_) return -ENOMEM;
    // Copied: a literal's bytes may be in flash, which DMA drivers cannot read.
    if (tx_length) memcpy(buffer_, tx, tx_length);
    tx_length_ = tx_length;
    rx_length_ = rx_length;
    owner_ = owner;
    active_ = true;
    int result = i2c_configure(controller_->dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(speed));
    if (result == 0) {
      i2c_msg messages[2] = {};
      int count = 0;
      if (tx_length || !rx_length) {
        messages[count++] = {buffer_, static_cast<uint32_t>(tx_length),
            static_cast<uint8_t>(rx_length ? I2C_MSG_WRITE : I2C_MSG_WRITE | I2C_MSG_STOP)};
      }
      if (rx_length) {
        messages[count] = {buffer_ + tx_length, static_cast<uint32_t>(rx_length),
            static_cast<uint8_t>(I2C_MSG_READ | I2C_MSG_STOP | (count ? I2C_MSG_RESTART : 0))};
        count++;
      }
      result = i2c_transfer(controller_->dev, messages, count, address);
    }
    result_ = result;
    // The library waits for the done state after start returns.
    notify(DONE);
    return 0;
  }

  bool belongs_to(I2cDeviceResource* owner) const { return active_ && owner_ == owner; }
  int rx_length() const { return rx_length_; }

  // Copies the received bytes to output and ends the operation.
  int finish(uint8_t* output) {
    ASSERT(active_);
    int result = result_;
    if (result == 0 && output && rx_length_) memcpy(output, buffer_ + tx_length_, rx_length_);
    retire();
    return result;
  }

  // Ends an operation whose finish will not come (a deadline, a closed
  // device). The transfer is over already; only its result is dropped.
  void retire() {
    Locker locker(resource_group()->event_source()->mutex());
    take_pending();
    free(buffer_);
    buffer_ = null;
    active_ = false;
    owner_ = null;
  }

  int device_count() const { return device_count_; }
  void retain_device() { device_count_++; }
  void release_device() { ASSERT(device_count_ > 0); device_count_--; }

 private:
  I2cController* controller_ = null;
  int pins_[2] = {-1, -1};
  int claimed_ = 0;
  I2cDeviceResource* owner_ = null;
  bool active_ = false;
  uint8_t* buffer_ = null;
  int tx_length_ = 0, rx_length_ = 0, result_ = 0;
  int device_count_ = 0;
};

class I2cDeviceResource : public ZephyrResource {
 public:
  TAG(I2cDeviceResource);
  I2cDeviceResource(ResourceGroup* group, I2cBusResource* bus, int address, uint32_t speed)
      : ZephyrResource(group), bus_(bus), address_(address), speed_(speed) { bus_->retain_device(); }
  ~I2cDeviceResource() override {
    if (bus_->belongs_to(this)) bus_->retire();
    bus_->release_device();
  }
  I2cBusResource* bus() const { return bus_; }
  int address() const { return address_; }
  uint32_t speed() const { return speed_; }
 private:
  I2cBusResource* bus_;
  int address_;
  uint32_t speed_;
};

class I2cResourceGroup : public ResourceGroup {
 public:
  TAG(I2cResourceGroup);
  explicit I2cResourceGroup(Process* process) : ResourceGroup(process, ZephyrEventSource::instance()) {}
  uint32_t on_event(Resource* resource, word data, uint32_t state) override {
    // Only buses notify; data is the pending flags, DONE.
    return state | static_cast<uint32_t>(data);
  }
};

MODULE_IMPLEMENTATION(i2c, MODULE_I2C)

PRIMITIVE(init) {
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (!proxy) FAIL(ALLOCATION_FAILED);
  auto group = _new I2cResourceGroup(process);
  if (!group) FAIL(MALLOC_FAILED);
  proxy->set_external_address(group);
  return proxy;
}

// The pins select the controller; pinctrl has routed them since boot, and
// sets their pulls, so pull_up is not used. Pins are numbers only: the
// library encodes a gpio.Pin as a negative number, and is refused before the
// pins are matched. A Pin configures its pin as GPIO, and closing it
// disconnects the pin, which kills the controller until reboot (ztoit#62).
PRIMITIVE(bus_create) {
  ARGS(I2cResourceGroup, group, int, sda, int, scl, bool, pull_up);
  USE(pull_up);
  if (sda < 0 || scl < 0) FAIL(INVALID_ARGUMENT);
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (!proxy) FAIL(ALLOCATION_FAILED);
  auto bus = _new I2cBusResource(group);
  if (!bus) FAIL(MALLOC_FAILED);
  group->register_resource(bus);
  AutoUnregisteringResource<I2cBusResource> cleanup(group, bus);
  int result = bus->open(sda, scl);
  if (result == -EBUSY) FAIL(ALREADY_IN_USE);
  if (result != 0) return zephyr_error(result, process);
  cleanup.set_external_address(proxy);
  return proxy;
}

PRIMITIVE(bus_close) {
  ARGS(I2cBusResource, bus);
  if (bus->device_count()) FAIL(ALREADY_IN_USE);
  bus->resource_group()->unregister_resource(bus);
  bus_proxy->clear_external_address();
  return process->null_object();
}

// The library bounds the wait with timeout_ms; the transfer itself is bounded
// by the driver's own timeout.
PRIMITIVE(bus_probe) {
  ARGS(I2cBusResource, bus, uint16, address, int, timeout_ms);
  if (address > 127 || timeout_ms <= 0) FAIL(INVALID_ARGUMENT);
  int result = bus->start(null, address, I2C_SPEED_STANDARD, null, 0, 0);
  if (result == -EBUSY) FAIL(ALREADY_IN_USE);
  if (result != 0) return zephyr_error(result, process);
  return process->null_object();
}

PRIMITIVE(bus_probe_finish) {
  ARGS(I2cBusResource, bus);
  if (!bus->belongs_to(null)) FAIL(INVALID_STATE);
  return BOOL(bus->finish(null) == 0);
}

// A transfer is over when its start returns; at most a result is dropped.
PRIMITIVE(bus_abort_controller_operation) {
  ARGS(I2cBusResource, bus);
  bus->retire();
  return process->null_object();
}

PRIMITIVE(device_create) {
  ARGS(I2cBusResource, bus, int, address_bits, uint16, address, uint32, frequency,
       uint32, timeout_us, bool, disable_ack_check);
  if (address_bits != 7) FAIL(UNIMPLEMENTED);
  if (address > 127) FAIL(INVALID_ARGUMENT);
  if (timeout_us != 0 || disable_ack_check) FAIL(UNIMPLEMENTED);
  if (frequency != 100000 && frequency != 400000) FAIL(INVALID_ARGUMENT);
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (!proxy) FAIL(ALLOCATION_FAILED);
  auto device = _new I2cDeviceResource(bus->resource_group(), bus, address,
      frequency == 100000 ? I2C_SPEED_STANDARD : I2C_SPEED_FAST);
  if (!device) FAIL(MALLOC_FAILED);
  bus->resource_group()->register_resource(device);
  proxy->set_external_address(device);
  return proxy;
}

PRIMITIVE(device_close) {
  ARGS(I2cDeviceResource, device);
  device->resource_group()->unregister_resource(device);
  device_proxy->clear_external_address();
  return process->null_object();
}

PRIMITIVE(device_transfer_start) {
  ARGS(I2cDeviceResource, device, Blob, tx, int, rx_length);
  if (rx_length < 0 || rx_length > 65535 || tx.length() > 65535) FAIL(OUT_OF_RANGE);
  if (!rx_length && !tx.length()) FAIL(INVALID_ARGUMENT);
  int result = device->bus()->start(device, device->address(), device->speed(),
      tx.address(), tx.length(), rx_length);
  if (result == -EBUSY) FAIL(ALREADY_IN_USE);
  if (result != 0) return zephyr_error(result, process);
  return process->null_object();
}

// Returns the library's CONTROLLER-RESULT-*: 0 ok, 1 NACK, 2 timeout, 3 any
// other error. The Espressif, nRF TWIM and emulated controllers all report a
// missing acknowledgement, of the address or of data, as -EIO.
PRIMITIVE(device_transfer_finish) {
  ARGS(I2cDeviceResource, device, MutableBlob, output, int, length);
  auto bus = device->bus();
  if (!bus->belongs_to(device)) FAIL(INVALID_STATE);
  if (length < bus->rx_length() || length > output.length()) FAIL(OUT_OF_BOUNDS);
  int result = bus->finish(output.address());
  return Smi::from(result == 0 ? 0 : result == -EIO ? 1 : result == -ETIMEDOUT ? 2 : 3);
}

PRIMITIVE(target_init) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_create) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_close) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_receive) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_write) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_set_write_pending) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_set_handler_mode) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_take_request_count) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(target_dropped_receive_count) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(register_target_create) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(register_target_close) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(register_target_get) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(register_target_set) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(register_target_read) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(register_target_write) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(register_target_dropped_write_count) { FAIL(UNIMPLEMENTED); }

}  // namespace toit

#endif  // An I2C child.
#endif  // ztoit,buses and toit,platform.
#endif  // TOIT_ZEPHYR
