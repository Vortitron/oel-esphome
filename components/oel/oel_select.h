#pragma once
/* A select for choosing the clip from Home Assistant ("cycle" or "off" too).
   Only when the YAML has one: a header for select that isn't there won't compile. */
#include "esphome/core/defines.h"
#ifdef USE_SELECT
#include "esphome/core/component.h"
#include "esphome/components/select/select.h"

#include "oel_component.h"

namespace esphome {
namespace oel {

class OelSelect : public select::Select, public Component {
 public:
  void set_parent(OelComponent *parent) { parent_ = parent; }
  void setup() override {
    parent_->add_on_clip_callback([this](const std::string &name) { this->publish_state(name); });
    if (!parent_->get_clip().empty())
      this->publish_state(parent_->get_clip());
  }

 protected:
  void control(const std::string &value) override { parent_->set_clip(value); }
  OelComponent *parent_{nullptr};
};

}  // namespace oel
}  // namespace esphome
#endif  // USE_SELECT
