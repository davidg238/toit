// Copyright (C) 2020 Toitware ApS. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the lib/LICENSE file.

class IpAddress:
  raw ::= ?

  constructor .raw:
    if raw.size != 4 and raw.size != 16:
      throw "BAD_FORMAT"

  constructor.deserialize bytes/ByteArray:
    return IpAddress bytes

  /**
  Parses an IPv4 address in dotted decimal (192.168.0.1), or an IPv6
    address in colon hexadecimal, optionally compressed with "::"
    (2001:db8::1234:5678). Embedded IPv4 and zone suffixes are not accepted.
  */
  constructor.parse ip/string:
    if ip.contains ":": return IpAddress (parse-ipv6_ ip)
    bytes := ByteArray 4
    i := 0
    ip.split ".":
      if i >= 4: throw "BAD_FORMAT"
      val := int.parse it
      if not 0 <= val <= 255: throw "BAD_FORMAT"
      bytes[i] = val
      i++
    if i != 4: throw "BAD_FORMAT"
    return IpAddress bytes

  static parse-ipv6_ ip/string -> ByteArray:
    halves := ip.split "::"
    if halves.size > 2: throw "BAD_FORMAT"
    head := parse-ipv6-groups_ halves[0]
    tail := halves.size == 2 ? (parse-ipv6-groups_ halves[1]) : []
    if halves.size == 1 and head.size != 8: throw "BAD_FORMAT"
    if halves.size == 2 and head.size + tail.size > 7: throw "BAD_FORMAT"
    bytes := ByteArray 16
    put := : | index/int group/int |
      bytes[2 * index] = group >> 8
      bytes[2 * index + 1] = group & 0xff
    head.size.repeat: put.call it head[it]
    tail.size.repeat: put.call (8 - tail.size + it) tail[it]
    return bytes

  static parse-ipv6-groups_ text/string -> List:
    if text.is-empty: return []
    return (text.split ":").map: | group/string |
      if not 1 <= group.size <= 4: throw "BAD_FORMAT"
      int.parse group --radix=16 --on-error=: throw "BAD_FORMAT"

  is-ipv6 -> bool: return raw.size == 16

  operator == other:
    other-raw := other.raw
    if other-raw.size != raw.size: return false
    raw.size.repeat:
      if other-raw[it] != raw[it]: return false
    return true

  hash-code:
    code := 0
    raw.size.repeat:
      code *= 11
      code += raw[it]
      code &= 0xfffff
    return code

  stringify:
    if raw.size == 4:
      buffer := ""
      4.repeat:
        if it != 0: buffer += "."
        buffer += raw[it].stringify
      return buffer

    if raw.size == 16:
      buffer := ""
      8.repeat:
        if it != 0: buffer += ":"
        field := raw[it*2] << 8 | raw[it*2 + 1]
        buffer += field.stringify 16
        // TODO: Consider using compressed format.
      return buffer

    return "<invalid-ip>"

  to-byte-array: return raw

  /**
  Checks a string to detect if it is a parseable IP address.
  IPv4 addresses are checked for decimal dot notation, eg. 192.168.0.1
  IPv6 addresses are checked for hexadecimal colon notation, eg. 2001:db8::1234:5678
  */
  static is-valid str/string --accept-ipv4/bool=true --accept-ipv6/bool=false -> bool:
    if accept-ipv4 and ipv4-string_ str: return true
    return accept-ipv6 and ipv6-string_ str

  static ipv4-string_ str/string -> bool:
    dots := 0
    last-dot := -1
    str.size.repeat: | i |
      char := str[i]
      if char == '.':
        part := str[last-dot + 1..i]
        if not 1 <= part.size <= 3: return false
        if part.size != 1 and part[0] == '0': return false  // Leading zeros.
        if part.size == 3 and (int.parse part) > 255: return false  // Parts must be in byte range.
        last-dot = i
        dots++
      else:
        if not '0' <= char <= '9': return false
    return dots == 3 and last-dot != str.size - 1

  static ipv6-string_ str/string -> bool:
    found-double-colon := false
    parts := 0
    digits-since-last-colon := -1
    str.do:
      if it == ':':
        if digits-since-last-colon > 4:
          return false
        else if digits-since-last-colon > 0:
          parts++
        else if digits-since-last-colon == 0:
          if found-double-colon: return false
          found-double-colon = true
        digits-since-last-colon = 0
      else:
        hex-char-to-value it --if-error=(: return false)
        if digits-since-last-colon < 0:
          digits-since-last-colon = 1
        else:
          digits-since-last-colon++
    if 1 <= digits-since-last-colon <= 4:
      parts++
    else if not str.ends-with "::":
      return false
    return (parts == 8 or (parts < 8 and found-double-colon))
