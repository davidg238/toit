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

/**
net.open on Zephyr: sockets run in the client process on the VM's own
  primitives (udp_zephyr.cc), so nothing is proxied, as on the host.
Thread comes up on the first open and goes down after the last close
  (the stock $NetworkState count). Without Thread in the build, open throws.
*/
class ZephyrNetworkProvider extends NetworkServiceProviderBase:
  state_ ::= NetworkState

  constructor:
    super "system/network/zephyr" --major=0 --minor=2 --tags=["thread"]

  connect client/int -> List:
    state_.up: ThreadModule
    try:
      resource := NetworkResource this client state_
      return [resource.serialize-for-rpc, NetworkService.PROXY-NONE, "thread"]
    finally: | is-exception exception |
      if is-exception:
        critical-do: state_.down

class ThreadModule implements NetworkModule:
  connect -> none:
    start := Time.monotonic-us
    thread-up_
    deadline := start + THREAD-ATTACH-TIMEOUT-MS_ * 1_000
    while thread-role_ < THREAD-ROLE-CHILD_:
      if Time.monotonic-us > deadline:
        print "thread: attach timed out"
        throw "thread: attach timed out"
      sleep --ms=100
    print "thread: up role=$thread-role_ in $((Time.monotonic-us - start) / 1_000) ms"

  disconnect -> none:
    detached := thread-down_
    print "thread: down $(detached ? "detached" : "sed")"

thread-up_ -> none:
  #primitive.zephyr.thread-up

thread-role_ -> int:
  #primitive.zephyr.thread-role

thread-down_ -> bool:
  #primitive.zephyr.thread-down
