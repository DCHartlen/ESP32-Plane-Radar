#pragma once

namespace services::tls {

/**
 * One HTTPS session at a time. Each takes ~48 KB of internal RAM at its handshake peak
 * (mbedTLS buffers are internal in this framework build), and there's room for one.
 */
void lock();
/** Takes the lock only if it's free; for callers that would rather retry later. */
bool tryLock();
void unlock();

/** Holds the lock until release() or the end of the scope. */
class Guard {
 public:
  Guard() { lock(); }
  ~Guard() { release(); }
  void release() {
    if (held_) {
      held_ = false;
      unlock();
    }
  }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  bool held_ = true;
};

}  // namespace services::tls
