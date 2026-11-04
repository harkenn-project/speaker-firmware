// The player's resampler (player.cpp): any rate in, the codec's rate out. Plain C++, so it can be tested on a PC
// (test/resample_test.cpp).
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include <algorithm>

namespace player {

// Resampler: a low-pass (two biquads, Butterworth) then linear interpolation to the codec's rate, then the same
// low-pass again at the new rate, which clears the faint mirror images linear interpolation leaves. The cut-off keeps
// music out of the wake word arbitration tones (19 kHz; audio.h) and below half the output rate.
struct Biquad {
  float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
  void lowpass(float fc, float fs, float q) {
    const float w = 2 * 3.14159265f * fc / fs, c = cosf(w), alpha = sinf(w) / (2 * q), a0 = 1 + alpha;
    b0 = (1 - c) / 2 / a0;
    b1 = (1 - c) / a0;
    b2 = b0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
  }
  float run(float x) {
    const float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }
};

struct Resampler {
  int in_rate = 0, out_rate = 0;
  Biquad lp[2], post[2];
  float step = 1, phase = 0, prev = 0, cur = 0;
  bool have_cur = false;  // cur is filtered input not yet used up

  void setup(int in, int out) {
    if (in == in_rate && out == out_rate) return;  // same rates: keep the state, so loops and tracks run on
    in_rate = in;
    out_rate = out;
    const float fc = std::min({0.45f * in, 0.42f * out, 11000.0f});
    lp[0].lowpass(fc, in, 0.5412f);
    lp[1].lowpass(fc, in, 1.3066f);
    const float fc_out = std::min(0.42f * out, 11000.0f);
    post[0].lowpass(fc_out, out, 0.5412f);
    post[1].lowpass(fc_out, out, 1.3066f);
    step = static_cast<float>(in) / out;
    phase = 0;
  }

  // Input samples in, output samples out (as many as fit in `max_out`); returns how many inputs it took.
  size_t run(const int16_t* in, size_t n_in, int16_t* out, size_t max_out, size_t* n_out, float gain) {
    size_t used = 0, made = 0;
    while (made < max_out) {
      if (!have_cur) {
        if (used >= n_in) break;
        cur = lp[1].run(lp[0].run(in[used++]));  // each input through the filter exactly once
        have_cur = true;
      }
      while (phase < 1.0f && made < max_out) {
        const float y = post[1].run(post[0].run(prev + (cur - prev) * phase)) * gain;
        out[made++] = static_cast<int16_t>(std::max(-32768.0f, std::min(32767.0f, y)));
        phase += step;
      }
      if (phase >= 1.0f) {
        phase -= 1.0f;
        prev = cur;
        have_cur = false;
      }
    }
    *n_out = made;
    return used;
  }
};

}  // namespace player
