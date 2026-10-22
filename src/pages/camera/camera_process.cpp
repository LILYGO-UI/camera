#include "pages/camera/camera_process.hpp"

#include "pages/camera/libcamera_backend.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {
std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char *setting(const char *key, const char *fallback)
{
    const char *value = std::getenv(key);
    return value && *value ? value : fallback;
}

std::string ffmpeg_path()
{
    const char *override_path = std::getenv("CAMERA_FFMPEG");
    if (override_path && *override_path) return access(override_path, X_OK) == 0 ? override_path : std::string{};
    const char *path = std::getenv("PATH");
    if (!path) return {};
    std::string paths(path);
    std::size_t begin = 0;
    while (begin <= paths.size()) {
        const auto end              = paths.find(':', begin);
        const std::string dir       = paths.substr(begin, end == std::string::npos ? end : end - begin);
        const std::string candidate = (dir.empty() ? "." : dir) + "/ffmpeg";
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return {};
}

bool ffmpeg_has_encoder(const std::string &ffmpeg, const char *name)
{
    int output[2] = {-1, -1};
    if (::pipe(output) < 0) return false;
    for (const int fd : output) fcntl(fd, F_SETFD, FD_CLOEXEC);
    std::vector<std::string> args = {ffmpeg, "-hide_banner", "-encoders"};
    std::vector<char *> argv;
    for (auto &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, output[0]);
    posix_spawn_file_actions_addclose(&actions, output[1]);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid        = -1;
    const int result = posix_spawn(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(output[1]);
    if (result) {
        ::close(output[0]);
        return false;
    }
    std::string listing;
    char buffer[4096];
    for (;;) {
        const auto count = ::read(output[0], buffer, sizeof(buffer));
        if (count > 0) {
            listing.append(buffer, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    ::close(output[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    const std::string token = std::string(" ") + name + " ";
    return listing.find(token) != std::string::npos ||
           listing.find(std::string(" ") + name + "\n") != std::string::npos;
}

bool hardware_h264_available(const std::string &ffmpeg)
{
    // Raspberry Pi's V4L2 M2M encoder is normally exposed as video10 or
    // video11. Require both the FFmpeg wrapper and a codec node so desktop
    // builds do not accidentally select an unusable encoder.
    const bool codec_node = access("/dev/video10", F_OK) == 0 || access("/dev/video11", F_OK) == 0;
    return codec_node && ffmpeg_has_encoder(ffmpeg, "h264_v4l2m2m");
}

void rotate_pixels(const std::vector<std::uint8_t> &source, int source_width, int source_height, int rotation,
                   std::vector<std::uint8_t> &output, int &width, int &height)
{
    width  = rotation % 180 ? source_height : source_width;
    height = rotation % 180 ? source_width : source_height;
    output.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < source_height; ++y)
        for (int x = 0; x < source_width; ++x) {
            int ox = x, oy = y;
            if (rotation == 90) {
                ox = source_height - 1 - y;
                oy = x;
            } else if (rotation == 180) {
                ox = source_width - 1 - x;
                oy = source_height - 1 - y;
            } else if (rotation == 270) {
                ox = y;
                oy = source_width - 1 - x;
            }
            const auto in  = (static_cast<std::size_t>(y) * source_width + x) * 3;
            const auto out = (static_cast<std::size_t>(oy) * width + ox) * 3;
            std::copy_n(source.data() + in, 3, output.data() + out);
        }
}

void resize_bgr(const std::vector<std::uint8_t> &source, int source_width, int source_height,
                std::vector<std::uint8_t> &output, int width, int height)
{
    output.resize(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const int source_y = std::min(source_height - 1, y * source_height / height);
        for (int x = 0; x < width; ++x) {
            const int source_x = std::min(source_width - 1, x * source_width / width);
            const auto in      = (static_cast<std::size_t>(source_y) * source_width + source_x) * 3;
            const auto out     = (static_cast<std::size_t>(y) * width + x) * 3;
            std::copy_n(source.data() + in, 3, output.data() + out);
        }
    }
}
}  // namespace

namespace {
// A bounded pipe to the native encoder. No shell or script participates. Video
// frames are queued to a writer thread so encoder backpressure never blocks the
// preview path. When the queue is full, the writer keeps a compact deferred
// frame count and drains it in the background; this preserves the fixed-rate
// output timeline without making capture wait on software encoding.
class Encoder {
public:
    ~Encoder()
    {
        cancel();
    }
    bool start(const std::string &path, const std::string &log_path, int width, int height, int rotation, bool video,
               CameraProfile profile, std::string &error)
    {
        cancel();
        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            writer_queue_.clear();
            deferred_frame_.reset();
            deferred_count_ = 0;
            writer_stop_    = false;
            writer_abort_   = false;
            writer_failed_  = false;
            writer_error_.clear();
        }
        video_            = video;
        const auto ffmpeg = ffmpeg_path();
        if (ffmpeg.empty()) {
            error = "FFmpeg not found. Check the installation or CAMERA_FFMPEG.";
            return false;
        }
        int sockets[2] = {-1, -1};
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) < 0) {
            error = std::strerror(errno);
            return false;
        }
        for (int fd : sockets) fcntl(fd, F_SETFD, FD_CLOEXEC);
#if defined(SO_NOSIGPIPE)
        int one = 1;
        setsockopt(sockets[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        const auto settings           = camera_profile_settings(profile);
        const int output_width        = rotation % 180 ? settings.height : settings.width;
        const int output_height       = rotation % 180 ? settings.width : settings.height;
        const std::string size        = std::to_string(output_width) + ":" + std::to_string(output_height);
        std::vector<std::string> args = {ffmpeg,        "-hide_banner",
                                         "-loglevel",   "error",
                                         "-nostdin",    "-y",
                                         "-f",          "rawvideo",
                                         "-pix_fmt",    "bgr24",
                                         "-video_size", std::to_string(width) + "x" + std::to_string(height),
                                         "-framerate",  std::to_string(settings.fps),
                                         "-i",          "pipe:0",
                                         "-an"};
        // The capture stream already matches the selected media size. Avoid a
        // redundant swscale pass in the common case; it adds CPU and memory
        // pressure on the device during recording.
        if (width != output_width || height != output_height) args.insert(args.end(), {"-vf", "scale=" + size});
        // Let libx264 use the available CPU cores when no hardware encoder is
        // present. The previous fixed two-thread limit made the fallback
        // encoder fall behind even at 720P30 on CM0.
        args.insert(args.end(), {"-threads", "0"});
        const char *codec_request = std::getenv("CAMERA_VIDEO_CODEC");
        const bool force_hardware = codec_request && std::strcmp(codec_request, "hardware") == 0;
        const bool force_software = codec_request && std::strcmp(codec_request, "software") == 0;
        const bool hardware_video = video && !force_software && (force_hardware || hardware_h264_available(ffmpeg));
        if (video) {
            if (hardware_video)
                // Request the common YUV420 hardware input explicitly. The
                // conversion is still far cheaper than software H.264 coding.
                args.insert(args.end(),
                            {"-c:v", "h264_v4l2m2m", "-b:v", "4M", "-pix_fmt", "yuv420p", "-movflags", "+faststart"});
            else
                args.insert(args.end(), {"-c:v", "libx264", "-preset", "ultrafast", "-tune", "zerolatency", "-pix_fmt",
                                         "yuv420p", "-movflags", "+faststart"});
        } else
            // The device gallery's bounded TJPGD fallback accepts baseline
            // JPEGs with common 4:2:0/4:2:2 chroma sampling, but not FFmpeg's
            // default vertical 4:2:2 layout. Keep camera photos decodable when the
            // device image does not provide libjpeg.
            args.insert(args.end(), {"-pix_fmt", "yuv420p", "-frames:v", "1", "-q:v", "2"});
        args.push_back(path);
        std::vector<char *> argv;
        for (auto &arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        posix_spawn_file_actions_addclose(&actions, sockets[0]);
        posix_spawn_file_actions_addclose(&actions, sockets[1]);
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        const int result = posix_spawn(&pid_, argv[0], &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);
        ::close(sockets[1]);
        if (result) {
            ::close(sockets[0]);
            pid_  = -1;
            error = std::strerror(result);
            return false;
        }
        input_ = sockets[0];
        fcntl(input_, F_SETFL, fcntl(input_, F_GETFL) | O_NONBLOCK);
        if (video_) {
            try {
                writer_ = std::thread([this] { writer_loop(); });
            } catch (const std::exception &exception) {
                cancel();
                error = std::string("Could not start the video encoder worker: ") + exception.what();
                return false;
            }
        }
        return true;
    }
    bool write_frame(const std::vector<std::uint8_t> &pixels, std::string &error)
    {
        if (video_) return enqueue_frames(pixels, 1, error);
        return write_frame_blocking(pixels, error, false);
    }
    bool write_repeated_frames(const std::vector<std::uint8_t> &pixels, std::size_t count, std::string &error)
    {
        if (count == 0) return true;
        if (!video_) {
            for (std::size_t index = 0; index < count; ++index)
                if (!write_frame_blocking(pixels, error, false)) return false;
            return true;
        }
        return enqueue_frames(pixels, count, error);
    }
    bool finish(const std::string &path, std::string &error)
    {
        std::string writer_error;
        if (!finish_writer(writer_error)) {
            // Keep draining/closing the child even when the writer failed. This
            // makes finish deterministic and lets the caller remove the .part
            // file without leaving a live ffmpeg process behind.
            close_input();
            wait_for_child();
            error = writer_error;
            return false;
        }
        close_input();
        const auto deadline = now_ms() + 15000;
        int status          = 0;
        while (pid_ > 0) {
            const auto result = waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                break;
            }
            if (result < 0 && errno != EINTR) {
                pid_  = -1;
                error = "Could not retrieve the FFmpeg encoding result.";
                return false;
            }
            if (now_ms() >= deadline) {
                cancel();
                error = "FFmpeg encoding timed out.";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(path, ec);
        if (!WIFEXITED(status) || WEXITSTATUS(status) || ec || bytes < 100 || !valid_container(path, bytes)) {
            error = "Media encoding failed. Check FFmpeg and available storage.";
            return false;
        }
        return true;
    }
    void cancel() noexcept
    {
        if (writer_.joinable()) {
            {
                std::lock_guard<std::mutex> lock(writer_mutex_);
                writer_abort_ = true;
                writer_stop_  = true;
                writer_queue_.clear();
                deferred_frame_.reset();
                deferred_count_ = 0;
            }
            writer_wake_.notify_one();
        }
        close_input();
        if (writer_.joinable()) writer_.join();
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
    }

private:
    // Keep a short burst of raw frames in memory. Once this queue is full, keep
    // only a shared copy of the newest deferred frame plus its frame count.
    // The writer drains those deferred frames after the queue advances, so the
    // capture worker never waits for encoder backpressure.
    static constexpr std::size_t kMaxQueuedFrames = 12;

    bool enqueue_frames(const std::vector<std::uint8_t> &pixels, std::size_t count, std::string &error)
    {
        if (count == 0) return true;
        std::shared_ptr<std::vector<std::uint8_t>> copy;
        try {
            // Repeated catch-up frames share one pixel buffer. This is common
            // when a worker iteration runs late and avoids multiplying the
            // memory cost of writing the same most-recent camera frame.
            copy = std::make_shared<std::vector<std::uint8_t>>(pixels);
        } catch (const std::exception &exception) {
            error = std::string("Could not queue an image frame: ") + exception.what();
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            if (writer_failed_) {
                error = writer_error_;
                return false;
            }
            if (writer_stop_ || writer_abort_) {
                error = writer_failed_ ? writer_error_ : "Video encoder is stopping.";
                return false;
            }
            // Deferred work is kept behind the already queued samples. Reusing
            // the newest payload for that deferred count bounds memory when the
            // encoder falls far behind while retaining the fixed-rate timeline.
            if (deferred_count_ != 0) {
                deferred_frame_ = copy;
                deferred_count_ += count;
                writer_wake_.notify_one();
                return true;
            }
            const auto available = kMaxQueuedFrames - writer_queue_.size();
            const auto batch     = std::min(count, available);
            for (std::size_t index = 0; index < batch; ++index) writer_queue_.push_back(copy);
            if (batch < count) {
                deferred_frame_ = copy;
                deferred_count_ = count - batch;
            }
            writer_wake_.notify_one();
        }
        return true;
    }

    bool write_frame_blocking(const std::vector<std::uint8_t> &pixels, std::string &error, bool cancellable)
    {
        const auto deadline = now_ms() + 5000;
        std::size_t offset  = 0;
        while (offset < pixels.size()) {
            if (cancellable) {
                std::lock_guard<std::mutex> lock(writer_mutex_);
                if (writer_abort_) {
                    error = "Video encoding was cancelled.";
                    return false;
                }
            }
#if defined(MSG_NOSIGNAL)
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            const auto count = ::send(input_, pixels.data() + offset, pixels.size() - offset, flags);
            if (count > 0) {
                offset += static_cast<std::size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && now_ms() < deadline) {
                pollfd fd{input_, POLLOUT, 0};
                ::poll(&fd, 1, 50);
                continue;
            }
            error = "FFmpeg could not accept image frames. Check the encoder and available storage.";
            return false;
        }
        return true;
    }

    void writer_loop()
    {
        for (;;) {
            std::shared_ptr<std::vector<std::uint8_t>> frame;
            {
                std::unique_lock<std::mutex> lock(writer_mutex_);
                writer_wake_.wait(lock, [this] {
                    return writer_abort_ || writer_stop_ || !writer_queue_.empty() || deferred_count_ != 0;
                });
                if (writer_abort_) return;
                if (writer_queue_.empty() && deferred_count_ != 0) {
                    const auto batch = std::min(deferred_count_, kMaxQueuedFrames);
                    for (std::size_t index = 0; index < batch; ++index) writer_queue_.push_back(deferred_frame_);
                    deferred_count_ -= batch;
                    if (deferred_count_ == 0) deferred_frame_.reset();
                }
                if (writer_queue_.empty()) {
                    if (writer_stop_) return;
                    continue;
                }
                frame = std::move(writer_queue_.front());
                writer_queue_.pop_front();
            }
            std::string error;
            if (!write_frame_blocking(*frame, error, true)) {
                std::lock_guard<std::mutex> lock(writer_mutex_);
                if (!writer_abort_) {
                    writer_failed_ = true;
                    writer_error_  = error;
                }
                writer_queue_.clear();
                deferred_frame_.reset();
                deferred_count_ = 0;
                writer_stop_    = true;
                writer_wake_.notify_all();
                return;
            }
        }
    }

    bool finish_writer(std::string &error)
    {
        if (!writer_.joinable()) return true;
        {
            std::lock_guard<std::mutex> lock(writer_mutex_);
            writer_stop_ = true;
        }
        writer_wake_.notify_one();
        writer_.join();
        std::lock_guard<std::mutex> lock(writer_mutex_);
        if (writer_failed_) {
            error = writer_error_;
            return false;
        }
        return true;
    }

    void wait_for_child() noexcept
    {
        if (pid_ > 0) {
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
    }

    bool valid_container(const std::string &path, std::uint64_t size) const
    {
        std::ifstream file(path, std::ios::binary);
        if (!file) return false;
        if (!video_) {
            if (file.get() != 0xff || file.get() != 0xd8) return false;
            file.seekg(-2, std::ios::end);
            return file.get() == 0xff && file.get() == 0xd9;
        }
        // Check finalized MP4 boxes without adding ffprobe as a runtime dependency.
        bool ftyp = false, moov = false, mdat = false;
        std::uint64_t offset = 0;
        while (offset < size) {
            if (size - offset < 8) return false;
            file.seekg(static_cast<std::streamoff>(offset));
            unsigned char header[16]{};
            if (!file.read(reinterpret_cast<char *>(header), 8)) return false;
            std::uint64_t length = 0;
            for (int i = 0; i < 4; ++i) length = (length << 8) | header[i];
            std::uint64_t header_size = 8;
            if (length == 1) {
                if (!file.read(reinterpret_cast<char *>(header + 8), 8)) return false;
                length = 0;
                for (int i = 8; i < 16; ++i) length = (length << 8) | header[i];
                header_size = 16;
            } else if (length == 0)
                length = size - offset;
            if (length < header_size || length > size - offset) return false;
            const std::string type(reinterpret_cast<char *>(header + 4), 4);
            ftyp = ftyp || (type == "ftyp" && length >= header_size + 8);
            moov = moov || (type == "moov" && length > header_size);
            mdat = mdat || (type == "mdat" && length > header_size);
            offset += length;
        }
        return ftyp && moov && mdat;
    }
    void close_input()
    {
        if (input_ >= 0) {
            ::close(input_);
            input_ = -1;
        }
    }
    pid_t pid_  = -1;
    int input_  = -1;
    bool video_ = false;
    std::thread writer_;
    std::mutex writer_mutex_;
    std::condition_variable writer_wake_;
    std::deque<std::shared_ptr<std::vector<std::uint8_t>>> writer_queue_;
    std::shared_ptr<std::vector<std::uint8_t>> deferred_frame_;
    std::size_t deferred_count_ = 0;
    bool writer_stop_           = false;
    bool writer_abort_          = false;
    bool writer_failed_         = false;
    std::string writer_error_;
};

void remove_file(const std::string &path)
{
    std::error_code ec;
    std::filesystem::remove(path, ec);
}
}  // namespace

struct CameraProcess::Impl {
    struct Command {
        std::string name;
        double zoom;
        CameraProfile profile = kDefaultCameraProfile;
    };
    std::unique_ptr<LibcameraBackend> backend;
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Command> commands;
    std::deque<CameraEvent> events;
    CameraFrame preview;
    std::atomic<bool> stopping{false};
    std::atomic<bool> running{false};
    bool simulated = false;
    bool ready     = false;
    std::atomic<bool> recording{false};
    int rotation                = 0;
    CameraProfile profile       = kDefaultCameraProfile;
    double zoom                 = 1;
    std::uint64_t sequence      = 0;
    std::uint64_t last_frame_ms = 0;
    LibcameraFrame frame;
    LibcameraFrame native_preview;
    Encoder video;
    std::uint64_t recording_started = 0;
    std::uint64_t recorded_frames   = 0;
    std::string recording_path;
    std::string output_dir;
    std::string runtime_dir;

    ~Impl()
    {
        stop();
    }
    void emit(CameraEvent event)
    {
        event.simulated   = simulated;
        event.zoom        = zoom;
        event.profile     = profile;
        event.has_profile = true;
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back(std::move(event));
    }
    void state(const std::string &name)
    {
        CameraEvent event;
        event.event = "state";
        event.state = name;
        emit(std::move(event));
    }
    void report(const std::string &message, bool fatal = false)
    {
        CameraEvent event;
        event.event   = "error";
        event.message = message;
        event.fatal   = fatal;
        emit(std::move(event));
    }
    std::string media_name(const char *kind, const char *extension)
    {
        const auto stamp =
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
        return (std::filesystem::path(output_dir) /
                ((simulated ? "SIM_" : "") + std::string(kind) + "_" + std::to_string(stamp) + extension))
            .string();
    }
    bool save_media(const std::string &temporary, const std::string &final_path, const char *kind, std::string &error)
    {
        std::error_code ec;
        std::filesystem::rename(temporary, final_path, ec);
        if (ec) {
            error = "Could not save media: " + ec.message();
            return false;
        }
        CameraEvent event;
        event.event = "media";
        event.path  = final_path;
        event.kind  = kind;
        emit(std::move(event));
        return true;
    }
    void make_simulated_frame()
    {
        std::vector<std::uint8_t> source(static_cast<std::size_t>(640) * 360 * 3);
        for (int y = 0; y < 360; ++y)
            for (int x = 0; x < 640; ++x) {
                const double fy = (static_cast<double>(y) / 360 - 0.5) / zoom + 0.5;
                const double fx = (static_cast<double>(x) / 640 - 0.5) / zoom + 0.5;
                int r, g, b;
                if (fy > 0.78 + 0.06 * std::sin(fx * 12)) {
                    r = 26 + static_cast<int>(fy * 13);
                    g = 72 + static_cast<int>(fy * 25);
                    b = 61;
                } else if (fy > 0.57 + 0.08 * std::sin(fx * 9 + 1)) {
                    r = 45;
                    g = 83;
                    b = 89;
                } else if (fy > 0.39 + 0.14 * std::abs(std::sin(fx * 7))) {
                    r = 91;
                    g = 113;
                    b = 137;
                } else if ((fx - 0.72) * (fx - 0.72) + ((fy - 0.27) * 360.0 / 640) * ((fy - 0.27) * 360.0 / 640) <
                           0.004) {
                    r = 255;
                    g = 214;
                    b = 149;
                } else {
                    r = static_cast<int>(77 + 123 * fy);
                    g = static_cast<int>(139 + 65 * fy);
                    b = static_cast<int>(177 + 22 * fy);
                }
                const auto at  = (static_cast<std::size_t>(y) * 640 + x) * 3;
                source[at]     = static_cast<std::uint8_t>(b);
                source[at + 1] = static_cast<std::uint8_t>(g);
                source[at + 2] = static_cast<std::uint8_t>(r);
            }
        rotate_pixels(source, 640, 360, rotation, frame.bgr, frame.width, frame.height);
    }
    void publish_frame()
    {
        CameraFrame next;
        next.width    = rotation % 180 ? 360 : 640;
        next.height   = rotation % 180 ? 640 : 360;
        next.sequence = ++sequence;
        if (simulated)
            resize_bgr(frame.bgr, frame.width, frame.height, next.pixels, next.width, next.height);
        else
            next.pixels = std::move(native_preview.bgr);
        std::lock_guard<std::mutex> lock(mutex);
        preview = std::move(next);
    }
    void capture()
    {
        state("capturing");
        const auto final_path = media_name("IMG", ".jpg");
        const auto temporary  = final_path + ".part.jpg";
        std::string error;
        if (!simulated && !backend->capture_frame(frame, error)) {
            report(error);
            state("ready");
            return;
        }
        Encoder image;
        bool ok = image.start(temporary, runtime_dir + "/photo.log", frame.width, frame.height, rotation, false,
                              profile, error) &&
                  image.write_frame(frame.bgr, error) && image.finish(temporary, error);
        image.cancel();
        if (ok) ok = save_media(temporary, final_path, "photo", error);
        if (!ok) {
            remove_file(temporary);
            report(error);
        }
        state("ready");
    }
    void start_recording()
    {
        recording_path = media_name("VID", ".mp4");
        std::string error;
        if (!simulated && !backend->capture_frame(frame, error)) {
            report(error);
            state("ready");
            return;
        }
        if (!video.start(recording_path + ".part.mp4", runtime_dir + "/video.log", frame.width, frame.height, rotation,
                         true, profile, error)) {
            report(error);
            state("ready");
            return;
        }
        recording         = true;
        recording_started = now_ms();
        recorded_frames   = 0;
        state("recording");
        record_until(recording_started, true);
    }
    void fail_recording(const std::string &error)
    {
        recording = false;
        video.cancel();
        remove_file(recording_path + ".part.mp4");
        report(error);
        state("ready");
    }
    void record_until(std::uint64_t timestamp, bool fresh_frame = false)
    {
        // The encoder's timebase follows elapsed time, independent of UI polling.
        const auto fps    = camera_profile_settings(profile).fps;
        const auto target = std::max<std::uint64_t>(1, (timestamp - recording_started) * fps / 1000 + 1);
        // Submit the newest captured frame and repeat it when the worker has
        // missed a timeline tick. This keeps the saved video's wall-clock
        // duration stable while the camera backend drops stale requests to
        // keep the live preview responsive.
        if (recording) {
            const auto missing =
                fresh_frame ? std::size_t{1} : (recorded_frames < target ? target - recorded_frames : 0);
            if (missing == 0) return;
            std::string error;
            if (!video.write_repeated_frames(frame.bgr, static_cast<std::size_t>(missing), error)) {
                fail_recording(error);
                return;
            }
            recorded_frames += missing;
        }
    }
    void finish_recording()
    {
        if (!recording) return;
        record_until(now_ms());
        if (!recording) return;
        recording = false;
        state("stopping");
        const auto temporary = recording_path + ".part.mp4";
        std::string error;
        bool ok = video.finish(temporary, error);
        if (ok) ok = save_media(temporary, recording_path, "video", error);
        if (!ok) {
            video.cancel();
            remove_file(temporary);
            report(error);
        }
        state("ready");
    }
    bool reopen(CameraProfile requested, std::string &error)
    {
        // open() releases the old capture queue before configuring the new one.
        // Wait for one frame so UI confirmation also proves the stream started.
        return backend->open(rotation, error, requested) && backend->set_zoom(zoom, error) &&
               backend->capture_frame(frame, error);
    }
    void change_profile(CameraProfile requested)
    {
        if (requested == profile) {
            state("ready");
            return;
        }
        state("reconfiguring");
        std::string error;
        if (!simulated && !reopen(requested, error)) {
            std::string recovery_error;
            if (!reopen(profile, recovery_error)) {
                ready = false;
                report("Failed to switch capture settings: " + error +
                           " Failed to restore previous settings: " + recovery_error,
                       true);
                stopping = true;
                return;
            }
            report("Failed to switch capture settings. Previous settings restored: " + error);
        } else {
            profile = requested;
        }
        last_frame_ms  = now_ms();
        native_preview = {};
        {
            std::lock_guard<std::mutex> lock(mutex);
            preview = {};
        }
        state("ready");
    }
    void handle(const Command &command)
    {
        if (!ready) return;
        if (command.name == "stop_recording") {
            finish_recording();
            return;
        }
        if (recording) return;
        if (command.name == "set_profile")
            change_profile(command.profile);
        else if (command.name == "capture")
            capture();
        else if (command.name == "start_recording")
            start_recording();
        else if (command.name == "set_zoom") {
            std::string error;
            if (!simulated && !backend->set_zoom(command.zoom, error)) {
                report(error);
                state("ready");
                return;
            }
            zoom = command.zoom;
            if (simulated) {
                make_simulated_frame();
                publish_frame();
            }
            state("ready");
        }
    }
    void run()
    {
        last_frame_ms  = now_ms();
        auto next_tick = std::chrono::steady_clock::now();
        try {
            while (!stopping.load()) {
                const bool new_frame = simulated || backend->next_frame(native_preview, recording ? &frame : nullptr);
                if (new_frame && !(simulated ? frame.bgr : native_preview.bgr).empty()) {
                    last_frame_ms = now_ms();
                    publish_frame();
                    if (recording) record_until(last_frame_ms, true);
                    if (!ready) {
                        ready = true;
                        CameraEvent event;
                        event.event  = "ready";
                        event.width  = rotation % 180 ? 360 : 640;
                        event.height = rotation % 180 ? 640 : 360;
                        emit(std::move(event));
                    }
                }
                if (!simulated) {
                    const auto error = backend->last_error();
                    if (!error.empty() || now_ms() - last_frame_ms > 10000) {
                        report(error.empty() ? "Camera preview timed out. Check the connection and retry." : error,
                               true);
                        break;
                    }
                }
                std::deque<Command> pending;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    pending.swap(commands);
                }
                for (const auto &command : pending) handle(command);
                if (recording && !new_frame) record_until(now_ms());
                next_tick += std::chrono::nanoseconds(1000000000 / camera_profile_settings(profile).fps);
                const auto now = std::chrono::steady_clock::now();
                if (next_tick < now) next_tick = now;
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_until(lock, next_tick, [&] { return stopping.load(); });
            }
            finish_recording();
        } catch (const std::exception &error) {
            video.cancel();
            if (!recording_path.empty()) remove_file(recording_path + ".part.mp4");
            report(std::string("Camera processing failed: ") + error.what(), true);
        }
        running = false;
    }
    void stop() noexcept
    {
        stopping = true;
        wake.notify_all();
        if (worker.joinable()) worker.join();
        backend.reset();
        video.cancel();
        running = false;
        if (!runtime_dir.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(runtime_dir, ec);
            runtime_dir.clear();
        }
    }
};

CameraProcess::CameraProcess() = default;
CameraProcess::~CameraProcess()
{
    stop();
}

bool CameraProcess::start(std::string &error, int rotation, CameraProfile profile)
{
    if (!valid_camera_profile(profile)) {
        error = "Unsupported capture settings.";
        return false;
    }
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        error = "Unsupported camera rotation angle.";
        return false;
    }
    stop();
    auto next        = std::make_unique<Impl>();
    next->rotation   = rotation;
    next->profile    = profile;
    next->simulated  = std::string(setting("CAMERA_SIMULATE", CAMERA_DEFAULT_SIMULATED ? "1" : "0")) == "1";
    next->output_dir = setting("CAMERA_OUTPUT_DIR", "");
    if (next->output_dir.empty()) {
        const char *home = std::getenv("HOME");
        if (!home || !*home) {
            error = "Please set CAMERA_OUTPUT_DIR to a capture folder.";
            return false;
        }
        next->output_dir = std::string(home) + "/Camera";
    }
    std::error_code ec;
    std::filesystem::create_directories(next->output_dir, ec);
    if (ec) {
        error = "Could not create the capture folder: " + ec.message();
        return false;
    }
    char temporary[]      = "/tmp/lilygo-camera-XXXXXX";
    const char *directory = mkdtemp(temporary);
    if (!directory) {
        error = "Could not create the camera temporary folder.";
        return false;
    }
    next->runtime_dir = directory;
    if (next->simulated)
        next->make_simulated_frame();
    else {
        next->backend = std::make_unique<LibcameraBackend>();
        if (!next->backend->open(rotation, error, profile)) return false;
    }
    next->running = true;
    try {
        next->worker = std::thread([owner = next.get()] { owner->run(); });
    } catch (const std::exception &e) {
        error = e.what();
        return false;
    }
    impl_ = std::move(next);
    return true;
}

bool CameraProcess::send(const std::string &command, double zoom)
{
    if (!impl_ || !impl_->running) return false;
    if (command == "shutdown") {
        stop();
        return true;
    }
    if (command != "capture" && command != "start_recording" && command != "stop_recording" && command != "set_zoom")
        return false;
    if (command == "set_zoom" && zoom != 1 && zoom != 2) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->commands.size() >= 16) return false;
    impl_->commands.push_back({command, zoom});
    return true;
}

bool CameraProcess::set_profile(CameraProfile profile)
{
    if (!valid_camera_profile(profile) || !impl_ || !impl_->running || impl_->recording.load()) return false;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->commands.size() >= 16) return false;
    impl_->commands.push_back({"set_profile", 1.0, profile});
    return true;
}

void CameraProcess::poll(const std::function<void(const CameraEvent &)> &event_callback,
                         const std::function<void(CameraFrame)> &frame_callback)
{
    if (!impl_) return;
    CameraFrame frame;
    std::deque<CameraEvent> events;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        frame          = std::move(impl_->preview);
        impl_->preview = {};
        events.swap(impl_->events);
    }
    if (!frame.pixels.empty()) frame_callback(std::move(frame));
    for (const auto &event : events) event_callback(event);
}

void CameraProcess::stop() noexcept
{
    impl_.reset();
}
