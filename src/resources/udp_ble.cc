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

#if defined(TOIT_ZEPHYR) && defined(CONFIG_BT) && !defined(CONFIG_NET_SOCKETS)

#include <errno.h>

#include "../objects.h"
#include "../objects_inline.h"
#include "../primitive.h"
#include "../process.h"
#include "../resource.h"

#include "../event_sources/ble_udp_zephyr.h"

#include "udp.h"

// UDP over Bluetooth LE to a relay (event_sources/ble_udp_zephyr.h), shaped like
// udp_zephyr.cc. IPv6 only: addresses are 16 bytes; a 4-byte all-zero address
// (the library's default bind) means any. There is one link, so a bind's address
// is otherwise ignored.

namespace toit {

static const int MAX_DATAGRAM = BleUdpEventSource::MAX_DATAGRAM;

class UdpResourceGroup : public ResourceGroup {
 public:
  TAG(UdpResourceGroup);
  UdpResourceGroup(Process* process, EventSource* event_source) : ResourceGroup(process, event_source) {}

  void close_socket(int id) {
    unregister_id(id);  // The event source frees the slot.
  }

 private:
  uint32_t on_event(Resource* resource, word data, uint32_t state) override {
    return state | static_cast<uint32_t>(data);
  }
};

// Peer of a connected socket, for write without an address.
struct Peer {
  bool set;
  uint8_t addr[16];
  int port;
};
static Peer peers[BleUdpEventSource::MAX_SOCKS];

static bool is_any(const Blob& address) {
  if (address.length() != 4 && address.length() != 16) return false;
  for (int i = 0; i < address.length(); i++) {
    if (address.address()[i] != 0) return false;
  }
  return true;
}

// Registers a claimed socket; frees it if that fails.
static Object* register_new(UdpResourceGroup* group, ByteArray* resource_proxy, int id) {
  peers[id].set = false;
  IntResource* resource = group->register_id(id);
  if (!resource) { BleUdpEventSource::release(id); return null; }
  resource_proxy->set_external_address(resource);
  return resource_proxy;
}

MODULE_IMPLEMENTATION(udp, MODULE_UDP)

PRIMITIVE(init) {
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (proxy == null) FAIL(ALLOCATION_FAILED);
  UdpResourceGroup* resource_group = _new UdpResourceGroup(process, BleUdpEventSource::instance());
  if (!resource_group) FAIL(MALLOC_FAILED);
  proxy->set_external_address(resource_group);
  return proxy;
}

// The library binds by create_socket then bind_socket (udp.toit's udp-bind_).
PRIMITIVE(create_socket) {
  ARGS(UdpResourceGroup, resource_group);
  ByteArray* resource_proxy = process->object_heap()->allocate_proxy();
  if (resource_proxy == null) FAIL(ALLOCATION_FAILED);
  int id = BleUdpEventSource::create();
  if (id < 0) return Primitive::os_error(errno, process);
  if (!register_new(resource_group, resource_proxy, id)) FAIL(MALLOC_FAILED);
  return resource_proxy;
}

PRIMITIVE(bind_socket) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, Blob, address, int, port);
  USE(proxy);
  if (!is_any(address) && address.length() != 16) FAIL(INVALID_ARGUMENT);
  if (BleUdpEventSource::bind_port(connection_resource->id(), port) != 0) {
    return Primitive::os_error(errno, process);
  }
  return process->null_object();
}

PRIMITIVE(bind) {
  ARGS(UdpResourceGroup, resource_group, Blob, address, int, port);
  if (!is_any(address) && address.length() != 16) FAIL(INVALID_ARGUMENT);
  ByteArray* resource_proxy = process->object_heap()->allocate_proxy();
  if (resource_proxy == null) FAIL(ALLOCATION_FAILED);
  int id = BleUdpEventSource::bind(port);
  if (id < 0) return Primitive::os_error(errno, process);
  if (!register_new(resource_group, resource_proxy, id)) FAIL(MALLOC_FAILED);
  return resource_proxy;
}

PRIMITIVE(connect) {
  ARGS(ByteArray, proxy, IntResource, connection, Blob, address, int, port);
  USE(proxy);
  int id = connection->id();
  if (address.length() != 16 || port <= 0 || port > 0xffff) FAIL(INVALID_ARGUMENT);
  memcpy(peers[id].addr, address.address(), 16);
  peers[id].port = port;
  peers[id].set = true;
  return connection_proxy;
}

PRIMITIVE(receive) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, Object, output);
  USE(proxy);
  int id = connection_resource->id();
  uint8 buffer[MAX_DATAGRAM];
  uint8 addr[16];
  int port = 0;
  // Peek, allocate, then consume: an allocation failure makes the interpreter GC
  // and re-run this primitive, so the datagram must still be queued (ztoit#18).
  int read = BleUdpEventSource::peek(id, buffer, sizeof(buffer), addr, &port);
  if (read < 0) return Smi::from(-1);  // UDP_READ follows the next datagram.
  Array* out = null;
  if (is_array(output)) {
    out = Array::cast(output);
    if (out->length() < 3) FAIL(INVALID_ARGUMENT);
  }
  ByteArray* array = process->allocate_byte_array(read);
  if (array == null) FAIL(ALLOCATION_FAILED);
  ByteArray* address = null;
  if (out != null) {
    address = process->allocate_byte_array(16);
    if (address == null) FAIL(ALLOCATION_FAILED);
  }
  BleUdpEventSource::consume(id);
  memcpy(ByteArray::Bytes(array).address(), buffer, read);
  if (out == null) return array;
  memcpy(ByteArray::Bytes(address).address(), addr, 16);
  out->at_put(0, array);
  out->at_put(1, address);
  out->at_put(2, Smi::from(port));
  return out;
}

PRIMITIVE(send) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, Blob, data, int, from, int, to, Object, address, int, port);
  USE(proxy);
  int id = connection_resource->id();
  if (from < 0 || from > to || to > data.length()) FAIL(OUT_OF_BOUNDS);
  const uint8_t* dest;
  int dest_port;
  if (address != process->null_object()) {
    Blob address_bytes;
    if (!address->byte_content(process->program(), &address_bytes, STRINGS_OR_BYTE_ARRAYS)) FAIL(WRONG_OBJECT_TYPE);
    if (address_bytes.length() != 16 || port <= 0 || port > 0xffff) FAIL(INVALID_ARGUMENT);
    dest = address_bytes.address();
    dest_port = port;
  } else {
    if (id < 0 || id >= BleUdpEventSource::MAX_SOCKS || !peers[id].set) {
      return Primitive::os_error(EDESTADDRREQ, process);
    }
    dest = peers[id].addr;
    dest_port = peers[id].port;
  }
  int wrote = BleUdpEventSource::send(id, dest, dest_port, data.address() + from, to - from);
  if (wrote == -1) {
    if (errno == EAGAIN) return Smi::from(-1);  // UDP_WRITE follows a freed buffer.
    return Primitive::os_error(errno, process);
  }
  return Smi::from(wrote);
}

PRIMITIVE(get_option) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, int, option);
  USE(proxy);
  switch (option) {
    case UDP_ADDRESS: return process->allocate_string_or_error("::");  // The node has no address of its own.
    case UDP_PORT: return Smi::from(BleUdpEventSource::port(connection_resource->id()));
    case UDP_BROADCAST: return BOOL(false);
    default: FAIL(UNIMPLEMENTED);
  }
}

PRIMITIVE(set_option) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, int, option, Object, raw);
  USE(proxy);
  USE(connection_resource);
  if (option == UDP_BROADCAST && raw == process->false_object()) return process->null_object();
  FAIL(UNIMPLEMENTED);
}

PRIMITIVE(error_number) {
  ARGS(IntResource, connection_resource);
  USE(connection_resource);
  return Smi::from(0);
}

PRIMITIVE(close) {
  ARGS(UdpResourceGroup, resource_group, IntResource, connection_resource);
  resource_group->close_socket(connection_resource->id());
  connection_resource_proxy->clear_external_address();
  return process->null_object();
}

PRIMITIVE(gc) {
  // Allocation failures surface as MALLOC_FAILED, never as NEEDS_GC.
  UNREACHABLE();
}

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_BT && !CONFIG_NET_SOCKETS
