#include "pages/camera/libcamera_backend.hpp"
#include "pages/camera/frame_transform.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <utility>

#ifndef CAMERA_HAS_LIBCAMERA
#define CAMERA_HAS_LIBCAMERA 0
#endif
#ifndef CAMERA_HAS_K230
#define CAMERA_HAS_K230 0
#endif

#if CAMERA_HAS_LIBCAMERA
#include <libcamera/camera.h>
#include <libcamera/camera_manager.h>
#include <libcamera/control_ids.h>
#include <libcamera/formats.h>
#include <libcamera/framebuffer_allocator.h>
#include <libcamera/orientation.h>
#include <libcamera/property_ids.h>
#include <libcamera/request.h>
#include <libcamera/stream.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if CAMERA_HAS_K230
#include <dirent.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
#endif

#if CAMERA_HAS_LIBCAMERA
namespace {
bool orientation_parts(libcamera::Orientation orientation, int &rotation, bool &mirrored)
{
    mirrored = false;
    switch (orientation) {
        case libcamera::Orientation::Rotate0:
            rotation = 0;
            return true;
        case libcamera::Orientation::Rotate90:
            rotation = 90;
            return true;
        case libcamera::Orientation::Rotate180:
            rotation = 180;
            return true;
        case libcamera::Orientation::Rotate270:
            rotation = 270;
            return true;
        case libcamera::Orientation::Rotate0Mirror:
            rotation = 0;
            mirrored = true;
            return true;
        case libcamera::Orientation::Rotate90Mirror:
            rotation = 90;
            mirrored = true;
            return true;
        case libcamera::Orientation::Rotate180Mirror:
            rotation = 180;
            mirrored = true;
            return true;
        case libcamera::Orientation::Rotate270Mirror:
            rotation = 270;
            mirrored = true;
            return true;
    }
    return false;
}

bool sync_buffer(int fd, std::uint64_t flags)
{
    dma_buf_sync sync{flags};
    int result;
    do {
        result = ::ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

bool duration_bound(const libcamera::ControlValue &value, bool lower, std::int64_t &bound)
{
    if (value.type() != libcamera::ControlTypeInteger64 || value.numElements() == 0) return false;
    if (!value.isArray()) {
        bound = value.get<std::int64_t>();
    } else {
        // The target headers select libcamera::Span (Debian 0.4) or std::span
        // (newer CM0 images). Keep dynamic extent for per-element bounds.
        using DurationLimits = decltype(libcamera::controls::FrameDurationLimits)::type;
        using DurationValues = decltype(std::declval<DurationLimits>().first(std::size_t{}));
        const auto values = value.get<DurationValues>();
        // libcamera pipelines may describe an array control with scalar bounds
        // or with a bound for each element. Both duration limits must fit.
        bound =
            lower ? *std::max_element(values.begin(), values.end()) : *std::min_element(values.begin(), values.end());
    }
    return bound > 0;
}
}  // namespace

struct LibcameraBackend::Impl {
    struct Mapping {
        void *address      = MAP_FAILED;
        std::size_t length = 0;
        std::size_t offset = 0;
        std::size_t bytes  = 0;
        int fd             = -1;
    };

    std::unique_ptr<libcamera::CameraManager> manager;
    std::shared_ptr<libcamera::Camera> camera;
    std::unique_ptr<libcamera::CameraConfiguration> configuration;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator;
    libcamera::Stream *stream = nullptr;
    std::vector<std::unique_ptr<libcamera::Request>> requests;
    std::map<const libcamera::FrameBuffer *, Mapping> mappings;
    mutable std::mutex mutex;
    std::condition_variable frame_ready;
    // Completed requests are handed to the worker through this queue. Old
    // requests are discarded when the worker falls behind so the next preview
    // frame stays close to the sensor's current output.
    std::deque<libcamera::Request *> pending_frames;
    bool retain_all_frames = false;
    std::atomic<std::uint64_t> completed_count{0};
    bool trace_enabled = false;
    std::chrono::steady_clock::time_point trace_started;
    std::uint64_t preview_count = 0, full_count = 0, processed_count = 0, processing_us = 0;
    std::atomic<bool> streaming{false};
    std::atomic<int> zoom{1};
    bool manager_started   = false;
    bool acquired          = false;
    bool camera_started    = false;
    bool supports_crop     = false;
    bool swap_red_blue     = false;
    bool pipeline_mirrored = false;
    int pipeline_rotation  = 0;
    int display_rotation   = 0;
    libcamera::Rectangle maximum_crop;
    std::string model;
    std::string failure;
    std::int64_t frame_duration = 33333;

    ~Impl()
    {
        close();
    }

    void unmap()
    {
        for (auto &item : mappings) {
            if (item.second.address != MAP_FAILED) ::munmap(item.second.address, item.second.length);
        }
        mappings.clear();
    }

    void close()
    {
        // stop() synchronously finishes/cancels outstanding callbacks. Keep the
        // request and mapped buffer ownership intact until it returns.
        streaming.store(false);
        frame_ready.notify_all();
        if (camera) {
            if (camera_started) camera->stop();
            camera->requestCompleted.disconnect(this);
            camera->disconnected.disconnect(this);
        }
        camera_started = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            pending_frames.clear();
            retain_all_frames = false;
        }
        requests.clear();
        unmap();
        allocator.reset();
        configuration.reset();
        stream = nullptr;
        if (camera) {
            if (acquired) camera->release();
            camera.reset();
        }
        acquired = false;
        if (manager) {
            if (manager_started) manager->stop();
            manager.reset();
        }
        manager_started = false;
    }

    void report_failure(const std::string &message)
    {
        streaming.store(false);
        std::lock_guard<std::mutex> lock(mutex);
        if (failure.empty()) failure = message;
        frame_ready.notify_all();
    }

    void disconnected()
    {
        report_failure("Camera disconnected.");
    }

    bool map_buffer(const libcamera::FrameBuffer *buffer, std::string &error)
    {
        if (buffer->planes().size() != 1) {
            error = "libcamera RGB images require a single-plane buffer.";
            return false;
        }
        const auto &plane = buffer->planes().front();
        const auto &cfg   = configuration->at(0);
        const std::size_t required =
            static_cast<std::size_t>(cfg.stride) * (cfg.size.height - 1) + static_cast<std::size_t>(cfg.size.width) * 3;
        if (plane.length < required) {
            error = "libcamera image buffer is too small.";
            return false;
        }
        const long page_value          = ::sysconf(_SC_PAGE_SIZE);
        const std::size_t page         = page_value > 0 ? static_cast<std::size_t>(page_value) : 4096;
        const std::size_t plane_offset = plane.offset;
        const std::size_t map_offset   = plane_offset - plane_offset % page;
        const std::size_t data_offset  = plane_offset - map_offset;
        if (plane.length > std::numeric_limits<std::size_t>::max() - data_offset) {
            error = "libcamera buffer size overflow.";
            return false;
        }
        const std::size_t map_length = data_offset + plane.length;
        void *address =
            ::mmap(nullptr, map_length, PROT_READ, MAP_SHARED, plane.fd.get(), static_cast<off_t>(map_offset));
        if (address == MAP_FAILED) {
            error = std::string("Could not map the libcamera image buffer: ") + std::strerror(errno);
            return false;
        }
        mappings.emplace(buffer, Mapping{address, map_length, data_offset, plane.length, plane.fd.get()});
        return true;
    }

    bool copy_frame(libcamera::FrameBuffer *buffer, LibcameraFrame *preview, LibcameraFrame *full_resolution)
    {
        const auto mapping_it = mappings.find(buffer);
        if (mapping_it == mappings.end()) {
            report_failure("libcamera returned an unmapped image buffer.");
            return false;
        }
        const auto &mapping = mapping_it->second;
        const auto &cfg     = configuration->at(0);
        const std::size_t required =
            static_cast<std::size_t>(cfg.stride) * (cfg.size.height - 1) + static_cast<std::size_t>(cfg.size.width) * 3;
        const auto planes = buffer->metadata().planes();
        if (mapping.bytes < required || (!planes.empty() && planes[0].bytesused && planes[0].bytesused < required)) {
            report_failure("libcamera returned insufficient image data.");
            return false;
        }
        if (!sync_buffer(mapping.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
            report_failure(std::string("Could not read the camera DMA buffer: ") + std::strerror(errno));
            return false;
        }
        // Even allocation/transform exceptions must end CPU access before the
        // request guard queues this DMA buffer back to the camera.
        struct ReadGuard {
            int fd;
            bool ended = false;
            ~ReadGuard()
            {
                if (!ended) sync_buffer(fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
            }
        } guard{mapping.fd};
        const camera_frame::Source source{static_cast<const std::uint8_t *>(mapping.address) + mapping.offset,
                                          mapping.bytes,
                                          static_cast<int>(cfg.size.width),
                                          static_cast<int>(cfg.size.height),
                                          cfg.stride,
                                          swap_red_blue};
        const camera_frame::Orientation orientation{pipeline_rotation, pipeline_mirrored, display_rotation};
        bool ok = true;
        if (preview) {
            preview->width  = display_rotation % 180 ? 360 : 640;
            preview->height = display_rotation % 180 ? 640 : 360;
            ok = camera_frame::transform_bgr(source, orientation, preview->width, preview->height, preview->bgr);
        }
        if (ok && full_resolution) {
            ok = camera_frame::output_size(source, orientation, full_resolution->width, full_resolution->height) &&
                 camera_frame::transform_bgr(source, orientation, full_resolution->width, full_resolution->height,
                                             full_resolution->bgr);
        }
        guard.ended = sync_buffer(mapping.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
        if (!guard.ended) {
            report_failure(std::string("Could not synchronize the camera DMA buffer: ") + std::strerror(errno));
            return false;
        }
        if (!ok) report_failure("Could not convert the libcamera image size or orientation.");
        return ok;
    }

    void trace_frame(bool preview, bool full_resolution, std::chrono::steady_clock::time_point start)
    {
        if (!trace_enabled) return;
        const auto now = std::chrono::steady_clock::now();
        processing_us += std::chrono::duration_cast<std::chrono::microseconds>(now - start).count();
        ++processed_count;
        preview_count += preview;
        full_count += full_resolution;
        const double seconds = std::chrono::duration<double>(now - trace_started).count();
        if (seconds < 2.0) return;
        std::fprintf(stderr, "[camera] sensor=%.1f fps preview=%.1f fps full=%.1f fps transform=%.2f ms/frame\n",
                     completed_count.exchange(0) / seconds, preview_count / seconds, full_count / seconds,
                     processing_us / (1000.0 * processed_count));
        trace_started = now;
        preview_count = full_count = processed_count = processing_us = 0;
    }

    bool consume(libcamera::Request *request, LibcameraFrame *preview, LibcameraFrame *full_resolution)
    {
        if (!request) return false;
        struct RequestGuard {
            Impl *owner;
            libcamera::Request *request;
            ~RequestGuard()
            {
                owner->requeue(request);
            }
        } guard{this, request};
        const auto start = std::chrono::steady_clock::now();
        const bool ok    = copy_frame(request->findBuffer(stream), preview, full_resolution);
        if (ok) trace_frame(preview != nullptr, full_resolution != nullptr, start);
        return ok;
    }

    void apply_zoom(libcamera::Request *request)
    {
        if (!supports_crop) return;
        const unsigned factor = static_cast<unsigned>(zoom.load());
        auto crop             = maximum_crop;
        crop.width            = std::max(1u, crop.width / factor);
        crop.height           = std::max(1u, crop.height / factor);
        crop.x += static_cast<int>((maximum_crop.width - crop.width) / 2);
        crop.y += static_cast<int>((maximum_crop.height - crop.height) / 2);
        request->controls().set(libcamera::controls::ScalerCrop, crop);
    }

    void requeue(libcamera::Request *request)
    {
        if (!request || !streaming.load()) return;
        request->reuse(libcamera::Request::ReuseBuffers);
        apply_zoom(request);  // reuse() clears request controls.
        if (camera->queueRequest(request) < 0 && streaming.load())
            report_failure("Could not requeue the libcamera image request.");
    }

    void completed(libcamera::Request *request)
    {
        if (!request || request->status() == libcamera::Request::RequestCancelled || !streaming.load()) return;
        auto *buffer = request->findBuffer(stream);
        if (!buffer) {
            report_failure("libcamera request is missing an image buffer.");
            return;
        }
        if (buffer->metadata().status != libcamera::FrameMetadata::FrameSuccess) {
            requeue(request);
            return;
        }
        ++completed_count;
        libcamera::Request *obsolete = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!streaming.load()) return;
            if (!retain_all_frames && pending_frames.size() >= preview_queue_limit()) {
                obsolete = pending_frames.front();
                pending_frames.pop_front();
            }
            pending_frames.push_back(request);
        }
        frame_ready.notify_one();
        // Never rotate/copy pixels on CameraManager's event thread. Stale
        // requests go straight back to capture so a slow full-resolution
        // conversion cannot turn the preview into a delayed FIFO.
        if (obsolete) requeue(obsolete);
    }

    std::size_t preview_queue_limit() const
    {
        // Keep at least one request available for capture. The allocator owns
        // the definitive bound, so this remains safe across camera pipelines
        // that expose different buffer counts.
        return std::max<std::size_t>(1, requests.size() > 1 ? requests.size() - 1 : 1);
    }

    bool configure_stream(CameraProfile profile, std::string &error)
    {
        const auto settings       = camera_profile_settings(profile);
        auto &stream_config       = configuration->at(0);
        stream_config.size        = {static_cast<unsigned>(settings.width), static_cast<unsigned>(settings.height)};
        stream_config.pixelFormat = libcamera::formats::RGB888;
        stream_config.bufferCount = 4;
        if (configuration->validate() == libcamera::CameraConfiguration::Invalid) {
            error = "Invalid libcamera viewfinder configuration.";
            return false;
        }
        const auto &cfg = configuration->at(0);
        if (cfg.size.width != static_cast<unsigned>(settings.width) ||
            cfg.size.height != static_cast<unsigned>(settings.height)) {
            error = "libcamera cannot output the selected resolution: " + std::to_string(settings.width) + "×" +
                    std::to_string(settings.height) + " (actual: " + cfg.size.toString() + ").";
            return false;
        }
        if (configuration->sensorConfig &&
            (configuration->sensorConfig->outputSize.width < static_cast<unsigned>(settings.width) ||
             configuration->sensorConfig->outputSize.height < static_cast<unsigned>(settings.height))) {
            error = "libcamera selected a sensor resolution below the selected quality mode.";
            return false;
        }
        if (cfg.pixelFormat != libcamera::formats::RGB888 && cfg.pixelFormat != libcamera::formats::BGR888) {
            error = "libcamera does not support RGB888/BGR888 image output: " + cfg.pixelFormat.toString();
            return false;
        }
        if (cfg.stride < cfg.size.width * 3u || cfg.stride > 1024u * 1024u ||
            !orientation_parts(configuration->orientation, pipeline_rotation, pipeline_mirrored)) {
            error = "libcamera returned an unsupported row stride or orientation.";
            return false;
        }
        swap_red_blue = cfg.pixelFormat == libcamera::formats::BGR888;
        if (camera->configure(configuration.get())) {
            error = "libcamera viewfinder configuration failed.";
            return false;
        }

        // The range is mode-dependent and is only authoritative after configure().
        // Merely setting FrameDurationLimits can otherwise silently clamp 60 FPS
        // to the slower sensor mode that automatic selection chose.
        const auto duration_info = camera->controls().find(&libcamera::controls::FrameDurationLimits);
        std::int64_t minimum = 0, maximum = 0;
        if (duration_info == camera->controls().end() || !duration_bound(duration_info->second.min(), true, minimum) ||
            !duration_bound(duration_info->second.max(), false, maximum)) {
            error = "The camera did not provide a verifiable frame duration range. Cannot enable " +
                    std::string(settings.label) + ".";
            return false;
        }
        if (frame_duration < minimum || frame_duration > maximum) {
            error = "The current camera sensor mode does not support " + std::string(settings.label) +
                    " (maximum about " + std::to_string(1000000 / minimum) + " FPS).";
            return false;
        }
        return true;
    }

    bool configure_profile(CameraProfile profile, bool imx219, std::string &error)
    {
        if (configure_stream(profile, error)) return true;
        if (!imx219) return false;
        const std::string automatic_error = error;

        // On IMX219 the pipeline's automatic choice can prefer a larger, slower
        // sensor mode. Try its other native 10-bit modes before declaring the
        // profile unsupported. This is done before allocating any buffers.
        auto raw = camera->generateConfiguration({libcamera::StreamRole::Raw});
        if (!raw || raw->empty()) return false;
        const auto settings = camera_profile_settings(profile);
        auto sizes          = raw->at(0).formats().sizes(raw->at(0).pixelFormat);
        std::sort(sizes.begin(), sizes.end(), [](const libcamera::Size &a, const libcamera::Size &b) {
            return static_cast<std::uint64_t>(a.width) * a.height < static_cast<std::uint64_t>(b.width) * b.height;
        });
        for (const auto &size : sizes) {
            // In particular, never upscale IMX219's fast 640x480 mode and call
            // it a higher quality profile. The sensor must supply at least the
            // requested detail.
            if (size.width < static_cast<unsigned>(settings.width) ||
                size.height < static_cast<unsigned>(settings.height))
                continue;
            libcamera::SensorConfiguration sensor_config;
            sensor_config.bitDepth      = 10;
            sensor_config.outputSize    = size;
            configuration->sensorConfig = sensor_config;
            if (configure_stream(profile, error)) {
                error.clear();
                return true;
            }
        }
        error = automatic_error;
        return false;
    }

    bool open(int requested_rotation, std::string &error, CameraProfile profile)
    {
        close();
        error.clear();
        {
            std::lock_guard<std::mutex> lock(mutex);
            failure.clear();
            model.clear();
        }
        if (requested_rotation != 0 && requested_rotation != 90 && requested_rotation != 180 &&
            requested_rotation != 270) {
            error = "Unsupported camera rotation angle.";
            return false;
        }
        if (!valid_camera_profile(profile)) {
            error = "Unsupported camera quality mode.";
            return false;
        }
        const auto settings = camera_profile_settings(profile);
        frame_duration      = (1000000 + settings.fps / 2) / settings.fps;
        display_rotation    = requested_rotation;
        const char *trace   = std::getenv("CAMERA_TRACE");
        trace_enabled       = trace && std::string(trace) == "1";
        trace_started       = std::chrono::steady_clock::now();
        completed_count     = 0;
        preview_count = full_count = processed_count = processing_us = 0;
        zoom.store(1);
        manager = std::make_unique<libcamera::CameraManager>();
        if (manager->start()) {
            error = "libcamera CameraManager failed to start.";
            return false;
        }
        manager_started = true;
        bool imx219     = false;
        for (const auto &candidate : manager->cameras()) {
            const auto model_property = candidate->properties().get(libcamera::properties::Model);
            const std::string name    = model_property ? std::string(*model_property) : candidate->id();
            std::string normalized    = name;
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool preferred = normalized.find("imx219") != std::string::npos;
            if (!camera || preferred) {
                camera = candidate;
                model  = name;
                imx219 = preferred;
                if (preferred) break;
            }
        }
        if (!camera) {
            error = "libcamera found no cameras.";
            return false;
        }
        if (camera->acquire()) {
            error = "Could not acquire the libcamera camera.";
            return false;
        }
        acquired      = true;
        configuration = camera->generateConfiguration({libcamera::StreamRole::Viewfinder});
        if (!configuration || configuration->size() != 1) {
            error = "Could not create the libcamera viewfinder configuration.";
            return false;
        }
        // Rotate0 asks libcamera to compensate the device-tree sensor mounting
        // rotation. An extra hard-coded Rotate180 would invert this board again.
        configuration->orientation = libcamera::Orientation::Rotate0;
        if (!configure_profile(profile, imx219, error)) return false;
        const auto crop = camera->properties().get(libcamera::properties::ScalerCropMaximum);
        supports_crop   = crop && crop->width && crop->height &&
                        camera->controls().find(&libcamera::controls::ScalerCrop) != camera->controls().end();
        if (supports_crop) maximum_crop = *crop;
        stream = configuration->at(0).stream();
        if (!stream) {
            error = "libcamera did not create a viewfinder stream.";
            return false;
        }
        allocator = std::make_unique<libcamera::FrameBufferAllocator>(camera);
        if (allocator->allocate(stream) <= 0) {
            error = "Could not allocate libcamera viewfinder buffers.";
            return false;
        }
        for (const auto &buffer : allocator->buffers(stream)) {
            if (!map_buffer(buffer.get(), error)) return false;
            auto request = camera->createRequest();
            if (!request || request->addBuffer(stream, buffer.get()) < 0) {
                error = "Could not create a libcamera request.";
                return false;
            }
            apply_zoom(request.get());
            requests.push_back(std::move(request));
        }
        if (requests.empty()) {
            error = "libcamera did not allocate image requests.";
            return false;
        }
        camera->requestCompleted.connect(this, &Impl::completed);
        camera->disconnected.connect(this, &Impl::disconnected);
        libcamera::ControlList start_controls(camera->controls());
        if (camera->controls().find(&libcamera::controls::AfMode) != camera->controls().end())
            start_controls.set(libcamera::controls::AfMode, libcamera::controls::AfModeContinuous);
        // The initializer-list overload infers int from literals; this control requires int64_t.
        const std::array<std::int64_t, 2> frame_durations{frame_duration, frame_duration};
        start_controls.set(libcamera::controls::FrameDurationLimits, frame_durations);
        if (camera->start(&start_controls)) {
            error = "libcamera camera failed to start.";
            return false;
        }
        camera_started = true;
        streaming.store(true);
        for (auto &request : requests) {
            if (camera->queueRequest(request.get()) < 0) {
                error = "Could not queue the libcamera request.";
                return false;
            }
        }
        return true;
    }

    libcamera::Request *take_request(bool wait, bool newest)
    {
        std::vector<libcamera::Request *> obsolete;
        libcamera::Request *request = nullptr;
        std::unique_lock<std::mutex> lock(mutex);
        if (wait)
            frame_ready.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return !pending_frames.empty() || !streaming.load(); });
        if (!streaming.load()) return nullptr;
        if (pending_frames.empty()) return nullptr;
        if (!newest) {
            request = pending_frames.front();
            pending_frames.pop_front();
        } else {
            request = pending_frames.back();
            pending_frames.pop_back();
            obsolete.reserve(pending_frames.size());
            while (!pending_frames.empty()) {
                obsolete.push_back(pending_frames.front());
                pending_frames.pop_front();
            }
        }
        lock.unlock();
        for (auto *stale : obsolete) requeue(stale);
        return request;
    }

    bool next_frame(LibcameraFrame &preview, LibcameraFrame *full_resolution)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            // A full-resolution copy is needed for recording, but it must not
            // turn the preview queue into a FIFO of stale frames. The recorder
            // already fills timing gaps with the newest frame, so dropping
            // obsolete requests is preferable to adding visible latency.
            retain_all_frames = false;
        }
        return consume(take_request(false, full_resolution != nullptr), &preview, full_resolution);
    }

    bool capture_frame(LibcameraFrame &frame, std::string &error)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            retain_all_frames = true;
        }
        if (consume(take_request(true, true), nullptr, &frame)) return true;
        std::lock_guard<std::mutex> lock(mutex);
        error = failure.empty() ? "Timed out waiting for a camera frame. Please retry." : failure;
        return false;
    }

    bool set_zoom(double value, std::string &error)
    {
        if (value != 1.0 && value != 2.0) {
            error = "Zoom must be 1× or 2×.";
            return false;
        }
        if (!streaming.load()) {
            error = "The camera has not started yet.";
            return false;
        }
        if (value != 1.0 && !supports_crop) {
            error = "This camera does not support ScalerCrop zoom.";
            return false;
        }
        zoom.store(static_cast<int>(value));
        return true;
    }
};
#elif CAMERA_HAS_K230
namespace {

constexpr const char *kIspMediaServer   = "/usr/bin/isp_media_server";
constexpr const char *kIspSensorDriver  = "/usr/lib/libvvcam.so";

bool ioctl_retry(int fd, unsigned long request, void *argument)
{
    int result;
    do {
        result = ::ioctl(fd, request, argument);
    } while (result < 0 && errno == EINTR);
    return result == 0;
}

bool process_running(const char *name)
{
    const std::string command = std::string("pidof ") + name + " >/dev/null 2>&1";
    const int result           = std::system(command.c_str());
    return result >= 0 && WIFEXITED(result) && WEXITSTATUS(result) == 0;
}

bool start_isp_media_server(std::string &error)
{
    // A system service may own the ISP. Reuse it even after a capture failure;
    // restarting here can kill another consumer or spawn an unprivileged
    // duplicate when the application cannot terminate the root-owned daemon.
    if (process_running("isp_media_server")) return true;

    const int output = ::open("/tmp/isp.out.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    const int errors = ::open("/tmp/isp.err.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (output < 0 || errors < 0) {
        const int saved = errno;
        if (output >= 0) ::close(output);
        if (errors >= 0) ::close(errors);
        error = std::string("Could not open ISP log files: ") + std::strerror(saved);
        return false;
    }

    std::vector<std::string> environment;
    for (char **entry = environ; entry && *entry; ++entry) {
        if (std::strncmp(*entry, "ISP_MEDIA_SENSOR_DRIVER=", 24) != 0) environment.emplace_back(*entry);
    }
    environment.emplace_back(std::string("ISP_MEDIA_SENSOR_DRIVER=") + kIspSensorDriver);
    std::vector<char *> envp;
    envp.reserve(environment.size() + 1);
    for (auto &entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);

    char arg0[] = "isp_media_server";
    char *argv[] = {arg0, nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errors, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, output);
    posix_spawn_file_actions_addclose(&actions, errors);
    pid_t pid = -1;
    const int result = posix_spawn(&pid, kIspMediaServer, &actions, nullptr, argv, envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(output);
    ::close(errors);
    if (result != 0) {
        error = std::string("Could not start isp_media_server: ") + std::strerror(result);
        return false;
    }
    return true;
}

std::string fourcc(uint32_t value)
{
    std::string result(4, '?');
    result[0] = static_cast<char>(value & 0xff);
    result[1] = static_cast<char>((value >> 8) & 0xff);
    result[2] = static_cast<char>((value >> 16) & 0xff);
    result[3] = static_cast<char>((value >> 24) & 0xff);
    return result;
}

std::vector<std::string> k230_camera_devices()
{
    if (const char *override = std::getenv("CAMERA_K230_DEVICE"); override && *override)
        return {override};

    std::vector<std::string> devices;
    DIR *directory = ::opendir("/dev");
    if (!directory) return devices;
    while (dirent *entry = ::readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.size() < 6 || name.compare(0, 5, "video") != 0) continue;
        const auto suffix = name.substr(5);
        if (suffix.find_first_not_of("0123456789") != std::string::npos) continue;
        devices.emplace_back("/dev/" + name);
    }
    ::closedir(directory);
    std::sort(devices.begin(), devices.end(), [](const std::string &left, const std::string &right) {
        return std::stoi(left.substr(std::strlen("/dev/video"))) <
               std::stoi(right.substr(std::strlen("/dev/video")));
    });
    // The v0.2.9 BSP's primary ISP output accepts 720P S_FMT but produces
    // displaced rows and chroma. The SP2 output scales correctly. Identify it
    // by its V4L2 name, since codec registration changes /dev/video numbering.
    std::stable_partition(devices.begin(), devices.end(), [](const std::string &device) {
        const auto name_path = std::string("/sys/class/video4linux/") + device.substr(5) + "/name";
        std::FILE *file = std::fopen(name_path.c_str(), "r");
        if (!file) return false;
        char name[64]{};
        const bool read = std::fscanf(file, "%63s", name) == 1;
        std::fclose(file);
        const std::string driver = read ? name : "";
        return driver.rfind("vvcam-video.", 0) == 0 &&
               driver.size() >= 2 && driver.compare(driver.size() - 2, 2, ".2") == 0;
    });
    return devices;
}

}  // namespace

struct LibcameraBackend::Impl {
    struct Buffer {
        void *address     = MAP_FAILED;
        std::size_t length = 0;
    };

    int fd                 = -1;
    bool streaming         = false;
    std::uint32_t pixel_format = 0;
    int width              = 0;
    int height             = 0;
    std::size_t stride     = 0;
    std::size_t image_size = 0;
    int display_rotation   = 0;
    int profile_width      = 0;
    int profile_height     = 0;
    int zoom               = 1;
    std::vector<Buffer> buffers;
    std::string device_path;
    mutable std::mutex mutex;
    std::string model;
    std::string failure;

    ~Impl()
    {
        close();
    }

    void report_failure(const std::string &message)
    {
        std::lock_guard<std::mutex> lock(mutex);
        failure = message;
    }

    void unmap_buffers()
    {
        for (auto &buffer : buffers) {
            if (buffer.address != MAP_FAILED) ::munmap(buffer.address, buffer.length);
        }
        buffers.clear();
    }

    void close()
    {
        if (fd >= 0 && streaming) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)ioctl_retry(fd, VIDIOC_STREAMOFF, &type);
        }
        streaming = false;
        unmap_buffers();
        if (fd >= 0) ::close(fd);
        fd          = -1;
        device_path.clear();
        pixel_format = 0;
        width       = height = 0;
        stride      = image_size = 0;
    }

    bool enum_format(std::uint32_t requested, std::string &error) const
    {
        v4l2_fmtdesc description{};
        description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (;;) {
            if (!ioctl_retry(fd, VIDIOC_ENUM_FMT, &description)) {
                if (errno != EINVAL)
                    error = std::string("Could not enumerate K230 camera formats: ") + std::strerror(errno);
                else
                    error = "K230 camera does not expose the requested NV16/NV12 format.";
                return false;
            }
            if (description.pixelformat == requested) return true;
            ++description.index;
        }
    }

    bool configure_device(std::uint32_t requested, std::string &error)
    {
        // The BSP can corrupt both row and chroma placement when its scaler
        // returns from 1080P to 720P, even on SP2. Keep GC2093 capture native;
        // output profiles are sampled in software without reconfiguring ISP.
        const auto settings = camera_profile_settings(CameraProfile::FullHd30);
        v4l2_capability capability{};
        if (!ioctl_retry(fd, VIDIOC_QUERYCAP, &capability)) {
            error = std::string("Could not query K230 camera capabilities: ") + std::strerror(errno);
            return false;
        }
        const std::uint32_t capabilities = capability.device_caps ? capability.device_caps : capability.capabilities;
        if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 || (capabilities & V4L2_CAP_STREAMING) == 0) {
            error = "K230 camera node does not provide V4L2 capture streaming.";
            return false;
        }
        if (!enum_format(requested, error)) return false;

        v4l2_format format{};
        format.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        format.fmt.pix.width       = static_cast<__u32>(settings.width);
        format.fmt.pix.height      = static_cast<__u32>(settings.height);
        format.fmt.pix.pixelformat = requested;
        format.fmt.pix.field       = V4L2_FIELD_NONE;
        if (!ioctl_retry(fd, VIDIOC_S_FMT, &format)) {
            error = std::string("Could not configure K230 camera format: ") + std::strerror(errno);
            return false;
        }
        if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV16 && format.fmt.pix.pixelformat != V4L2_PIX_FMT_NV12) {
            error = "K230 camera selected an unsupported pixel format: " + fourcc(format.fmt.pix.pixelformat);
            return false;
        }
        if (format.fmt.pix.width == 0 || format.fmt.pix.height == 0) {
            error = "K230 camera returned an empty capture size.";
            return false;
        }

        width         = static_cast<int>(format.fmt.pix.width);
        height        = static_cast<int>(format.fmt.pix.height);
        stride        = format.fmt.pix.bytesperline ? format.fmt.pix.bytesperline : format.fmt.pix.width;
        pixel_format  = format.fmt.pix.pixelformat;
        const auto rows = pixel_format == V4L2_PIX_FMT_NV12 ? static_cast<std::size_t>(height) * 3 / 2
                                                              : static_cast<std::size_t>(height) * 2;
        const auto minimum_size = stride * rows;
        image_size = std::max<std::size_t>(format.fmt.pix.sizeimage, minimum_size);
        if (stride < static_cast<std::size_t>(width) || image_size < minimum_size) {
            error = "K230 camera returned an invalid stride or buffer size.";
            return false;
        }
        return true;
    }

    bool allocate_buffers(std::string &error)
    {
        v4l2_requestbuffers request{};
        request.count  = 4;
        request.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        request.memory = V4L2_MEMORY_MMAP;
        if (!ioctl_retry(fd, VIDIOC_REQBUFS, &request) || request.count < 2) {
            error = std::string("Could not allocate K230 camera buffers: ") + std::strerror(errno);
            return false;
        }
        buffers.reserve(request.count);
        for (std::uint32_t index = 0; index < request.count; ++index) {
            v4l2_buffer buffer{};
            buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index  = index;
            if (!ioctl_retry(fd, VIDIOC_QUERYBUF, &buffer)) {
                error = std::string("Could not query K230 camera buffer: ") + std::strerror(errno);
                return false;
            }
            // Several V4L2 MMAP implementations reject a read-only mapping
            // even when the consumer only inspects completed frames.
            void *address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
            if (address == MAP_FAILED) {
                error = std::string("Could not map K230 camera buffer: ") + std::strerror(errno);
                return false;
            }
            buffers.push_back({address, buffer.length});
        }
        for (std::uint32_t index = 0; index < buffers.size(); ++index) {
            v4l2_buffer buffer{};
            buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buffer.memory = V4L2_MEMORY_MMAP;
            buffer.index  = index;
            if (!ioctl_retry(fd, VIDIOC_QBUF, &buffer)) {
                error = std::string("Could not queue K230 camera buffer: ") + std::strerror(errno);
                return false;
            }
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (!ioctl_retry(fd, VIDIOC_STREAMON, &type)) {
            error = std::string("Could not start K230 camera stream: ") + std::strerror(errno);
            return false;
        }
        streaming = true;
        return true;
    }

    bool prepare(std::string &error)
    {
        const std::array<std::uint32_t, 2> formats{V4L2_PIX_FMT_NV16, V4L2_PIX_FMT_NV12};
        std::string last_error = "K230 camera formats are not available.";
        for (const auto &device : k230_camera_devices()) {
            for (const auto requested : formats) {
                close();
                fd = ::open(device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
                if (fd < 0) {
                    last_error = std::string("Could not open K230 camera node ") + device + ": " +
                                 std::strerror(errno);
                    continue;
                }
                device_path = device;
                if (!configure_device(requested, last_error)) continue;
                if (!allocate_buffers(last_error)) continue;
                return true;
            }
        }
        close();
        error = last_error;
        return false;
    }

    bool dequeue(v4l2_buffer &buffer, std::string &error)
    {
        pollfd descriptor{fd, POLLIN, 0};
        int result;
        do {
            result = ::poll(&descriptor, 1, 2000);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            error = "Timed out waiting for a K230 camera frame.";
            return false;
        }
        if (result < 0) {
            error = std::string("Could not wait for a K230 camera frame: ") + std::strerror(errno);
            return false;
        }
        buffer = {};
        buffer.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (!ioctl_retry(fd, VIDIOC_DQBUF, &buffer)) {
            error = std::string("Could not dequeue a K230 camera frame: ") + std::strerror(errno);
            return false;
        }
        if (buffer.index >= buffers.size() || buffers[buffer.index].address == MAP_FAILED) {
            error = "K230 camera returned an invalid buffer index.";
            return false;
        }
        return true;
    }

    bool copy_frame(const v4l2_buffer &buffer, LibcameraFrame *preview, LibcameraFrame *full, std::string &error)
    {
        const auto &mapped = buffers[buffer.index];
        // Some BSP revisions leave bytesused at the luma-plane size. QUERYBUF
        // describes the full single-plane MMAP layout, including chroma.
        const camera_frame::NvSource source{static_cast<const std::uint8_t *>(mapped.address), mapped.length,
                                             width, height, stride, pixel_format == V4L2_PIX_FMT_NV12};
        bool ok = true;
        if (preview) {
            preview->width  = display_rotation % 180 ? 360 : 640;
            preview->height = display_rotation % 180 ? 640 : 360;
            ok = camera_frame::transform_nv_to_bgr(source, display_rotation, zoom,
                                                  preview->width, preview->height, preview->bgr);
        }
        if (ok && full) {
            full->width  = display_rotation % 180 ? profile_height : profile_width;
            full->height = display_rotation % 180 ? profile_width : profile_height;
            ok = camera_frame::transform_nv_to_bgr(source, display_rotation, zoom,
                                                  full->width, full->height, full->bgr);
        }
        if (!ok) error = "Could not convert the K230 camera frame layout, orientation or size.";
        return ok;
    }

    bool open(int requested_rotation, std::string &error, CameraProfile profile)
    {
        close();
        error.clear();
        {
            std::lock_guard<std::mutex> lock(mutex);
            failure.clear();
            model.clear();
        }
        if (requested_rotation != 0 && requested_rotation != 90 && requested_rotation != 180 &&
            requested_rotation != 270) {
            error = "Unsupported camera rotation angle.";
            return false;
        }
        if (!valid_camera_profile(profile)) {
            error = "Unsupported camera quality mode.";
            return false;
        }
        const auto settings = camera_profile_settings(profile);
        profile_width       = settings.width;
        profile_height      = settings.height;
        display_rotation    = requested_rotation;
        zoom                = 1;

        std::string server_error;
        for (int attempt = 0; attempt < 8; ++attempt) {
            if (attempt == 0) {
                if (!start_isp_media_server(server_error) && !server_error.empty()) error = server_error;
            }
            if (prepare(error)) {
                std::lock_guard<std::mutex> lock(mutex);
                model = "GC2093 via " + device_path + " (" + fourcc(pixel_format) + ")";
                std::fprintf(stderr, "[camera] K230 capture %s %s %dx%d stride=%zu; software output %dx%d\n",
                             device_path.c_str(), fourcc(pixel_format).c_str(), width, height, stride,
                             profile_width, profile_height);
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        if (error.empty()) error = "K230 camera pipeline is unavailable.";
        close();
        return false;
    }

    bool set_profile(CameraProfile profile, std::string &error)
    {
        if (!streaming || !valid_camera_profile(profile)) {
            error = !streaming ? "K230 camera is not streaming." : "Unsupported camera quality mode.";
            return false;
        }
        const auto settings = camera_profile_settings(profile);
        profile_width = settings.width;
        profile_height = settings.height;
        error.clear();
        return true;
    }

    bool next_frame(LibcameraFrame &preview, LibcameraFrame *full)
    {
        v4l2_buffer buffer{};
        std::string error;
        if (!dequeue(buffer, error)) {
            report_failure(error);
            return false;
        }
        bool ok = false;
        try {
            ok = copy_frame(buffer, &preview, full, error);
        } catch (const std::exception &exception) {
            error = std::string("Could not process the K230 camera frame: ") + exception.what();
        }
        v4l2_buffer queued{};
        queued.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        queued.memory = V4L2_MEMORY_MMAP;
        queued.index  = buffer.index;
        if (!ioctl_retry(fd, VIDIOC_QBUF, &queued)) {
            if (error.empty()) error = std::string("Could not requeue the K230 camera buffer: ") + std::strerror(errno);
            ok = false;
        }
        if (!ok) report_failure(error);
        return ok;
    }

    bool capture_frame(LibcameraFrame &frame, std::string &error)
    {
        v4l2_buffer buffer{};
        if (!dequeue(buffer, error)) {
            report_failure(error);
            return false;
        }
        bool ok = false;
        try {
            ok = copy_frame(buffer, nullptr, &frame, error);
        } catch (const std::exception &exception) {
            error = std::string("Could not process the K230 camera frame: ") + exception.what();
        }
        v4l2_buffer queued{};
        queued.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        queued.memory = V4L2_MEMORY_MMAP;
        queued.index  = buffer.index;
        if (!ioctl_retry(fd, VIDIOC_QBUF, &queued)) {
            if (error.empty()) error = std::string("Could not requeue the K230 camera buffer: ") + std::strerror(errno);
            ok = false;
        }
        if (!ok) report_failure(error);
        return ok;
    }

    bool set_zoom(double value, std::string &error)
    {
        if (value != 1.0 && value != 2.0) {
            error = "Zoom must be 1× or 2×.";
            return false;
        }
        if (!streaming) {
            error = "The camera has not started yet.";
            return false;
        }
        zoom = static_cast<int>(value);
        return true;
    }
};
#else
struct LibcameraBackend::Impl {};
#endif

LibcameraBackend::LibcameraBackend() : impl_(std::make_unique<Impl>())
{
}
LibcameraBackend::~LibcameraBackend()
{
    close();
}

bool LibcameraBackend::open(int rotation, std::string &error, CameraProfile profile)
{
#if CAMERA_HAS_LIBCAMERA
    if (impl_->open(rotation, error, profile)) return true;
    impl_->close();
    return false;
#elif CAMERA_HAS_K230
    if (impl_->open(rotation, error, profile)) return true;
    impl_->close();
    return false;
#else
    (void)rotation;
    (void)profile;
    error = "This build does not include the libcamera C++ backend.";
    return false;
#endif
}

bool LibcameraBackend::set_profile(CameraProfile profile, std::string &error)
{
#if CAMERA_HAS_LIBCAMERA
    return open(impl_->display_rotation, error, profile);
#elif CAMERA_HAS_K230
    return impl_->set_profile(profile, error);
#else
    (void)profile;
    error = "This build does not include a native camera backend.";
    return false;
#endif
}

void LibcameraBackend::close() noexcept
{
#if CAMERA_HAS_LIBCAMERA
    if (impl_) impl_->close();
#elif CAMERA_HAS_K230
    if (impl_) impl_->close();
#endif
}

bool LibcameraBackend::next_frame(LibcameraFrame &preview, LibcameraFrame *full_resolution)
{
#if CAMERA_HAS_LIBCAMERA
    return impl_->next_frame(preview, full_resolution);
#elif CAMERA_HAS_K230
    return impl_->next_frame(preview, full_resolution);
#else
    (void)preview;
    (void)full_resolution;
    return false;
#endif
}

bool LibcameraBackend::capture_frame(LibcameraFrame &frame, std::string &error)
{
#if CAMERA_HAS_LIBCAMERA
    return impl_->capture_frame(frame, error);
#elif CAMERA_HAS_K230
    return impl_->capture_frame(frame, error);
#else
    (void)frame;
    error = "This build does not include the libcamera C++ backend.";
    return false;
#endif
}

bool LibcameraBackend::set_zoom(double zoom, std::string &error)
{
#if CAMERA_HAS_LIBCAMERA
    return impl_->set_zoom(zoom, error);
#elif CAMERA_HAS_K230
    return impl_->set_zoom(zoom, error);
#else
    (void)zoom;
    error = "This build does not include the libcamera C++ backend.";
    return false;
#endif
}

std::string LibcameraBackend::sensor() const
{
#if CAMERA_HAS_LIBCAMERA
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->model;
#elif CAMERA_HAS_K230
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->model;
#else
    return {};
#endif
}

std::string LibcameraBackend::last_error() const
{
#if CAMERA_HAS_LIBCAMERA
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->failure;
#elif CAMERA_HAS_K230
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->failure;
#else
    return {};
#endif
}
