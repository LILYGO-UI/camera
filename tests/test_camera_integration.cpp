#include "pages/camera/camera_process.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

extern char **environ;

namespace {
using Clock = std::chrono::steady_clock;
using Path  = std::filesystem::path;

class Environment {
public:
    explicit Environment(const char *key) : key_(key)
    {
        const char *value = std::getenv(key);
        had_value_        = value;
        if (value) value_ = value;
    }
    ~Environment()
    {
        if (had_value_)
            setenv(key_.c_str(), value_.c_str(), 1);
        else
            unsetenv(key_.c_str());
    }

private:
    std::string key_, value_;
    bool had_value_ = false;
};

int run_tool(std::vector<std::string> arguments, const Path &output)
{
    std::vector<char *> argv;
    for (auto &argument : arguments) argv.push_back(argument.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    pid_t child      = -1;
    const int result = posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    assert(result == 0 && "ffmpeg and ffprobe are required for media integration tests");
    int status = 0;
    while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::map<std::string, std::string> probe(const Path &media)
{
    const Path result = media.string() + ".probe";
    assert(run_tool({"ffprobe", "-v", "error", "-show_entries",
                     "stream=width,height,r_frame_rate,codec_type:format=duration", "-of", "default=noprint_wrappers=1",
                     media.string()},
                    result) == 0);
    std::ifstream input(result);
    std::map<std::string, std::string> fields;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator != std::string::npos) fields[line.substr(0, separator)] = line.substr(separator + 1);
    }
    std::filesystem::remove(result);
    return fields;
}

void check_media(const Path &media, const CameraFrame &preview, bool video, double expected_duration = 0)
{
    assert(std::filesystem::is_regular_file(media));
    assert(std::filesystem::file_size(media) > 100);
    const auto fields = probe(media);
    assert(fields.at("codec_type") == "video");
    assert(std::stoi(fields.at("width")) == (preview.width > preview.height ? 1920 : 1080));
    assert(std::stoi(fields.at("height")) == (preview.width > preview.height ? 1080 : 1920));
    if (video) {
        assert(fields.at("r_frame_rate") == "30/1");
        if (expected_duration > 0) {
            const double duration = std::stod(fields.at("duration"));
            if (std::abs(duration - expected_duration) >= 0.3)
                std::cerr << "Video duration " << duration << " disagrees with wall time " << expected_duration << '\n';
            assert(std::abs(duration - expected_duration) < 0.3);
        }
    } else {
        std::ifstream jpeg(media, std::ios::binary);
        assert(jpeg.get() == 0xff && jpeg.get() == 0xd8);
        jpeg.seekg(-2, std::ios::end);
        assert(jpeg.get() == 0xff && jpeg.get() == 0xd9);
    }
    // Decode the saved image/video and compare it with the viewfinder. This
    // catches channel-order and rotation errors that headers alone cannot find.
    const Path pixels      = media.string() + ".decoded";
    const std::string size = std::to_string(preview.width) + ":" + std::to_string(preview.height);
    assert(run_tool({"ffmpeg", "-hide_banner", "-loglevel", "error", "-i", media.string(), "-frames:v", "1", "-vf",
                     "scale=" + size, "-pix_fmt", "bgr24", "-f", "rawvideo", "pipe:1"},
                    pixels) == 0);
    std::ifstream input(pixels, std::ios::binary);
    std::vector<unsigned char> decoded((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(decoded.size() == preview.pixels.size());
    std::uint64_t difference = 0;
    for (std::size_t index = 0; index < decoded.size(); ++index)
        difference += static_cast<unsigned>(std::abs(static_cast<int>(decoded[index]) - preview.pixels[index]));
    const double average_difference = static_cast<double>(difference) / decoded.size();
    assert(average_difference < 8.0);
    std::filesystem::remove(pixels);
}

struct Session {
    CameraProcess camera;
    CameraFrame frame;
    bool ready = false;
    std::string state;
    double zoom = 1;
    std::vector<CameraEvent> media;
    std::vector<CameraEvent> errors;
    bool allow_errors = false;

    void poll()
    {
        camera.poll(
            [&](const CameraEvent &event) {
                if (event.event == "error") {
                    errors.push_back(event);
                    if (!allow_errors) {
                        std::cerr << event.message << '\n';
                        std::abort();
                    }
                } else if (event.event == "ready") {
                    assert(event.simulated);
                    ready = true;
                    state = "ready";
                    zoom  = event.zoom;
                } else if (event.event == "state") {
                    state = event.state;
                    zoom  = event.zoom;
                } else if (event.event == "media") {
                    assert(event.simulated);
                    assert(std::filesystem::is_regular_file(event.path));
                    media.push_back(event);
                }
            },
            [&](CameraFrame next) {
                assert(next.sequence > frame.sequence);
                assert(next.pixels.size() == static_cast<std::size_t>(next.width) * next.height * 3);
                frame = std::move(next);
            });
    }
    template <typename Predicate>
    void wait_for(Predicate predicate)
    {
        const auto deadline = Clock::now() + std::chrono::seconds(15);
        while (!predicate() && Clock::now() < deadline) {
            poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(predicate());
    }
    void start(const Path &directory, int rotation = 0)
    {
        setenv("CAMERA_OUTPUT_DIR", directory.c_str(), 1);
        std::string error;
        assert(camera.start(error, rotation));
        wait_for([&] { return ready && !frame.pixels.empty(); });
    }
    void capture()
    {
        const auto count = media.size();
        assert(camera.send("capture"));
        wait_for([&] { return media.size() == count + 1 && state == "ready"; });
        assert(media.back().kind == "photo");
        check_media(media.back().path, frame, false);
    }
    double record_for(int milliseconds)
    {
        assert(camera.send("start_recording"));
        wait_for([&] { return state == "recording"; });
        const auto start    = Clock::now();
        const auto deadline = start + std::chrono::milliseconds(milliseconds);
        unsigned polls      = 0;
        // Neither the number nor regularity of UI polls controls the encoder's
        // 30 fps timeline. Include a longer pause to expose poll-driven writers.
        while (Clock::now() < deadline) {
            poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(++polls % 2 ? 15 : 65));
        }
        return std::chrono::duration<double>(Clock::now() - start).count();
    }
    void finish_recording(double duration)
    {
        const auto count = media.size();
        assert(camera.send("stop_recording"));
        wait_for([&] { return media.size() == count + 1 && state == "ready"; });
        assert(media.back().kind == "video");
        check_media(media.back().path, frame, true, duration);
    }
};

void check_rotation(const CameraFrame &source, const CameraFrame &rotated, int rotation)
{
    const bool portrait = rotation % 180 != 0;
    assert(rotated.width == (portrait ? source.height : source.width));
    assert(rotated.height == (portrait ? source.width : source.height));
    for (int y = 0; y < source.height; ++y)
        for (int x = 0; x < source.width; ++x) {
            int ox = x, oy = y;
            if (rotation == 90) {
                ox = source.height - 1 - y;
                oy = x;
            }
            if (rotation == 180) {
                ox = source.width - 1 - x;
                oy = source.height - 1 - y;
            }
            if (rotation == 270) {
                ox = y;
                oy = source.width - 1 - x;
            }
            const auto from = (static_cast<std::size_t>(y) * source.width + x) * 3;
            const auto to   = (static_cast<std::size_t>(oy) * rotated.width + ox) * 3;
            assert(std::equal(source.pixels.begin() + from, source.pixels.begin() + from + 3,
                              rotated.pixels.begin() + to));
        }
}

void check_no_partials(const Path &directory)
{
    for (const auto &entry : std::filesystem::recursive_directory_iterator(directory)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        assert(name.find(".part") == std::string::npos);
        assert(entry.path().extension() == ".jpg" || entry.path().extension() == ".mp4");
    }
}

void test_encoder_failure(const Path &directory, const std::string &self)
{
    Environment encoder("CAMERA_FFMPEG"), fixture("CAMERA_TEST_ENCODER_MODE");
    Session session;
    session.start(directory);
    session.allow_errors = true;
    for (const char *mode : {"delayed_failure", "empty_output", "invalid_media"}) {
        const auto errors   = session.errors.size();
        const auto sequence = session.frame.sequence;
        setenv("CAMERA_TEST_ENCODER_MODE", mode, 1);
        setenv("CAMERA_FFMPEG", self.c_str(), 1);
        const auto before = Clock::now();
        assert(session.camera.send("capture"));
        assert(Clock::now() - before < std::chrono::milliseconds(250));
        session.wait_for([&] { return session.errors.size() > errors && session.state == "ready"; });
        assert(!session.errors.back().fatal);
        assert(session.media.empty());
        session.wait_for([&] { return session.frame.sequence > sequence; });
    }
    {
        const auto errors = session.errors.size();
        setenv("CAMERA_TEST_ENCODER_MODE", "delayed_failure", 1);
        assert(session.camera.send("start_recording"));
        session.wait_for([&] { return session.errors.size() > errors && session.state == "ready"; });
        assert(!session.errors.back().fatal);
        assert(session.media.empty());
    }
    unsetenv("CAMERA_TEST_ENCODER_MODE");
    {
        const auto errors = session.errors.size();
        setenv("CAMERA_FFMPEG", "/no/such/native-camera-ffmpeg", 1);
        assert(session.camera.send("capture"));
        session.wait_for([&] { return session.errors.size() > errors && session.state == "ready"; });
        assert(!session.errors.back().fatal);
        assert(session.media.empty());
    }
    unsetenv("CAMERA_FFMPEG");
    session.allow_errors = false;
    session.capture();
    check_no_partials(directory);
}
}  // namespace

int main(int argc, char **argv)
{
    (void)argc;
    // An executable encoder fixture avoids Python or shell test scripts.
    if (const char *fixture = std::getenv("CAMERA_TEST_ENCODER_MODE")) {
        if (std::string(fixture) == "delayed_failure") {
            std::this_thread::sleep_for(std::chrono::milliseconds(650));
            return 7;
        }
        if (std::string(fixture) == "invalid_media") {
            char bytes[16384];
            while (read(STDIN_FILENO, bytes, sizeof(bytes)) > 0) {
            }
            std::ofstream invalid(argv[argc - 1], std::ios::binary);
            invalid << std::string(256, 'x');
        }
        return 0;  // Successful exit without media must still be rejected.
    }
    Environment output("CAMERA_OUTPUT_DIR"), simulated("CAMERA_SIMULATE"), encoder("CAMERA_FFMPEG");
    unsetenv("CAMERA_FFMPEG");
    setenv("CAMERA_SIMULATE", "1", 1);
    char temporary[]    = "/tmp/camera-integration-XXXXXX";
    const char *created = mkdtemp(temporary);
    assert(created);
    const Path root = Path(created) / "照片 and videos";
    std::filesystem::create_directories(root);
    CameraProcess invalid;
    std::string error;
#if !defined(__linux__)
    setenv("CAMERA_SIMULATE", "0", 1);
    assert(!invalid.start(error));
    assert(!error.empty());  // A device session must never silently simulate.
    setenv("CAMERA_SIMULATE", "1", 1);
#endif
    for (int rotation : {-90, 45, 360}) {
        assert(!invalid.start(error, rotation));
        assert(!error.empty());
    }
    {
        std::ofstream blocker(root / "file");
        blocker << "not a directory";
        blocker.close();
        setenv("CAMERA_OUTPUT_DIR", (root / "file").c_str(), 1);
        assert(!invalid.start(error));
        std::filesystem::remove(root / "file");
    }
    CameraFrame unrotated;
    {
        Session session;
        session.start(root / "normal");
        unrotated = session.frame;
        session.capture();
        assert(session.camera.send("set_zoom", 2));
        session.wait_for([&] { return session.zoom == 2 && session.frame.pixels != unrotated.pixels; });
        session.capture();
        const double duration = session.record_for(1400);
        session.finish_recording(duration);
        const auto existing = session.media.size();
        session.record_for(550);
        session.camera.stop();  // Closing the app still commits the active video.
        unsigned videos = 0;
        for (const auto &entry : std::filesystem::directory_iterator(root / "normal"))
            if (entry.path().extension() == ".mp4") {
                ++videos;
                check_media(entry.path(), session.frame, true);
            }
        assert(videos == 2);
        assert(session.media.size() == existing);
    }
    for (int rotation : {90, 180, 270}) {
        Session rotated;
        rotated.start(root / std::to_string(rotation), rotation);
        check_rotation(unrotated, rotated.frame, rotation);
        rotated.capture();
        rotated.finish_recording(rotated.record_for(450));
    }
    test_encoder_failure(root / "encoder failure", std::filesystem::absolute(argv[0]).string());
    check_no_partials(root);
    std::filesystem::remove_all(created);
    std::cout
        << "Native preview, media direction/colour, 30 fps timing, zoom, encoder recovery and shutdown verified\n";
}
