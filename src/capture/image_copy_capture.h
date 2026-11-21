#pragma once

#include "capture/screencopy_capture.h"
#include "core/timer_manager.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>

class ImageCaptureGlobals;
class WaylandConnection;
struct ImageCopyCapturePending;
struct wl_output;

// Single-frame whole-output snapshots through ext-image-copy-capture, for compositors that
// never expose wlr-screencopy (COSMIC). region/overlayCursor exist for ScreencopyCapture API
// parity: frames always cover the whole output (callers crop) and the cursor rides the ICC
// pointer sessions PointerCursorCapture already runs.
class ImageCopyCapture {
public:
  using CompletionCallback = std::function<void(std::optional<ScreencopyImage>, std::string error)>;

  explicit ImageCopyCapture(WaylandConnection& wayland);
  ~ImageCopyCapture();

  bool available() const noexcept;
  bool busy() const noexcept { return m_pending != nullptr; }
  void capture(wl_output* output, std::optional<LogicalRect> region, bool overlayCursor, CompletionCallback onComplete);
  void cancelInFlight();

private:
  friend struct ImageCopyCapturePending;
  void fail(std::string message);
  void finish(ScreencopyImage image);

  WaylandConnection& m_wayland;
  ImageCaptureGlobals& m_imageCapture;
  std::unique_ptr<ImageCopyCapturePending> m_pending;
  CompletionCallback m_onComplete;
  Timer m_timeout;
};
