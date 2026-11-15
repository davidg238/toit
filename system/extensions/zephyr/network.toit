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

import system.services show ServiceProvider ServiceResource
import system.api.network show NetworkService

import ..shared.network-base

// net.open on Zephyr: sockets run in the client process on the VM's own
// primitives (udp_zephyr.cc), so nothing is proxied, as on the host. Without a
// network stack in the build, the socket primitives fail at first use.
class ZephyrNetworkProvider extends NetworkServiceProviderBase:
  constructor:
    super "system/network/zephyr" --major=0 --minor=1

  connect client/int -> List:
    resource := ZephyrNetworkResource this client
    return [resource.serialize-for-rpc, NetworkService.PROXY-NONE, "thread"]

class ZephyrNetworkResource extends ServiceResource:
  constructor provider/ServiceProvider client/int:
    super provider client

  on-closed -> none:
