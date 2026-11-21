#include "capture/screenshot_capture.h"

#include "capture/screencopy_util.h"
#include "core/application_identity.h"
#include "wayland/wayland_connection.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>

namespace {
  // NOCTALIA_SCREENSHOT_BACKEND=screencopy|icc forces a backend. Unset prefers wlr-screencopy,
  // falling back to ICC only when the compositor never exposes it (documented in shell.mdx).
  enum class BackendMode {
    Auto,
    Screencopy,
    Icc,
  };

  BackendMode backendModeFromEnv() {
    const std::string name = applicationEnvVar("SCREENSHOT_BACKEND");
    const char* value = std::getenv(name.c_str());
    if (value != nullptr) {
      const std::string_view mode(value);
      if (mode == "screencopy") {
        return BackendMode::Screencopy;
      }
      if (mode == "icc") {
        return BackendMode::Icc;
      }
    }
    return BackendMode::Auto;
  }
} // namespace

ScreenshotCapture::ScreenshotCapture(WaylandConnection& wayland)
    : m_wayland(wayland), m_backgroundCapture(wayland), m_iccCapture(wayland), m_cursorCapture(wayland) {}

ScreenshotCapture::~ScreenshotCapture() { cancelInFlight(); }

bool ScreenshotCapture::available() const noexcept {
  switch (backendModeFromEnv()) {
  case BackendMode::Screencopy:
    return m_backgroundCapture.available();
  case BackendMode::Icc:
    return m_iccCapture.available();
  case BackendMode::Auto:
    break;
  }
  return m_backgroundCapture.available() || m_iccCapture.available();
}

void ScreenshotCapture::capture(
    wl_output* output, std::optional<LogicalRect> region, bool showCursor, bool retainCursor,
    CompletionCallback onComplete
) {
  if (m_busy) {
    if (onComplete) {
      onComplete(std::nullopt, "capture already in progress");
    }
    return;
  }
  m_useIcc = !m_backgroundCapture.available();
  switch (backendModeFromEnv()) {
  case BackendMode::Screencopy:
    m_useIcc = false;
    break;
  case BackendMode::Icc:
    m_useIcc = true;
    break;
  case BackendMode::Auto:
    break;
  }
  m_busy = true;
  m_output = output;
  m_region = region;
  m_showCursor = showCursor;
  m_cursor.reset();
  m_cursorStatus = capture::CursorToggleStatus::NotCaptured;
  m_onComplete = std::move(onComplete);
  if ((retainCursor || (m_useIcc && showCursor)) && m_cursorCapture.available() && output != nullptr) {
    m_cursorCapture.capture(
        output, [this](std::optional<capture::CapturedCursor> cursor, const std::string& /*error*/) {
          m_cursor = std::move(cursor);
          m_cursorStatus =
              m_cursor ? capture::CursorToggleStatus::Available : capture::CursorToggleStatus::CaptureFailed;
          captureBackground();
        }
    );
    return;
  }
  captureBackground();
}

void ScreenshotCapture::captureBackground() {
  m_timeout.start(std::chrono::seconds(1), [this] {
    m_backgroundCapture.cancelInFlight();
    m_iccCapture.cancelInFlight();
    finish(std::nullopt, "capture frame timed out");
  });
  const auto onFrame = [this](std::optional<ScreencopyImage> image, std::string error) {
    finish(std::move(image), std::move(error));
  };
  if (m_useIcc) {
    m_iccCapture.capture(m_output, std::nullopt, false, onFrame);
    return;
  }
  // Capturing an overlaid wlr frame would bake the cursor into every concurrent output copy.
  m_backgroundCapture.capture(m_output, m_cursor ? std::nullopt : m_region, m_cursor ? false : m_showCursor, onFrame);
}

void ScreenshotCapture::cancelInFlight() {
  m_timeout.stop();
  m_cursorCapture.cancelInFlight();
  m_backgroundCapture.cancelInFlight();
  m_iccCapture.cancelInFlight();
  m_cursor.reset();
  m_busy = false;
  m_onComplete = {};
}

void ScreenshotCapture::finish(std::optional<ScreencopyImage> image, std::string error) {
  m_timeout.stop();
  std::optional<capture::ScreenshotImage> result;
  // Whole-output frames need a client-side crop when a region was requested.
  const auto cropToRegion = [this](std::optional<capture::ScreenshotImage>& shot) {
    const auto output = std::ranges::find(m_wayland.outputs(), m_output, &WaylandOutput::output);
    if (output == m_wayland.outputs().end()) {
      shot.reset();
      return;
    }
    shot = capture::cropScreenshotImage(*shot, output->logicalWidth, output->logicalHeight, *m_region);
  };
  if (image) {
    if (!screencopy::orientCaptureNative(*image, m_wayland, m_output)) {
      error = "Failed to orient screenshot";
    } else if (!m_cursor) {
      // ICC output sessions are created with paint_cursors off, so only the screencopy server
      // bakes the cursor into the frame itself.
      result = capture::ScreenshotImage{
          .image = std::move(*image),
          .cursorVisible = m_showCursor && !m_useIcc,
          .cursorStatus = m_cursorStatus,
      };
      if (result && m_useIcc && m_region) {
        cropToRegion(result);
        if (!result) {
          error = "Failed to crop screenshot";
        }
      }
    } else {
      result = capture::composeCapturedCursor(std::move(*image), std::move(*m_cursor));
      if (result && m_region) {
        cropToRegion(result);
        if (!result) {
          error = "Failed to crop screenshot";
        }
      }
      if (result && m_showCursor) {
        if (result->alternative) {
          std::swap(result->image, *result->alternative);
        }
        result->cursorVisible = true;
      }
    }
  }
  m_cursor.reset();
  m_busy = false;
  auto onComplete = std::exchange(m_onComplete, {});
  if (onComplete) {
    onComplete(std::move(result), std::move(error));
  }
}
