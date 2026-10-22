#include "pages/camera/camera_model.hpp"
#include <cassert>
#include <iostream>

namespace {
void test_capture_profiles()
{
    CameraModel model;
    assert(model.state().profile == kDefaultCameraProfile);
    const auto defaults = camera_profile_settings(kDefaultCameraProfile);
    assert(defaults.width == 1280 && defaults.height == 720 && defaults.fps == 30);
    assert(!model.select_profile(CameraProfile::Hd60));
    CameraEvent event;
    event.event       = "ready";
    event.has_profile = true;
    event.profile     = CameraProfile::FullHd30;
    model.apply(event, 0);
    assert(!model.select_profile(CameraProfile::FullHd30));
    assert(!model.select_profile(static_cast<CameraProfile>(99)));
    assert(model.select_mode(CameraMode::Video));
    assert(model.select_profile(CameraProfile::Hd60));
    assert(model.state().phase == CameraPhase::Reconfiguring);
    assert(model.state().profile == CameraProfile::FullHd30);
    assert(model.shutter().empty());
    assert(!model.select_mode(CameraMode::Photo));
    assert(!model.select_profile(CameraProfile::Hd60));
    event.event   = "state";
    event.state   = "reconfiguring";
    event.profile = CameraProfile::Hd60;
    model.apply(event, 1);
    assert(model.state().profile == CameraProfile::FullHd30);
    event.state = "ready";
    model.apply(event, 2);
    assert(model.state().phase == CameraPhase::Ready);
    assert(model.state().profile == CameraProfile::Hd60);
    assert(model.state().mode == CameraMode::Video);
    assert(model.state().status == "Tap to record · No audio");

    // Failed reconfiguration keeps the worker's restored, confirmed profile.
    assert(model.select_profile(CameraProfile::FullHd30));
    CameraEvent failure;
    failure.event   = "error";
    failure.message = "Camera mode unavailable";
    model.apply(failure, 3);
    assert(model.state().phase == CameraPhase::Reconfiguring);
    model.apply(event, 4);
    assert(model.state().phase == CameraPhase::Ready);
    assert(model.state().profile == CameraProfile::Hd60);
    assert(model.state().error == failure.message);
    event.has_profile = false;
    event.profile     = CameraProfile::FullHd30;
    model.apply(event, 5);
    assert(model.state().profile == CameraProfile::Hd60);
    event.has_profile = true;
    event.profile     = static_cast<CameraProfile>(99);
    model.apply(event, 6);
    assert(model.state().profile == CameraProfile::Hd60);

    event       = {};
    event.event = "media";
    event.kind  = "photo";
    event.path  = "/tmp/profile.jpg";
    model.apply(event, 7);
    model.toggle_review();
    assert(model.state().reviewing);
    assert(!model.select_profile(CameraProfile::FullHd30));
    model.toggle_review();
    for (const char *phase : {"recording", "capturing", "stopping"}) {
        event       = {};
        event.event = "state";
        event.state = phase;
        model.apply(event, 8);
        assert(!model.select_profile(CameraProfile::FullHd30));
    }
    failure.fatal = true;
    model.apply(failure, 9);
    assert(!model.select_profile(CameraProfile::FullHd30));
    model.retry();
    assert(model.state().profile == CameraProfile::Hd60);
}
}  // namespace

int main()
{
    test_capture_profiles();
    CameraModel model;
    assert(model.shutter().empty());
    assert(!model.select_mode(CameraMode::Video));
    CameraEvent event;
    event.event     = "ready";
    event.simulated = true;
    event.width     = 640;
    event.height    = 360;
    event.zoom      = 1.0;
    model.apply(event, 100);
    assert(model.state().phase == CameraPhase::Ready && model.state().simulated);
    assert(model.shutter() == "capture");
    assert(model.shutter().empty());
    assert(!model.select_mode(CameraMode::Video));
    event       = {};
    event.event = "media";
    event.kind  = "photo";
    event.path  = "/tmp/照片 \"一\".jpg";
    model.apply(event, 200);
    assert(model.state().saved_count == 1);
    assert(model.state().last_media == "/tmp/照片 \"一\".jpg");
    event       = {};
    event.event = "state";
    event.state = "ready";
    model.apply(event, 210);
    model.toggle_review();
    assert(model.state().reviewing && model.shutter().empty());
    CameraEvent disconnected;
    disconnected.event = "error";
    disconnected.fatal = true;
    model.apply(disconnected, 211);
    assert(!model.state().reviewing && model.state().phase == CameraPhase::Error);
    model.apply(event, 212);
    assert(model.state().phase == CameraPhase::Error);
    model.retry();
    model.apply(event, 212);
    model.toggle_review();
    model.toggle_review();
    assert(model.select_mode(CameraMode::Video));
    assert(model.shutter() == "start_recording");
    assert(model.shutter().empty());
    event.state = "recording";
    model.apply(event, 1000);
    assert(!model.select_mode(CameraMode::Photo));
    assert(model.tick(3200));
    assert(model.state().elapsed_seconds == 2);
    model.apply(event, 3300);
    assert(!model.tick(3400));
    assert(model.shutter() == "stop_recording");
    assert(model.shutter().empty());
    assert(!model.tick(5000));
    event         = {};
    event.event   = "error";
    event.message = "Disk full";
    model.apply(event, 6000);
    event       = {};
    event.event = "state";
    event.state = "ready";
    model.apply(event, 6100);
    assert(model.state().saved_count == 1);
    assert(model.state().error == "Disk full");
    assert(model.shutter() == "start_recording");
    assert(model.state().error.empty());
    event         = {};
    event.event   = "error";
    event.fatal   = true;
    event.message = "Disconnected";
    model.apply(event, 6200);
    assert(model.state().phase == CameraPhase::Error);
    assert(model.shutter().empty());
    event       = {};
    event.event = "state";
    event.state = "stopping";
    model.apply(event, 6300);
    event       = {};
    event.event = "media";
    event.kind  = "video";
    event.path  = "/tmp/finalized.mp4";
    model.apply(event, 6400);
    event       = {};
    event.event = "state";
    event.state = "ready";
    model.apply(event, 6500);
    assert(model.state().phase == CameraPhase::Error);
    assert(model.state().last_media == "/tmp/finalized.mp4");
    assert(model.state().error == "Disconnected");
    model.retry();
    assert(model.state().phase == CameraPhase::Starting);
    assert(model.state().error.empty());
    model.toggle_grid();
    assert(model.state().grid);
    std::cout << "camera model state and error recovery tests passed\n";
}
