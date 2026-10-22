#include "pages/camera/camera_model.hpp"

bool CameraModel::select_mode(CameraMode mode)
{
    if (state_.phase != CameraPhase::Ready || state_.reviewing) return false;
    state_.mode = mode;
    state_.error.clear();
    state_.status = mode == CameraMode::Photo ? "Tap shutter to take a photo" : "Tap to record · No audio";
    return true;
}

bool CameraModel::select_profile(CameraProfile profile)
{
    if (state_.phase != CameraPhase::Ready || state_.reviewing || !valid_camera_profile(profile) ||
        state_.profile == profile)
        return false;
    state_.phase  = CameraPhase::Reconfiguring;
    state_.status = "Switching quality…";
    state_.error.clear();
    return true;
}

std::string CameraModel::shutter()
{
    if (state_.reviewing) return {};
    if (state_.phase == CameraPhase::Recording) {
        state_.phase  = CameraPhase::Stopping;
        state_.status = "Saving video…";
        return "stop_recording";
    }
    if (state_.phase != CameraPhase::Ready) return {};
    state_.error.clear();
    if (state_.mode == CameraMode::Photo) {
        state_.phase  = CameraPhase::Capturing;
        state_.status = "Saving photo…";
        return "capture";
    }
    state_.phase  = CameraPhase::StartingRecording;
    state_.status = "Starting recording…";
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
        state_.status = state_.mode == CameraMode::Photo ? "Tap shutter to take a photo" : "Tap to record · No audio";
        state_.zoom   = event.zoom;
        if (event.has_profile && valid_camera_profile(event.profile)) state_.profile = event.profile;
    } else if (event.event == "state") {
        state_.zoom = event.zoom;
        if (event.state == "recording") {
            if (state_.phase != CameraPhase::Recording) {
                state_.recording_started_ms = now_ms;
                state_.elapsed_seconds      = 0;
            }
            state_.phase  = CameraPhase::Recording;
            state_.status = "Recording · No audio";
        } else if (event.state == "ready") {
            state_.phase = CameraPhase::Ready;
            if (event.has_profile && valid_camera_profile(event.profile)) state_.profile = event.profile;
            if (state_.error.empty() &&
                (state_.status == "Connecting to camera…" || state_.status == "Switching quality…" ||
                 state_.status == "Saving video…" || state_.status == "Saving photo…" ||
                 state_.status == "Starting recording…" || state_.status == "Recording · No audio"))
                state_.status =
                    state_.mode == CameraMode::Photo ? "Tap shutter to take a photo" : "Tap to record · No audio";
        } else if (event.state == "reconfiguring") {
            state_.phase  = CameraPhase::Reconfiguring;
            state_.status = "Switching quality…";
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
            state_.status = event.kind == "video" ? "Video saved" : "Photo saved";
            state_.error.clear();
        }
    } else if (event.event == "error") {
        state_.error  = event.message;
        state_.status = event.fatal ? "Camera connection failed" : "Operation failed";
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
    state_.status    = "Connecting to camera…";
    state_.reviewing = false;
    state_.zoom      = 1.0;
}
