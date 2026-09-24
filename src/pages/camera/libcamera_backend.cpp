#include "pages/camera/libcamera_backend.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

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

#if CAMERA_HAS_LIBCAMERA
namespace {
void rotate_pixel(int x, int y, int width, int height, int rotation, int &out_x, int &out_y)
{
    switch (rotation) {
        case 90:
            out_x = height - 1 - y;
            out_y = x;
            break;
        case 180:
            out_x = width - 1 - x;
            out_y = height - 1 - y;
            break;
        case 270:
            out_x = y;
            out_y = width - 1 - x;
            break;
        default:
            out_x = x;
            out_y = y;
            break;
    }
}

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
    LibcameraFrame latest;
    bool has_frame = false;
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
        if (camera) {
            if (camera_started) camera->stop();
            camera->requestCompleted.disconnect(this);
            camera->disconnected.disconnect(this);
        }
        camera_started = false;
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
        std::lock_guard<std::mutex> lock(mutex);
        latest    = {};
        has_frame = false;
    }

    void report_failure(const std::string &message)
    {
        streaming.store(false);
        std::lock_guard<std::mutex> lock(mutex);
        if (failure.empty()) failure = message;
    }

    void disconnected()
    {
        report_failure("摄像头连接已断开。");
    }

    bool map_buffer(const libcamera::FrameBuffer *buffer, std::string &error)
    {
        if (buffer->planes().size() != 1) {
            error = "libcamera RGB 图像必须使用单平面缓冲区。";
            return false;
        }
        const auto &plane = buffer->planes().front();
        const auto &cfg   = configuration->at(0);
        const std::size_t required =
            static_cast<std::size_t>(cfg.stride) * (cfg.size.height - 1) + static_cast<std::size_t>(cfg.size.width) * 3;
        if (plane.length < required) {
            error = "libcamera 图像缓冲区长度不足。";
            return false;
        }
        const long page_value          = ::sysconf(_SC_PAGE_SIZE);
        const std::size_t page         = page_value > 0 ? static_cast<std::size_t>(page_value) : 4096;
        const std::size_t plane_offset = plane.offset;
        const std::size_t map_offset   = plane_offset - plane_offset % page;
        const std::size_t data_offset  = plane_offset - map_offset;
        if (plane.length > std::numeric_limits<std::size_t>::max() - data_offset) {
            error = "libcamera 缓冲区长度溢出。";
            return false;
        }
        const std::size_t map_length = data_offset + plane.length;
        void *address =
            ::mmap(nullptr, map_length, PROT_READ, MAP_SHARED, plane.fd.get(), static_cast<off_t>(map_offset));
        if (address == MAP_FAILED) {
            error = std::string("无法映射 libcamera 图像缓冲区：") + std::strerror(errno);
            return false;
        }
        mappings.emplace(buffer, Mapping{address, map_length, data_offset, plane.length, plane.fd.get()});
        return true;
    }

    bool copy_frame(libcamera::FrameBuffer *buffer)
    {
        const auto mapping_it = mappings.find(buffer);
        if (mapping_it == mappings.end()) {
            report_failure("libcamera 返回了未映射的图像缓冲区。");
            return false;
        }
        const auto &mapping = mapping_it->second;
        const auto &cfg     = configuration->at(0);
        const int width     = static_cast<int>(cfg.size.width);
        const int height    = static_cast<int>(cfg.size.height);
        const std::size_t required =
            static_cast<std::size_t>(cfg.stride) * (height - 1) + static_cast<std::size_t>(width) * 3;
        const auto planes = buffer->metadata().planes();
        if (mapping.bytes < required || (!planes.empty() && planes[0].bytesused && planes[0].bytesused < required)) {
            report_failure("libcamera 返回的图像数据长度不足。");
            return false;
        }
        if (!sync_buffer(mapping.fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ)) {
            report_failure(std::string("无法读取相机 DMA 缓冲区：") + std::strerror(errno));
            return false;
        }

        const int upright_width  = pipeline_rotation % 180 ? height : width;
        const int upright_height = pipeline_rotation % 180 ? width : height;
        LibcameraFrame frame;
        frame.width  = display_rotation % 180 ? upright_height : upright_width;
        frame.height = display_rotation % 180 ? upright_width : upright_height;
        frame.bgr.resize(static_cast<std::size_t>(frame.width) * frame.height * 3);
        const auto *src = static_cast<const std::uint8_t *>(mapping.address) + mapping.offset;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                // Undo the validated pipeline orientation, then apply the
                // clockwise display rotation shared with simulation and export.
                const int unmirrored_x = pipeline_mirrored ? width - 1 - x : x;
                int upright_x, upright_y, out_x, out_y;
                rotate_pixel(unmirrored_x, y, width, height, (360 - pipeline_rotation) % 360, upright_x, upright_y);
                rotate_pixel(upright_x, upright_y, upright_width, upright_height, display_rotation, out_x, out_y);
                const auto *pixel = src + static_cast<std::size_t>(y) * cfg.stride + static_cast<std::size_t>(x) * 3;
                auto *dst         = frame.bgr.data() + (static_cast<std::size_t>(out_y) * frame.width + out_x) * 3;
                // libcamera RGB888 maps to V4L2 BGR24: its memory is B, G, R.
                dst[0] = pixel[swap_red_blue ? 2 : 0];
                dst[1] = pixel[1];
                dst[2] = pixel[swap_red_blue ? 0 : 2];
            }
        }
        if (!sync_buffer(mapping.fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) {
            report_failure(std::string("无法完成相机 DMA 缓冲区同步：") + std::strerror(errno));
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex);
        latest    = std::move(frame);
        has_frame = true;
        return true;
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

    void completed(libcamera::Request *request)
    {
        if (!request || request->status() == libcamera::Request::RequestCancelled || !streaming.load()) return;
        auto *buffer = request->findBuffer(stream);
        if (!buffer) {
            report_failure("libcamera 请求缺少图像缓冲区。");
            return;
        }
        if (buffer->metadata().status == libcamera::FrameMetadata::FrameSuccess && !copy_frame(buffer)) return;
        if (!streaming.load()) return;
        // ReuseBuffers keeps the stream attachment; addBuffer() here would
        // return -EEXIST and exhaust the request queue after its first frames.
        request->reuse(libcamera::Request::ReuseBuffers);
        apply_zoom(request);  // reuse() clears request controls.
        if (camera->queueRequest(request) < 0 && streaming.load()) report_failure("无法重新提交 libcamera 图像请求。");
    }

    bool open(int requested_rotation, std::string &error)
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
            error = "不支持的相机旋转角度。";
            return false;
        }
        display_rotation = requested_rotation;
        zoom.store(1);
        manager = std::make_unique<libcamera::CameraManager>();
        if (manager->start()) {
            error = "libcamera CameraManager 启动失败。";
            return false;
        }
        manager_started = true;
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
                if (preferred) break;
            }
        }
        if (!camera) {
            error = "libcamera 没有发现摄像头。";
            return false;
        }
        if (camera->acquire()) {
            error = "无法占用 libcamera 摄像头。";
            return false;
        }
        acquired      = true;
        configuration = camera->generateConfiguration({libcamera::StreamRole::Viewfinder});
        if (!configuration || configuration->size() != 1) {
            error = "无法创建 libcamera 取景配置。";
            return false;
        }
        // Rotate0 asks libcamera to compensate the device-tree sensor mounting
        // rotation. An extra hard-coded Rotate180 would invert this board again.
        configuration->orientation = libcamera::Orientation::Rotate0;
        auto &stream_config        = configuration->at(0);
        stream_config.size         = {1920, 1080};
        stream_config.pixelFormat  = libcamera::formats::RGB888;
        stream_config.bufferCount  = 4;
        if (configuration->validate() == libcamera::CameraConfiguration::Invalid) {
            error = "libcamera 取景配置无效。";
            return false;
        }
        const auto &cfg = configuration->at(0);
        if (cfg.pixelFormat != libcamera::formats::RGB888 && cfg.pixelFormat != libcamera::formats::BGR888) {
            error = "libcamera 不支持 RGB888/BGR888 图像输出：" + cfg.pixelFormat.toString();
            return false;
        }
        // Bound arithmetic and allocations before trusting the adjusted format.
        if (!cfg.size.width || !cfg.size.height || cfg.size.width > 8192 || cfg.size.height > 8192 ||
            cfg.stride < cfg.size.width * 3u || cfg.stride > 1024u * 1024u ||
            !orientation_parts(configuration->orientation, pipeline_rotation, pipeline_mirrored)) {
            error = "libcamera 返回了不支持的尺寸、行跨度或方向。";
            return false;
        }
        swap_red_blue = cfg.pixelFormat == libcamera::formats::BGR888;
        if (camera->configure(configuration.get())) {
            error = "libcamera 取景配置失败。";
            return false;
        }
        const auto crop = camera->properties().get(libcamera::properties::ScalerCropMaximum);
        supports_crop   = crop && crop->width && crop->height &&
                        camera->controls().find(&libcamera::controls::ScalerCrop) != camera->controls().end();
        if (supports_crop) maximum_crop = *crop;
        stream = configuration->at(0).stream();
        if (!stream) {
            error = "libcamera 没有创建取景流。";
            return false;
        }
        allocator = std::make_unique<libcamera::FrameBufferAllocator>(camera);
        if (allocator->allocate(stream) <= 0) {
            error = "无法分配 libcamera 取景缓冲区。";
            return false;
        }
        for (const auto &buffer : allocator->buffers(stream)) {
            if (!map_buffer(buffer.get(), error)) return false;
            auto request = camera->createRequest();
            if (!request || request->addBuffer(stream, buffer.get()) < 0) {
                error = "无法创建 libcamera 请求。";
                return false;
            }
            apply_zoom(request.get());
            requests.push_back(std::move(request));
        }
        if (requests.empty()) {
            error = "libcamera 没有分配图像请求。";
            return false;
        }
        camera->requestCompleted.connect(this, &Impl::completed);
        camera->disconnected.connect(this, &Impl::disconnected);
        libcamera::ControlList start_controls(camera->controls());
        if (camera->controls().find(&libcamera::controls::AfMode) != camera->controls().end())
            start_controls.set(libcamera::controls::AfMode, libcamera::controls::AfModeContinuous);
        if (camera->controls().find(&libcamera::controls::FrameDurationLimits) != camera->controls().end())
            start_controls.set(libcamera::controls::FrameDurationLimits, {33333, 33333});
        if (camera->start(&start_controls)) {
            error = "libcamera 摄像头启动失败。";
            return false;
        }
        camera_started = true;
        streaming.store(true);
        for (auto &request : requests) {
            if (camera->queueRequest(request.get()) < 0) {
                error = "无法提交 libcamera 请求。";
                return false;
            }
        }
        return true;
    }

    bool next_frame(LibcameraFrame &frame)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!has_frame) return false;
        frame     = std::move(latest);
        has_frame = false;
        return true;
    }

    bool set_zoom(double value, std::string &error)
    {
        if (value != 1.0 && value != 2.0) {
            error = "变焦倍数必须是 1 或 2。";
            return false;
        }
        if (!streaming.load()) {
            error = "摄像头尚未启动。";
            return false;
        }
        if (value != 1.0 && !supports_crop) {
            error = "当前摄像头不支持 ScalerCrop 变焦。";
            return false;
        }
        zoom.store(static_cast<int>(value));
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

bool LibcameraBackend::open(int rotation, std::string &error)
{
#if CAMERA_HAS_LIBCAMERA
    if (impl_->open(rotation, error)) return true;
    impl_->close();
    return false;
#else
    (void)rotation;
    error = "当前构建未包含 libcamera C++ 后端。";
    return false;
#endif
}

void LibcameraBackend::close() noexcept
{
#if CAMERA_HAS_LIBCAMERA
    if (impl_) impl_->close();
#endif
}

bool LibcameraBackend::next_frame(LibcameraFrame &frame)
{
#if CAMERA_HAS_LIBCAMERA
    return impl_->next_frame(frame);
#else
    (void)frame;
    return false;
#endif
}

bool LibcameraBackend::set_zoom(double zoom, std::string &error)
{
#if CAMERA_HAS_LIBCAMERA
    return impl_->set_zoom(zoom, error);
#else
    (void)zoom;
    error = "当前构建未包含 libcamera C++ 后端。";
    return false;
#endif
}

std::string LibcameraBackend::sensor() const
{
#if CAMERA_HAS_LIBCAMERA
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
#else
    return {};
#endif
}
