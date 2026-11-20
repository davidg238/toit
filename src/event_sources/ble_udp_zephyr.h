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

#if defined(TOIT_ZEPHYR) && defined(CONFIG_BT) && !defined(CONFIG_NET_SOCKETS)

#include "../resource.h"

namespace toit {

// UDP datagrams over Bluetooth LE to a relay (a phone) that forwards each one as
// real UDP: the transport is rete's udp_ble.c (rete 2b8e9291), whose wire is
// rete's protocol/RELAY-BLE.md. The node advertises, the relay bonds once and
// opens one L2CAP channel; one SDU is one datagram, ports and the host (as text)
// before the payload. Built when the board has Bluetooth and no IP stack: the
// backend follows the radio, as in rete.
//
// A socket's resource id is its index in the socket table. Bluetooth callbacks
// run on Zephyr's Bluetooth thread, which is not a Toit thread and must not take
// Toit locks: they fill the socket's ring, mark it pending and give a semaphore,
// and this source's own thread dispatches UDP_READ / UDP_WRITE.
class BleUdpEventSource : public EventSource, public Thread {
 public:
  static const int MAX_DATAGRAM = 1280;  // As udp_zephyr.cc.
  static const int MAX_SOCKS = 4;        // As rete.

  BleUdpEventSource();
  ~BleUdpEventSource() override;

  static BleUdpEventSource* instance() { return instance_; }

  // Whether the relay's channel is open.
  static bool ready();

  // A socket bound to port (0: an ephemeral port). Answers its id, or -1 with errno set.
  static int bind(int port);
  static int port(int id);
  // Answers len when sent or dropped (no channel: dropped, as UDP over a down
  // link), or -1 with errno: EAGAIN (no transmit buffer; UDP_WRITE follows when
  // one frees), EMSGSIZE, EBADF.
  static int send(int id, const uint8_t addr[16], int remote_port, const uint8_t* data, int len);
  // Copies the oldest datagram without consuming it: the payload (up to cap),
  // the sender's 16-byte address and port. Answers the payload length, or -1
  // when the socket's queue is empty. Datagrams that don't fit cap or whose
  // host is not IPv6 text are dropped here.
  static int peek(int id, uint8_t* payload, int cap, uint8_t addr[16], int* remote_port);
  static void consume(int id);
  // Frees the socket's slot and queue, so the next binding starts empty.
  static void release(int id);

 protected:
  void on_register_resource(Locker& locker, Resource* r) override;
  void on_unregister_resource(Locker& locker, Resource* r) override;

 private:
  void entry() override;

  bool stop_ = false;
  static BleUdpEventSource* instance_;
};

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_BT && !CONFIG_NET_SOCKETS
