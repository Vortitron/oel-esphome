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
  float scale = fminf((float) sw_ / head_.width, (float) sh_ / head_.height);
  pan_ = 0;
  shrink_ = gap_ = false;
  if (scale < 1 && sh_ >= head_.height) {
    pan_ = head_.width - sw_;
    dw_ = sw_;
    dh_ = head_.height;
  } else {
    shrink_ = scale < 1;
    dw_ = (int) (head_.width * scale);
    dh_ = (int) (head_.height * scale);
    if (dw_ < 1)
      dw_ = 1;
    if (dh_ < 1)
      dh_ = 1;
    gap_ = !mono_panel_ && scale >= 3;
  }
  dx_ = (sw_ - dw_) / 2;
  dy_ = (sh_ - dh_) / 2;
  index_ = 0;
  clock_ = played_ = 0;
  level_ = target_ = beat_ = 0;
  offset_ = drawn_offset_ = 0;
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

int Player::index_at_(int x, int y) const {
  size_t i = (size_t) y * head_.width + x;
  if (head_.bits == 2)
    return cur_[i >> 2] >> (6 - 2 * (i & 3)) & 3;
  return cur_[i >> 1] >> (i & 1 ? 0 : 4) & 15;
}

uint32_t Player::mono_(int dx, int dy, int index) const {
  return levels_[index] > BAYER[dy & 3][dx & 3] ? 0xFFFFFF : 0;
}

static int prev_index(const uint8_t *buf, int bits, size_t i) {
  if (bits == 2)
    return buf[i >> 2] >> (6 - 2 * (i & 3)) & 3;
  return buf[i >> 1] >> (i & 1 ? 0 : 4) & 15;
}

void Player::draw_(Canvas &c, bool all) {
  const int w = head_.width, h = head_.height;
  if (shrink_) {
    /* Each pixel the average of the dots it covers. */
    for (int y = 0; y < dh_; y++) {
      int y0 = y * h / dh_, y1 = (y + 1) * h / dh_;
      if (y1 <= y0)
        y1 = y0 + 1;
      int run = -1;
      uint32_t run_rgb = 0;
      for (int x = 0; x <= dw_; x++) {
        uint32_t rgb = 0;
        if (x < dw_) {
          int x0 = x * w / dw_, x1 = (x + 1) * w / dw_;
          if (x1 <= x0)
            x1 = x0 + 1;
          int r = 0, g = 0, b = 0, lv = 0, n = 0;
          for (int yy = y0; yy < y1; yy++)
            for (int xx = x0; xx < x1; xx++) {
              int k = index_at_(xx, yy);
              const uint8_t *p = palette_ + 3 * k;
              r += p[0];
              g += p[1];
              b += p[2];
              lv += levels_[k];
              n++;
            }
          if (mono_panel_)
            rgb = lv * 16 / n > BAYER[(y + dy_) & 3][(x + dx_) & 3] * 16 ? 0xFFFFFF : 0;
          else
            rgb = (uint32_t) (r / n) << 16 | (g / n) << 8 | (b / n);
        }
        if (run >= 0 && (x == dw_ || rgb != run_rgb)) {
          c.fill(dx_ + run, dy_ + y, x - run, 1, run_rgb);
          run = -1;
        }
        if (run < 0 && x < dw_) {
          run = x;
          run_rgb = rgb;
        }
      }
    }
    return;
  }

  /* Dot for dot, or each dot a block of pixels. */
  for (int sy = 0; sy < h; sy++) {
    int y0 = dy_ + sy * dh_ / h, y1 = dy_ + (sy + 1) * dh_ / h;
    if (y1 <= y0)
      continue;
    int first = pan_ ? offset_ : 0, last = pan_ ? offset_ + dw_ : w;
    int run = -1, run_x = 0;
    uint32_t run_rgb = 0;
    for (int sx = first; sx <= last; sx++) {
      bool changed = false;
      int k = 0, x0 = 0, x1 = 0;
      if (sx < last) {
        k = index_at_(sx, sy);
        size_t i = (size_t) sy * w + sx;
        changed = all || !have_prev_ || prev_index(prev_, head_.bits, i) != k;
        int col = sx - first;
        x0 = dx_ + (pan_ ? col : col * dw_ / w);
        x1 = dx_ + (pan_ ? col + 1 : (col + 1) * dw_ / w);
      }
      if (mono_panel_) {
        /* Dithered a pixel at a time; runs of one value go as one rectangle. */
        if (changed)
          for (int py = y0; py < y1; py++)
            for (int px = x0; px < x1; px++)
              c.fill(px, py, 1, 1, mono_(px, py, k));
        continue;
      }
      const uint8_t *p = palette_ + 3 * k;
      uint32_t rgb = (uint32_t) p[0] << 16 | p[1] << 8 | p[2];
      if (gap_) {
        if (changed)
          c.fill(x0, y0, x1 - x0 - 1, y1 - y0 - 1, rgb);
        continue;
      }
      if (run >= 0 && (!changed || rgb != run_rgb)) {
        c.fill(run_x, y0, x0 - run_x, y1 - y0, run_rgb);
        run = -1;
      }
      if (changed && run < 0) {
        run = sx;
        run_x = x0;
        run_rgb = rgb;
      }
    }
  }
}

/* A meter's frames are two banks of levels, silent to full, one after the
   other (the stereos' two styles of the same meter). */
void Player::next_meter_level_(float dt) {
  /* Made-up music: a new peak every beat, a quick rise and a slow fall. */
  float top = head_.frames / 2 - 1;
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
  if (head_.kind == STILL || !cur_)
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
  } else {
    next = (index_ + (int) lrintf(step / ft)) % head_.frames;
  }
  bool moved = false;
  if (pan_ > 0) {
    /* There and back, about ten seconds each way. */
    float u = fmodf(played_ / 20, 1);
    offset_ = (int) lrintf(pan_ * (0.5f - 0.5f * cosf(u * 2 * 3.14159265f)));
    moved = offset_ != drawn_offset_;
  }
  if (next == index_ && !moved && have_prev_)
    return;
  uint8_t *t = prev_;
  prev_ = cur_;
  cur_ = t;
  have_prev_ = true;
  if (!decode_(next, cur_)) {
    memcpy(cur_, prev_, frame_bytes_);
    return;
  }
  index_ = next;
  draw_(c, moved || shrink_);
  drawn_offset_ = offset_;
}

}  // namespace oelcore
