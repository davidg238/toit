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

// IPv6 address <-> text for the BLE relay frame (udp_ble.cc), whose host is text.
// Header-only and libc-free so a host test needs no Zephyr, and a BLE build needs
// no IP stack. Writes the uncompressed form without leading zeros (what Java's
// Inet6Address prints); reads that form and "::" forms. IPv6 only, like
// udp_zephyr.cc: anything else (an IPv4 address, a zone) is not IPv6 text.

#pragma once

#include <stddef.h>
#include <stdint.h>

#define IPV6_TEXT_MAX 40  // 8 groups of 4 hex digits, 7 colons, a NUL.

// Writes addr as text and a NUL; answers the length without the NUL, or -1 when
// out is too small.
static inline int ipv6_format(const uint8_t addr[16], char* out, size_t cap) {
  static const char hex[] = "0123456789abcdef";
  char tmp[IPV6_TEXT_MAX];
  int n = 0;
  for (int g = 0; g < 8; g++) {
    unsigned v = (unsigned)addr[2 * g] << 8 | addr[2 * g + 1];
    if (g > 0) tmp[n++] = ':';
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
      unsigned d = (v >> shift) & 0xf;
      if (d != 0 || started || shift == 0) {
        tmp[n++] = hex[d];
        started = true;
      }
    }
  }
  if ((size_t)n + 1 > cap) return -1;
  for (int i = 0; i < n; i++) out[i] = tmp[i];
  out[n] = 0;
  return n;
}

// Parses len bytes of text (no NUL needed) into out; answers 0, or -1 when it is
// not IPv6 text.
static inline int ipv6_parse(const char* text, size_t len, uint8_t out[16]) {
  uint16_t groups[8];
  int count = 0;     // Groups read.
  int gap = -1;      // Index in groups where "::" stands, or -1.
  size_t i = 0;
  if (len >= 2 && text[0] == ':' && text[1] == ':') {
    gap = 0;
    i = 2;
  } else if (len == 0 || text[0] == ':') {
    return -1;
  }
  while (i < len) {
    unsigned v = 0;
    int digits = 0;
    while (i < len && text[i] != ':') {
      char c = text[i];
      unsigned d;
      if (c >= '0' && c <= '9') d = c - '0';
      else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
      else return -1;
      if (++digits > 4) return -1;
      v = v << 4 | d;
      i++;
    }
    if (digits == 0 || count == 8) return -1;
    groups[count++] = (uint16_t)v;
    if (i == len) break;
    i++;  // The colon.
    if (i == len) return -1;  // A trailing single colon.
    if (text[i] == ':') {
      if (gap >= 0) return -1;  // A second "::".
      gap = count;
      i++;
    }
  }
  if (gap < 0 ? count != 8 : count > 7) return -1;
  int fill = 8 - count;
  for (int g = 0, src = 0; g < 8; g++) {
    uint16_t v = 0;
    if (gap >= 0 && g >= gap && g < gap + fill) {
      v = 0;
    } else {
      v = groups[src++];
    }
    out[2 * g] = (uint8_t)(v >> 8);
    out[2 * g + 1] = (uint8_t)v;
  }
  return 0;
}
