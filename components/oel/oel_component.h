#pragma once
/*
 * The ESPHome end of the OEL player: draws the chosen clip straight into a
 * display's buffer and pushes it at the clip's own rate, instead of going
 * through a display lambda.
 *
 * mode: always       the display is the clips' (no lambda or pages)
 * mode: screensaver  the display keeps its own drawing until oel.start, and
 *                    oel.stop gives it back
 */
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/display/display.h"

#include "oel_player.h"

#include <memory>
#include <string>
#include <vector>

namespace esphome {
namespace oel {

/* The display's own drawing, which screensaver mode moves aside while a
   clip plays and puts back after. They are protected; a pointer to member
   formed in a derived class reaches them without changing the display. */
struct DisplayInternals : display::Display {
  using Writer = decltype(writer_);
  static auto writer() { return &DisplayInternals::writer_; }
  static auto page() { return &DisplayInternals::page_; }
  static auto auto_clear() { return &DisplayInternals::auto_clear_enabled_; }
};

class DisplayCanvas : public ::oelcore::Canvas {
 public:
  void attach(display::Display *d) {
    display_ = d;
    width_ = d->get_width();
    height_ = d->get_height();
    mono_ = d->get_display_type() == display::DISPLAY_TYPE_BINARY;
  }
  int width() const override { return width_; }
  int height() const override { return height_; }
  bool mono() const override { return mono_; }
  void fill(int x, int y, int w, int h, uint32_t rgb) override;
  bool take_dirty() {
    bool d = dirty_;
    dirty_ = false;
    return d;
  }

 protected:
  display::Display *display_{nullptr};
  int width_{0}, height_{0};
  bool mono_{false}, dirty_{false};
};

struct Clip {
  std::string name;
  const uint8_t *data;
  size_t size;
};

class OelComponent : public Component {
 public:
  void set_display(display::Display *display) { display_ = display; }
  void add_clip(const char *name, const uint8_t *data, size_t size) { clips_.push_back({name, data, size}); }
  void set_initial_clip(const std::string &name) { initial_ = name; }
  /* How long each clip has when cycling (a moving clip always gets one
     whole pass). */
  void set_clip_time(uint32_t ms) { clip_time_ms_ = ms; }
  void set_screensaver_mode(bool on) { screensaver_mode_ = on; }

  /* Screensaver mode: start() plays the selected clip (or cycles), stop()
     gives the display back and redraws it. */
  void start();
  void stop();
  /* On to another clip now (cycling: a different one). */
  void next();
  bool is_running() const { return running_; }

  /* A clip's name, "cycle" or "off". */
  bool set_clip(const std::string &name);
  const std::string &get_clip() const { return current_; }
  void add_on_clip_callback(std::function<void(const std::string &)> &&cb) { callback_.add(std::move(cb)); }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  const Clip *find_(const std::string &name) const;
  bool play_(const Clip *clip);
  const Clip *pick_();

  display::Display *display_{nullptr};
  DisplayCanvas canvas_;
  std::vector<Clip> clips_;
  std::unique_ptr<::oelcore::Player> player_;
  const Clip *playing_{nullptr};
  std::string initial_{"cycle"}, current_;
  uint32_t clip_time_ms_{20000};
  uint32_t last_ms_{0}, started_ms_{0};
  bool screensaver_mode_{false}, running_{false};
  DisplayInternals::Writer saved_writer_{};
  display::DisplayPage *saved_page_{nullptr};
  bool saved_auto_clear_{true};
  HighFrequencyLoopRequester high_freq_;
  CallbackManager<void(const std::string &)> callback_;
};

}  // namespace oel
}  // namespace esphome
