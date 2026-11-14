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

#include "../resource.h"
#include "../os.h"

namespace toit {

// Socket readiness for Zephyr's BSD sockets, the counterpart of
// EpollEventSource. zsock_poll is level-triggered, so each fd is armed: a
// readiness event is dispatched once and disarmed, and the primitive that
// later gets EAGAIN re-arms it (rearm). Toit's monitor clears its state
// after the EAGAIN and waits; the notification from a re-armed dispatch is
// processed after that clear, so no wakeup is lost.
class ZsockEventSource : public EventSource, public Thread {
 public:
  static ZsockEventSource* instance() { return instance_; }

  ZsockEventSource();
  ~ZsockEventSource() override;

  // Re-arm `events` (ZSOCK_POLLIN/ZSOCK_POLLOUT) for fd after an EAGAIN.
  void rearm(int fd, short events);

 protected:
  void on_register_resource(Locker& locker, Resource* resource) override;
  void on_unregister_resource(Locker& locker, Resource* resource) override;

 private:
  static const int MAX_FDS = 8;
  struct Entry {
    int fd;           // -1 when free.
    short armed;      // Events still to report.
    bool closing;     // Unregistered: the poll thread closes it.
  };

  void entry() override;
  void wake();
  Entry* find(int fd);

  Entry entries_[MAX_FDS];
  int wake_fd_ = -1;
  bool stop_ = false;

  static ZsockEventSource* instance_;
};

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_NET_SOCKETS
