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

/* Chooses the clip, or with set_aspect_select() the aspect. */
class OelSelect : public select::Select, public Component {
 public:
  void set_parent(OelComponent *parent) { parent_ = parent; }
  void set_aspect_select() { aspect_ = true; }
  void setup() override {
    auto publish = [this](const std::string &name) { this->publish_state(name); };
    if (aspect_) {
      parent_->add_on_aspect_callback(publish);
      this->publish_state(parent_->get_aspect());
      return;
    }
    parent_->add_on_clip_callback(publish);
    if (!parent_->get_clip().empty())
      this->publish_state(parent_->get_clip());
  }

 protected:
  void control(const std::string &value) override {
    if (aspect_)
      parent_->set_aspect(value);
    else
      parent_->set_clip(value);
  }
  OelComponent *parent_{nullptr};
  bool aspect_{false};
};

}  // namespace oel
}  // namespace esphome
#endif  // USE_SELECT
