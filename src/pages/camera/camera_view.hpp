#pragma once

#include "pages/camera/camera_view_model.hpp"
#include <lvgl.h>

// The parent owns every LVGL object. The ViewModel must outlive this View.
class CameraView {
public:
    explicit CameraView(CameraViewModel &view_model) noexcept;
    ~CameraView();
    lv_obj_t *create(lv_obj_t *parent);
    void destroy() noexcept;

private:
    static void action_event(lv_event_t *event);
    static void size_event(lv_event_t *event);
    static void delete_event(lv_event_t *event);
    static void key_event(lv_event_t *event);
    static void revision_changed(lv_observer_t *observer, lv_subject_t *subject);
    void refresh();
    void apply_layout();
    void fit_preview();

    struct Widgets {
        lv_obj_t *surface     = nullptr;
        lv_obj_t *header      = nullptr;
        lv_obj_t *grid_button = nullptr;
        lv_obj_t *body        = nullptr;
        lv_obj_t *preview     = nullptr;
        lv_obj_t *image       = nullptr;
        lv_obj_t *stage       = nullptr;
        lv_obj_t *grid[4]{};
        lv_obj_t *badge           = nullptr;
        lv_obj_t *timer           = nullptr;
        lv_obj_t *message         = nullptr;
        lv_obj_t *message_title   = nullptr;
        lv_obj_t *message_body    = nullptr;
        lv_obj_t *retry           = nullptr;
        lv_obj_t *zoom_row        = nullptr;
        lv_obj_t *zoom_one        = nullptr;
        lv_obj_t *zoom_two        = nullptr;
        lv_obj_t *status          = nullptr;
        lv_obj_t *dock            = nullptr;
        lv_obj_t *mode_row        = nullptr;
        lv_obj_t *photo_mode      = nullptr;
        lv_obj_t *video_mode      = nullptr;
        lv_obj_t *capture_row     = nullptr;
        lv_obj_t *gallery         = nullptr;
        lv_obj_t *thumbnail       = nullptr;
        lv_obj_t *gallery_empty   = nullptr;
        lv_obj_t *shutter         = nullptr;
        lv_obj_t *shutter_inner   = nullptr;
        lv_obj_t *count           = nullptr;
        lv_obj_t *hint            = nullptr;
        lv_obj_t *review          = nullptr;
        lv_obj_t *review_header   = nullptr;
        lv_obj_t *review_title    = nullptr;
        lv_obj_t *review_close    = nullptr;
        lv_obj_t *review_image    = nullptr;
        lv_obj_t *review_filename = nullptr;
        lv_obj_t *review_note     = nullptr;
    } w_;
    CameraViewModel &view_model_;
    lv_image_dsc_t preview_descriptor_{};
    lv_image_dsc_t thumbnail_descriptor_{};
    bool laying_out_ = false;
};
