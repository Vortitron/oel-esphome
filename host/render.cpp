/*
 * render.cpp - plays a clip on the desktop as the ESP32 would, frame by frame,
 * for checking the player and making the builder's previews.
 *
 *   host/render CLIP.oel WxH[m|g] FRAMES OUTDIR [FPS] [SEED]
 *
 * CLIP.oel is a blob made by components/oel/lkd.py; "m" after the size makes
 * a 1-bit panel, "g" a greyscale one (an SSD1322). Frames go to OUTDIR/NNNN.ppm.
 *
 *   OEL_ASPECT=auto|letterbox|crop|stretch   how the clip meets the screen
 *   OEL_LEVEL=sweep                          drive a meter from a slow sine
 *                                            wave, as a sensor would
 *   OEL_CUTOFF=0..100                        a 1-bit panel's cutoff, percent
 */
#include "../components/oel/oel_player.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace oelcore;

class HostCanvas : public Canvas {
 public:
  HostCanvas(int w, int h, bool mono, bool grey) : w_(w), h_(h), mono_(mono), grey_(grey), rgb_(w * h * 3, 0) {}
  int width() const override { return w_; }
  int height() const override { return h_; }
  bool mono() const override { return mono_; }
  bool grey() const override { return grey_; }
  void fill(int x, int y, int w, int h, uint32_t c) override {
    for (int j = y; j < y + h; j++)
      for (int i = x; i < x + w; i++) {
        if (i < 0 || j < 0 || i >= w_ || j >= h_) {
          fprintf(stderr, "fill outside the screen at %d,%d\n", i, j);
          exit(1);
        }
        uint8_t *p = &rgb_[(j * w_ + i) * 3];
        p[0] = c >> 16;
        p[1] = c >> 8;
        p[2] = c;
      }
  }
  void save(const char *path) const {
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", w_, h_);
    fwrite(rgb_.data(), 1, rgb_.size(), f);
    fclose(f);
  }

 private:
  int w_, h_;
  bool mono_, grey_;
  std::vector<uint8_t> rgb_;
};

int main(int argc, char **argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s CLIP.oel WxH[m|g] FRAMES OUTDIR [FPS] [SEED]\n", argv[0]);
    return 2;
  }
  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    perror(argv[1]);
    return 1;
  }
  std::vector<uint8_t> blob;
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0)
    blob.insert(blob.end(), buf, buf + n);
  fclose(f);
  int w = 0, h = 0;
  sscanf(argv[2], "%dx%d", &w, &h);
  bool mono = argv[2][strlen(argv[2]) - 1] == 'm', grey = argv[2][strlen(argv[2]) - 1] == 'g';
  int frames = atoi(argv[3]);
  float fps = argc > 5 ? atof(argv[5]) : 20;
  rng.state = argc > 6 ? strtoul(argv[6], nullptr, 10) | 1 : 1;
  HostCanvas canvas(w, h, mono, grey);
  Player player;
  if (const char *a = getenv("OEL_ASPECT")) {
    const char *names[] = {"auto", "letterbox", "crop", "stretch"};
    for (int i = 0; i < 4; i++)
      if (strcmp(a, names[i]) == 0)
        player.set_aspect((Aspect) i);
  }
  if (const char *c = getenv("OEL_CUTOFF"))
    player.set_cutoff(atoi(c));
  bool sweep = getenv("OEL_LEVEL") != nullptr;
  if (!player.begin(canvas, blob.data(), blob.size())) {
    fprintf(stderr, "%s: not a clip\n", argv[1]);
    return 1;
  }
  char path[512];
  for (int i = 0; i < frames; i++) {
    if (sweep)
      player.set_level(0.5f - 0.5f * cosf(i / fps * 1.3f), 0);
    if (i > 0)
      player.frame(canvas, 1 / fps);
    snprintf(path, sizeof path, "%s/%04d.ppm", argv[4], i);
    canvas.save(path);
  }
  const ClipHeader &hd = player.header();
  printf("%dx%d, %d frames, %d colours, kind %d\n", hd.width, hd.height, hd.frames, hd.colours, hd.kind);
  return 0;
}
