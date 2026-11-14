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

#if defined(TOIT_ZEPHYR) && defined(CONFIG_NET_SOCKETS) && defined(CONFIG_NET_TCP)

#include <cstring>
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
#include "tcp.h"

// TCP over Zephyr's BSD sockets, shaped like tcp_linux.cc. IPv6 only; sockets
// are non-blocking, and every primitive that gets EAGAIN re-arms the fd in
// ZsockEventSource (POLLIN for read and accept, POLLOUT for write).

namespace toit {

static int make_socket() {
  int fd = zsock_socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
  if (fd < 0) return -1;
  if (zsock_fcntl(fd, ZVFS_F_SETFL, ZVFS_O_NONBLOCK) < 0) {
    int error = errno;
    zsock_close(fd);
    errno = error;
    return -1;
  }
  return fd;
}

class SocketResourceGroup : public ResourceGroup {
 public:
  TAG(SocketResourceGroup);
  SocketResourceGroup(Process* process, EventSource* event_source) : ResourceGroup(process, event_source) {}

  void close_socket(int id) {
    unregister_id(id);  // The event source closes the fd.
  }

 private:
  uint32_t on_event(Resource* resource, word data, uint32_t state) override {
    if (data & ZSOCK_POLLIN) state |= TCP_READ;
    if (data & ZSOCK_POLLOUT) state |= TCP_WRITE;
    if (data & ZSOCK_POLLHUP) state |= TCP_CLOSE;
    if (data & ZSOCK_POLLERR) state |= TCP_ERROR;
    return state;
  }
};

MODULE_IMPLEMENTATION(tcp, MODULE_TCP)

PRIMITIVE(init) {
  ByteArray* proxy = process->object_heap()->allocate_proxy();
  if (proxy == null) FAIL(ALLOCATION_FAILED);
  SocketResourceGroup* resource_group = _new SocketResourceGroup(process, ZsockEventSource::instance());
  if (!resource_group) FAIL(MALLOC_FAILED);
  proxy->set_external_address(resource_group);
  return proxy;
}

PRIMITIVE(close) {
  ARGS(SocketResourceGroup, resource_group, IntResource, fd_resource);
  resource_group->close_socket(fd_resource->id());
  fd_resource_proxy->clear_external_address();
  return process->null_object();
}

PRIMITIVE(close_write) {
  ARGS(ByteArray, proxy, IntResource, fd_resource);
  USE(proxy);
  if (zsock_shutdown(fd_resource->id(), ZSOCK_SHUT_WR) != 0) return Primitive::os_error(errno, process);
  return process->null_object();
}

// Registers fd and wraps it in a fresh proxy, closing fd on failure.
static Object* register_socket(SocketResourceGroup* resource_group, int fd, Process* process) {
  ByteArray* resource_proxy = process->object_heap()->allocate_proxy();
  if (resource_proxy == null) { zsock_close(fd); FAIL(ALLOCATION_FAILED); }
  IntResource* resource = resource_group->register_id(fd);
  if (!resource) { zsock_close(fd); FAIL(MALLOC_FAILED); }
  resource_proxy->set_external_address(resource);
  return resource_proxy;
}

PRIMITIVE(connect) {
  ARGS(SocketResourceGroup, resource_group, Blob, address, int, port, int, window_size);
  USE(window_size);  // Zephyr sizes TCP windows by Kconfig.
  struct sockaddr_in6 addr;
  if (!zsock_address_from_bytes(address.address(), address.length(), port, &addr)) FAIL(INVALID_ARGUMENT);
  int fd = make_socket();
  if (fd == -1) return Primitive::os_error(errno, process);
  if (zsock_connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 && errno != EINPROGRESS) {
    int error = errno;
    zsock_close(fd);
    return Primitive::os_error(error, process);
  }
  // Completion (or failure) arrives as POLLOUT (or POLLERR/POLLHUP); the fd is
  // registered armed for both directions.
  return register_socket(resource_group, fd, process);
}

PRIMITIVE(accept) {
  ARGS(SocketResourceGroup, resource_group, IntResource, listen_fd_resource);
  int listen_fd = listen_fd_resource->id();
  int fd = zsock_accept(listen_fd, null, null);
  if (fd == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ZsockEventSource::instance()->rearm(listen_fd, ZSOCK_POLLIN);
      return process->null_object();
    }
    return Primitive::os_error(errno, process);
  }
  if (zsock_fcntl(fd, ZVFS_F_SETFL, ZVFS_O_NONBLOCK) < 0) {
    int error = errno;
    zsock_close(fd);
    return Primitive::os_error(error, process);
  }
  return register_socket(resource_group, fd, process);
}

PRIMITIVE(listen) {
  ARGS(SocketResourceGroup, resource_group, cstring, hostname, int, port, int, backlog);
  struct sockaddr_in6 addr;
  if (!zsock_address_from_string(hostname, port, &addr)) FAIL(INVALID_ARGUMENT);
  int fd = make_socket();
  if (fd == -1) return Primitive::os_error(errno, process);
  int yes = 1;
  zsock_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  if (zsock_bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
      zsock_listen(fd, backlog) != 0) {
    int error = errno;
    zsock_close(fd);
    return Primitive::os_error(error, process);
  }
  return register_socket(resource_group, fd, process);
}

PRIMITIVE(write) {
  ARGS(ByteArray, proxy, IntResource, fd_resource, Blob, data, int, from, int, to);
  USE(proxy);
  int fd = fd_resource->id();
  if (from < 0 || from > to || to > data.length()) FAIL(OUT_OF_BOUNDS);
  int wrote = zsock_send(fd, data.address() + from, to - from, 0);
  if (wrote == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ZsockEventSource::instance()->rearm(fd, ZSOCK_POLLOUT);
      return Smi::from(-1);
    }
    return Primitive::os_error(errno, process);
  }
  return Smi::from(wrote);
}

PRIMITIVE(read) {
  ARGS(ByteArray, proxy, IntResource, fd_resource);
  USE(proxy);
  int fd = fd_resource->id();
  // No FIONREAD here: read into a preferred-size buffer and shrink it.
  word capacity = ByteArray::PREFERRED_IO_BUFFER_SIZE;
  ByteArray* array = process->allocate_byte_array(capacity, /*force_external*/ true);
  if (array == null) FAIL(ALLOCATION_FAILED);
  int read = zsock_recv(fd, ByteArray::Bytes(array).address(), capacity, 0);
  if (read == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      ZsockEventSource::instance()->rearm(fd, ZSOCK_POLLIN);
      return Smi::from(-1);
    }
    return Primitive::os_error(errno, process);
  }
  if (read == 0) return process->null_object();
  array->resize_external(process, read);
  return array;
}

PRIMITIVE(error_number) {
  ARGS(IntResource, fd_resource);
  int error = 0;
  socklen_t errlen = sizeof(error);
  if (zsock_getsockopt(fd_resource->id(), SOL_SOCKET, SO_ERROR, &error, &errlen) != 0) error = errno;
  return Smi::from(error);
}

PRIMITIVE(error) {
  ARGS(int, error);
  return process->allocate_string_or_error(strerror(error));
}

PRIMITIVE(get_option) {
  ARGS(ByteArray, proxy, IntResource, fd_resource, int, option);
  USE(proxy);
  int fd = fd_resource->id();
  switch (option) {
    case TCP_ADDRESS: return zsock_name(fd, process, false, false);
    case TCP_PORT: return zsock_name(fd, process, false, true);
    case TCP_PEER_ADDRESS: return zsock_name(fd, process, true, false);
    case TCP_PEER_PORT: return zsock_name(fd, process, true, true);
    case TCP_NO_DELAY: {
      int value = 0;
      socklen_t size = sizeof(value);
      if (zsock_getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &size) != 0) {
        return Primitive::os_error(errno, process);
      }
      return BOOL(value != 0);
    }
    default:
      FAIL(UNIMPLEMENTED);
  }
}

PRIMITIVE(set_option) {
  ARGS(ByteArray, proxy, IntResource, fd_resource, int, option, Object, raw);
  USE(proxy);
  int fd = fd_resource->id();
  switch (option) {
    case TCP_NO_DELAY: {
      int value;
      if (raw == process->true_object()) value = 1;
      else if (raw == process->false_object()) value = 0;
      else FAIL(WRONG_OBJECT_TYPE);
      if (zsock_setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)) != 0) {
        return Primitive::os_error(errno, process);
      }
      return process->null_object();
    }
    default:
      FAIL(UNIMPLEMENTED);
  }
}

PRIMITIVE(gc) {
  // Allocation failures surface as MALLOC_FAILED, never as NEEDS_GC.
  UNREACHABLE();
}

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_NET_SOCKETS && CONFIG_NET_TCP
