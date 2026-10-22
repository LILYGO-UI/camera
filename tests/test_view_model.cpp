#include "pages/camera/camera_view_model.hpp"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>

namespace {
void test_capture_profile_selection()
{
    CameraViewModel vm(false);
    assert(vm.state().profile == kDefaultCameraProfile);
    vm.select_profile(CameraProfile::Hd60);
    assert(vm.state().phase == CameraPhase::Starting);
    CameraEvent ready;
    ready.event = "ready";
    vm.receive(ready, 0);
    vm.set_zoom(2.0);
    vm.select_mode(CameraMode::Video);
    vm.select_profile(CameraProfile::Hd60);
    assert(vm.state().phase == CameraPhase::Ready);
    assert(vm.state().profile == CameraProfile::Hd60);
    assert(vm.state().zoom == 2.0 && vm.state().mode == CameraMode::Video);
    vm.select_profile(static_cast<CameraProfile>(99));
    assert(vm.state().profile == CameraProfile::Hd60);
    vm.select_profile(CameraProfile::FullHd30);
    assert(vm.state().profile == CameraProfile::FullHd30);
    vm.shutter();
    vm.select_profile(CameraProfile::Hd60);
    assert(vm.state().profile == CameraProfile::FullHd30);
    assert(vm.state().phase == CameraPhase::StartingRecording);
    vm.receive(ready, 1);
    CameraEvent media;
    media.event = "media";
    media.kind  = "photo";
    media.path  = "/tmp/profile.jpg";
    vm.receive(media, 2);
    vm.toggle_review();
    vm.select_profile(CameraProfile::Hd60);
    assert(vm.state().profile == CameraProfile::FullHd30);
    assert(vm.state().reviewing);
}

void test_frame_pixel_limit()
{
    CameraViewModel vm(false);
    CameraFrame frame;
    frame.width    = 1;
    frame.height   = 1;
    frame.sequence = 1;
    frame.pixels   = {11, 22, 33};
    vm.set_frame(frame);
    // Enlarging the allowable height must not enlarge the memory budget.
    frame.width    = 1920;
    frame.height   = 1081;
    frame.sequence = 2;
    frame.pixels.resize(static_cast<std::size_t>(frame.width) * frame.height * 3);
    vm.set_frame(std::move(frame));
    assert(vm.frame().sequence == 1);
    assert(vm.frame().width == 1 && vm.frame().height == 1);
}

void test_operation_error_and_retry_state()
{
    CameraViewModel vm(false);
    CameraEvent event;
    event.event     = "ready";
    event.simulated = true;
    vm.receive(event, 0);
    CameraFrame frame;
    frame.width = frame.height = 1;
    frame.sequence             = 1;
    frame.pixels               = {20, 40, 60};
    vm.set_frame(frame);
    vm.shutter();
    event         = {};
    event.event   = "error";
    event.message = "JPEG encoder failed";
    vm.receive(event, 1);
    event.event = "state";
    event.state = "ready";
    vm.receive(event, 2);
    assert(vm.state().phase == CameraPhase::Ready);
    assert(vm.state().saved_count == 0);
    assert(vm.frame().pixels == frame.pixels);
    vm.shutter();
    assert(vm.state().phase == CameraPhase::Capturing);
    event         = {};
    event.event   = "error";
    event.fatal   = true;
    event.message = "Camera disconnected";
    vm.receive(event, 3);
    assert(vm.state().phase == CameraPhase::Error);
    event       = {};
    event.event = "state";
    event.state = "ready";
    vm.receive(event, 4);
    assert(vm.state().phase == CameraPhase::Error);
    vm.retry();
    assert(vm.state().phase == CameraPhase::Starting);
    assert(vm.frame().pixels.empty());
    event.event = "ready";
    vm.receive(event, 5);
    assert(vm.state().phase == CameraPhase::Ready);
}

void test_native_startup_retry()
{
    const char *old_simulate_value = std::getenv("CAMERA_SIMULATE");
    const char *old_output_value   = std::getenv("CAMERA_OUTPUT_DIR");
    const bool had_simulate        = old_simulate_value;
    const bool had_output          = old_output_value;
    const std::string old_simulate = old_simulate_value ? old_simulate_value : "";
    const std::string old_output   = old_output_value ? old_output_value : "";
    char temporary[]               = "/tmp/camera-view-model-XXXXXX";
    const char *directory          = mkdtemp(temporary);
    assert(directory);
    const auto blocker = std::filesystem::path(directory) / "not-a-directory";
    std::ofstream(blocker) << "x";
    setenv("CAMERA_SIMULATE", "1", 1);
    setenv("CAMERA_OUTPUT_DIR", blocker.c_str(), 1);
    {
        CameraViewModel vm(true);
        const auto failure_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vm.state().phase != CameraPhase::Error && std::chrono::steady_clock::now() < failure_deadline) {
            vm.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(vm.state().phase == CameraPhase::Error);
        assert(!vm.state().error.empty());
        setenv("CAMERA_OUTPUT_DIR", directory, 1);
        vm.retry();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((vm.state().phase != CameraPhase::Ready || vm.frame().pixels.empty()) &&
               std::chrono::steady_clock::now() < deadline) {
            vm.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(vm.state().phase == CameraPhase::Ready);
        assert(vm.state().simulated);
        assert(vm.state().error.empty());
        assert(vm.frame().width == 640 && vm.frame().height == 360);
        vm.select_profile(CameraProfile::Hd60);
        const auto switch_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vm.state().phase != CameraPhase::Ready && std::chrono::steady_clock::now() < switch_deadline) {
            vm.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(vm.state().phase == CameraPhase::Ready);
        assert(vm.state().profile == CameraProfile::Hd60);
        CameraEvent failure;
        failure.event   = "error";
        failure.fatal   = true;
        failure.message = "Test reconnection";
        vm.receive(failure, 0);
        vm.retry();
        const auto reconnect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (vm.state().phase != CameraPhase::Ready && std::chrono::steady_clock::now() < reconnect_deadline) {
            vm.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(vm.state().phase == CameraPhase::Ready);
        assert(vm.state().profile == CameraProfile::Hd60);
    }
    std::filesystem::remove_all(directory);
    if (had_simulate)
        setenv("CAMERA_SIMULATE", old_simulate.c_str(), 1);
    else
        unsetenv("CAMERA_SIMULATE");
    if (had_output)
        setenv("CAMERA_OUTPUT_DIR", old_output.c_str(), 1);
    else
        unsetenv("CAMERA_OUTPUT_DIR");
}
}  // namespace

int main()
{
    lv_init();
    {
        CameraViewModel vm(false);
        int notifications = 0;
        auto *observer    = lv_subject_add_observer(
            vm.revision_subject(),
            [](lv_observer_t *observer, lv_subject_t *) { ++*static_cast<int *>(lv_observer_get_user_data(observer)); },
            &notifications);
        const int initial = notifications;
        vm.shutter();
        assert(notifications == initial);
        CameraEvent event;
        event.event     = "ready";
        event.simulated = true;
        vm.receive(event, 1);
        assert(notifications > initial);
        vm.set_zoom(2);
        assert(vm.state().zoom == 2);
        vm.set_zoom(20);
        assert(vm.state().zoom == 2);
        vm.select_mode(CameraMode::Video);
        vm.shutter();
        assert(vm.state().phase == CameraPhase::StartingRecording);
        vm.select_mode(CameraMode::Photo);
        assert(vm.state().mode == CameraMode::Video);
        vm.toggle_review();
        assert(!vm.state().reviewing);
        event       = {};
        event.event = "state";
        event.state = "recording";
        event.zoom  = 2;
        vm.receive(event, 100);
        vm.set_zoom(1);
        assert(vm.state().zoom == 2);
        vm.shutter();
        assert(vm.state().phase == CameraPhase::Stopping);
        CameraFrame frame;
        frame.width    = 2;
        frame.height   = 1;
        frame.sequence = 1;
        frame.pixels   = {0, 0, 255, 0, 255, 0};
        vm.set_frame(frame);
        event       = {};
        event.event = "media";
        event.kind  = "video";
        event.path  = "/tmp/video.mp4";
        vm.receive(event, 200);
        assert(vm.thumbnail().pixels == frame.pixels);
        frame.pixels[0] = 42;
        frame.sequence  = 2;
        vm.set_frame(frame);
        assert(vm.thumbnail().pixels[0] == 0);
        event       = {};
        event.event = "state";
        event.state = "ready";
        vm.receive(event, 201);
        vm.toggle_review();
        assert(vm.state().reviewing);
        vm.shutter();
        assert(vm.state().phase == CameraPhase::Ready);
        lv_observer_remove(observer);
    }
    test_capture_profile_selection();
    test_frame_pixel_limit();
    test_operation_error_and_retry_state();
    test_native_startup_retry();
    lv_deinit();
}
