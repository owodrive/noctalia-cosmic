#pragma once

#include "render/core/image_resize.h"

#include <cstdint>
#include <wayland-client-protocol.h>

// Format selection and conversion mapping shared by the shm capture backends.

// Higher is preferred when choosing a wl_shm format to request.
inline int formatPreference(std::uint32_t format) {
  switch (format) {
  case WL_SHM_FORMAT_ARGB8888:
    return 4;
  case WL_SHM_FORMAT_ABGR8888:
    return 3;
  case WL_SHM_FORMAT_XRGB8888:
    return 2;
  case WL_SHM_FORMAT_XBGR8888:
    return 1;
  default:
    return 0;
  }
}

// wl_shm names formats by little-endian word order, so ARGB8888 is B, G, R, A in memory.
inline ResizeSourceLayout resizeLayoutFor(std::uint32_t format) noexcept {
  switch (format) {
  case WL_SHM_FORMAT_ARGB8888:
    return ResizeSourceLayout::BgraPremultiplied;
  case WL_SHM_FORMAT_XRGB8888:
    return ResizeSourceLayout::Bgrx;
  case WL_SHM_FORMAT_ABGR8888:
    return ResizeSourceLayout::RgbaPremultiplied;
  default:
    return ResizeSourceLayout::Rgbx;
  }
}
