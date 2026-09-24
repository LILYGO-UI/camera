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
// A bounded pipe to the native encoder. No shell or script participates, and
// all writes/waits run on the camera thread rather than LVGL's event thread.
class Encoder {
public:
    ~Encoder()
    {
        cancel();
    }
    bool start(const std::string &path, const std::string &log_path, int width, int height, int rotation, bool video,
               std::string &error)
    {
        cancel();
        video_            = video;
        const auto ffmpeg = ffmpeg_path();
        if (ffmpeg.empty()) {
            error = "找不到 FFmpeg，请检查安装或 CAMERA_FFMPEG。";
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
        const std::string size        = rotation % 180 ? "1080:1920" : "1920:1080";
        std::vector<std::string> args = {ffmpeg,
                                         "-hide_banner",
                                         "-loglevel",
                                         "error",
                                         "-nostdin",
                                         "-y",
                                         "-f",
                                         "rawvideo",
                                         "-pix_fmt",
                                         "bgr24",
                                         "-video_size",
                                         std::to_string(width) + "x" + std::to_string(height),
                                         "-framerate",
                                         "30",
                                         "-i",
                                         "pipe:0",
                                         "-an",
                                         "-vf",
                                         "scale=" + size,
                                         "-threads",
                                         "2"};
        if (video)
            args.insert(args.end(),
                        {"-c:v", "libx264", "-preset", "ultrafast", "-pix_fmt", "yuv420p", "-movflags", "+faststart"});
        else
            args.insert(args.end(), {"-frames:v", "1", "-q:v", "2"});
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
        return true;
    }
    bool write_frame(const std::vector<std::uint8_t> &pixels, std::string &error)
    {
        const auto deadline = now_ms() + 5000;
        std::size_t offset  = 0;
        while (offset < pixels.size()) {
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
            error = "FFmpeg 无法接收图像帧，请检查编码器和剩余空间。";
            return false;
        }
        return true;
    }
    bool finish(const std::string &path, std::string &error)
    {
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
                error = "无法获取 FFmpeg 编码结果。";
                return false;
            }
            if (now_ms() >= deadline) {
                cancel();
                error = "FFmpeg 编码超时。";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::error_code ec;
        const auto bytes = std::filesystem::file_size(path, ec);
        if (!WIFEXITED(status) || WEXITSTATUS(status) || ec || bytes < 100 || !valid_container(path, bytes)) {
            error = "媒体编码失败，请检查 FFmpeg 和剩余空间。";
            return false;
        }
        return true;
    }
    void cancel() noexcept
    {
        close_input();
        if (pid_ > 0) {
            kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
    }

private:
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
    bool simulated              = false;
    bool ready                  = false;
    bool recording              = false;
    int rotation                = 0;
    double zoom                 = 1;
    std::uint64_t sequence      = 0;
    std::uint64_t last_frame_ms = 0;
    LibcameraFrame frame;
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
        event.simulated = simulated;
        event.zoom      = zoom;
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
            error = "无法保存媒体: " + ec.message();
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
        resize_bgr(frame.bgr, frame.width, frame.height, next.pixels, next.width, next.height);
        std::lock_guard<std::mutex> lock(mutex);
        preview = std::move(next);
    }
    void capture()
    {
        state("capturing");
        const auto final_path = media_name("IMG", ".jpg");
        const auto temporary  = final_path + ".part.jpg";
        std::string error;
        Encoder image;
        bool ok =
            image.start(temporary, runtime_dir + "/photo.log", frame.width, frame.height, rotation, false, error) &&
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
        if (!video.start(recording_path + ".part.mp4", runtime_dir + "/video.log", frame.width, frame.height, rotation,
                         true, error)) {
            report(error);
            state("ready");
            return;
        }
        recording         = true;
        recording_started = now_ms();
        recorded_frames   = 0;
        state("recording");
        record_until(recording_started);
    }
    void fail_recording(const std::string &error)
    {
        recording = false;
        video.cancel();
        remove_file(recording_path + ".part.mp4");
        report(error);
        state("ready");
    }
    void record_until(std::uint64_t timestamp)
    {
        // The encoder's timebase follows elapsed time, independent of UI polling.
        const auto target = std::max<std::uint64_t>(1, (timestamp - recording_started) * 30 / 1000 + 1);
        if (target > recorded_frames + 60) {
            fail_recording("视频编码速度不足，录像已停止。");
            return;
        }
        while (recording && recorded_frames < target) {
            std::string error;
            if (!video.write_frame(frame.bgr, error)) {
                fail_recording(error);
                return;
            }
            ++recorded_frames;
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
    void handle(const Command &command)
    {
        if (!ready) return;
        if (command.name == "stop_recording") {
            finish_recording();
            return;
        }
        if (recording) return;
        if (command.name == "capture")
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
                bool new_frame = simulated || backend->next_frame(frame);
                if (new_frame && !frame.bgr.empty()) {
                    last_frame_ms = now_ms();
                    publish_frame();
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
                        report(error.empty() ? "相机取景超时，请检查连接后重试。" : error, true);
                        break;
                    }
                }
                std::deque<Command> pending;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    pending.swap(commands);
                }
                for (const auto &command : pending) handle(command);
                if (recording) record_until(now_ms());
                next_tick += std::chrono::microseconds(33333);
                const auto now = std::chrono::steady_clock::now();
                if (next_tick < now) next_tick = now;
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait_until(lock, next_tick, [&] { return stopping.load(); });
            }
            finish_recording();
        } catch (const std::exception &error) {
            video.cancel();
            if (!recording_path.empty()) remove_file(recording_path + ".part.mp4");
            report(std::string("相机处理失败: ") + error.what(), true);
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

bool CameraProcess::start(std::string &error, int rotation)
{
    if (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270) {
        error = "不支持的相机旋转角度。";
        return false;
    }
    stop();
    auto next        = std::make_unique<Impl>();
    next->rotation   = rotation;
    next->simulated  = std::string(setting("CAMERA_SIMULATE", CAMERA_DEFAULT_SIMULATED ? "1" : "0")) == "1";
    next->output_dir = setting("CAMERA_OUTPUT_DIR", "");
    if (next->output_dir.empty()) {
        const char *home = std::getenv("HOME");
        if (!home || !*home) {
            error = "请设置 CAMERA_OUTPUT_DIR 保存目录。";
            return false;
        }
        next->output_dir = std::string(home) + "/Pictures/Camera";
    }
    std::error_code ec;
    std::filesystem::create_directories(next->output_dir, ec);
    if (ec) {
        error = "无法创建相机保存目录: " + ec.message();
        return false;
    }
    char temporary[]      = "/tmp/lilygo-camera-XXXXXX";
    const char *directory = mkdtemp(temporary);
    if (!directory) {
        error = "无法创建相机临时目录。";
        return false;
    }
    next->runtime_dir = directory;
    if (next->simulated)
        next->make_simulated_frame();
    else {
        next->backend = std::make_unique<LibcameraBackend>();
        if (!next->backend->open(rotation, error)) return false;
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
