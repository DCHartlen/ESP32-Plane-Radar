#include "hardware/buttons.h"

#include <Arduino.h>

#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "config.h"
#include "hardware/panel.h"

namespace {

constexpr UBaseType_t kEventQueueDepth = 4;
constexpr uint32_t kTaskStackBytes = 3072;
constexpr UBaseType_t kTaskPriority = 1;
constexpr BaseType_t kTaskCore = 0;

struct ButtonState {
  uint8_t pca_pin;
  ButtonEvent tap_event;
  bool last_raw = false;
  bool pressed = false;     // debounced
  bool armed = false;       // a release has been seen (ignores a hold carried over from reset)
  bool long_fired = false;  // this press already passed kResetHoldMs
  TickType_t pressed_at = 0;
};

QueueHandle_t s_events = nullptr;
std::atomic<bool> s_reset_requested{false};
ButtonState s_up{config::kExpanderPinButtonUp, ButtonEvent::Up};
ButtonState s_down{config::kExpanderPinButtonDown, ButtonEvent::Down};

bool readPressed(uint8_t pca_pin) {
  const bool high = panelReadButton(pca_pin);
  return config::kButtonActiveLow ? !high : high;
}

void update(ButtonState& b, bool is_up) {
  const bool raw = readPressed(b.pca_pin);
  const bool stable = raw == b.last_raw;
  b.last_raw = raw;
  if (!stable) {
    return;
  }

  const TickType_t now = xTaskGetTickCount();
  if (raw && !b.pressed) {
    b.pressed = true;
    b.long_fired = false;
    b.pressed_at = now;
  } else if (!raw && b.pressed) {
    b.pressed = false;
    if (b.armed && !b.long_fired) {
      xQueueSend(s_events, &b.tap_event, 0);  // drop the tap if the queue is full
    }
    b.armed = true;
  } else if (!raw) {
    b.armed = true;
  }

  if (b.pressed && b.armed && !b.long_fired &&
      now - b.pressed_at >= pdMS_TO_TICKS(config::kResetHoldMs)) {
    b.long_fired = true;
    if (is_up) {
      s_reset_requested.store(true);
    }
  }
}

void buttonTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    update(s_up, true);
    update(s_down, false);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(config::kButtonPollMs));
  }
}

}  // namespace

void buttonsInit() {
  if (s_events != nullptr) {
    return;
  }
  s_events = xQueueCreate(kEventQueueDepth, sizeof(ButtonEvent));
  xTaskCreatePinnedToCore(buttonTask, "buttons", kTaskStackBytes, nullptr, kTaskPriority,
                          nullptr, kTaskCore);
}

ButtonEvent buttonsConsumeEvent() {
  ButtonEvent event = ButtonEvent::None;
  if (s_events == nullptr || xQueueReceive(s_events, &event, 0) != pdTRUE) {
    return ButtonEvent::None;
  }
  return event;
}

bool buttonsResetRequested() { return s_reset_requested.load(); }
