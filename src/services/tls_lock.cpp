#include "services/tls_lock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace services::tls {

namespace {

/** Created on first use; function-local statics are initialised thread-safely. */
SemaphoreHandle_t mutex() {
  static SemaphoreHandle_t s_mutex = xSemaphoreCreateMutex();
  return s_mutex;
}

}  // namespace

void lock() { xSemaphoreTake(mutex(), portMAX_DELAY); }

bool tryLock() { return xSemaphoreTake(mutex(), 0) == pdTRUE; }

void unlock() { xSemaphoreGive(mutex()); }

}  // namespace services::tls
