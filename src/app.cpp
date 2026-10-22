#include "app.hpp"

#include "app_identity.h"
#include "pages/camera/camera_view.hpp"

#include <memory>

namespace {

struct AppState {
    std::unique_ptr<CameraViewModel> camera_view_model;
    std::unique_ptr<CameraView> camera_view;
};

AppState app_state;

void app_open(const cm0_app_context_t *context)
{
    // Preserve AppKit's status-bar inset; remove the template's light gutters.
    lv_obj_set_style_pad_hor(context->root, 0, 0);
    lv_obj_set_style_pad_bottom(context->root, 38, 0);
    lv_obj_set_style_bg_color(context->root, lv_color_hex(0x000000), 0);
    // The CM0 camera needs a further 90-degree counterclockwise correction
    // relative to the portrait panel after libcamera handles the sensor mount.
    // SDL's synthetic scene has no physical mounting offset.
#if defined(CM0_APP_SIMULATOR)
    constexpr int camera_mount_rotation = 0;
#else
    constexpr int camera_mount_rotation = 270;
#endif
    // Combine the mounting correction with LVGL scanout compensation. Send the
    // same angle to preview and media export, including their width/height swap.
    const int display_rotation  = static_cast<int>(lv_display_get_rotation(lv_obj_get_display(context->root))) * 90;
    const int camera_rotation   = (display_rotation + camera_mount_rotation) % 360;
    app_state.camera_view_model = std::make_unique<CameraViewModel>(true, camera_rotation);
    app_state.camera_view       = std::make_unique<CameraView>(*app_state.camera_view_model);
    app_state.camera_view->create(context->root);
}

void app_close()
{
    if (app_state.camera_view) app_state.camera_view->destroy();
    app_state.camera_view.reset();
    app_state.camera_view_model.reset();
}

const cm0_app_descriptor_t app_descriptor{
    CM0_APP_API_VERSION, sizeof(cm0_app_descriptor_t), LILYGO_UI_CAMERA_APP_ID, app_open, app_close,
};

}  // namespace

extern "C" const cm0_app_descriptor_t *cm0_app_get_descriptor()
{
    return &app_descriptor;
}
