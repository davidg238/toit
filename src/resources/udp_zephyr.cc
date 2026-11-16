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

#if defined(TOIT_ZEPHYR) && defined(CONFIG_NET_SOCKETS)

#include <zephyr/net/socket.h>

#include "../objects.h"
#include "../objects_inline.h"
#include "../os.h"
#include "../primitive.h"
#include "../process.h"
#include "../resource.h"
#include "../vm.h"

#include "../event_sources/zsock_zephyr.h"

#include "socket_address_zephyr.h"
#include "udp.h"

// UDP over Zephyr's BSD sockets, shaped like udp_linux.cc. IPv6 only (Thread
// is IPv6-only); addresses as in socket_address_zephyr.h.

namespace toit {

static const int MAX_DATAGRAM = 1280;  // The IPv6 minimum MTU bounds a Thread datagram.

class UdpResourceGroup : public ResourceGroup {
 public:
  TAG(UdpResourceGroup);
  UdpResourceGroup(Process* process, EventSource* event_source) : ResourceGroup(process, event_source) {}

  int create_socket() {
    return zsock_socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
  }

  void close_socket(int id) {
    unregister_id(id);  // The event source closes the fd.
  }

 private:
  uint32_t on_event(Resource* resource, word data, uint32_t state) override {
    if (data & ZSOCK_POLLIN) state |= UDP_READ;
    if (data & ZSOCK_POLLOUT) state |= UDP_WRITE;
    if (data & (ZSOCK_POLLERR | ZSOCK_POLLHUP)) state |= UDP_ERROR;
    return state;
  }
};

MODULE_IMPLEMENTATION(udp, MODULE_UDP)

PRIMITIVE(init) {
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (proxy == null) FAIL(ALLOCATION_FAILED);
  UdpResourceGroup* resource_group = _new UdpResourceGroup(process, ZsockEventSource::instance());
  if (!resource_group) FAIL(MALLOC_FAILED);
  proxy->set_external_address(resource_group);
  return proxy;
}

PRIMITIVE(create_socket) {
  ARGS(UdpResourceGroup, resource_group);
  ByteArray* resource_proxy = process->object_heap()->allocate_proxy();
  if (resource_proxy == null) FAIL(ALLOCATION_FAILED);
  int id = resource_group->create_socket();
  if (id == -1) return Primitive::os_error(errno, process);
  IntResource* resource = resource_group->register_id(id);
  if (!resource) { zsock_close(id); FAIL(MALLOC_FAILED); }
  resource_proxy->set_external_address(resource);
  return resource_proxy;
}

PRIMITIVE(bind_socket) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, Blob, address, int, port);
  USE(proxy);
  struct sockaddr_in6 addr;
  if (!zsock_address_from_bytes(address.address(), address.length(), port, &addr)) FAIL(INVALID_ARGUMENT);
  if (zsock_bind(connection_resource->id(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    return Primitive::os_error(errno, process);
  }
  return process->null_object();
}

PRIMITIVE(bind) {
  ARGS(UdpResourceGroup, resource_group, Blob, address, int, port);
  struct sockaddr_in6 addr;
  if (!zsock_address_from_bytes(address.address(), address.length(), port, &addr)) FAIL(INVALID_ARGUMENT);
  ByteArray* resource_proxy = process->object_heap()->allocate_proxy();
  if (resource_proxy == null) FAIL(ALLOCATION_FAILED);
  int id = resource_group->create_socket();
  if (id == -1) return Primitive::os_error(errno, process);
  if (zsock_bind(id, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    int error = errno;
    zsock_close(id);
    return Primitive::os_error(error, process);
  }
  IntResource* resource = resource_group->register_id(id);
  if (!resource) { zsock_close(id); FAIL(MALLOC_FAILED); }
  resource_proxy->set_external_address(resource);
  return resource_proxy;
}

PRIMITIVE(connect) {
  ARGS(ByteArray, proxy, IntResource, connection, Blob, address, int, port);
  USE(proxy);
  struct sockaddr_in6 addr;
  if (!zsock_address_from_bytes(address.address(), address.length(), port, &addr)) FAIL(INVALID_ARGUMENT);
  if (zsock_connect(connection->id(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    return Primitive::os_error(errno, process);
  }
  return connection_proxy;
}

PRIMITIVE(receive) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, Object, output);
  USE(proxy);
  int fd = connection_resource->id();
  uint8 buffer[MAX_DATAGRAM];
  struct sockaddr_in6 addr;
  socklen_t addr_len = sizeof(addr);
  // Peek, allocate, then consume: an allocation failure makes the interpreter GC
  // and re-run this primitive, so the datagram must still be queued (ztoit#18).
  int read = zsock_recvfrom(fd, buffer, sizeof(buffer), ZSOCK_MSG_DONTWAIT | ZSOCK_MSG_PEEK,
                            reinterpret_cast<struct sockaddr*>(&addr), &addr_len);
  if (read == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ZsockEventSource::instance()->rearm(fd, ZSOCK_POLLIN);
      return Smi::from(-1);
    }
    return Primitive::os_error(errno, process);
  }
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
  zsock_recvfrom(fd, buffer, sizeof(buffer), ZSOCK_MSG_DONTWAIT, null, null);  // Consume the peeked datagram.
  memcpy(ByteArray::Bytes(array).address(), buffer, read);
  if (out == null) return array;

  memcpy(ByteArray::Bytes(address).address(), &addr.sin6_addr, 16);
  out->at_put(0, array);
  out->at_put(1, address);
  out->at_put(2, Smi::from(ntohs(addr.sin6_port)));
  return out;
}

PRIMITIVE(send) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, Blob, data, int, from, int, to, Object, address, int, port);
  USE(proxy);
  int fd = connection_resource->id();
  if (from < 0 || from > to || to > data.length()) FAIL(OUT_OF_BOUNDS);
  struct sockaddr_in6 addr;
  struct sockaddr* to_addr = null;
  socklen_t size = 0;
  if (address != process->null_object()) {
    Blob address_bytes;
    if (!address->byte_content(process->program(), &address_bytes, STRINGS_OR_BYTE_ARRAYS)) FAIL(WRONG_OBJECT_TYPE);
    if (!zsock_address_from_bytes(address_bytes.address(), address_bytes.length(), port, &addr)) FAIL(INVALID_ARGUMENT);
    to_addr = reinterpret_cast<struct sockaddr*>(&addr);
    size = sizeof(addr);
  }
  int wrote = zsock_sendto(fd, data.address() + from, to - from, ZSOCK_MSG_DONTWAIT, to_addr, size);
  if (wrote == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ZsockEventSource::instance()->rearm(fd, ZSOCK_POLLOUT);
      return Smi::from(-1);
    }
    return Primitive::os_error(errno, process);
  }
  return Smi::from(wrote);
}

PRIMITIVE(get_option) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, int, option);
  USE(proxy);
  int fd = connection_resource->id();
  switch (option) {
    case UDP_ADDRESS: return zsock_name(fd, process, false, false);
    case UDP_PORT: return zsock_name(fd, process, false, true);
    case UDP_BROADCAST: return BOOL(false);
    default: FAIL(UNIMPLEMENTED);
  }
}

PRIMITIVE(set_option) {
  ARGS(ByteArray, proxy, IntResource, connection_resource, int, option, Object, raw);
  USE(proxy);
  USE(connection_resource);
  // No IPv4 broadcast on an IPv6-only stack; multicast and reuse come later.
  if (option == UDP_BROADCAST && raw == process->false_object()) return process->null_object();
  FAIL(UNIMPLEMENTED);
}

PRIMITIVE(error_number) {
  ARGS(IntResource, connection_resource);
  int error = 0;
  socklen_t errlen = sizeof(error);
  if (zsock_getsockopt(connection_resource->id(), SOL_SOCKET, SO_ERROR, &error, &errlen) != 0) {
    error = errno;
  }
  return Smi::from(error);
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

#endif  // TOIT_ZEPHYR && CONFIG_NET_SOCKETS
