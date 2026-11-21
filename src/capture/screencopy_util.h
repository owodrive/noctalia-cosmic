#pragma once

#include "capture/screencopy_capture.h"
#include "wayland/wayland_connection.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace screencopy {

  inline constexpr auto kBlockingCaptureTimeout = std::chrono::milliseconds(500);

  enum class [[nodiscard]] CaptureOutputStatus : std::uint8_t {
    Success,
    CaptureFailed,
    EventLoopFailed,
  };

  // Blocking single-output capture, templated over the backend so both frame backends
  // (ScreencopyCapture for wlr-screencopy, ImageCopyCapture for ext-image-copy-capture)
  // share one pump: each only needs capture()/busy()/cancelInFlight(), which they share.
  template <typename CaptureBackend>
  CaptureOutputStatus captureOutputBlocking(
      CaptureBackend& capture, WaylandConnection& wayland, wl_output* output, ScreencopyImage& out, std::string& error,
      bool overlayCursor = false, WaylandConnection* eventConnection = nullptr,
      std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt
  ) {
    error.clear();
    const auto captureDeadline = deadline.value_or(std::chrono::steady_clock::now() + kBlockingCaptureTimeout);
    if (std::chrono::steady_clock::now() >= captureDeadline) {
      error = "capture timed out";
      return CaptureOutputStatus::CaptureFailed;
    }

    bool finished = false;
    capture.capture(
        output, std::nullopt, overlayCursor, [&](std::optional<ScreencopyImage> image, const std::string& err) {
          finished = true;
          if (!err.empty() || !image.has_value()) {
            error = err.empty() ? "capture failed" : err;
            return;
          }
          out = std::move(*image);
        }
    );

    if (!error.empty()) {
      return CaptureOutputStatus::CaptureFailed;
    }

    const std::array targets{
        wayland::DispatchTarget{
            .connection = eventConnection != &wayland ? eventConnection : nullptr,
            .role = "primary",
        },
        wayland::DispatchTarget{
            .connection = &wayland,
            .role = "capture",
        },
    };
    const wayland::DispatchResult waitResult =
        wayland::dispatchUntil(targets, captureDeadline, [&finished, &capture] { return finished || !capture.busy(); });
    if (waitResult.status != wayland::DispatchStatus::Completed) {
      capture.cancelInFlight();
      if (waitResult.status == wayland::DispatchStatus::TimedOut) {
        error = "capture timed out";
        return CaptureOutputStatus::CaptureFailed;
      }
      error = waitResult.error;
      return CaptureOutputStatus::EventLoopFailed;
    }

    if (!error.empty() || !finished) {
      if (error.empty()) {
        error = "capture failed";
      }
      return CaptureOutputStatus::CaptureFailed;
    }

    if (out.width <= 0 || out.height <= 0 || out.rgba.empty()) {
      error = "capture returned an empty frame";
      return CaptureOutputStatus::CaptureFailed;
    }

    return CaptureOutputStatus::Success;
  }

  [[nodiscard]] bool orientCaptureNative(ScreencopyImage& image, const WaylandConnection& wayland, wl_output* output);

  void orientCaptureForTransform(ScreencopyImage& image, std::int32_t transform);

  void transformCapture(ScreencopyImage& image, std::int32_t transform);

} // namespace screencopy
