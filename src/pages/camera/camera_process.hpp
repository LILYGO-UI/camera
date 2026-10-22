#pragma once

#include "pages/camera/camera_model.hpp"
#include "pages/camera/camera_profile.hpp"

#include <functional>
#include <memory>
#include <string>

// Owns the native libcamera backend and keeps all camera callbacks away from
// LVGL. The simulator follows the same event and media contract in-process.
class CameraProcess {
public:
    CameraProcess();
    ~CameraProcess();
    CameraProcess(const CameraProcess &)            = delete;
    CameraProcess &operator=(const CameraProcess &) = delete;
    // Clockwise image rotation compensates for LVGL's counterclockwise scanout.
    bool start(std::string &error, int rotation = 0, CameraProfile profile = kDefaultCameraProfile);
    bool send(const std::string &command, double zoom = 1.0);
    bool set_profile(CameraProfile profile);
    void poll(const std::function<void(const CameraEvent &)> &event_callback,
              const std::function<void(CameraFrame)> &frame_callback);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
