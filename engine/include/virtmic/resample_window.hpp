#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace virtmic::resample {

// Drop consumed samples from the front of `src`, keeping `pos` relative to the
// new window. When the whole buffer is consumed, clear it and subtract
// `src.size()` from `pos` so the fractional phase is preserved.
inline void AdvanceSlidingWindow(std::vector<float>& src, double& pos) {
  if (src.empty()) return;
  const std::size_t drop = static_cast<std::size_t>(std::floor(pos));
  if (drop == 0) return;
  if (drop < src.size()) {
    src.erase(src.begin(),
              src.begin() + static_cast<std::ptrdiff_t>(drop));
    pos -= static_cast<double>(drop);
  } else {
    pos -= static_cast<double>(src.size());
    src.clear();
  }
}

// Linear-resample `src` toward `dst_rate`, appending into `dst`. Updates
// `pos` and shrinks `src` via AdvanceSlidingWindow.
inline void LinearResampleToRate(std::vector<float>& src, double& pos,
                                 int src_rate, int dst_rate,
                                 std::vector<float>& dst) {
  if (src.empty() || src_rate <= 0 || dst_rate <= 0) return;
  const double ratio =
      static_cast<double>(dst_rate) / static_cast<double>(src_rate);
  while (true) {
    const double src_index = pos;
    const std::size_t i0 = static_cast<std::size_t>(std::floor(src_index));
    const std::size_t i1 = i0 + 1;
    if (i1 >= src.size()) break;

    const double frac = src_index - static_cast<double>(i0);
    const float s0 = src[i0];
    const float s1 = src[i1];
    dst.push_back(static_cast<float>((1.0 - frac) * s0 + frac * s1));
    pos += 1.0 / ratio;
  }
  AdvanceSlidingWindow(src, pos);
}

}  // namespace virtmic::resample
