#pragma once
/* The cutoff as a number (a slider in Home Assistant). Only when the YAML has
   one: a header for number that isn't there won't compile. */
#include "esphome/core/defines.h"
#ifdef USE_NUMBER
#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/core/version.h"
#include "esphome/components/number/number.h"

#include <cmath>

#include "oel_component.h"

namespace esphome {
namespace oel {

class OelNumber : public number::Number, public Component {
 public:
  void set_parent(OelComponent *parent) { parent_ = parent; }
  void set_restore_value(bool restore) { restore_ = restore; }
  void setup() override {
    float value = parent_->get_cutoff();
    if (restore_) {
#if ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 7, 0)
      pref_ = this->make_entity_preference<float>();
#else
      pref_ = global_preferences->make_preference<float>(this->get_object_id_hash());
#endif
      float saved;
      if (pref_.load(&saved) && saved >= 0 && saved <= 100) {
        value = saved;
        parent_->set_cutoff((int) lrintf(saved));
      }
    }
    this->publish_state(value);
  }

 protected:
  void control(float value) override {
    parent_->set_cutoff((int) lrintf(value));
    if (restore_)
      pref_.save(&value);
    this->publish_state(value);
  }
  OelComponent *parent_{nullptr};
  bool restore_{true};
  ESPPreferenceObject pref_;
};

}  // namespace oel
}  // namespace esphome
#endif  // USE_NUMBER
