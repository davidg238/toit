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

// Socket addresses for the IPv6-only Zephyr stack (udp_zephyr.cc, tcp_zephyr.cc).
// The Toit library still spells "any" as the IPv4 #[0,0,0,0] or "0.0.0.0";
// those, and only those, IPv4 forms mean in6addr_any.

// Fills `addr` from a 16-byte address, or the 4-byte any-address.
static inline bool zsock_address_from_bytes(const uint8* bytes, word length, int port, struct sockaddr_in6* addr) {
  memset(addr, 0, sizeof(*addr));
  addr->sin6_family = AF_INET6;
  addr->sin6_port = htons(port);
  if (length == 16) {
    memcpy(&addr->sin6_addr, bytes, 16);
    return true;
  }
  if (length == 4) {
    for (int i = 0; i < 4; i++) if (bytes[i] != 0) return false;
    return true;  // in6addr_any.
  }
  return false;
}

// Fills `addr` from a textual address: "", "0.0.0.0" and "::" mean any.
static inline bool zsock_address_from_string(const char* text, int port, struct sockaddr_in6* addr) {
  memset(addr, 0, sizeof(*addr));
  addr->sin6_family = AF_INET6;
  addr->sin6_port = htons(port);
  if (text[0] == '\0' || strcmp(text, "0.0.0.0") == 0) return true;
  return zsock_inet_pton(AF_INET6, text, &addr->sin6_addr) == 1;
}

// The local or peer address of fd as a string, or its port as a Smi. A
// connected socket that is still bound to :: (as UDP stays) reports the source
// address Zephyr would pick for its peer: net.Client.address relies on this.
static inline Object* zsock_name(int fd, Process* process, bool peer, bool want_port) {
  struct sockaddr_in6 sin;
  socklen_t len = sizeof(sin);
  int result = peer
      ? zsock_getpeername(fd, reinterpret_cast<struct sockaddr*>(&sin), &len)
      : zsock_getsockname(fd, reinterpret_cast<struct sockaddr*>(&sin), &len);
  if (result != 0) return Primitive::os_error(errno, process);
  if (want_port) return Smi::from(ntohs(sin.sin6_port));
  struct sockaddr_in6 other;
  socklen_t other_len = sizeof(other);
  if (!peer && net_ipv6_is_addr_unspecified(&sin.sin6_addr) &&
      zsock_getpeername(fd, reinterpret_cast<struct sockaddr*>(&other), &other_len) == 0) {
    const struct in6_addr* source = net_if_ipv6_select_src_addr(null, &other.sin6_addr);
    if (source != null) sin.sin6_addr = *source;
  }
  char buffer[INET6_ADDRSTRLEN];
  zsock_inet_ntop(AF_INET6, &sin.sin6_addr, buffer, sizeof(buffer));
  return process->allocate_string_or_error(buffer);
}

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_NET_SOCKETS
