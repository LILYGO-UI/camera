#pragma once

#include "pages/camera/camera_profile.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct LibcameraFrame {
    int width  = 0;
    int height = 0;
    std::vector<std::uint8_t> bgr;
};

// Camera callbacks only hand off requests. The camera worker consumes requests
// and transforms them before returning their buffers to the capture queue.
class LibcameraBackend {
public:
    LibcameraBackend();
    ~LibcameraBackend();
    LibcameraBackend(const LibcameraBackend &)            = delete;
    LibcameraBackend &operator=(const LibcameraBackend &) = delete;

    bool open(int rotation, std::string &error, CameraProfile profile = kDefaultCameraProfile);
    void close() noexcept;
    // Preview is 640x360 (swapped for portrait). Full resolution is optional,
    // so ordinary viewfinding does not copy/rotate a full-size image every frame.
    bool next_frame(LibcameraFrame &preview, LibcameraFrame *full_resolution = nullptr);
    bool capture_frame(LibcameraFrame &frame, std::string &error);
    bool set_zoom(double zoom, std::string &error);
    std::string sensor() const;
    // Async request failures and disconnects are reported to the UI poll loop.
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
