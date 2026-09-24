#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct LibcameraFrame {
    int width  = 0;
    int height = 0;
    std::vector<std::uint8_t> bgr;
};

// Small ownership wrapper around libcamera's asynchronous request API.  The
// UI thread only consumes copied frames; camera callbacks never touch LVGL.
class LibcameraBackend {
public:
    LibcameraBackend();
    ~LibcameraBackend();
    LibcameraBackend(const LibcameraBackend &)            = delete;
    LibcameraBackend &operator=(const LibcameraBackend &) = delete;

    bool open(int rotation, std::string &error);
    void close() noexcept;
    bool next_frame(LibcameraFrame &frame);
    bool set_zoom(double zoom, std::string &error);
    std::string sensor() const;
    // Async request failures and disconnects are reported to the UI poll loop.
    std::string last_error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
