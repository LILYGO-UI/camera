#include "pages/camera/camera_model.hpp"

bool CameraModel::select_mode(CameraMode mode)
{
    if (state_.phase != CameraPhase::Ready || state_.reviewing) return false;
    state_.mode = mode;
    state_.error.clear();
    state_.status = mode == CameraMode::Photo ? "轻点快门拍照" : "轻点开始录像 · 无声";
    return true;
}

std::string CameraModel::shutter()
{
    if (state_.reviewing) return {};
    if (state_.phase == CameraPhase::Recording) {
        state_.phase  = CameraPhase::Stopping;
        state_.status = "正在保存视频…";
        return "stop_recording";
    }
    if (state_.phase != CameraPhase::Ready) return {};
    state_.error.clear();
    if (state_.mode == CameraMode::Photo) {
        state_.phase  = CameraPhase::Capturing;
        state_.status = "正在保存照片…";
        return "capture";
    }
    state_.phase  = CameraPhase::StartingRecording;
    state_.status = "正在开始录像…";
    return "start_recording";
}

void CameraModel::apply(const CameraEvent &event, std::uint64_t now_ms)
{
    // Cleanup can finish a video after a fatal device error. Preserve that file,
    // but only an explicit retry may make this failed camera session ready again.
    if (state_.phase == CameraPhase::Error && event.event != "media" && event.event != "error") return;
    if (event.event == "ready") {
        state_.phase     = CameraPhase::Ready;
        state_.simulated = event.simulated;
        state_.error.clear();
        state_.status = "轻点快门拍照";
        state_.zoom   = event.zoom;
    } else if (event.event == "state") {
        state_.zoom = event.zoom;
        if (event.state == "recording") {
            if (state_.phase != CameraPhase::Recording) {
                state_.recording_started_ms = now_ms;
                state_.elapsed_seconds      = 0;
            }
            state_.phase  = CameraPhase::Recording;
            state_.status = "正在录像 · 无声";
        } else if (event.state == "ready") {
            state_.phase = CameraPhase::Ready;
            if (state_.error.empty() && state_.status.find("正在") == 0)
                state_.status = state_.mode == CameraMode::Photo ? "轻点快门拍照" : "轻点开始录像 · 无声";
        } else if (event.state == "capturing") {
            state_.phase = CameraPhase::Capturing;
        } else if (event.state == "stopping") {
            state_.phase = CameraPhase::Stopping;
        }
    } else if (event.event == "media" && !event.path.empty()) {
        state_.last_media = event.path;
        state_.last_kind  = event.kind;
        ++state_.saved_count;
        if (state_.phase != CameraPhase::Error) {
            state_.status = event.kind == "video" ? "视频已保存" : "照片已保存";
            state_.error.clear();
        }
    } else if (event.event == "error") {
        state_.error  = event.message;
        state_.status = event.fatal ? "相机连接失败" : "操作未完成";
        if (event.fatal) {
            state_.phase     = CameraPhase::Error;
            state_.reviewing = false;
        }
        // Nonfatal errors are followed by the worker's authoritative state.
    }
}

bool CameraModel::tick(std::uint64_t now_ms)
{
    if (state_.phase != CameraPhase::Recording) return false;
    const auto seconds = static_cast<unsigned>((now_ms - state_.recording_started_ms) / 1000);
    if (seconds == state_.elapsed_seconds) return false;
    state_.elapsed_seconds = seconds;
    return true;
}

void CameraModel::toggle_grid()
{
    state_.grid = !state_.grid;
}

void CameraModel::toggle_review()
{
    if (state_.reviewing)
        state_.reviewing = false;
    else if (state_.phase == CameraPhase::Ready && !state_.last_media.empty())
        state_.reviewing = true;
}

void CameraModel::retry()
{
    if (state_.phase != CameraPhase::Error) return;
    state_.phase = CameraPhase::Starting;
    state_.error.clear();
    state_.status    = "正在连接相机…";
    state_.reviewing = false;
    state_.zoom      = 1.0;
}
