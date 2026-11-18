/* SPDX-License-Identifier: MIT
 * Test of my_formats() in nobgra.c: includes the layer source and replaces the next layer with a fake list. */
#include <stdio.h>
#include "../sources/nobgra.c"

static VkSurfaceFormatKHR fake[4]; static uint32_t fake_n;
static VkResult fake_formats(VkPhysicalDevice pd, VkSurfaceKHR s, uint32_t *c, VkSurfaceFormatKHR *f) {
  (void)pd; (void)s;
  if (!f) { *c = fake_n; return VK_SUCCESS; }
  uint32_t k = *c < fake_n ? *c : fake_n;
  for (uint32_t i = 0; i < k; i++) f[i] = fake[i];
  *c = k; return k < fake_n ? VK_INCOMPLETE : VK_SUCCESS;
}
static int bad;
#define CHECK(cond, msg) do { if (!(cond)) { printf("  FAIL %s\n", msg); bad++; } else printf("  PASS %s\n", msg); } while (0)
int main(void) {
  VkSurfaceFormatKHR out[4]; uint32_t c;
  next_formats = fake_formats;
  /* BGRA + RGBA: only RGBA remains */
  fake[0].format = VK_FORMAT_B8G8R8A8_UNORM; fake[1].format = VK_FORMAT_R8G8B8A8_UNORM; fake[2].format = VK_FORMAT_B8G8R8A8_SRGB; fake_n = 3;
  my_formats(0, 0, &c, NULL); CHECK(c == 1, "count with RGBA available = 1");
  c = 4; my_formats(0, 0, &c, out); CHECK(c == 1 && out[0].format == VK_FORMAT_R8G8B8A8_UNORM, "list = RGBA only");
  /* only BGRA: the original list comes back (not an empty one) */
  fake[0].format = VK_FORMAT_B8G8R8A8_UNORM; fake[1].format = VK_FORMAT_B8G8R8A8_SRGB; fake_n = 2;
  my_formats(0, 0, &c, NULL); CHECK(c == 2, "count with only BGRA = 2 (original list)");
  c = 4; my_formats(0, 0, &c, out); CHECK(c == 2 && out[0].format == VK_FORMAT_B8G8R8A8_UNORM && out[1].format == VK_FORMAT_B8G8R8A8_SRGB, "list with only BGRA is the original");
  c = 1; VkResult r = my_formats(0, 0, &c, out); CHECK(c == 1 && r == VK_INCOMPLETE, "small buffer: VK_INCOMPLETE");
  fake_n = 0; my_formats(0, 0, &c, NULL); CHECK(c == 0, "empty list stays empty");
  return bad != 0;
}
