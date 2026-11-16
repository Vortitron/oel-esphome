#pragma once
/*
 * oel_player.h - plays one Pioneer OEL clip (a blob made by lkd.py) onto a
 * Canvas. Knows nothing of ESPHome, so host/render.cpp runs the same code.
 *
 * How a clip meets the screen (Aspect):
 *   LETTERBOX  all of it, its shape kept, black bars around
 *   CROP       the screen filled, its shape kept; what overflows is panned
 *              across slowly, there and back
 *   STRETCH    the screen filled, its shape not kept
 *   AUTO       letterbox, except crop where the clip would have to shrink
 *              but the screen is tall enough (a 256x64 clip on a 128x64
 *              OLED): halving a dithered picture loses it
 * Scaled up, each dot is a block of pixels, with a gap after it once it is
 * 3 pixels or more each way, like the stereo's panel. Scaled down, a pixel
 * is the average of the dots it covers.
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
  /* A greyscale panel (an SSD1322's 16 shades): each colour is a grey by
     its brightness, the clip's brightest at full. */
  virtual bool grey() const { return false; }
  /* Clipped by the caller. On a 1-bit panel rgb is 0 or 0xFFFFFF; on a
     greyscale one, a grey (r = g = b). */
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
enum Aspect { AUTO = 0, LETTERBOX = 1, CROP = 2, STRETCH = 3 };

class Player {
 public:
  ~Player();
  /* False if the blob isn't a clip this code understands. */
  static bool parse(const uint8_t *blob, size_t size, ClipHeader &out);
  /* Before begin(). */
  void set_aspect(Aspect a) { aspect_ = a; }
  bool begin(Canvas &c, const uint8_t *blob, size_t size);
  /* Move on by dt seconds, drawing when a frame is due. */
  void frame(Canvas &c, float dt);
  /* Seconds of one pass through the clip (a still: none). */
  float pass_time() const;
  /* A meter's level from outside, 0-1 (NaN reads as 0), instead of the
     made-up music; and which of its two banks of frames to show (0 or 1). */
  void set_level(float v, int bank) {
    external_ = true;
    wanted_ = v != v ? 0 : v < 0 ? 0 : v > 1 ? 1 : v;
    bank_ = bank != 0;
  }
  const ClipHeader &header() const { return head_; }
  /* On a 1-bit panel, the share of the way from black to white (0-100)
     below which a shade is left dark instead of dithered: higher drops the
     faint speckle of the dim greys. Takes effect at the next redraw(). */
  void set_cutoff(int percent);
  /* Frame-rate modulation on a 1-bit panel: 2-4 subframes, each a call to
     frame() drawn whole, a dot lit in some of them, so a shade is lit for a
     share of the time as well as of its dots; 1 is off. Steady with the
     screen written over 100 times a second (SPI); below that it shimmers. */
  void set_subframes(int n) {
    subframes_ = n < 1 ? 1 : n > 4 ? 4 : n;
    phase_ = 0;
  }
  int subframes() const { return subframes_; }
  /* Draw the whole frame again (after set_cutoff). */
  void redraw(Canvas &c) {
    if (cur_)
      draw_(c, true);
  }

 private:
  bool decode_(int index, uint8_t *out);
  bool advance_(float dt, bool &moved);
  uint32_t colour_(int k) const;
  struct Span {
    int16_t a, b;
    bool gap;
  };
  bool layout_();
  void spans_(Span *out, int n, int offset, float scale, int size);
  int index_at_(int x, int y) const;
  void draw_(Canvas &c, bool all);
  void next_meter_level_(float dt);

  ClipHeader head_;
  const uint8_t *blob_ = nullptr, *palette_ = nullptr, *levels_ = nullptr, *data_ = nullptr;
  const uint8_t *offsets_ = nullptr;
  size_t size_ = 0, frame_bytes_ = 0;
  uint8_t *cur_ = nullptr, *prev_ = nullptr;
  void *inflator_ = nullptr;
  bool mono_panel_ = false, grey_panel_ = false, have_prev_ = false;
  int sw_ = 0, sh_ = 0;
  Aspect aspect_ = AUTO;
  /* The clip's pixels on screen (x, y, width, height), its scale each way,
     how far it overflows each way (cropped) and how far it is panned. */
  int vx_ = 0, vy_ = 0, vw_ = 0, vh_ = 0;
  float sx_ = 1, sy_ = 1;
  int pan_x_ = 0, pan_y_ = 0, ox_ = 0, oy_ = 0;
  bool gap_ = false;
  Span *cols_ = nullptr, *rows_ = nullptr;
  int index_ = 0;
  float clock_ = 0, played_ = 0;
  float level_ = 0, target_ = 0, beat_ = 0;
  bool bank_ = false, external_ = false;
  float wanted_ = 0;
  /* Each colour's share of lit dots, 0-16, after the cutoff. */
  int cutoff_ = 0;
  int subframes_ = 1, phase_ = 0;
  uint8_t mono_[16] = {};
  /* Each colour as a grey, on a greyscale panel. */
  uint8_t grey_[16] = {};
};

}  // namespace oelcore
