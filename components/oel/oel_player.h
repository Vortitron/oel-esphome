#pragma once
/*
 * oel_player.h - plays one Pioneer OEL clip (a blob made by lkd.py) onto a
 * Canvas. Knows nothing of ESPHome, so host/render.cpp runs the same code.
 *
 * The clip is scaled to the screen's width keeping its shape (nearest dot,
 * with a gap between dots once each is 3 pixels or more, like the stereo's
 * panel). Where it would have to shrink but the screen is tall enough, it is
 * shown dot for dot and the view pans across instead: halving a dithered
 * picture loses it. Shrunk anyway, a pixel is the average of what it covers.
 *
 * Only what changed since the last frame is drawn.
 */
#include <cstddef>
#include <cstdint>

namespace oelcore {

class Canvas {
 public:
  virtual ~Canvas() = default;
  virtual int width() const = 0;
  virtual int height() const = 0;
  /* A 1-bit panel: each colour lights a share of its dots, dithered. */
  virtual bool mono() const { return false; }
  /* Clipped by the caller. On a 1-bit panel rgb is 0 or 0xFFFFFF. */
  virtual void fill(int x, int y, int w, int h, uint32_t rgb) = 0;
};

/* xorshift32, for the meters' made-up music. */
struct Random {
  uint32_t state = 0x9e3779b9u;
  uint32_t next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }
};
extern Random rng;

struct ClipHeader {
  int width = 0, height = 0, frames = 0, bits = 2, colours = 0, kind = 0, frame_ms = 60;
};

enum Kind { CLIP = 0, METER = 1, STILL = 2 };

class Player {
 public:
  ~Player();
  /* False if the blob isn't a clip this code understands. */
  static bool parse(const uint8_t *blob, size_t size, ClipHeader &out);
  bool begin(Canvas &c, const uint8_t *blob, size_t size);
  /* Move on by dt seconds, drawing when a frame is due. */
  void frame(Canvas &c, float dt);
  /* Seconds of one pass through the clip (a still: none). */
  float pass_time() const;
  const ClipHeader &header() const { return head_; }

 private:
  bool decode_(int index, uint8_t *out);
  int index_at_(int x, int y) const;
  uint32_t mono_(int dx, int dy, int index) const;
  void draw_(Canvas &c, bool all);
  void next_meter_level_(float dt);

  ClipHeader head_;
  const uint8_t *blob_ = nullptr, *palette_ = nullptr, *levels_ = nullptr, *data_ = nullptr;
  const uint8_t *offsets_ = nullptr;
  size_t size_ = 0, frame_bytes_ = 0;
  uint8_t *cur_ = nullptr, *prev_ = nullptr;
  void *inflator_ = nullptr;
  bool mono_panel_ = false, have_prev_ = false;
  int sw_ = 0, sh_ = 0;
  /* Where the clip goes on screen, and at what scale (in 1/256ths). */
  int dx_ = 0, dy_ = 0, dw_ = 0, dh_ = 0;
  int pan_ = 0, offset_ = 0, drawn_offset_ = -1;
  bool shrink_ = false, gap_ = false;
  int index_ = 0;
  float clock_ = 0, played_ = 0;
  float level_ = 0, target_ = 0, beat_ = 0;
  bool bank_ = false;
};

}  // namespace oelcore
