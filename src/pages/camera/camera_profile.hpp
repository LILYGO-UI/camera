#pragma once

enum class CameraProfile { FullHd30, Hd30, Hd60 };

// 720P30 is the conservative default for IMX219: the libcamera pipeline can
// produce a 1280x720 viewfinder stream, while 60 FPS remains driver dependent.
constexpr CameraProfile kDefaultCameraProfile = CameraProfile::Hd30;

struct CameraProfileSettings {
    int width;
    int height;
    int fps;
    const char *label;
};

constexpr bool valid_camera_profile(CameraProfile profile)
{
    return profile == CameraProfile::FullHd30 || profile == CameraProfile::Hd30 || profile == CameraProfile::Hd60;
}

constexpr CameraProfileSettings camera_profile_settings(CameraProfile profile)
{
    switch (profile) {
        case CameraProfile::Hd30:
            return {1280, 720, 30, "720P · 30"};
        case CameraProfile::Hd60:
            return {1280, 720, 60, "720P · 60"};
        case CameraProfile::FullHd30:
        default:
            return {1920, 1080, 30, "1080P · 30"};
    }
}
