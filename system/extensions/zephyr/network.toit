// Copyright (C) 2026 Toitware ApS.
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

import system.api.network show NetworkService
import system.base.network show NetworkModule NetworkResource NetworkState

import ..shared.network-base

THREAD-ATTACH-TIMEOUT-MS_ ::= 60_000  // As ztoit's boot-time attach was.
THREAD-ROLE-CHILD_ ::= 2  // otDeviceRole: child, router (3) and leader (4) are attached.
WIFI-CONNECT-TIMEOUT-MS_ ::= 30_000  // Association plus DHCP; not tuned.
LINK-THREAD_ ::= 1  // link-kind_.
LINK-WIFI_ ::= 2

/**
net.open on Zephyr: sockets run in the client process on the VM's own
  primitives (udp_zephyr.cc), so nothing is proxied, as on the host.
The build's link (Thread or WiFi, $link-kind_) comes up on the first open
  and goes down after the last close (the stock $NetworkState count). Without
  either in the build, open throws.
*/
class ZephyrNetworkProvider extends NetworkServiceProviderBase:
  state_ ::= NetworkState

  constructor:
    super "system/network/zephyr" --major=0 --minor=2 --tags=[link-name_]

  connect client/int -> List:
    state_.up: link-kind_ == LINK-WIFI_ ? WifiModule : ThreadModule
    try:
      resource := NetworkResource this client state_
      return [resource.serialize-for-rpc, NetworkService.PROXY-NONE, link-name_]
    finally: | is-exception exception |
      if is-exception:
        critical-do: state_.down

class ThreadModule implements NetworkModule:
  attached_ := false

  connect -> none:
    start := Time.monotonic-us
    thread-up_
    deadline := start + THREAD-ATTACH-TIMEOUT-MS_ * 1_000
    while thread-role_ < THREAD-ROLE-CHILD_:
      if Time.monotonic-us > deadline:
        print "thread: attach timed out"
        throw "thread: attach timed out"
      sleep --ms=100
    attached_ = true
    print "thread: up role=$thread-role_ in $((Time.monotonic-us - start) / 1_000) ms"

  disconnect -> none:
    // A failed attach disables Thread rather than leave it retrying.
    detached := thread-down_ (not attached_)
    print "thread: down $(detached ? "detached" : "sed")"

class WifiModule implements NetworkModule:
  connect -> none:
    start := Time.monotonic-us
    wifi-up_
    deadline := start + WIFI-CONNECT-TIMEOUT-MS_ * 1_000
    while not wifi-ready_:
      if Time.monotonic-us > deadline:
        wifi-down_
        print "wifi: connect timed out"
        throw "wifi: connect timed out"
      sleep --ms=100
    print "wifi: up in $((Time.monotonic-us - start) / 1_000) ms"

  disconnect -> none:
    wifi-down_
    print "wifi: down"

link-name_ -> string:
  return link-kind_ == LINK-WIFI_ ? "wifi" : "thread"

link-kind_ -> int:
  #primitive.zephyr.link-kind

wifi-up_ -> none:
  #primitive.zephyr.wifi-up

wifi-ready_ -> bool:
  #primitive.zephyr.wifi-ready

wifi-down_ -> none:
  #primitive.zephyr.wifi-down

thread-up_ -> none:
  #primitive.zephyr.thread-up

thread-role_ -> int:
  #primitive.zephyr.thread-role

thread-down_ disable/bool -> bool:
  #primitive.zephyr.thread-down
