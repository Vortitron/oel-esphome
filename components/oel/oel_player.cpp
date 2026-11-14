#include "oel_player.h"

#include <cmath>
#include <cstdlib>
#include <cstring>

/* The ESP32s have miniz's inflate in ROM. Its usual wrapper keeps the 11 KB
   decompressor on the stack, too much for ESPHome's loop task, so it is
   driven directly with the state on the heap. The desktop uses zlib. */
#ifdef ESP_PLATFORM
#include "miniz.h"
#else
#include <zlib.h>
#endif

namespace oelcore {

Random rng;

namespace {

const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

uint32_t u32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t) p[3] << 24; }
int u16(const uint8_t *p) { return p[0] | p[1] << 8; }

}  // namespace

Player::~Player() {
  free(cur_);
  free(prev_);
  free(inflator_);
  free(cols_);
  free(rows_);
}

bool Player::parse(const uint8_t *b, size_t size, ClipHeader &h) {
  if (size < 16 || memcmp(b, "OEL1", 4) != 0)
    return false;
  h.width = u16(b + 4);
  h.height = u16(b + 6);
  h.frames = u16(b + 8);
  h.bits = b[10];
  h.colours = b[11];
  h.kind = b[12];
  h.frame_ms = b[13] ? b[13] : 60;
  if (h.width < 1 || h.height < 1 || h.frames < 1 || (h.bits != 2 && h.bits != 4) || h.colours < 1 ||
      h.colours > (1 << h.bits))
    return false;
  size_t table = (16 + 4 * h.colours + 3) & ~(size_t) 3;
  return size >= table + 4 * (h.frames + 1) && u32(b + table + 4 * h.frames) + table + 4 * (h.frames + 1) <= size;
}

bool Player::begin(Canvas &c, const uint8_t *blob, size_t size) {
  if (!parse(blob, size, head_))
    return false;
  blob_ = blob;
  size_ = size;
  palette_ = blob + 16;
  levels_ = palette_ + 3 * head_.colours;
  offsets_ = blob + ((16 + 4 * head_.colours + 3) & ~3);
  data_ = offsets_ + 4 * (head_.frames + 1);
  frame_bytes_ = ((size_t) head_.width * head_.height * head_.bits + 7) / 8;
  free(cur_);
  free(prev_);
  cur_ = (uint8_t *) malloc(frame_bytes_);
  prev_ = (uint8_t *) malloc(frame_bytes_);
#ifdef ESP_PLATFORM
  if (!inflator_)
    inflator_ = malloc(sizeof(tinfl_decompressor));
  if (!inflator_)
    return false;
#endif
  if (!cur_ || !prev_)
    return false;

  mono_panel_ = c.mono();
  sw_ = c.width();
  sh_ = c.height();
  if (!layout_())
    return false;
  index_ = 0;
  clock_ = played_ = 0;
  level_ = target_ = beat_ = 0;
  have_prev_ = false;
  c.fill(0, 0, sw_, sh_, 0);
  if (!decode_(0, cur_))
    return false;
  draw_(c, true);
  return true;
}

float Player::pass_time() const {
  return head_.kind == CLIP ? head_.frames * head_.frame_ms / 1000.0f : 0;
}

bool Player::decode_(int index, uint8_t *out) {
  uint32_t start = u32(offsets_ + 4 * index), end = u32(offsets_ + 4 * (index + 1));
  const uint8_t *src = data_ + start;
  size_t len = end - start;
  if (len == frame_bytes_) {
    memcpy(out, src, len);
    return true;
  }
#ifdef ESP_PLATFORM
  auto *d = (tinfl_decompressor *) inflator_;
  tinfl_init(d);
  size_t in = len, got = frame_bytes_;
  tinfl_status st = tinfl_decompress(d, src, &in, out, out, &got,
                                     TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  return st == TINFL_STATUS_DONE && got == frame_bytes_;
#else
  uLongf got = frame_bytes_;
  return uncompress(out, &got, src, len) == Z_OK && got == frame_bytes_;
#endif
}

/* Where the clip goes on screen and at what scale, by the aspect asked for. */
bool Player::layout_() {
  const float w = head_.width, h = head_.height;
  float fit = fminf(sw_ / w, sh_ / h), fill = fmaxf(sw_ / w, sh_ / h);
  Aspect a = aspect_;
  if (a == AUTO)
    a = fit < 1 && sh_ >= head_.height ? CROP : LETTERBOX;
  if (a == STRETCH) {
    sx_ = sw_ / w;
    sy_ = sh_ / h;
  } else {
    sx_ = sy_ = a == CROP ? fill : fit;
  }
  int full_w = (int) lrintf(w * sx_), full_h = (int) lrintf(h * sy_);
  if (full_w < 1)
    full_w = 1;
  if (full_h < 1)
    full_h = 1;
  vw_ = full_w < sw_ ? full_w : sw_;
  vh_ = full_h < sh_ ? full_h : sh_;
  vx_ = (sw_ - vw_) / 2;
  vy_ = (sh_ - vh_) / 2;
  pan_x_ = full_w - vw_;
  pan_y_ = full_h - vh_;
  ox_ = pan_x_ / 2;
  oy_ = pan_y_ / 2;
  gap_ = !mono_panel_ && sx_ >= 3 && sy_ >= 3;
  free(cols_);
  free(rows_);
  cols_ = (Span *) malloc(sizeof(Span) * vw_);
  rows_ = (Span *) malloc(sizeof(Span) * vh_);
  if (!cols_ || !rows_)
    return false;
  spans_(rows_, vh_, oy_, sy_, head_.height);
  spans_(cols_, vw_, ox_, sx_, head_.width);
  return true;
}

/* For each screen column (or row): the clip's dots it shows, first and one
   past the last, and whether it is the gap after a dot. Scaled up, a pixel
   shows the dot under its centre; scaled down, all the dots it covers. */
void Player::spans_(Span *out, int n, int offset, float scale, int size) {
  for (int i = 0; i < n; i++) {
    int a, b;
    if (scale >= 1) {
      a = (int) floorf((i + offset + 0.5f) / scale);
      b = a + 1;
    } else {
      a = (int) floorf((i + offset) / scale + 1e-4f);
      b = (int) floorf((i + offset + 1) / scale + 1e-4f);
      if (b <= a)
        b = a + 1;
    }
    if (a > size - 1)
      a = size - 1;
    if (b > size)
      b = size;
    out[i] = {(int16_t) a, (int16_t) b, false};
  }
  if (gap_)
    for (int i = 0; i < n; i++) {
      int next = (int) floorf((i + 1 + offset + 0.5f) / scale);
      out[i].gap = next != out[i].a;
    }
}

int Player::index_at_(int x, int y) const {
  size_t i = (size_t) y * head_.width + x;
  if (head_.bits == 2)
    return cur_[i >> 2] >> (6 - 2 * (i & 3)) & 3;
  return cur_[i >> 1] >> (i & 1 ? 0 : 4) & 15;
}

static int prev_index(const uint8_t *buf, int bits, size_t i) {
  if (bits == 2)
    return buf[i >> 2] >> (6 - 2 * (i & 3)) & 3;
  return buf[i >> 1] >> (i & 1 ? 0 : 4) & 15;
}

/* Each screen pixel from the dots it shows: one dot's colour, or the average
   of several. Runs of one changed colour along a row go as one rectangle. */
void Player::draw_(Canvas &c, bool all) {
  const int w = head_.width;
  for (int j = 0; j < vh_; j++) {
    const Span &r = rows_[j];
    const int py = vy_ + j;
    int run = -1;
    uint32_t run_rgb = 0;
    for (int i = 0; i <= vw_; i++) {
      bool changed = false;
      uint32_t rgb = 0;
      if (i < vw_) {
        const Span &col = cols_[i];
        const int px = vx_ + i;
        if (col.gap || r.gap) {
          changed = all;  // black, and stays so
        } else if (col.b - col.a <= 1 && r.b - r.a <= 1) {
          int k = index_at_(col.a, r.a);
          changed = all || !have_prev_ || prev_index(prev_, head_.bits, (size_t) r.a * w + col.a) != k;
          if (mono_panel_) {
            rgb = levels_[k] > BAYER[py & 3][px & 3] ? 0xFFFFFF : 0;
          } else {
            const uint8_t *p = palette_ + 3 * k;
            rgb = (uint32_t) p[0] << 16 | p[1] << 8 | p[2];
          }
        } else {
          int rr = 0, gg = 0, bb = 0, lv = 0, n = 0;
          for (int yy = r.a; yy < r.b; yy++)
            for (int xx = col.a; xx < col.b; xx++) {
              int k = index_at_(xx, yy);
              const uint8_t *p = palette_ + 3 * k;
              rr += p[0];
              gg += p[1];
              bb += p[2];
              lv += levels_[k];
              n++;
            }
          changed = true;
          if (mono_panel_)
            rgb = lv > BAYER[py & 3][px & 3] * n ? 0xFFFFFF : 0;
          else
            rgb = (uint32_t) (rr / n) << 16 | (gg / n) << 8 | (bb / n);
        }
      }
      if (run >= 0 && (!changed || rgb != run_rgb)) {
        c.fill(vx_ + run, py, i - run, 1, run_rgb);
        run = -1;
      }
      if (changed && run < 0) {
        run = i;
        run_rgb = rgb;
      }
    }
  }
}

/* A meter's frames are two banks of levels, silent to full, one after the
   other (the stereos' two styles of the same meter). */
void Player::next_meter_level_(float dt) {
  float top = head_.frames / 2 - 1;
  if (external_) {
    /* A real meter's ballistics: up at once, back down at a steady fall, so a
       sensor that updates now and then still moves smoothly. */
    float want = wanted_ * top;
    if (want > level_)
      level_ += (want - level_) * fminf(1, dt * 25);
    else
      level_ = fmaxf(want, level_ - top * dt * 1.2f);
    return;
  }
  /* Made-up music: a new peak every beat, a quick rise and a slow fall. */
  beat_ -= dt;
  if (beat_ <= 0) {
    float u = rng.uniform();
    target_ = top * (0.15f + 0.85f * u);
    if (rng.uniform() < 0.08f)
      target_ = top * 0.1f;  // a quiet bar
    beat_ = 0.12f + 0.3f * rng.uniform();
    if (rng.uniform() < 0.04f)
      bank_ = !bank_;
  }
  if (target_ > level_)
    level_ += (target_ - level_) * fminf(1, dt * 20);
  else
    level_ -= top * dt * 1.4f;
  if (level_ < 0)
    level_ = 0;
  if (level_ > top)
    level_ = top;
  if (level_ >= target_ - 0.5f)
    target_ = 0;  // peaked: fall back until the next beat
}

void Player::frame(Canvas &c, float dt) {
  if (!cur_)
    return;
  const bool panning = pan_x_ > 0 || pan_y_ > 0;
  if (head_.kind == STILL && !panning)
    return;
  const float ft = head_.frame_ms / 1000.0f;
  clock_ += dt;
  if (clock_ < ft)
    return;
  float step = clock_ > 4 * ft ? ft : clock_ - fmodf(clock_, ft);
  clock_ -= step;
  played_ += step;

  int next = index_;
  if (head_.kind == METER) {
    next_meter_level_(step);
    next = (int) lrintf(level_) + (bank_ ? head_.frames / 2 : 0);
  } else if (head_.kind == CLIP) {
    next = (index_ + (int) lrintf(step / ft)) % head_.frames;
  }
  bool moved = false;
  if (panning) {
    /* There and back across what is cropped, about ten seconds each way. */
    float u = 0.5f - 0.5f * cosf(fmodf(played_ / 20, 1) * 2 * 3.14159265f);
    int ox = (int) lrintf(pan_x_ * u), oy = (int) lrintf(pan_y_ * u);
    if (ox != ox_ || oy != oy_) {
      ox_ = ox;
      oy_ = oy;
      spans_(cols_, vw_, ox_, sx_, head_.width);
      spans_(rows_, vh_, oy_, sy_, head_.height);
      moved = true;
    }
  }
  if (next == index_ && !moved && have_prev_)
    return;
  if (next != index_ || !have_prev_) {
    uint8_t *t = prev_;
    prev_ = cur_;
    cur_ = t;
    have_prev_ = true;
    if (!decode_(next, cur_)) {
      memcpy(cur_, prev_, frame_bytes_);
      return;
    }
    index_ = next;
  }
  draw_(c, moved);
}

}  // namespace oelcore
