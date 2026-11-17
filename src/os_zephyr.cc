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

#include "top.h"

#ifdef TOIT_ZEPHYR

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>

#include "os.h"
#include "process.h"
#include "program.h"
#include "utils.h"

// --- 64-bit atomics ---------------------------------------------------------
// RV32 has no 64-bit atomic instructions, so GCC calls these helpers for
// std::atomic<uint64> (TwoSpaceHeap::total_bytes_allocated_). A spinlock
// gives the required ordering on any core count.

static struct k_spinlock atomic64_lock;

extern "C" uint64_t __atomic_load_8(const volatile void* ptr, int) {
  k_spinlock_key_t key = k_spin_lock(&atomic64_lock);
  uint64_t value = *static_cast<const volatile uint64_t*>(ptr);
  k_spin_unlock(&atomic64_lock, key);
  return value;
}

extern "C" uint64_t __atomic_fetch_add_8(volatile void* ptr, uint64_t value, int) {
  k_spinlock_key_t key = k_spin_lock(&atomic64_lock);
  volatile uint64_t* target = static_cast<volatile uint64_t*>(ptr);
  uint64_t previous = *target;
  *target = previous + value;
  k_spin_unlock(&atomic64_lock, key);
  return previous;
}

extern "C" uint64_t __atomic_fetch_sub_8(volatile void* ptr, uint64_t value, int) {
  k_spinlock_key_t key = k_spin_lock(&atomic64_lock);
  volatile uint64_t* target = static_cast<volatile uint64_t*>(ptr);
  uint64_t previous = *target;
  *target = previous - value;
  k_spin_unlock(&atomic64_lock, key);
  return previous;
}

// --- File descriptors -------------------------------------------------------
// No filesystem. Picolibc's fopen still links against these; the host-only
// paths that call it (ar archives, snapshot bundles) fail cleanly with ENOSYS.

extern "C" int open(const char*, int, ...) { errno = ENOSYS; return -1; }
extern "C" int close(int) { errno = ENOSYS; return -1; }
extern "C" ssize_t read(int, void*, size_t) { errno = ENOSYS; return -1; }
extern "C" ssize_t write(int, const void*, size_t) { errno = ENOSYS; return -1; }
extern "C" off_t lseek(int, off_t, int) { errno = ENOSYS; return -1; }

namespace toit {

// --- Time -------------------------------------------------------------------
// os.cc derives monotonic time from this and reads real time via sys_clock.

int64 OS::get_system_time() {
  return static_cast<int64>(k_ticks_to_us_floor64(k_uptime_ticks()));
}

bool OS::set_real_time(struct timespec* time) {
  return sys_clock_settime(SYS_CLOCK_REALTIME, time) == 0;
}

int OS::num_cores() {
  return CONFIG_MP_MAX_NUM_CPUS;
}

void OS::close(int fd) {}

// --- Threads ----------------------------------------------------------------
// The current Thread* is found from k_current_get() (see Thread::current()).
// Stacks come from the libc heap, aligned as
// K_KERNEL_STACK_DEFINE would; valid because CONFIG_USERSPACE is off.

static const int DEFAULT_STACK_SIZE = 4 * KB;

struct ThreadData {
  struct k_thread thread;
  k_thread_stack_t* stack;
  struct k_sem terminated;
  Thread* owner;  // Thread::current() finds it from k_current_get().
};

Thread::Thread(const char* name)
    : name_(name)
    , handle_(null)
    , locker_(null) {}

void* thread_start(void* arg) {
  Thread* thread = unvoid_cast<Thread*>(arg);
  thread->_boot();
  return null;
}

static void zephyr_thread_start(void* arg, void*, void*) {
  thread_start(arg);
}

// The current Toit thread is found from the Zephyr thread, with no per-thread
// slot: a spawned thread's k_thread sits in its ThreadData, and the one thread
// Toit did not spawn (main, the system thread) is recorded here. Not
// k_thread_custom_data: Zephyr has one such slot, and Espressif's WiFi adapter
// (esp_wifi_adapter.c) keeps a semaphore in it, set on main during WiFi init.
// Not __thread: the ESP32 has no ARCH_HAS_THREAD_LOCAL_STORAGE in Zephyr 4.2,
// where it compiles to one global shared by all threads (ztoit, rete #521).
static k_tid_t system_tid_ = null;
static Thread* system_thread_ = null;

void Thread::_boot() {
  ASSERT(current() == this);
  entry();
  if (handle_ != null) {
    k_sem_give(&reinterpret_cast<ThreadData*>(handle_)->terminated);
  }
}

bool Thread::spawn(int stack_size, int core) {
  ThreadData* data = _new ThreadData();
  if (data == null) return false;
  if (stack_size == 0) stack_size = DEFAULT_STACK_SIZE;
  size_t align = Z_KERNEL_STACK_OBJ_ALIGN;
  size_t length = Utils::round_up(K_KERNEL_STACK_LEN(stack_size), align);
  data->stack = static_cast<k_thread_stack_t*>(aligned_alloc(align, length));
  if (data->stack == null) {
    delete data;
    return false;
  }
  k_sem_init(&data->terminated, 0, 1);
  data->owner = this;
  handle_ = void_cast(data);
  k_tid_t tid = k_thread_create(&data->thread, data->stack, stack_size,
                                zephyr_thread_start, this, null, null,
                                K_LOWEST_APPLICATION_THREAD_PRIO, 0, K_NO_WAIT);
  k_thread_name_set(tid, name_);
  return true;
}

void Thread::run() {
  ASSERT(handle_ == null);
  thread_start(void_cast(this));
}

void Thread::join() {
  ASSERT(handle_ != null);
  auto data = reinterpret_cast<ThreadData*>(handle_);
  k_sem_take(&data->terminated, K_FOREVER);
  k_thread_join(&data->thread, K_FOREVER);
  free(data->stack);
  delete data;
  handle_ = null;
}

void Thread::ensure_system_thread() {
  if (system_thread_ != null) return;
  Thread* thread = _new SystemThread();
  if (thread == null) FATAL("unable to allocate SystemThread");
  system_tid_ = k_current_get();
  system_thread_ = thread;
}

Thread* Thread::current() {
  k_tid_t tid = k_current_get();
  Thread* result = (tid == system_tid_)
      ? system_thread_
      : CONTAINER_OF(tid, ThreadData, thread)->owner;
  if (result == null) FATAL("thread must be present");
  return result;
}

// --- Mutexes and condition variables ----------------------------------------

class ConditionVariable {
 public:
  explicit ConditionVariable(Mutex* mutex) : mutex_(mutex) {
    k_condvar_init(&condvar_);
  }

  void wait() {
    if (!mutex_->is_locked()) FATAL("wait on unlocked mutex");
    k_condvar_wait(&condvar_, &mutex_->mutex_, K_FOREVER);
  }

  // Returns true if signaled, false on timeout.
  bool wait_us(int64 us) {
    if (us <= 0LL) return false;
    if (!mutex_->is_locked()) FATAL("wait on unlocked mutex");
    return k_condvar_wait(&condvar_, &mutex_->mutex_, K_USEC(us)) == 0;
  }

  void signal() {
    if (!mutex_->is_locked()) FATAL("signal on unlocked mutex");
    k_condvar_signal(&condvar_);
  }

  void signal_all() {
    if (!mutex_->is_locked()) FATAL("signal_all on unlocked mutex");
    k_condvar_broadcast(&condvar_);
  }

 private:
  Mutex* mutex_;
  struct k_condvar condvar_;
};

Mutex* OS::allocate_mutex(int level, const char* title) { return _new Mutex(level, title); }
void OS::dispose(Mutex* mutex) { delete mutex; }
bool OS::is_locked(Mutex* mutex) { return mutex->is_locked(); }
void OS::lock(Mutex* mutex) { mutex->lock(); }
void OS::unlock(Mutex* mutex) { mutex->unlock(); }

ConditionVariable* OS::allocate_condition_variable(Mutex* mutex) { return _new ConditionVariable(mutex); }
void OS::wait(ConditionVariable* condition) { condition->wait(); }
bool OS::wait_us(ConditionVariable* condition, int64 us) { return condition->wait_us(us); }
void OS::signal(ConditionVariable* condition) { condition->signal(); }
void OS::signal_all(ConditionVariable* condition) { condition->signal_all(); }
void OS::dispose(ConditionVariable* condition) { delete condition; }

// --- Toit heap pages --------------------------------------------------------
// Pages come from one static, page-aligned arena, so the GC metadata covers
// exactly it. A page map, not sys_heap: sys_heap keeps its struct at the arena
// start and an 8-byte header before each block, so page-aligned blocks could
// never be adjacent and a 48 KB arena yielded 5 pages of 12 (rete #521).

// CONFIG_TOIT_HEAP_NOINIT puts the arena in .noinit, which the ESP32 links into
// its second DRAM bank (CONFIG_ESP32_REGION_1_NOINIT), leaving the first to the C
// heap that thread stacks and GC metadata come from (ztoit WROOM-32, rete #521).
#if defined(CONFIG_TOIT_HEAP_NOINIT)
static uint8 toit_arena[CONFIG_TOIT_HEAP_SIZE] __noinit __aligned(TOIT_PAGE_SIZE);
#else
static uint8 toit_arena[CONFIG_TOIT_HEAP_SIZE] __aligned(TOIT_PAGE_SIZE);
#endif
static const int TOIT_ARENA_PAGES = CONFIG_TOIT_HEAP_SIZE / TOIT_PAGE_SIZE;
static_assert(CONFIG_TOIT_HEAP_SIZE % TOIT_PAGE_SIZE == 0, "arena must be whole pages");
static bool toit_page_used[TOIT_ARENA_PAGES];
static struct k_spinlock toit_heap_lock;

// First fit over runs of free pages.
void* OS::allocate_pages(uword size) {
  int count = Utils::round_up(size, TOIT_PAGE_SIZE) / TOIT_PAGE_SIZE;
  void* result = null;
  k_spinlock_key_t key = k_spin_lock(&toit_heap_lock);
  int run = 0;
  for (int i = 0; i < TOIT_ARENA_PAGES; i++) {
    run = toit_page_used[i] ? 0 : run + 1;
    if (run == count) {
      int first = i - count + 1;
      for (int j = first; j <= i; j++) toit_page_used[j] = true;
      result = toit_arena + first * TOIT_PAGE_SIZE;
      break;
    }
  }
  k_spin_unlock(&toit_heap_lock, key);
  return result;
}

void OS::free_pages(void* address, uword size) {
  int first = (static_cast<uint8*>(address) - toit_arena) / TOIT_PAGE_SIZE;
  int count = Utils::round_up(size, TOIT_PAGE_SIZE) / TOIT_PAGE_SIZE;
  k_spinlock_key_t key = k_spin_lock(&toit_heap_lock);
  for (int j = first; j < first + count; j++) toit_page_used[j] = false;
  k_spin_unlock(&toit_heap_lock, key);
}

OS::HeapMemoryRange OS::get_heap_memory_range() {
  HeapMemoryRange range;
  range.address = toit_arena;
  range.size = sizeof(toit_arena);
  return range;
}

void* OS::grab_virtual_memory(void* address, uword size) { return malloc(size); }
void OS::ungrab_virtual_memory(void* address, uword size) { free(address); }
bool OS::use_virtual_memory(void* address, uword size) { return true; }
void OS::unuse_virtual_memory(void* address, uword size) {}

// --- Lifecycle and reporting ------------------------------------------------

void OS::set_up() {
  Thread::ensure_system_thread();
  set_up_mutexes();
}

void OS::tear_down() {}

const char* OS::get_platform() { return "Zephyr"; }
const char* OS::get_architecture() { return CONFIG_ARCH; }

int OS::read_entire_file(char* name, uint8** buffer) { return -1; }

void OS::out_of_memory(const char* reason) {
  FATAL("%s", reason);
}

void OS::set_heap_tag(word tag) {}
word OS::get_heap_tag() { return 0; }

void OS::heap_summary_report(int max_pages, const char* marker, Process* process) {
  const uint8* uuid = process->program()->id();
  printf("%s process %d: %08x-%04x-%04x-%04x-%04x%08x.\n",
      (marker && strlen(marker) > 0) ? marker : "Out of memory",
      process->id(),
      static_cast<int>(Utils::read_unaligned_uint32_be(uuid)),
      static_cast<int>(Utils::read_unaligned_uint16_be(uuid + 4)),
      static_cast<int>(Utils::read_unaligned_uint16_be(uuid + 6)),
      static_cast<int>(Utils::read_unaligned_uint16_be(uuid + 8)),
      static_cast<int>(Utils::read_unaligned_uint16_be(uuid + 10)),
      static_cast<int>(Utils::read_unaligned_uint32_be(uuid + 12)));
}

char* OS::getenv(const char* variable) { UNIMPLEMENTED(); }
bool OS::setenv(const char* variable, const char* value) { UNIMPLEMENTED(); }
bool OS::unsetenv(const char* variable) { UNIMPLEMENTED(); }

}  // namespace toit

#endif  // TOIT_ZEPHYR
