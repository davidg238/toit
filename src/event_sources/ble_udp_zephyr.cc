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

#if defined(TOIT_ZEPHYR) && defined(CONFIG_BT) && !defined(CONFIG_NET_SOCKETS)

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/settings/settings.h>

#include "../objects_inline.h"
#include "../resources/ipv6_text.h"
#include "../resources/relay_frame.h"
#include "../resources/udp.h"
#include "ble_udp_zephyr.h"

// The transport below is rete's udp_ble.c (rete 2b8e9291) with its reasons kept;
// what changed: the doorbell is this event source, rete's radio on/off and link
// doorbell are gone, a port can't be bound twice, and the datagram bound is
// MAX_DATAGRAM.

namespace toit {

static const uint16_t RELAY_PSM = 0x0080;  // Fixed by the contract; the relay dials it.
static const int SDU_MTU = RELAY_FRAME_HDR_MAX + BleUdpEventSource::MAX_DATAGRAM;
static const int MAX_SOCKS = BleUdpEventSource::MAX_SOCKS;
static const int EPHEMERAL_BASE = 49152;
// Each socket's ring holds two of the largest frames with their length prefix.
// rete's 1 KB a socket would drop a datagram over ~1 KB, which udp_zephyr.cc allows.
static const int RING_PER_SOCK = 2 * (2 + SDU_MTU);

// The relay's service UUID, so the phone can filter its scan.
#define RELAY_UUID BT_UUID_128_ENCODE(0x6f657673, 0x6d00, 0x4000, 0x8000, 0x00000000439aULL)

static void tx_buf_freed(struct net_buf* buf);
NET_BUF_POOL_DEFINE(ble_udp_tx_pool, 2, BT_L2CAP_SDU_BUF_SIZE(SDU_MTU), CONFIG_BT_CONN_TX_USER_DATA_SIZE, tx_buf_freed);
NET_BUF_POOL_DEFINE(ble_udp_rx_pool, 1, BT_L2CAP_SDU_BUF_SIZE(SDU_MTU), 8, NULL);

static struct bt_l2cap_le_chan le_chan;
static volatile bool chan_up;
static const int UNBOUND = -1;
static int sock_port[MAX_SOCKS];  // 0 = free, UNBOUND = created; under sock_lock.
static K_MUTEX_DEFINE(sock_lock);

// Received datagrams, whole frames, each stored as [u16 len][frame], one ring per
// socket, as UDP keeps one queue per socket (rete#449: one shared ring made each
// socket wait behind the other's reply). Written by the Bluetooth thread, read by
// the VM thread.
static struct { uint8_t b[RING_PER_SOCK]; size_t head, tail, used; } rings[MAX_SOCKS];
static K_MUTEX_DEFINE(ring_lock);

// Event source wakeups: a bit per socket with a new datagram, and a flag for a
// freed transmit buffer.
static atomic_t pending_read;
static atomic_t pending_write;
K_SEM_DEFINE(ble_udp_sem, 0, 1);

static void ring_put(int s, const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    rings[s].b[rings[s].tail] = p[i];
    rings[s].tail = (rings[s].tail + 1) % RING_PER_SOCK;
  }
  rings[s].used += n;
}
static void ring_peek(int s, size_t off, uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) p[i] = rings[s].b[(rings[s].head + off + i) % RING_PER_SOCK];
}
static void ring_drop(int s, size_t n) {
  rings[s].head = (rings[s].head + n) % RING_PER_SOCK;
  rings[s].used -= n;
}
static void ring_clear(int s) {
  rings[s].head = rings[s].tail = rings[s].used = 0;
}

static struct net_buf* alloc_buf_cb(struct bt_l2cap_chan* chan) {
  (void)chan;
  return net_buf_alloc(&ble_udp_rx_pool, K_NO_WAIT);
}

static int recv_cb(struct bt_l2cap_chan* chan, struct net_buf* buf) {
  (void)chan;
  uint16_t len = buf->len;
  k_mutex_lock(&ring_lock, K_FOREVER);
  // Malformed, for an unbound port, or a full ring: drop, as UDP would. sock_port
  // is read without sock_lock (the VM takes sock_lock then ring_lock; this takes
  // only ring_lock): a socket bound or closed mid-frame costs that one datagram.
  struct relay_frame f;
  int s = -1;
  if (relay_frame_parse(buf->data, len, &f) == 0) {
    for (int i = 0; i < MAX_SOCKS; i++) {
      if (sock_port[i] != 0 && sock_port[i] == f.local_port) { s = i; break; }  // 0 is a free slot.
    }
  }
  bool queued = false;
  if (s >= 0 && rings[s].used + 2 + len <= (size_t)RING_PER_SOCK) {
    uint8_t l[2];
    sys_put_be16(len, l);
    ring_put(s, l, 2);
    ring_put(s, buf->data, len);
    queued = true;
  }
  k_mutex_unlock(&ring_lock);
  if (queued) {  // After ring_put: readable before the wakeup.
    atomic_or(&pending_read, BIT(s));
    k_sem_give(&ble_udp_sem);
  }
  return 0;  // The stack frees buf.
}

// A sender that got EAGAIN waits for UDP_WRITE: signal it when a transmit buffer is
// back in the pool. The channel's sent callback is not used, since nothing orders it
// after the buffer's release.
static void wake_writers() {
  atomic_set(&pending_write, 1);
  k_sem_give(&ble_udp_sem);
}
static void tx_buf_freed(struct net_buf* buf) {
  net_buf_destroy(buf);
  wake_writers();
}

static void connected_cb(struct bt_l2cap_chan* chan) {
  printk("ble: relay channel up (tx mtu %u, rx mtu %u)\n",
         BT_L2CAP_LE_CHAN(chan)->tx.mtu, BT_L2CAP_LE_CHAN(chan)->rx.mtu);
  chan_up = true;
}

// The channel is the only reason the link exists: when it closes, drop the link
// too, so the node advertises again. Left up, a link without a channel holds the
// node's one connection and the relay, scanning for an advertiser, never finds it.
static void disconnected_cb(struct bt_l2cap_chan* chan) {
  chan_up = false;
  printk("ble: relay channel down\n");
  if (chan->conn) (void)bt_conn_disconnect(chan->conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
  wake_writers();  // A sender waiting for a buffer now drops instead.
}

static const struct bt_l2cap_chan_ops chan_ops = {
  .connected = connected_cb,
  .disconnected = disconnected_cb,
  .alloc_buf = alloc_buf_cb,
  .recv = recv_cb,
};

static int accept_cb(struct bt_conn* conn, struct bt_l2cap_server* server, struct bt_l2cap_chan** chan) {
  (void)conn;
  (void)server;
  if (chan_up) return -EBUSY;  // One relay at a time.
  memset(&le_chan, 0, sizeof le_chan);
  le_chan.chan.ops = &chan_ops;
  le_chan.rx.mtu = SDU_MTU;
  *chan = &le_chan.chan;
  return 0;
}

static struct bt_l2cap_server server = {
  .psm = RELAY_PSM,
  .sec_level = BT_SECURITY_L2,  // Encrypted: bonded phones only.
  .accept = accept_cb,
};

static const struct bt_data ad[] = {
  BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
  BT_DATA_BYTES(BT_DATA_UUID128_ALL, RELAY_UUID),
};
static const struct bt_data sd[] = {
  BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static void adv_start() {
  int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
  if (err && err != -EALREADY) printk("ble: advertising failed (%d)\n", err);
}

// A connection consumes the advertiser; start it again once the connection object
// is free, so the relay can always find the node after a drop.
static void recycled_cb() { adv_start(); }
static void conn_connected(struct bt_conn* conn, uint8_t err) {
  (void)conn;
  printk("ble: link up (err %u)\n", err);
}
static void conn_disconnected(struct bt_conn* conn, uint8_t reason) {
  (void)conn;
  printk("ble: link down (reason 0x%02x)\n", reason);
}
BT_CONN_CB_DEFINE(ble_udp_conn_cbs) = {
  .connected = conn_connected,
  .disconnected = conn_disconnected,
  .recycled = recycled_cb,
};

static void count_bond(const struct bt_bond_info* info, void* n) {
  (void)info;
  (*static_cast<int*>(n))++;
}

// Pair once (rete#439): the first phone bonds, every later pairing is refused. A
// full flash erase drops the bond, which is how a lost phone is replaced.
static enum bt_security_err pairing_accept(struct bt_conn* conn, const struct bt_conn_pairing_feat* feat) {
  (void)conn;
  (void)feat;
  int bonds = 0;
  bt_foreach_bond(BT_ID_DEFAULT, count_bond, &bonds);
  if (bonds > 0) {
    printk("ble: pairing refused, already bonded\n");
    return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
  }
  return BT_SECURITY_ERR_SUCCESS;
}
static void pairing_complete(struct bt_conn* conn, bool bonded) {
  (void)conn;
  printk("ble: paired, %s\n", bonded ? "bonded" : "NOT bonded");
}
static void pairing_failed(struct bt_conn* conn, enum bt_security_err reason) {
  (void)conn;
  printk("ble: pairing failed (%d)\n", reason);
}

static struct bt_conn_auth_cb auth_cb = { .pairing_accept = pairing_accept };
static struct bt_conn_auth_info_cb auth_info_cb = {
  .pairing_complete = pairing_complete,
  .pairing_failed = pairing_failed,
};

// The radio comes up with the VM, not on net.open, so the bonded relay can
// reconnect before the first datagram.
static void radio_start() {
  int err = bt_enable(null);
  if (err) { printk("ble: bt_enable failed (%d)\n", err); return; }
  (void)settings_subsys_init();
  (void)settings_load_subtree("bt");  // The bond, if there is one.
  bt_conn_auth_cb_register(&auth_cb);
  bt_conn_auth_info_cb_register(&auth_info_cb);
  err = bt_l2cap_server_register(&server);
  if (err) { printk("ble: l2cap server failed (%d)\n", err); return; }
  adv_start();
  printk("ble: advertising as \"%s\", relay psm 0x%04x\n", CONFIG_BT_DEVICE_NAME, RELAY_PSM);
}

BleUdpEventSource* BleUdpEventSource::instance_ = null;

BleUdpEventSource::BleUdpEventSource() : EventSource("BleUdp"), Thread("BleUdp") {
  ASSERT(instance_ == null);
  instance_ = this;
  radio_start();
  if (!spawn(4096)) FATAL("ble udp event source: no thread");
}

BleUdpEventSource::~BleUdpEventSource() {
  { Locker locker(mutex()); stop_ = true; }
  k_sem_give(&ble_udp_sem);
  join();
  instance_ = null;
}

void BleUdpEventSource::entry() {
  while (true) {
    k_sem_take(&ble_udp_sem, K_FOREVER);
    Locker locker(mutex());
    if (stop_) return;
    uint32_t bits = static_cast<uint32_t>(atomic_clear(&pending_read));
    bool write = atomic_clear(&pending_write) != 0;
    for (int id = 0; id < MAX_SOCKS; id++) {
      word data = ((bits >> id) & 1 ? UDP_READ : 0) | (write ? UDP_WRITE : 0);
      if (data == 0) continue;
      IntResource* resource = find_resource_by_id(locker, id);
      if (resource != null) dispatch(locker, resource, data);
    }
  }
}

// A new socket can send at once; a full buffer pool later clears that until a buffer frees.
void BleUdpEventSource::on_register_resource(Locker& locker, Resource* r) {
  dispatch(locker, r, UDP_WRITE);
}

void BleUdpEventSource::on_unregister_resource(Locker& locker, Resource* r) {
  USE(locker);
  release(static_cast<IntResource*>(r)->id());
}

void BleUdpEventSource::release(int id) {
  if (id < 0 || id >= MAX_SOCKS) return;
  k_mutex_lock(&sock_lock, K_FOREVER);
  sock_port[id] = 0;
  k_mutex_lock(&ring_lock, K_FOREVER);
  ring_clear(id);
  k_mutex_unlock(&ring_lock);
  k_mutex_unlock(&sock_lock);
}

bool BleUdpEventSource::ready() { return chan_up; }

int BleUdpEventSource::create() {
  int id = -1;
  k_mutex_lock(&sock_lock, K_FOREVER);
  for (int i = 0; i < MAX_SOCKS; i++) {
    if (sock_port[i] == 0) {
      k_mutex_lock(&ring_lock, K_FOREVER);
      ring_clear(i);  // Nothing stale reaches the new socket.
      sock_port[i] = UNBOUND;
      k_mutex_unlock(&ring_lock);
      id = i;
      break;
    }
  }
  k_mutex_unlock(&sock_lock);
  if (id < 0) errno = EMFILE;
  return id;
}

int BleUdpEventSource::bind_port(int id, int port) {
  if (port < 0 || port > 0xffff) { errno = EINVAL; return -1; }
  int result = 0;
  k_mutex_lock(&sock_lock, K_FOREVER);
  if (id < 0 || id >= MAX_SOCKS || sock_port[id] != UNBOUND) {
    errno = EBADF;  // Free, or bound already.
    result = -1;
  } else if (port != 0) {
    for (int i = 0; i < MAX_SOCKS; i++) {
      if (sock_port[i] == port) { errno = EADDRINUSE; result = -1; }
    }
  } else {  // The lowest ephemeral port no socket holds.
    for (port = EPHEMERAL_BASE; ; port++) {
      bool used = false;
      for (int i = 0; i < MAX_SOCKS; i++) used = used || sock_port[i] == port;
      if (!used) break;
    }
  }
  if (result == 0) sock_port[id] = port;
  k_mutex_unlock(&sock_lock);
  return result;
}

int BleUdpEventSource::bind(int port) {
  int id = create();
  if (id < 0) return -1;
  if (bind_port(id, port) != 0) {
    int error = errno;
    release(id);
    errno = error;
    return -1;
  }
  return id;
}

int BleUdpEventSource::port(int id) {
  if (id < 0 || id >= MAX_SOCKS) return 0;
  k_mutex_lock(&sock_lock, K_FOREVER);
  int p = sock_port[id];
  k_mutex_unlock(&sock_lock);
  return p < 0 ? 0 : p;
}

int BleUdpEventSource::send(int id, const uint8_t addr[16], int remote_port, const uint8_t* data, int len) {
  if (len < 0 || len > MAX_DATAGRAM) { errno = EMSGSIZE; return -1; }
  char host[IPV6_TEXT_MAX];
  int hl = ipv6_format(addr, host, sizeof host);
  k_mutex_lock(&sock_lock, K_FOREVER);
  int result = len;
  if (id < 0 || id >= MAX_SOCKS || sock_port[id] <= 0) {  // Free or unbound.
    errno = EBADF;
    result = -1;
  } else if (chan_up) {  // Down: dropped, as UDP over a down link (rete).
    struct net_buf* buf = net_buf_alloc(&ble_udp_tx_pool, K_NO_WAIT);
    if (buf == null) {
      errno = EAGAIN;
      result = -1;
    } else {
      net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
      int h = relay_frame_put_header(net_buf_tail(buf), net_buf_tailroom(buf),
                                     static_cast<uint16_t>(sock_port[id]), static_cast<uint16_t>(remote_port),
                                     host, hl);
      if (h < 0 || static_cast<size_t>(h + len) > net_buf_tailroom(buf)) {
        net_buf_unref(buf);
        errno = EMSGSIZE;
        result = -1;
      } else {
        net_buf_add(buf, h);
        net_buf_add_mem(buf, data, len);
        int err = bt_l2cap_chan_send(&le_chan.chan, buf);
        if (err < 0) net_buf_unref(buf);
        if (err == -EMSGSIZE) {  // Over the relay's receive MTU.
          errno = EMSGSIZE;
          result = -1;
        }
        // Any other error is a channel that closed under us: dropped, as above.
      }
    }
  }
  k_mutex_unlock(&sock_lock);
  return result;
}

int BleUdpEventSource::peek(int id, uint8_t* payload, int cap, uint8_t addr[16], int* remote_port) {
  if (id < 0 || id >= MAX_SOCKS) return -1;
  int n = -1;
  k_mutex_lock(&sock_lock, K_FOREVER);  // Then ring_lock; recv_cb takes only ring_lock.
  k_mutex_lock(&ring_lock, K_FOREVER);
  while (rings[id].used > 0) {
    uint8_t l[2], h[RELAY_FRAME_HDR_MAX];
    struct relay_frame f;
    ring_peek(id, 0, l, 2);
    uint16_t flen = sys_get_be16(l);
    ring_peek(id, 2, h, flen < sizeof h ? flen : sizeof h);
    if (relay_frame_parse(h, flen, &f) != 0 || f.local_port != sock_port[id] ||
        f.payload_len > static_cast<size_t>(cap) ||
        ipv6_parse(reinterpret_cast<const char*>(h + f.host_off), f.host_len, addr) != 0) {
      ring_drop(id, 2 + flen);  // A previous binding's, malformed, too big, or not IPv6.
      continue;
    }
    *remote_port = f.remote_port;
    ring_peek(id, 2 + f.payload_off, payload, f.payload_len);
    n = static_cast<int>(f.payload_len);
    break;
  }
  k_mutex_unlock(&ring_lock);
  k_mutex_unlock(&sock_lock);
  return n;
}

void BleUdpEventSource::consume(int id) {
  if (id < 0 || id >= MAX_SOCKS) return;
  k_mutex_lock(&ring_lock, K_FOREVER);
  if (rings[id].used > 0) {
    uint8_t l[2];
    ring_peek(id, 0, l, 2);
    ring_drop(id, 2 + sys_get_be16(l));
  }
  k_mutex_unlock(&ring_lock);
}

}  // namespace toit

#endif  // TOIT_ZEPHYR && CONFIG_BT && !CONFIG_NET_SOCKETS
