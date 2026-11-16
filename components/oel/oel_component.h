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
    grey_ = d->get_display_type() == display::DISPLAY_TYPE_GRAYSCALE;
  }
  int width() const override { return width_; }
  int height() const override { return height_; }
  bool mono() const override { return mono_; }
  bool grey() const override { return grey_; }
  void fill(int x, int y, int w, int h, uint32_t rgb) override;
  bool take_dirty() {
    bool d = dirty_;
    dirty_ = false;
    return d;
  }

 protected:
  display::Display *display_{nullptr};
  int width_{0}, height_{0};
  bool mono_{false}, grey_{false}, dirty_{false};
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
  /* How a clip meets the screen: auto, letterbox, crop or stretch. Changing
     it while a clip plays starts that clip again laid out the new way. */
  bool set_aspect(const std::string &name);
  const std::string &get_aspect() const { return aspect_name_; }
  void add_on_aspect_callback(std::function<void(const std::string &)> &&cb) { aspect_callback_.add(std::move(cb)); }
  /* A 1-bit panel's cutoff, percent: shades dimmer than this are left
     dark instead of dithered. */
  void set_cutoff(int percent);
  int get_cutoff() const { return cutoff_; }
  /* Frame-rate modulation on a 1-bit panel: 2-4 subframes (1 is off). The
     display is then rewritten as fast as it goes; it wants SPI, as I2C
     flickers. */
  void set_frame_rate_modulation(int subframes);
  int get_frame_rate_modulation() const { return subframes_; }
  /* What drives the meters (in the units of min and max), instead of the
     made-up music; and which bank of their frames to show. */
  void set_meter_source(std::function<float()> &&f, float min, float max, int bank) {
    meter_source_ = std::move(f);
    meter_min_ = min;
    meter_max_ = max;
    meter_bank_ = bank;
  }

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
  void feed_meter_();

  display::Display *display_{nullptr};
  DisplayCanvas canvas_;
  std::vector<Clip> clips_;
  std::unique_ptr<::oelcore::Player> player_;
  const Clip *playing_{nullptr};
  std::string initial_{"cycle"}, current_;
  uint32_t clip_time_ms_{20000};
  ::oelcore::Aspect aspect_{::oelcore::AUTO};
  std::string aspect_name_{"auto"};
  int cutoff_{30};
  int subframes_{1};
  uint32_t sent_{0}, rate_ms_{0};
  CallbackManager<void(const std::string &)> aspect_callback_;
  std::function<float()> meter_source_;
  float meter_min_{0}, meter_max_{100};
  int meter_bank_{0};
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
