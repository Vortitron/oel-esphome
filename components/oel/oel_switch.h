#pragma once
/* Frame-rate modulation as a switch. Only when the YAML has one: a header
   for switch that isn't there won't compile. */
#include "esphome/core/defines.h"
#ifdef USE_SWITCH
#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"

#include "oel_component.h"

namespace esphome {
namespace oel {

class OelSwitch : public switch_::Switch, public Component {
 public:
  void set_parent(OelComponent *parent) { parent_ = parent; }
  void setup() override {
    /* What was saved, if this restore mode keeps it; else ALWAYS_ON or
       ALWAYS_OFF; else (nothing saved yet) what the YAML says. */
    optional<bool> saved;
    if (this->restore_mode & switch_::RESTORE_MODE_PERSISTENT_MASK)
      saved = this->get_initial_state();
    bool on;
    if (saved.has_value())
      on = *saved;
    else if (this->restore_mode == switch_::SWITCH_ALWAYS_ON || this->restore_mode == switch_::SWITCH_ALWAYS_OFF)
      on = this->restore_mode == switch_::SWITCH_ALWAYS_ON;
    else
      on = parent_->get_frame_rate_modulation() > 1;
    this->write_state(on);
  }

 protected:
  void write_state(bool state) override {
    parent_->set_frame_rate_modulation(state ? parent_->frame_rate_modulation_when_on() : 1);
    this->publish_state(state);
  }
  OelComponent *parent_{nullptr};
};

}  // namespace oel
}  // namespace esphome
#endif  // USE_SWITCH
