#include "capture/image_copy_capture.h"

#include "capture/shm_format_util.h"
#include "ext-image-capture-source-v1-client-protocol.h"
#include "ext-image-copy-capture-v1-client-protocol.h"
#include "render/core/image_resize.h"
#include "shell/wayland/capture_globals.h"
#include "wayland/wayland_connection.h"

#include <cerrno>
#include <chrono>
#include <limits>
#include <new>
#include <sys/mman.h>
#include <unistd.h>
#include <utility>

struct ImageCopyCapturePending {
  ImageCopyCapture* owner = nullptr;
  ext_image_capture_source_v1* source = nullptr;
  ext_image_copy_capture_session_v1* session = nullptr;
  ext_image_copy_capture_frame_v1* frame = nullptr;
  wl_shm_pool* pool = nullptr;
  wl_buffer* buffer = nullptr;
  void* mapped = MAP_FAILED;
  std::size_t mappedSize = 0;
  int fd = -1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t format = 0;
  bool hasFormat = false;
  bool receivingConstraints = false;
  bool constraintsRetried = false;
  std::uint32_t bufferFormat = 0;
  int bufferWidth = 0;
  int bufferHeight = 0;

  ~ImageCopyCapturePending() {
    releaseBufferResources();
    if (session != nullptr) {
      ext_image_copy_capture_session_v1_destroy(session);
    }
    if (source != nullptr) {
      ext_image_capture_source_v1_destroy(source);
    }
  }

  // Drops frame and buffer only. The session and its constraint listener stay alive so a
  // re-negotiated constraint batch can restart the capture (see failed()).
  void releaseBufferResources() {
    if (frame != nullptr) {
      ext_image_copy_capture_frame_v1_destroy(frame);
      frame = nullptr;
    }
    if (buffer != nullptr) {
      wl_buffer_destroy(buffer);
      buffer = nullptr;
    }
    if (pool != nullptr) {
      wl_shm_pool_destroy(pool);
      pool = nullptr;
    }
    if (mapped != MAP_FAILED) {
      munmap(mapped, mappedSize);
      mapped = MAP_FAILED;
      mappedSize = 0;
    }
    if (fd >= 0) {
      close(fd);
      fd = -1;
    }
  }

  void beginConstraints() {
    if (!receivingConstraints) {
      receivingConstraints = true;
      width = 0;
      height = 0;
      hasFormat = false;
    }
  }

  static void bufferSize(void* data, ext_image_copy_capture_session_v1*, std::uint32_t width, std::uint32_t height) {
    auto& pending = *static_cast<ImageCopyCapturePending*>(data);
    pending.beginConstraints();
    pending.width = width;
    pending.height = height;
  }

  static void shmFormat(void* data, ext_image_copy_capture_session_v1*, std::uint32_t format) {
    auto& pending = *static_cast<ImageCopyCapturePending*>(data);
    pending.beginConstraints();
    const int preference = formatPreference(format);
    if (preference > 0 && (!pending.hasFormat || preference > formatPreference(pending.format))) {
      pending.format = format;
      pending.hasFormat = true;
    }
  }

  static void dmabufDevice(void* data, ext_image_copy_capture_session_v1*, wl_array*) {
    static_cast<ImageCopyCapturePending*>(data)->beginConstraints();
  }

  static void dmabufFormat(void* data, ext_image_copy_capture_session_v1*, std::uint32_t, wl_array*) {
    static_cast<ImageCopyCapturePending*>(data)->beginConstraints();
  }

  static void constraintsDone(void* data, ext_image_copy_capture_session_v1*) {
    auto& pending = *static_cast<ImageCopyCapturePending*>(data);
    pending.receivingConstraints = false;
    if (pending.frame == nullptr) {
      pending.captureFrame();
    }
  }

  static void stopped(void* data, ext_image_copy_capture_session_v1*) {
    static_cast<ImageCopyCapturePending*>(data)->owner->fail("output capture session stopped");
  }

  static void frameTransform(void*, ext_image_copy_capture_frame_v1*, std::uint32_t) {}
  static void damage(void*, ext_image_copy_capture_frame_v1*, std::int32_t, std::int32_t, std::int32_t, std::int32_t) {}
  static void presentationTime(void*, ext_image_copy_capture_frame_v1*, std::uint32_t, std::uint32_t, std::uint32_t) {}

  static void ready(void* data, ext_image_copy_capture_frame_v1*) {
    auto& pending = *static_cast<ImageCopyCapturePending*>(data);
    auto* owner = pending.owner;
    try {
      // Whole-output frame in tightly packed RGBA, physical pixels, unoriented.
      // orientCaptureNative() applies the wl_output transform afterwards.
      ScreencopyImage image;
      image.width = pending.bufferWidth;
      image.height = pending.bufferHeight;
      image.rgba = resizeToRgba(
          static_cast<const std::uint8_t*>(pending.mapped), pending.bufferWidth, pending.bufferHeight,
          resizeLayoutFor(pending.bufferFormat), pending.bufferWidth, pending.bufferHeight
      );
      if (image.rgba.empty()) {
        owner->fail("failed to decode output capture frame");
        return;
      }
      owner->finish(std::move(image));
    } catch (const std::bad_alloc&) {
      owner->fail("failed to allocate output capture frame");
    }
  }

  static void failed(void* data, ext_image_copy_capture_frame_v1*, std::uint32_t reason) {
    auto& pending = *static_cast<ImageCopyCapturePending*>(data);
    auto* owner = pending.owner;
    switch (reason) {
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS:
      // Re-negotiate once: drop frame and buffer, let the re-issued constraint batch re-arm
      // the capture. The capture timeout backstops a batch that never arrives.
      if (!pending.constraintsRetried) {
        pending.constraintsRetried = true;
        pending.releaseBufferResources();
        return;
      }
      owner->fail("output capture buffer constraints changed");
      return;
    case EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED:
      owner->fail("output capture session stopped");
      return;
    default:
      owner->fail("compositor failed to capture output image");
      return;
    }
  }

  inline static const ext_image_copy_capture_session_v1_listener sessionListener = {
      .buffer_size = bufferSize,
      .shm_format = shmFormat,
      .dmabuf_device = dmabufDevice,
      .dmabuf_format = dmabufFormat,
      .done = constraintsDone,
      .stopped = stopped,
  };
  inline static const ext_image_copy_capture_frame_v1_listener frameListener = {
      .transform = frameTransform,
      .damage = damage,
      .presentation_time = presentationTime,
      .ready = ready,
      .failed = failed,
  };

  void captureFrame() {
    if (width == 0 || height == 0) {
      owner->fail("output image is unavailable (empty buffer constraints)");
      return;
    }
    if (!hasFormat) {
      owner->fail("output capture has no supported shared-memory format");
      return;
    }
    const auto maxPoolSize = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
    if (width > maxPoolSize / 4U || height > maxPoolSize / (width * 4U)) {
      owner->fail("output capture buffer is too large");
      return;
    }
    bufferWidth = static_cast<int>(width);
    bufferHeight = static_cast<int>(height);
    bufferFormat = format;
    // ICC announces no stride, so the client picks one: width * 4 is bpp-aligned for every
    // 8888 layout accepted above and needs no padding row.
    const int stride = bufferWidth * 4;
    mappedSize = static_cast<std::size_t>(stride) * height;
#ifdef __linux__
    fd = memfd_create("noctalia-output-capture", MFD_CLOEXEC | MFD_ALLOW_SEALING);
#endif
    if (fd < 0 || ftruncate(fd, static_cast<off_t>(mappedSize)) < 0) {
      owner->fail("failed to allocate output capture shared-memory file");
      return;
    }
    mapped = mmap(nullptr, mappedSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
      owner->fail("failed to map output capture shared-memory buffer");
      return;
    }
    pool = wl_shm_create_pool(owner->m_wayland.shm(), fd, static_cast<std::int32_t>(mappedSize));
    if (pool == nullptr) {
      owner->fail("failed to create output capture shared-memory pool");
      return;
    }
    buffer = wl_shm_pool_create_buffer(pool, 0, bufferWidth, bufferHeight, stride, bufferFormat);
    if (buffer == nullptr) {
      owner->fail("failed to create output capture shared-memory buffer");
      return;
    }
    frame = ext_image_copy_capture_session_v1_create_frame(session);
    if (frame == nullptr || ext_image_copy_capture_frame_v1_add_listener(frame, &frameListener, this) != 0) {
      owner->fail("failed to create output capture frame");
      return;
    }
    ext_image_copy_capture_frame_v1_attach_buffer(frame, buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(frame, 0, 0, bufferWidth, bufferHeight);
    ext_image_copy_capture_frame_v1_capture(frame);
    if (wl_display_flush(owner->m_wayland.display()) < 0 && errno != EAGAIN) {
      owner->fail("failed to submit output capture frame");
    }
  }
};

ImageCopyCapture::ImageCopyCapture(WaylandConnection& wayland)
    : m_wayland(wayland), m_imageCapture(wayland.extension<ImageCaptureGlobals>()) {}

ImageCopyCapture::~ImageCopyCapture() { cancelInFlight(); }

bool ImageCopyCapture::available() const noexcept {
  return m_imageCapture.outputCaptureAvailable() && m_wayland.shm() != nullptr && m_wayland.display() != nullptr;
}

void ImageCopyCapture::capture(
    wl_output* output, std::optional<LogicalRect> region, bool overlayCursor, CompletionCallback onComplete
) {
  (void)region;
  (void)overlayCursor;
  if (busy() || !available() || output == nullptr) {
    if (onComplete) {
      onComplete(std::nullopt, busy() ? "output capture already in progress" : "output capture unavailable");
    }
    return;
  }
  try {
    m_pending = std::make_unique<ImageCopyCapturePending>();
  } catch (const std::bad_alloc&) {
    if (onComplete) {
      onComplete(std::nullopt, "failed to allocate output capture session");
    }
    return;
  }
  m_onComplete = std::move(onComplete);
  m_pending->owner = this;
  m_pending->source =
      ext_output_image_capture_source_manager_v1_create_source(m_imageCapture.outputSourceManager(), output);
  if (m_pending->source == nullptr) {
    fail("failed to create output image capture source");
    return;
  }
  m_pending->session =
      ext_image_copy_capture_manager_v1_create_session(m_imageCapture.copyCaptureManager(), m_pending->source, 0);
  if (m_pending->session == nullptr
      || ext_image_copy_capture_session_v1_add_listener(
             m_pending->session, &ImageCopyCapturePending::sessionListener, m_pending.get()
         ) != 0) {
    fail("failed to create output image capture session");
    return;
  }
  m_timeout.start(std::chrono::seconds(1), [this] {
    fail("output capture timed out: compositor did not provide a capturable image");
  });
  if (wl_display_flush(m_wayland.display()) < 0 && errno != EAGAIN) {
    fail("failed to submit output capture session");
  }
}

void ImageCopyCapture::cancelInFlight() {
  m_timeout.stop();
  m_pending.reset();
  m_onComplete = {};
}

void ImageCopyCapture::fail(std::string message) {
  auto onComplete = std::exchange(m_onComplete, {});
  m_timeout.stop();
  m_pending.reset();
  if (onComplete) {
    onComplete(std::nullopt, std::move(message));
  }
}

void ImageCopyCapture::finish(ScreencopyImage image) {
  auto onComplete = std::exchange(m_onComplete, {});
  m_timeout.stop();
  m_pending.reset();
  if (onComplete) {
    onComplete(std::move(image), {});
  }
}
