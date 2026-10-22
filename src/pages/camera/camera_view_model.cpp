#include "pages/camera/camera_view_model.hpp"
#include "pages/camera/camera_process.hpp"

#include <chrono>
#include <limits>

namespace {
std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

CameraViewModel::CameraViewModel(bool start_backend, int rotation)
    : backend_enabled_(start_backend), rotation_(rotation)
{
    if (backend_enabled_) {
        start();
        timer_ = lv_timer_create(
            [](lv_timer_t *timer) { static_cast<CameraViewModel *>(lv_timer_get_user_data(timer))->poll(); }, 33, this);
    }
}

CameraViewModel::~CameraViewModel()
{
    if (timer_) lv_timer_delete(timer_);
    process_.reset();
}

void CameraViewModel::start()
{
    process_ = std::make_unique<CameraProcess>();
    std::string error;
    if (!process_->start(error, rotation_, state().profile)) {
        CameraEvent event;
        event.event   = "error";
        event.fatal   = true;
        event.message = error;
        receive(event, now_ms());
    }
}

void CameraViewModel::publish()
{
    revision_.set(revision_.value() == std::numeric_limits<std::int32_t>::max() ? 0 : revision_.value() + 1);
}

void CameraViewModel::select_mode(CameraMode mode)
{
    if (model_.select_mode(mode)) publish();
}

void CameraViewModel::select_profile(CameraProfile profile)
{
    if (!model_.select_profile(profile)) return;
    publish();
    if (process_) {
        if (!process_->set_profile(profile)) {
            CameraEvent event;
            event.event   = "error";
            event.fatal   = true;
            event.message = "Camera service disconnected. Please retry.";
            receive(event, now_ms());
        }
    } else if (!backend_enabled_) {
        CameraEvent event;
        event.event       = "state";
        event.state       = "ready";
        event.zoom        = state().zoom;
        event.profile     = profile;
        event.has_profile = true;
        receive(event, now_ms());
    }
}

void CameraViewModel::shutter()
{
    const auto command = model_.shutter();
    if (command.empty()) return;
    publish();
    if (process_ && !process_->send(command)) {
        CameraEvent event;
        event.event   = "error";
        event.fatal   = true;
        event.message = "Camera service disconnected. Please retry.";
        receive(event, now_ms());
    }
}

void CameraViewModel::toggle_grid()
{
    model_.toggle_grid();
    publish();
}
void CameraViewModel::toggle_review()
{
    model_.toggle_review();
    publish();
}

void CameraViewModel::set_zoom(double zoom)
{
    if (state().phase != CameraPhase::Ready || state().reviewing || (zoom != 1.0 && zoom != 2.0)) return;
    if (process_) {
        if (!process_->send("set_zoom", zoom)) {
            CameraEvent event;
            event.event   = "error";
            event.fatal   = true;
            event.message = "Camera service disconnected. Please retry.";
            receive(event, now_ms());
        }
    } else if (!backend_enabled_) {
        CameraEvent event;
        event.event = "state";
        event.state = "ready";
        event.zoom  = zoom;
        receive(event, now_ms());
    }
}

void CameraViewModel::retry()
{
    if (state().phase != CameraPhase::Error) return;
    model_.retry();
    frame_ = {};
    publish();
    if (backend_enabled_) {
        if (process_) {
            // A stuck camera can need bounded termination. Keep LVGL responsive
            // while waiting, and acquire the camera only after the old owner exits.
            retiring_process_ = std::async(std::launch::async, [old = std::move(process_)]() mutable { old.reset(); });
        } else
            start();
    }
}

void CameraViewModel::poll()
{
    if (retiring_process_.valid() && retiring_process_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        retiring_process_.get();
        start();
    }
    if (process_)
        process_->poll([this](const CameraEvent &event) { receive(event, now_ms()); },
                       [this](CameraFrame frame) { set_frame(std::move(frame)); });
    if (model_.tick(now_ms())) publish();
}

void CameraViewModel::receive(const CameraEvent &event, std::uint64_t timestamp)
{
    model_.apply(event, timestamp);
    if (event.event == "media" && !event.path.empty()) thumbnail_ = frame_;
    publish();
}

void CameraViewModel::set_frame(CameraFrame frame)
{
    if (frame.width <= 0 || frame.height <= 0 || frame.width > 1920 || frame.height > 1920 ||
        frame.width * frame.height > 1920 * 1080 ||
        frame.pixels.size() != static_cast<std::size_t>(frame.width) * frame.height * 3)
        return;
    frame_ = std::move(frame);
    publish();
}
