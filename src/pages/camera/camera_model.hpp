#pragma once

#include <cstdint>
#include <string>
#include <vector>

enum class CameraMode { Photo, Video };
enum class CameraPhase { Starting, Ready, Capturing, StartingRecording, Recording, Stopping, Error };

struct CameraEvent {
    std::string event;
    std::string state;
    std::string path;
    std::string kind;
    std::string message;
    bool simulated = false;
    bool fatal     = false;
    int width      = 0;
    int height     = 0;
    double zoom    = 1.0;
};

struct CameraFrame {
    int width              = 0;
    int height             = 0;
    std::uint64_t sequence = 0;
    std::vector<std::uint8_t> pixels;  // packed BGR888, LVGL RGB888 on little-endian hosts
};

struct CameraState {
    CameraMode mode                    = CameraMode::Photo;
    CameraPhase phase                  = CameraPhase::Starting;
    bool simulated                     = false;
    bool grid                          = false;
    bool reviewing                     = false;
    double zoom                        = 1.0;
    std::uint64_t recording_started_ms = 0;
    unsigned elapsed_seconds           = 0;
    unsigned saved_count               = 0;
    std::string status                 = "正在连接相机…";
    std::string error;
    std::string last_media;
    std::string last_kind;
};

class CameraModel {
public:
    const CameraState &state() const noexcept
    {
        return state_;
    }
    bool select_mode(CameraMode mode);
    std::string shutter();
    void apply(const CameraEvent &event, std::uint64_t now_ms);
    bool tick(std::uint64_t now_ms);
    void toggle_grid();
    void toggle_review();
    void retry();

private:
    CameraState state_;
};
