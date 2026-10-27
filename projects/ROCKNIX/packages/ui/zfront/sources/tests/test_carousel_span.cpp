// The carousel builds every tile that can be on screen, also the half-visible one about to slide in from the edge: it must exist
// before the camera crosses an integer position, or it pops in.
#include <cmath>

#include "check.hpp"
#include "scene.hpp"

using namespace zf;

int main() {
  for (int n : {2, 3, 4, 5, 8, 20}) {
    const int half = (n < 5 ? n : 5) / 2;
    for (float cam = -4.0f; cam < 12.0f; cam += 0.05f) {
      int first = 0, last = 0;
      carousel_span(cam, n, first, last);
      // tiles at rest sit up to half + 1 positions away and the next one is visible from about 0.4 earlier
      const float reach = float(half) + 1.4f;
      for (int p = int(std::floor(cam - reach)) - 1; p <= int(std::ceil(cam + reach)) + 1; ++p) {
        if (std::fabs(float(p) - cam) <= reach) CHECK(p >= first && p <= last);
      }
      CHECK(last - first <= 2 * (half + 2));  // and it is not a lot more than that
    }
  }
  return zt::done();
}
