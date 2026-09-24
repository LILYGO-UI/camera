#pragma once

#include "components/lv_subject.hpp"
#include "pages/camera/camera_model.hpp"

#include <memory>
#include <future>

class CameraProcess;

class CameraViewModel {
public:
    explicit CameraViewModel(bool start_backend = true, int rotation = 0);
    ~CameraViewModel();
    CameraViewModel(const CameraViewModel &)            = delete;
    CameraViewModel &operator=(const CameraViewModel &) = delete;

    const CameraState &state() const noexcept
    {
        return model_.state();
    }
    const CameraFrame &frame() const noexcept
    {
        return frame_;
    }
    const CameraFrame &thumbnail() const noexcept
    {
        return thumbnail_;
    }
    lv_subject_t *revision_subject() noexcept
    {
        return revision_.get();
    }

    void select_mode(CameraMode mode);
    void shutter();
    void toggle_grid();
    void toggle_review();
    void set_zoom(double zoom);
    void retry();
    void poll();
    // Event injection also enables deterministic headless rendering and state tests.
    void receive(const CameraEvent &event, std::uint64_t now_ms);
    void set_frame(CameraFrame frame);

private:
    void publish();
    void start();
    CameraModel model_;
    IntSubject revision_{0};
    std::unique_ptr<CameraProcess> process_;
    std::future<void> retiring_process_;
    CameraFrame frame_;
    CameraFrame thumbnail_;
    lv_timer_t *timer_    = nullptr;
    bool backend_enabled_ = true;
    int rotation_         = 0;
};
