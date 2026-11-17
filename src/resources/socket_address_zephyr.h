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

#pragma once

#include "../top.h"

#if defined(TOIT_ZEPHYR) && defined(CONFIG_NET_SOCKETS)

#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>

#include "../objects.h"
#include "../process.h"
#include "../primitive.h"

namespace toit {

// Socket addresses for the Zephyr stack (udp_zephyr.cc, tcp_zephyr.cc). A
// socket's family is the build's: AF_INET6 when the build has IPv6 (Thread),
// else AF_INET (an IPv4-only WiFi build). Nothing is mapped between them.
// The Toit library still spells "any" as the IPv4 #[0,0,0,0] or "0.0.0.0";
// in an IPv6 build those, and only those, IPv4 forms mean in6addr_any.
#if defined(CONFIG_NET_IPV6)
typedef struct sockaddr_in6 zsock_addr_t;
static const int ZSOCK_FAMILY = AF_INET6;
static const int ZSOCK_ADDR_BYTES = 16;
#define ZSOCK_ADDR_STRLEN INET6_ADDRSTRLEN
static inline void* zsock_ip(zsock_addr_t* addr) { return &addr->sin6_addr; }
static inline int zsock_port(const zsock_addr_t* addr) { return ntohs(addr->sin6_port); }
static inline void zsock_init(zsock_addr_t* addr, int port) {
  memset(addr, 0, sizeof(*addr));
  addr->sin6_family = AF_INET6;
  addr->sin6_port = htons(port);
}
#else
typedef struct sockaddr_in zsock_addr_t;
static const int ZSOCK_FAMILY = AF_INET;
static const int ZSOCK_ADDR_BYTES = 4;
#define ZSOCK_ADDR_STRLEN INET_ADDRSTRLEN
static inline void* zsock_ip(zsock_addr_t* addr) { return &addr->sin_addr; }
static inline int zsock_port(const zsock_addr_t* addr) { return ntohs(addr->sin_port); }
static inline void zsock_init(zsock_addr_t* addr, int port) {
  memset(addr, 0, sizeof(*addr));
  addr->sin_family = AF_INET;
  addr->sin_port = htons(port);
}
#endif

// Fills `addr` from an address of the build's family, or the 4-byte any-address.
static inline bool zsock_address_from_bytes(const uint8* bytes, word length, int port, zsock_addr_t* addr) {
  zsock_init(addr, port);
  if (length == ZSOCK_ADDR_BYTES) {
    memcpy(zsock_ip(addr), bytes, ZSOCK_ADDR_BYTES);
    return true;
  }
  if (length == 4) {
    for (int i = 0; i < 4; i++) if (bytes[i] != 0) return false;
    return true;  // Any.
  }
  return false;
}

// Fills `addr` from a textual address: "" and "0.0.0.0" (and "::") mean any.
static inline bool zsock_address_from_string(const char* text, int port, zsock_addr_t* addr) {
  zsock_init(addr, port);
  if (text[0] == '\0' || strcmp(text, "0.0.0.0") == 0) return true;
  return zsock_inet_pton(ZSOCK_FAMILY, text, zsock_ip(addr)) == 1;
}

// The local or peer address of fd as a string, or its port as a Smi. A
// connected socket that is still bound to any (as UDP stays) reports the source
// address Zephyr would pick for its peer: net.Client.address relies on this.
static inline Object* zsock_name(int fd, Process* process, bool peer, bool want_port) {
  zsock_addr_t sin;
  socklen_t len = sizeof(sin);
  int result = peer
      ? zsock_getpeername(fd, reinterpret_cast<struct sockaddr*>(&sin), &len)
      : zsock_getsockname(fd, reinterpret_cast<struct sockaddr*>(&sin), &len);
  if (result != 0) return Primitive::os_error(errno, process);
  if (want_port) return Smi::from(zsock_port(&sin));
  zsock_addr_t other;
  socklen_t other_len = sizeof(other);
#if defined(CONFIG_NET_IPV6)
  if (!peer && net_ipv6_is_addr_unspecified(&sin.sin6_addr) &&
      zsock_getpeername(fd, reinterpret_cast<struct sockaddr*>(&other), &other_len) == 0) {
    const struct in6_addr* source = net_if_ipv6_select_src_addr(null, &other.sin6_addr);
    if (source != null) sin.sin6_addr = *source;
  }
#else
  if (!peer && net_ipv4_is_addr_unspecified(&sin.sin_addr) &&
      zsock_getpeername(fd, reinterpret_cast<struct sockaddr*>(&other), &other_len) == 0) {
    const struct in_addr* source = net_if_ipv4_select_src_addr(null, &other.sin_addr);
    if (source != null) sin.sin_addr = *source;
  }
#endif
  char buffer[ZSOCK_ADDR_STRLEN];
  zsock_inet_ntop(ZSOCK_FAMILY, zsock_ip(&sin), buffer, sizeof(buffer));
  return process->allocate_string_or_error(buffer);
}

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_NET_SOCKETS
