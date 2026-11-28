// Copyright (C) 2026 Toit contributors.
// Use of this source code is governed by the LGPL-2.1 license in LICENSE.

// Toit's standard uart library on the UARTs a board's ztoit,buses devicetree
// node lists (ztoit#62; zephyr-port.md, addendum 2026-11-28). Not upstream's
// uart_zephyr.cc, which is Nordic-only (nRF5340, the async DMA API, run-time
// pinctrl): this one is on Zephyr's interrupt-driven API, any driver.
//
// The driver's callback (an ISR, or a driver's own thread) and the VM thread
// share two rings, each with one producer and one consumer, which Zephyr's
// ring_buf allows without a lock: the callback fills the receive ring and
// drains the transmit ring.
#include "../top.h"
#ifdef TOIT_ZEPHYR
#include <zephyr/devicetree.h>
#if DT_HAS_COMPAT_STATUS_OKAY(ztoit_buses) && DT_HAS_COMPAT_STATUS_OKAY(toit_platform)

#define ZTOIT_BUSES DT_COMPAT_GET_ANY_STATUS_OKAY(ztoit_buses)
// A child that names tx or rx is a UART.
#define ZTOIT_IS_UART(node) UTIL_OR(DT_NODE_HAS_PROP(node, tx), DT_NODE_HAS_PROP(node, rx))
#define ZTOIT_UART_CHILD(node) ZTOIT_IS_UART(node) ||
#if DT_FOREACH_CHILD_STATUS_OKAY(ZTOIT_BUSES, ZTOIT_UART_CHILD) 0
#ifndef CONFIG_UART_INTERRUPT_DRIVEN
#error "the ztoit,buses node has a UART child: set CONFIG_UART_INTERRUPT_DRIVEN"
#endif

#include "../objects_inline.h"
#include "../os.h"
#include "../primitive.h"
#include "../primitive_zephyr.h"
#include "../process.h"
#include "../event_sources/zephyr.h"
#include "pins_zephyr.h"
#include <string.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>

namespace toit {

// The console's UART is the system's (print, the log): it must not be a child.
#define ZTOIT_UART_NOT_CONSOLE(node) IF_ENABLED(ZTOIT_IS_UART(node), (                 \
    static_assert(!COND_CODE_1(DT_HAS_CHOSEN(zephyr_console),                          \
        (DT_SAME_NODE(DT_PHANDLE(node, controller), DT_CHOSEN(zephyr_console))), (0)), \
        "a ztoit,buses UART child names the console UART");))
DT_FOREACH_CHILD_STATUS_OKAY(ZTOIT_BUSES, ZTOIT_UART_NOT_CONSOLE)

// The controller's node labels, for the device-path constructor.
#define ZTOIT_UART_LABEL(label) #label,
#define ZTOIT_UART_LABELS(node) IF_ENABLED(ZTOIT_IS_UART(node), (                \
    static const char* const UTIL_CAT(ztoit_uart_labels_, DT_DEP_ORD(node))[] = { \
      DT_FOREACH_NODELABEL(DT_PHANDLE(node, controller), ZTOIT_UART_LABEL) null };))
DT_FOREACH_CHILD_STATUS_OKAY(ZTOIT_BUSES, ZTOIT_UART_LABELS)

struct UartController {
  const device* dev;
  int tx, rx;  // -1 when the child does not name the pin.
  const char* const* labels;
  bool open;
};

#define ZTOIT_UART_ENTRY(node) IF_ENABLED(ZTOIT_IS_UART(node), (                  \
    {DEVICE_DT_GET(DT_PHANDLE(node, controller)), DT_PROP_OR(node, tx, -1),       \
     DT_PROP_OR(node, rx, -1), UTIL_CAT(ztoit_uart_labels_, DT_DEP_ORD(node)), false},))
static UartController controllers[] = {
  DT_FOREACH_CHILD_STATUS_OKAY(ZTOIT_BUSES, ZTOIT_UART_ENTRY)
};

// The library's sizes (lib/uart.toit): 768 B to receive, 4096 B with
// --large-buffers (tx-flags bit 16); 256 B to transmit.
static const int RX_SIZE = 768;
static const int RX_LARGE_SIZE = 4096;
static const int TX_SIZE = 256;
static const int LARGE_BUFFERS = 16;

// The library's states (lib/uart.toit).
static const uint32_t READ_STATE = 1 << 0;
static const uint32_t WRITE_STATE = 1 << 2;

class UartResource : public ZephyrResource {
 public:
  TAG(UartResource);
  explicit UartResource(ResourceGroup* group) : ZephyrResource(group) {}

  ~UartResource() override {
    // Unregistered from the event source already. The callback is gone
    // before the rings are: interrupts off first, then the callback cleared,
    // with interrupts locked so the driver's ISR cannot be half way through.
    if (started_) {
      const device* dev = controller_->dev;
      unsigned key = irq_lock();
      uart_irq_rx_disable(dev);
      uart_irq_tx_disable(dev);
      uart_irq_err_disable(dev);
      uart_irq_callback_user_data_set(dev, null, null);
      irq_unlock(key);
    }
    free(rx_buffer_);
    free(tx_buffer_);
    // Released without reconfiguring: pinctrl keeps routing the pins.
    for (int i = 0; i < claimed_; i++) zephyr_pin_unclaim(pins_[i]);
    if (controller_) {
      Locker locker(OS::global_mutex());
      controller_->open = false;
    }
  }

  // Takes the controller and the given pins (-1: not given, its direction
  // not started), and starts the port.
  int open(UartController* controller, int tx, int rx, const uart_config& config, int rx_size) {
    {
      Locker locker(OS::global_mutex());
      if (controller->open) return -EBUSY;
      controller->open = true;
    }
    controller_ = controller;
    const device* dev = controller->dev;
    if (!device_is_ready(dev)) return -ENODEV;
    for (int pin : {tx, rx}) {
      if (pin == -1) continue;
      int result = zephyr_pin_claim(pin);
      if (result != 0) return result;
      pins_[claimed_++] = pin;
    }
    if (rx != -1) {
      rx_buffer_ = static_cast<uint8_t*>(malloc(rx_size));
      if (!rx_buffer_) return -ENOMEM;
      ring_buf_init(&rx_ring_, rx_size, rx_buffer_);
    }
    if (tx != -1) {
      tx_buffer_ = static_cast<uint8_t*>(malloc(TX_SIZE));
      if (!tx_buffer_) return -ENOMEM;
      ring_buf_init(&tx_ring_, TX_SIZE, tx_buffer_);
    }
    int result = uart_configure(dev, &config);
    if (result != 0) return result;
    config_ = config;
    result = uart_irq_callback_user_data_set(dev, callback, this);
    if (result != 0) return result;
    started_ = true;
    if (rx != -1) {
      uart_irq_err_enable(dev);
      uart_irq_rx_enable(dev);
    }
    return 0;
  }

  bool receives() const { return rx_buffer_ != null; }
  bool transmits() const { return tx_buffer_ != null; }

  uint32_t baud_rate() const { return config_.baudrate; }
  int set_baud_rate(uint32_t baud_rate) {
    uart_config config = config_;
    config.baudrate = baud_rate;
    int result = uart_configure(controller_->dev, &config);
    if (result == 0) config_ = config;
    return result;
  }

  // Copies as much of data as the transmit ring takes and starts the TX-ready
  // interrupt. Returns the count taken.
  int write(const uint8_t* data, int length) {
    unsigned key = irq_lock();
    int taken = ring_buf_put(&tx_ring_, data, length);
    if (taken && !tx_on_) {
      tx_on_ = true;
      uart_irq_tx_enable(controller_->dev);
    }
    irq_unlock(key);
    return taken;
  }

  // Whether the transmit ring is empty. The driver's FIFO may still be shifting.
  bool flushed() { return !transmits() || ring_buf_is_empty(&tx_ring_); }

  int available() { return ring_buf_size_get(&rx_ring_); }
  void read(uint8_t* data, int length) { ring_buf_get(&rx_ring_, data, length); }

  // A lower bound on bytes lost: bytes that found the receive ring full, and
  // one for each overrun the driver reported.
  int64 errors() const { return static_cast<int64>(dropped_) + overruns_; }

 private:
  static void callback(const device* dev, void* user_data) {
    static_cast<UartResource*>(user_data)->service(dev);
  }

  void service(const device* dev) {
    uart_irq_update(dev);
    int errors = uart_err_check(dev);
    if (errors > 0 && (errors & UART_ERROR_OVERRUN)) overruns_++;
    uint32_t notified = 0;
    if (receives()) {
      // Reads the FIFO empty even when the ring is full: a level-triggered
      // receive interrupt would otherwise fire again at once.
      while (uart_irq_rx_ready(dev)) {
        uint8_t* space;
        int room = ring_buf_put_ptr(&rx_ring_, &space, 0);
        if (room > 0) {
          int n = uart_fifo_read(dev, space, room);
          if (n <= 0) break;
          ring_buf_commit(&rx_ring_, n);
          notified |= READ_STATE;
        } else {
          uint8_t scratch[16];
          int n = uart_fifo_read(dev, scratch, sizeof(scratch));
          if (n <= 0) break;
          dropped_ += n;
        }
      }
    }
    if (tx_on_) {
      while (uart_irq_tx_ready(dev) > 0) {
        uint8_t* data;
        int length = ring_buf_get_ptr(&tx_ring_, &data, 0);
        if (length == 0) break;
        int n = uart_fifo_fill(dev, data, length);
        if (n <= 0) break;
        ring_buf_consume(&tx_ring_, n);
      }
      // Empty: stop the interrupt and wake a writer or a flush. Locked
      // against write, which puts and enables in one step, so a byte put
      // after this check finds tx_on_ false and enables again.
      unsigned key = irq_lock();
      if (ring_buf_is_empty(&tx_ring_)) {
        uart_irq_tx_disable(dev);
        tx_on_ = false;
        notified |= WRITE_STATE;
      }
      irq_unlock(key);
    }
    if (notified) notify(notified);
  }

  UartController* controller_ = null;
  int pins_[2] = {-1, -1};
  int claimed_ = 0;
  bool started_ = false;
  uart_config config_ = {};
  ring_buf rx_ring_ = {};
  ring_buf tx_ring_ = {};
  uint8_t* rx_buffer_ = null;
  uint8_t* tx_buffer_ = null;
  bool tx_on_ = false;  // The TX-ready interrupt is enabled. Under irq_lock.
  volatile uint32_t dropped_ = 0;
  volatile uint32_t overruns_ = 0;
};

class UartResourceGroup : public ResourceGroup {
 public:
  TAG(UartResourceGroup);
  explicit UartResourceGroup(Process* process) : ResourceGroup(process, ZephyrEventSource::instance()) {}
  uint32_t on_event(Resource* resource, word data, uint32_t state) override {
    // data is the pending states the callback set.
    return state | static_cast<uint32_t>(data);
  }
};

// The library's framing values (lib/uart.toit) as a Zephyr uart_config.
static int make_config(int baud_rate, int data_bits, int stop_bits, int parity, uart_config* config) {
  if (baud_rate <= 0) return -EINVAL;
  if (data_bits < 5 || data_bits > 9) return -EINVAL;
  if (stop_bits < 1 || stop_bits > 3) return -EINVAL;
  if (parity < 1 || parity > 3) return -EINVAL;
  config->baudrate = baud_rate;
  config->data_bits = UART_CFG_DATA_BITS_5 + (data_bits - 5);
  // STOP-BITS-1, -1-5, -2 are 1, 2, 3: UART_CFG_STOP_BITS_1, _1_5, _2.
  config->stop_bits = stop_bits;
  config->parity = parity == 1 ? UART_CFG_PARITY_NONE
      : parity == 2 ? UART_CFG_PARITY_EVEN : UART_CFG_PARITY_ODD;
  config->flow_ctrl = UART_CFG_FLOW_CTRL_NONE;
  return 0;
}

static Object* open_port(Process* process, UartResourceGroup* group, UartController* controller,
                         int tx, int rx, const uart_config& config, int rx_size) {
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (!proxy) FAIL(ALLOCATION_FAILED);
  auto port = _new UartResource(group);
  if (!port) FAIL(MALLOC_FAILED);
  group->register_resource(port);
  AutoUnregisteringResource<UartResource> cleanup(group, port);
  int result = port->open(controller, tx, rx, config, rx_size);
  if (result != 0) return zephyr_error(result, process);
  cleanup.set_external_address(proxy);
  return proxy;
}

MODULE_IMPLEMENTATION(uart, MODULE_UART)

PRIMITIVE(init) {
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (!proxy) FAIL(ALLOCATION_FAILED);
  auto group = _new UartResourceGroup(process);
  if (!group) FAIL(MALLOC_FAILED);
  proxy->set_external_address(group);
  return proxy;
}

// The pins select the controller whose child names them; a pin not given is
// not claimed and its direction not started. Pins are numbers only: the
// library encodes a gpio.Pin as -2 or less, refused as in i2c (ztoit#62).
PRIMITIVE(create) {
  ARGS(UartResourceGroup, group, int, tx, int, rx, int, rts, int, cts,
       int, baud_rate, int, data_bits, int, stop_bits, int, parity,
       int, options, int, mode);
  if (tx < -1 || rx < -1 || rts != -1 || cts != -1) FAIL(INVALID_ARGUMENT);
  if (mode != 0) FAIL(INVALID_ARGUMENT);   // MODE-UART only.
  if ((options & 3) != 0) FAIL(INVALID_ARGUMENT);  // Inverted TX or RX.
  uart_config config;
  if (make_config(baud_rate, data_bits, stop_bits, parity, &config) != 0) FAIL(INVALID_ARGUMENT);
  UartController* match = null;
  for (auto& c : controllers) {
    if ((tx == -1 || tx == c.tx) && (rx == -1 || rx == c.rx)) { match = &c; break; }
  }
  if (!match) FAIL(INVALID_ARGUMENT);
  int rx_size = (options & LARGE_BUFFERS) ? RX_LARGE_SIZE : RX_SIZE;
  return open_port(process, group, match, tx, rx, config, rx_size);
}

// The device path is the controller's node label ("uart2"); the port takes
// every pin the child names.
PRIMITIVE(create_path) {
  ARGS(UartResourceGroup, group, cstring, path, int, baud_rate, int, data_bits,
       int, stop_bits, int, parity);
  uart_config config;
  if (make_config(baud_rate, data_bits, stop_bits, parity, &config) != 0) FAIL(INVALID_ARGUMENT);
  UartController* match = null;
  for (auto& c : controllers) {
    for (auto label = c.labels; *label && !match; label++) {
      if (strcmp(*label, path) == 0) match = &c;
    }
    if (match) break;
  }
  if (!match) FAIL(INVALID_ARGUMENT);
  return open_port(process, group, match, match->tx, match->rx, config, RX_SIZE);
}

PRIMITIVE(create_console) { FAIL(UNIMPLEMENTED); }

PRIMITIVE(close) {
  ARGS(UartResourceGroup, group, UartResource, port);
  group->unregister_resource(port);
  port_proxy->clear_external_address();
  return process->null_object();
}

PRIMITIVE(get_baud_rate) {
  ARGS(UartResource, port);
  return Primitive::integer(port->baud_rate(), process);
}

PRIMITIVE(set_baud_rate) {
  ARGS(UartResource, port, int, baud_rate);
  if (baud_rate <= 0) FAIL(INVALID_ARGUMENT);
  int result = port->set_baud_rate(baud_rate);
  if (result != 0) return zephyr_error(result, process);
  return process->null_object();
}

// Returns the count the transmit ring took, maybe 0; the library then waits
// for the write state, which the callback sets when the ring is empty.
PRIMITIVE(write) {
  ARGS(UartResource, port, Blob, data, int, from, int, to, int, break_length);
  if (from < 0 || from > to || to > data.length()) FAIL(OUT_OF_RANGE);
  if (break_length != 0) FAIL(UNIMPLEMENTED);
  if (!port->transmits()) FAIL(INVALID_STATE);  // Opened without tx.
  return Smi::from(port->write(data.address() + from, to - from));
}

PRIMITIVE(wait_tx) {
  ARGS(UartResource, port);
  return BOOL(port->flushed());
}

// The bytes received since the last read, or null if none. Never waits.
PRIMITIVE(read) {
  ARGS(UartResource, port);
  if (!port->receives()) return process->null_object();
  int n = port->available();
  if (n == 0) return process->null_object();
  ByteArray* result = process->allocate_byte_array(n);
  if (!result) FAIL(ALLOCATION_FAILED);  // The bytes stay in the ring for the retry.
  port->read(ByteArray::Bytes(result).address(), n);
  return result;
}

PRIMITIVE(set_control_flags) { FAIL(UNIMPLEMENTED); }
PRIMITIVE(get_control_flags) { FAIL(UNIMPLEMENTED); }

PRIMITIVE(errors) {
  ARGS(UartResource, port);
  return Primitive::integer(port->errors(), process);
}

}  // namespace toit

#endif  // A UART child.
#endif  // ztoit,buses and toit,platform.
#endif  // TOIT_ZEPHYR
