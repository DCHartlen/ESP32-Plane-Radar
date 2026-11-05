#include "hardware/buttons.h"

#include <Arduino.h>

#include <algorithm>
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
  ButtonEvent hold_event;
  bool last_raw = false;
  bool pressed = false;   // debounced
  bool armed = false;     // a release has been seen (ignores a hold carried over from reset)
  bool consumed = false;  // this press already gave a hold or joined a two-button press
  TickType_t pressed_at = 0;
};

QueueHandle_t s_events = nullptr;
std::atomic<bool> s_reset_requested{false};
std::atomic<unsigned long> s_both_held_ms{0};
ButtonState s_up{config::kExpanderPinButtonUp, ButtonEvent::Up, ButtonEvent::UpHold};
ButtonState s_down{config::kExpanderPinButtonDown, ButtonEvent::Down, ButtonEvent::DownHold};

bool readPressed(uint8_t pca_pin) {
  const bool high = panelReadButton(pca_pin);
  return config::kButtonActiveLow ? !high : high;
}

/** Debounces one button; a release that ends a plain press queues its tap. */
void debounce(ButtonState& b, TickType_t now) {
  const bool raw = readPressed(b.pca_pin);
  const bool stable = raw == b.last_raw;
  b.last_raw = raw;
  if (!stable) {
    return;
  }

  if (raw && !b.pressed) {
    b.pressed = true;
    b.consumed = false;
    b.pressed_at = now;
  } else if (!raw && b.pressed) {
    b.pressed = false;
    if (b.armed && !b.consumed) {
      xQueueSend(s_events, &b.tap_event, 0);  // drop the tap if the queue is full
    }
    b.armed = true;
  } else if (!raw) {
    b.armed = true;
  }
}

/** A button held alone long enough gives its hold event, once per press. */
void checkHold(ButtonState& b, const ButtonState& other, TickType_t now) {
  if (b.pressed && b.armed && !b.consumed && !other.pressed &&
      now - b.pressed_at >= pdMS_TO_TICKS(config::kButtonHoldMs)) {
    b.consumed = true;
    xQueueSend(s_events, &b.hold_event, 0);
  }
}

void update() {
  const TickType_t now = xTaskGetTickCount();
  debounce(s_up, now);
  debounce(s_down, now);

  if (s_up.pressed && s_down.pressed && s_up.armed && s_down.armed) {
    s_up.consumed = true;
    s_down.consumed = true;
    // Timed from the second press (the shorter of the two holds, which survives tick wrap).
    const TickType_t since =
        std::min<TickType_t>(now - s_up.pressed_at, now - s_down.pressed_at);
    const unsigned long held_ms = static_cast<unsigned long>(pdTICKS_TO_MS(since));
    s_both_held_ms.store(held_ms);
    if (held_ms >= config::kResetHoldMs) {
      s_reset_requested.store(true);
    }
    return;
  }
  s_both_held_ms.store(0);
  checkHold(s_up, s_down, now);
  checkHold(s_down, s_up, now);
}

void buttonTask(void*) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    update();
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

unsigned long buttonsBothHeldMs() { return s_both_held_ms.load(); }

bool buttonsResetRequested() { return s_reset_requested.load(); }
