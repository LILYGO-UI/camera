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
    // The camera is fixed to the panel. Counter-rotate its pixels before LVGL
    // rotates the complete UI into the panel's native orientation. Read the
    // display rotation, not the aspect ratio: resizing an SDL window is not a
    // physical camera rotation. The worker also applies this to saved media.
    const int camera_rotation   = static_cast<int>(lv_display_get_rotation(lv_obj_get_display(context->root))) * 90;
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
