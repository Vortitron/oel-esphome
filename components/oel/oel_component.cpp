#include "oel_component.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace oel {

static const char *const TAG = "oel";

void DisplayCanvas::fill(int x, int y, int w, int h, uint32_t rgb) {
  if (w <= 0 || h <= 0)
    return;
  dirty_ = true;
  if (mono_) {
    display_->filled_rectangle(x, y, w, h, rgb ? display::COLOR_ON : display::COLOR_OFF);
    return;
  }
  if (grey_) {
    /* A greyscale driver reads the white channel. */
    uint8_t v = rgb & 255;
    display_->filled_rectangle(x, y, w, h, Color(v, v, v, v));
    return;
  }
  display_->filled_rectangle(x, y, w, h, Color(rgb >> 16, rgb >> 8 & 255, rgb & 255));
}

void OelComponent::setup() {
  ::oelcore::rng.state = random_uint32() | 1;
  canvas_.attach(display_);
  if (!screensaver_mode_) {
    /* We push the frames; the display's own timed redraw would clear them. */
    display_->stop_poller();
    display_->set_auto_clear(false);
  }
  if (!set_clip(initial_))
    set_clip(screensaver_mode_ ? "cycle" : "off");
}

const Clip *OelComponent::find_(const std::string &name) const {
  for (const auto &c : clips_)
    if (c.name == name)
      return &c;
  return nullptr;
}

/* Any clip but the one playing. */
const Clip *OelComponent::pick_() {
  if (clips_.empty())
    return nullptr;
  if (clips_.size() == 1)
    return &clips_[0];
  const Clip *c;
  do {
    c = &clips_[::oelcore::rng.next() % clips_.size()];
  } while (c == playing_);
  return c;
}

static const char *const ASPECTS[] = {"auto", "letterbox", "crop", "stretch"};

bool OelComponent::set_aspect(const std::string &name) {
  for (int i = 0; i < 4; i++)
    if (name == ASPECTS[i]) {
      aspect_ = (::oelcore::Aspect) i;
      aspect_name_ = name;
      aspect_callback_.call(aspect_name_);
      if (player_ && playing_) {
        uint32_t started = started_ms_;
        play_(playing_);
        started_ms_ = started;  // its turn isn't restarted
      }
      return true;
    }
  ESP_LOGW(TAG, "No aspect called '%s' (auto, letterbox, crop or stretch)", name.c_str());
  return false;
}

void OelComponent::set_cutoff(int percent) {
  cutoff_ = percent < 0 ? 0 : percent > 100 ? 100 : percent;
  if (!player_ || !playing_ || !canvas_.mono())
    return;
  player_->set_cutoff(cutoff_);
  player_->redraw(canvas_);
  if (canvas_.take_dirty())
    display_->update();
}

void OelComponent::set_frame_rate_modulation(int subframes) {
  subframes_ = subframes < 1 ? 1 : subframes > 4 ? 4 : subframes;
  if (subframes_ > 1)
    subframes_on_ = subframes_;
  sent_ = 0;
  rate_ms_ = millis();
  if (!player_ || !playing_)
    return;
  player_->set_subframes(subframes_);
  player_->redraw(canvas_);
  if (canvas_.take_dirty())
    display_->update();
}

void OelComponent::feed_meter_() {
  if (!meter_source_ || !player_ || player_->header().kind != ::oelcore::METER)
    return;
  float v = meter_source_();
  float span = meter_max_ - meter_min_;
  player_->set_level(span == 0 ? 0 : (v - meter_min_) / span, meter_bank_);
}

bool OelComponent::play_(const Clip *clip) {
  if (clip == nullptr)
    return false;
  if (!player_)
    player_.reset(new ::oelcore::Player());
  player_->set_aspect(aspect_);
  player_->set_cutoff(cutoff_);
  player_->set_subframes(subframes_);
  if (!player_->begin(canvas_, clip->data, clip->size)) {
    ESP_LOGW(TAG, "Couldn't play %s (out of memory, or not a clip)", clip->name.c_str());
    player_.reset();
    playing_ = nullptr;
    return false;
  }
  playing_ = clip;
  feed_meter_();
  ESP_LOGD(TAG, "Playing %s", clip->name.c_str());
  canvas_.take_dirty();
  display_->update();
  last_ms_ = started_ms_ = millis();
  high_freq_.start();
  return true;
}

bool OelComponent::set_clip(const std::string &name) {
  if (name != "cycle" && name != "off" && find_(name) == nullptr) {
    ESP_LOGW(TAG, "No clip called '%s' in this build", name.c_str());
    return false;
  }
  current_ = name;
  callback_.call(current_);
  if (screensaver_mode_ && !running_)
    return true;  // what start() will play
  if (name == "off") {
    if (screensaver_mode_) {
      stop();
    } else {
      player_.reset();
      playing_ = nullptr;
      high_freq_.stop();
      canvas_.fill(0, 0, canvas_.width(), canvas_.height(), 0);
      display_->update();
    }
    return true;
  }
  play_(name == "cycle" ? pick_() : find_(name));
  return true;
}

void OelComponent::next() {
  if (!player_ || clips_.empty())
    return;
  if (current_ == "cycle") {
    play_(pick_());
    return;
  }
  /* A fixed clip: move the selection on to the next one in the list. */
  size_t i = playing_ ? playing_ - clips_.data() : 0;
  set_clip(clips_[(i + 1) % clips_.size()].name);
}

void OelComponent::start() {
  if (!screensaver_mode_ || running_ || current_ == "off")
    return;
  /* Move the display's own drawing aside, so its updates push our frames
     instead of drawing over them. */
  auto *d = static_cast<display::Display *>(display_);
  saved_writer_ = std::move(d->*DisplayInternals::writer());
  d->*DisplayInternals::writer() = DisplayInternals::Writer{};
  saved_page_ = d->*DisplayInternals::page();
  d->*DisplayInternals::page() = nullptr;
  saved_auto_clear_ = d->*DisplayInternals::auto_clear();
  d->set_auto_clear(false);
  running_ = true;
  if (!play_(current_ == "cycle" ? pick_() : find_(current_)))
    stop();
}

void OelComponent::stop() {
  if (!running_)
    return;
  player_.reset();
  playing_ = nullptr;
  high_freq_.stop();
  auto *d = static_cast<display::Display *>(display_);
  d->*DisplayInternals::writer() = std::move(saved_writer_);
  saved_writer_ = DisplayInternals::Writer{};
  d->*DisplayInternals::page() = saved_page_;
  d->set_auto_clear(saved_auto_clear_);
  running_ = false;
  ESP_LOGD(TAG, "Stopped; the display is its own again");
  d->update();
}

void OelComponent::loop() {
  if (!player_)
    return;
  uint32_t now = millis();
  uint32_t elapsed = now - last_ms_;
  /* About 50 a second; with frame-rate modulation, each subframe as soon
     as the last is sent. */
  const bool flicker = subframes_ > 1 && canvas_.mono();
  if (elapsed < (flicker ? 2u : 20u))
    return;
  last_ms_ = now;
  feed_meter_();
  player_->frame(canvas_, elapsed / 1000.0f);
  if (canvas_.take_dirty())
    display_->update();
  if (flicker) {
    /* How fast the subframes go out: below about 100 a second it flickers. */
    sent_++;
    if (now - rate_ms_ >= 10000) {
      ESP_LOGD(TAG, "%.0f subframes a second (%d-way frame-rate modulation)", sent_ * 1000.0f / (now - rate_ms_),
               subframes_);
      sent_ = 0;
      rate_ms_ = now;
    }
  }
  if (current_ == "cycle" && clips_.size() > 1) {
    uint32_t pass = (uint32_t) (player_->pass_time() * 1000);
    uint32_t due = clip_time_ms_;
    /* A moving clip ends on a whole pass. */
    if (pass > 0)
      due = ((clip_time_ms_ + pass - 1) / pass) * pass;
    if (now - started_ms_ >= due)
      play_(pick_());
  }
}

void OelComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "OEL clips:");
  ESP_LOGCONFIG(TAG, "  Screen: %dx%d%s", canvas_.width(), canvas_.height(),
                canvas_.mono() ? " (1-bit)" : canvas_.grey() ? " (greyscale)" : "");
  ESP_LOGCONFIG(TAG, "  Mode: %s; playing: %s; aspect: %s", screensaver_mode_ ? "screensaver" : "always",
                current_.c_str(), aspect_name_.c_str());
  if (canvas_.mono())
    ESP_LOGCONFIG(TAG, "  Cutoff: %d%%; frame-rate modulation: %d subframe%s", cutoff_, subframes_,
                  subframes_ == 1 ? " (off)" : "s");
  if (meter_source_)
    ESP_LOGCONFIG(TAG, "  Meters show %.6g to %.6g (bank %d)", meter_min_, meter_max_, meter_bank_ + 1);
  for (const auto &c : clips_) {
    ::oelcore::ClipHeader h;
    if (::oelcore::Player::parse(c.data, c.size, h))
      ESP_LOGCONFIG(TAG, "  %s: %dx%d, %d frames, %u bytes", c.name.c_str(), h.width, h.height, h.frames,
                    (unsigned) c.size);
    else
      ESP_LOGCONFIG(TAG, "  %s: not a clip", c.name.c_str());
  }
}

}  // namespace oel
}  // namespace esphome
