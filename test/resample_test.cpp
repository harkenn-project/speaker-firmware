// A PC check of the player's resampler (src/resample.h) and its use of minimp3. Not part of the firmware build.
//
//   cl /O2 /EHsc /std:c++17 /I src /I lib/minimp3 test/resample_test.cpp /Fe:resample_test.exe
//   resample_test.exe some.mp3 out48.wav
//
// Checks: the output doesn't depend on how the input is chopped up; a 1 kHz tone keeps its level and stays clean;
// nothing comes out near the wake word arbitration tones (18.6-19.8 kHz); a loop (the same audio twice in a row)
// joins without a jump. Writes the decoded, resampled track to listen to.
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include <minimp3.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "resample.h"

using player::Resampler;

static int failures = 0;
#define CHECK(cond, ...)               \
  do {                                 \
    printf(cond ? "  ok    " : "  FAIL  "); \
    printf(__VA_ARGS__);               \
    printf("\n");                      \
    if (!(cond)) failures++;           \
  } while (0)

// Runs `in` through a fresh resampler in chunks of random size (and random output room), like the player does.
std::vector<int16_t> resample(const std::vector<int16_t>& in, int in_rate, int out_rate, unsigned seed, float gain = 1) {
  Resampler r;
  r.setup(in_rate, out_rate);
  std::mt19937 rng(seed);
  std::vector<int16_t> out;
  size_t pos = 0;
  int16_t buf[1152];
  while (pos < in.size()) {
    const size_t chunk = std::min<size_t>(in.size() - pos, 1 + rng() % 1152);
    size_t taken = 0;
    while (taken < chunk) {
      size_t made = 0;
      const size_t room = 1 + rng() % 1152;
      taken += r.run(in.data() + pos + taken, chunk - taken, buf, room, &made, gain);
      out.insert(out.end(), buf, buf + made);
    }
    pos += chunk;
  }
  return out;
}

double goertzel(const std::vector<int16_t>& x, size_t from, size_t n, double hz, int rate) {
  const double c = 2 * cos(2 * M_PI * hz / rate);
  double s1 = 0, s2 = 0;
  for (size_t i = from; i < from + n && i < x.size(); i++) {
    const double s0 = x[i] + c * s1 - s2;
    s2 = s1;
    s1 = s0;
  }
  return 2 * sqrt(std::max(0.0, s1 * s1 + s2 * s2 - c * s1 * s2)) / n;
}

void write_wav(const char* path, const std::vector<int16_t>& x, int rate) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  const uint32_t data = x.size() * 2, riff = 36 + data, fmt = 16, byte_rate = rate * 2;
  const uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
  fwrite("RIFF", 1, 4, f), fwrite(&riff, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f), fwrite(&fmt, 4, 1, f);
  fwrite(&pcm, 2, 1, f), fwrite(&ch, 2, 1, f), fwrite(&rate, 4, 1, f), fwrite(&byte_rate, 4, 1, f);
  fwrite(&align, 2, 1, f), fwrite(&bits, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&data, 4, 1, f);
  fwrite(x.data(), 2, x.size(), f);
  fclose(f);
}

// Decodes an MP3 the way player.cpp does (frame by frame, stereo averaged to mono).
std::vector<int16_t> decode(const char* path, int* rate) {
  std::vector<int16_t> out;
  FILE* f = fopen(path, "rb");
  if (!f) return out;
  std::vector<uint8_t> data;
  uint8_t chunk[65536];
  size_t n;
  while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) data.insert(data.end(), chunk, chunk + n);
  fclose(f);
  size_t pos = 0;
  if (data.size() > 10 && data[0] == 'I' && data[1] == 'D' && data[2] == '3') {
    pos = 10 + ((data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 | (data[8] & 0x7F) << 7 | (data[9] & 0x7F));
  }
  static mp3dec_t dec;
  mp3dec_init(&dec);
  int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
  while (pos < data.size()) {
    mp3dec_frame_info_t info;
    const int samples = mp3dec_decode_frame(&dec, data.data() + pos, data.size() - pos, frame, &info);
    if (!info.frame_bytes) break;
    pos += info.frame_bytes;
    *rate = info.hz;
    for (int i = 0; i < samples; i++) {
      out.push_back(info.channels == 2 ? static_cast<int16_t>((frame[2 * i] + frame[2 * i + 1]) / 2) : frame[i]);
    }
  }
  return out;
}

int main(int argc, char** argv) {
  for (int out_rate : {48000, 16000}) {
    printf("44.1 kHz -> %d Hz\n", out_rate);
    // A 1 kHz tone at half scale.
    std::vector<int16_t> tone(44100 * 2);
    for (size_t i = 0; i < tone.size(); i++) tone[i] = static_cast<int16_t>(16384 * sin(2 * M_PI * 1000 * i / 44100.0));
    const auto a = resample(tone, 44100, out_rate, 1), b = resample(tone, 44100, out_rate, 2);
    CHECK(a == b, "same output however the input is chopped up (%zu samples)", a.size());
    CHECK(std::abs(static_cast<long>(a.size()) - static_cast<long>(tone.size() * out_rate / 44100)) <= 2,
          "length %zu, expected %zu", a.size(), tone.size() * out_rate / 44100);
    const size_t from = out_rate / 2, len = out_rate;  // after the filter settles
    const double level = goertzel(a, from, len, 1000, out_rate);
    CHECK(fabs(level - 16384) < 16384 * 0.03, "1 kHz level %.0f of 16384", level);
    double err = 0, sig = 0;
    for (size_t i = from; i < from + len; i++) {
      const double phase = goertzel(a, from, 1, 1000, out_rate);  // unused: amplitude only
      (void)phase;
      sig += a[i] * static_cast<double>(a[i]);
    }
    // Residual after removing the 1 kHz tone (fit by least squares).
    double cs = 0, sn = 0;
    for (size_t i = from; i < from + len; i++) {
      cs += a[i] * cos(2 * M_PI * 1000.0 * i / out_rate);
      sn += a[i] * sin(2 * M_PI * 1000.0 * i / out_rate);
    }
    cs *= 2.0 / len, sn *= 2.0 / len;
    for (size_t i = from; i < from + len; i++) {
      const double fit = cs * cos(2 * M_PI * 1000.0 * i / out_rate) + sn * sin(2 * M_PI * 1000.0 * i / out_rate);
      err += (a[i] - fit) * (a[i] - fit);
    }
    const double snr = 10 * log10(sig / std::max(err, 1e-9));
    CHECK(snr > 45, "1 kHz stays clean: %.1f dB signal to distortion", snr);
    // White noise in: what comes out near the arbitration tones?
    if (out_rate == 48000) {
      std::mt19937 rng(7);
      std::normal_distribution<double> nd(0, 6000);
      std::vector<int16_t> noise(44100 * 2);
      for (auto& v : noise) v = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, nd(rng))));
      const auto o = resample(noise, 44100, 48000, 3);
      double worst = 0;
      for (double hz : {18600.0, 19200.0, 19800.0}) worst = std::max(worst, goertzel(o, 24000, 4800, hz, 48000));
      const double mid = goertzel(o, 24000, 4800, 2000, 48000);
      CHECK(worst < mid / 30, "near the arbitration tones %.1f vs %.1f at 2 kHz (%.0f dB down)", worst, mid,
            20 * log10(mid / std::max(worst, 1e-9)));
    }
  }
  if (argc >= 2) {
    int rate = 0;
    const auto pcm = decode(argv[1], &rate);
    printf("%s: %zu samples at %d Hz (%.1f s)\n", argv[1], pcm.size(), rate, pcm.size() / static_cast<double>(rate));
    CHECK(pcm.size() > static_cast<size_t>(rate), "decoded");
    // A loop: the track twice in a row through one resampler, as Repeat::One does.
    std::vector<int16_t> twice(pcm);
    twice.insert(twice.end(), pcm.begin(), pcm.end());
    const auto out = resample(twice, rate, 48000, 4);
    const size_t join = pcm.size() * 48000 / rate;
    int jump = 0, usual = 0;
    for (size_t i = join - 48000; i < join + 48000 && i + 1 < out.size(); i++) {
      const int d = std::abs(out[i + 1] - out[i]);
      if (i >= join - 4 && i <= join + 4) jump = std::max(jump, d);
      else usual = std::max(usual, d);
    }
    CHECK(jump <= usual, "loop join: biggest step %d, %d around it", jump, usual);
    if (argc >= 3) write_wav(argv[2], resample(pcm, rate, 48000, 5), 48000);
  }
  printf(failures ? "%d FAILED\n" : "all passed\n", failures);
  return failures ? 1 : 0;
}
