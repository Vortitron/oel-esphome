#pragma once
/* oel.start, oel.stop, oel.next and oel.is_running. */
#include "esphome/core/automation.h"
#include "esphome/core/macros.h"
#include "esphome/core/version.h"

#include "oel_component.h"

namespace esphome {
namespace oel {

/* ESPHome 2025.11 started passing an action's arguments by reference. */
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2025, 11, 0)
#define OEL_ARGS const Ts &...x
#else
#define OEL_ARGS Ts... x
#endif

template<typename... Ts> class StartAction : public Action<Ts...>, public Parented<OelComponent> {
 public:
  void play(OEL_ARGS) override { this->parent_->start(); }
};

template<typename... Ts> class StopAction : public Action<Ts...>, public Parented<OelComponent> {
 public:
  void play(OEL_ARGS) override { this->parent_->stop(); }
};

template<typename... Ts> class NextAction : public Action<Ts...>, public Parented<OelComponent> {
 public:
  void play(OEL_ARGS) override { this->parent_->next(); }
};

template<typename... Ts> class IsRunningCondition : public Condition<Ts...>, public Parented<OelComponent> {
 public:
  bool check(OEL_ARGS) override { return this->parent_->is_running(); }
};

#undef OEL_ARGS

}  // namespace oel
}  // namespace esphome
