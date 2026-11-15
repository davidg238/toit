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

#if defined(TOIT_ZEPHYR) && defined(CONFIG_NET_SOCKETS)

#include <zephyr/net/socket.h>
#include <zephyr/zvfs/eventfd.h>

#include "../objects_inline.h"
#include "zsock_zephyr.h"

namespace toit {

ZsockEventSource* ZsockEventSource::instance_ = null;

ZsockEventSource::ZsockEventSource() : EventSource("Zsock"), Thread("Zsock") {
  ASSERT(instance_ == null);
  instance_ = this;
  for (auto& e : entries_) e = { -1, 0, false };
  wake_fd_ = zvfs_eventfd(0, ZVFS_EFD_NONBLOCK);
  if (wake_fd_ < 0) FATAL("zsock event source: no eventfd (%d)", errno);
  if (!spawn(4096)) FATAL("zsock event source: no thread");
}

ZsockEventSource::~ZsockEventSource() {
  { Locker locker(mutex()); stop_ = true; }
  wake();
  join();
  zsock_close(wake_fd_);
  instance_ = null;
}

void ZsockEventSource::wake() {
  zvfs_eventfd_write(wake_fd_, 1);
}

ZsockEventSource::Entry* ZsockEventSource::find(int fd) {
  for (auto& e : entries_) if (e.fd == fd) return &e;
  return null;
}

void ZsockEventSource::rearm(int fd, short events) {
  { Locker locker(mutex());
    Entry* e = find(fd);
    if (e == null || e->closing) return;
    e->armed |= events;
  }
  wake();
}

void ZsockEventSource::on_register_resource(Locker& locker, Resource* resource) {
  int fd = static_cast<IntResource*>(resource)->id();
  Entry* e = find(-1);
  if (e == null) FATAL("zsock event source: more than %d sockets", MAX_FDS);
  *e = { fd, ZSOCK_POLLIN | ZSOCK_POLLOUT, false };
  wake();
}

void ZsockEventSource::on_unregister_resource(Locker& locker, Resource* resource) {
  Entry* e = find(static_cast<IntResource*>(resource)->id());
  if (e != null) e->closing = true;
  wake();
}

void ZsockEventSource::entry() {
  // Zephyr's poll fails with ENOMEM above CONFIG_ZVFS_POLL_MAX fds, and its fd
  // table holds CONFIG_ZVFS_OPEN_MAX: size both for every socket plus the wake fd.
  static_assert(CONFIG_ZVFS_POLL_MAX >= MAX_FDS + 1, "CONFIG_ZVFS_POLL_MAX < sockets + wake fd");
  static_assert(CONFIG_ZVFS_OPEN_MAX >= MAX_FDS + 1, "CONFIG_ZVFS_OPEN_MAX < sockets + wake fd");
  struct zsock_pollfd fds[MAX_FDS + 1];
  while (true) {
    int count = 0;
    fds[count++] = { wake_fd_, ZSOCK_POLLIN, 0 };
    { Locker locker(mutex());
      if (stop_) return;
      for (auto& e : entries_) {
        if (e.fd < 0) continue;
        if (e.closing) {
          // Only this thread polls, so closing here never races a poll.
          zsock_close(e.fd);
          e = { -1, 0, false };
          continue;
        }
        // Errors are always reported; an fd with nothing armed is skipped.
        if (e.armed != 0) fds[count++] = { e.fd, e.armed, 0 };
      }
    }
    int ready = zsock_poll(fds, count, -1);
    if (ready < 0) {
      if (errno == EINTR) continue;
      FATAL("zsock_poll: %d", errno);
    }
    if (fds[0].revents & ZSOCK_POLLIN) {
      zvfs_eventfd_t value;
      zvfs_eventfd_read(wake_fd_, &value);
    }
    Locker locker(mutex());
    for (int i = 1; i < count; i++) {
      short revents = fds[i].revents;
      if (revents == 0) continue;
      Entry* e = find(fds[i].fd);
      if (e == null || e->closing) continue;
      e->armed &= ~(revents & (ZSOCK_POLLIN | ZSOCK_POLLOUT));
      Resource* r = find_resource_by_id(locker, e->fd);
      if (r != null) dispatch(locker, r, revents);
    }
  }
}

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_NET_SOCKETS
