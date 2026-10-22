#include "pages/camera/camera_view.hpp"

#include <cm0/typography.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <tuple>

namespace {
// Camera-specific semantic palette: immersive canvas, white controls, yellow
// active mode, red record, secondary text and translucent control surfaces.
constexpr unsigned kCanvas  = 0x000000;
constexpr unsigned kSurface = 0x171719;
constexpr unsigned kWhite   = 0xfafafa;
constexpr unsigned kMuted   = 0x929297;
constexpr unsigned kYellow  = 0xffd60a;
constexpr unsigned kRecord  = 0xff453a;

void font(lv_obj_t *object, int size)
{
    if (const auto *face = lilygo_ui_font_get(size)) lv_obj_set_style_text_font(object, face, 0);
}

lv_obj_t *container(lv_obj_t *parent, const char *name)
{
    auto *object = lv_obj_create(parent);
    lv_obj_remove_style_all(object);
    lv_obj_set_name_static(object, name);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(object, lv_color_hex(kWhite), 0);
    font(object, 22);
    return object;
}

lv_obj_t *label(lv_obj_t *parent, const char *text, int size, unsigned color = kWhite)
{
    auto *object = lv_label_create(parent);
    lv_label_set_text(object, text);
    font(object, size);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    return object;
}

lv_obj_t *button(lv_obj_t *parent, const char *name, const char *text, lv_event_cb_t action, void *data)
{
    auto *object = container(parent, name);
    lv_obj_add_flag(object, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(object, 64, 48);
    lv_obj_set_style_radius(object, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(object, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_opa(object, LV_OPA_40, LV_STATE_DISABLED);
    lv_obj_add_event_cb(object, action, LV_EVENT_CLICKED, data);
    if (text && *text) {
        auto *caption = label(object, text, 22);
        lv_obj_center(caption);
    }
    return object;
}

void visible(lv_obj_t *object, bool show)
{
    if (show)
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}

void disabled(lv_obj_t *object, bool value)
{
    if (value)
        lv_obj_add_state(object, LV_STATE_DISABLED);
    else
        lv_obj_remove_state(object, LV_STATE_DISABLED);
}

void selection(lv_obj_t *object, bool selected)
{
    lv_obj_set_style_text_color(lv_obj_get_child(object, 0), lv_color_hex(selected ? kYellow : kWhite), 0);
}

void image_descriptor(lv_image_dsc_t &descriptor, const CameraFrame &frame)
{
    descriptor               = {};
    descriptor.header.magic  = LV_IMAGE_HEADER_MAGIC;
    descriptor.header.cf     = LV_COLOR_FORMAT_RGB888;
    descriptor.header.w      = static_cast<std::uint32_t>(frame.width);
    descriptor.header.h      = static_cast<std::uint32_t>(frame.height);
    descriptor.header.stride = static_cast<std::uint32_t>(frame.width * 3);
    descriptor.data_size     = static_cast<std::uint32_t>(frame.pixels.size());
    descriptor.data          = frame.pixels.data();
}

bool valid_frame(const CameraFrame &frame)
{
    return frame.width > 0 && frame.height > 0 &&
           frame.pixels.size() == static_cast<std::size_t>(frame.width) * frame.height * 3;
}

bool same_controls(const CameraState &a, const CameraState &b)
{
    return std::tie(a.mode, a.profile, a.phase, a.simulated, a.grid, a.reviewing, a.zoom, a.elapsed_seconds,
                    a.saved_count, a.status, a.error, a.last_media, a.last_kind) ==
           std::tie(b.mode, b.profile, b.phase, b.simulated, b.grid, b.reviewing, b.zoom, b.elapsed_seconds,
                    b.saved_count, b.status, b.error, b.last_media, b.last_kind);
}
}  // namespace

CameraView::CameraView(CameraViewModel &view_model) noexcept : view_model_(view_model)
{
}
CameraView::~CameraView()
{
    destroy();
}

lv_obj_t *CameraView::create(lv_obj_t *parent)
{
    destroy();
    // Image scaling can flush layout; wait until every control exists.
    laying_out_ = true;
    w_.surface  = container(parent, "camera_surface");
    lv_obj_set_size(w_.surface, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(w_.surface, lv_color_hex(kCanvas), 0);
    lv_obj_set_style_bg_opa(w_.surface, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(w_.surface, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(w_.surface, size_event, LV_EVENT_SIZE_CHANGED, this);
    lv_obj_add_event_cb(w_.surface, delete_event, LV_EVENT_DELETE, this);
    lv_obj_add_event_cb(w_.surface, key_event, LV_EVENT_KEY, this);
    if (auto *group = lv_group_get_default()) lv_group_add_obj(group, w_.surface);

    w_.header = container(w_.surface, "camera_header");
    lv_obj_set_size(w_.header, LV_PCT(100), 76);
    lv_obj_set_style_pad_hor(w_.header, 24, 0);
    lv_obj_set_flex_flow(w_.header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w_.header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    w_.grid_button = button(w_.header, "camera_grid", "", action_event, this);
    lv_obj_set_size(w_.grid_button, 48, 48);
    auto *grid_icon = container(w_.grid_button, "camera_grid_icon");
    lv_obj_set_size(grid_icon, 23, 23);
    lv_obj_center(grid_icon);
    lv_obj_set_style_border_width(grid_icon, 1, 0);
    lv_obj_set_style_border_color(grid_icon, lv_color_hex(kWhite), 0);
    for (int i = 0; i < 4; ++i) {
        auto *line = container(grid_icon, nullptr);
        lv_obj_set_style_bg_color(line, lv_color_hex(kWhite), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_set_size(line, i < 2 ? 1 : LV_PCT(100), i < 2 ? LV_PCT(100) : 1);
        lv_obj_set_pos(line, i < 2 ? LV_PCT((i + 1) * 33) : 0, i >= 2 ? LV_PCT((i - 1) * 33) : 0);
    }
    auto *title = label(w_.header, "Camera", 22);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    w_.profile_button = button(w_.header, "camera_profile", "", action_event, this);
    lv_obj_set_size(w_.profile_button, 112, 48);
    lv_obj_set_style_bg_opa(w_.profile_button, LV_OPA_COVER, 0);
    w_.profile_label = label(w_.profile_button, camera_profile_settings(kDefaultCameraProfile).label, 14);
    lv_obj_set_name_static(w_.profile_label, "camera_profile_label");
    lv_obj_center(w_.profile_label);

    w_.body = container(w_.surface, "camera_body");
    lv_obj_set_width(w_.body, LV_PCT(100));
    lv_obj_set_flex_grow(w_.body, 1);
    w_.preview = container(w_.body, "camera_preview");
    lv_obj_set_flex_grow(w_.preview, 1);
    lv_obj_add_event_cb(w_.preview, size_event, LV_EVENT_SIZE_CHANGED, this);
    w_.stage = container(w_.preview, "camera_image_stage");
    lv_obj_set_size(w_.stage, LV_PCT(100), LV_PCT(100));
    lv_obj_center(w_.stage);
    lv_obj_set_style_bg_color(w_.stage, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(w_.stage, LV_OPA_COVER, 0);
    w_.image = lv_image_create(w_.stage);
    lv_obj_set_name_static(w_.image, "camera_live_image");
    lv_obj_set_size(w_.image, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(w_.image);
    lv_image_set_pivot(w_.image, LV_PCT(50), LV_PCT(50));
    for (int i = 0; i < 4; ++i) {
        w_.grid[i] = container(w_.stage, nullptr);
        lv_obj_set_style_bg_color(w_.grid[i], lv_color_hex(kWhite), 0);
        lv_obj_set_style_bg_opa(w_.grid[i], LV_OPA_40, 0);
        lv_obj_set_size(w_.grid[i], i < 2 ? 1 : LV_PCT(100), i < 2 ? LV_PCT(100) : 1);
        lv_obj_set_pos(w_.grid[i], i < 2 ? LV_PCT((i + 1) * 33) : 0, i >= 2 ? LV_PCT((i - 1) * 33) : 0);
    }
    w_.badge = label(w_.preview, "Simulated preview", 14, kYellow);
    lv_obj_set_name_static(w_.badge, "camera_simulator_badge");
    lv_obj_set_style_bg_color(w_.badge, lv_color_hex(kCanvas), 0);
    lv_obj_set_style_bg_opa(w_.badge, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(w_.badge, 8, 0);
    lv_obj_set_style_radius(w_.badge, 6, 0);
    lv_obj_align(w_.badge, LV_ALIGN_TOP_LEFT, 20, 14);
    w_.timer = label(w_.preview, "00:00:00", 22);
    lv_obj_set_name_static(w_.timer, "camera_timer");
    lv_obj_set_style_bg_color(w_.timer, lv_color_hex(kRecord), 0);
    lv_obj_set_style_bg_opa(w_.timer, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(w_.timer, 14, 0);
    lv_obj_set_style_pad_ver(w_.timer, 6, 0);
    lv_obj_set_style_radius(w_.timer, 6, 0);
    lv_obj_align(w_.timer, LV_ALIGN_TOP_MID, 0, 14);

    w_.message = container(w_.preview, "camera_message");
    lv_obj_set_width(w_.message, LV_PCT(86));
    lv_obj_set_style_max_width(w_.message, 420, 0);
    lv_obj_set_height(w_.message, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(w_.message, LV_PCT(100), 0);
    lv_obj_add_flag(w_.message, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(w_.message, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(w_.message, LV_OPA_90, 0);
    lv_obj_set_style_pad_all(w_.message, 24, 0);
    lv_obj_set_style_pad_row(w_.message, 14, 0);
    lv_obj_set_style_radius(w_.message, 20, 0);
    lv_obj_set_flex_flow(w_.message, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w_.message, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(w_.message);
    w_.message_title = label(w_.message, "Connecting camera", 28);
    lv_obj_set_width(w_.message_title, LV_PCT(100));
    lv_label_set_long_mode(w_.message_title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(w_.message_title, LV_TEXT_ALIGN_CENTER, 0);
    w_.message_body = label(w_.message, "You can shoot when the camera is ready", 14, kMuted);
    lv_obj_set_width(w_.message_body, LV_PCT(100));
    lv_label_set_long_mode(w_.message_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(w_.message_body, LV_TEXT_ALIGN_CENTER, 0);
    w_.retry = button(w_.message, "camera_retry", "Retry", action_event, this);
    lv_obj_set_size(w_.retry, 108, 48);
    lv_obj_set_style_bg_opa(w_.retry, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(w_.retry, lv_color_hex(0x333336), 0);

    w_.zoom_row = container(w_.preview, "camera_zoom");
    lv_obj_set_size(w_.zoom_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(w_.zoom_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(w_.zoom_row, 5, 0);
    lv_obj_set_style_pad_column(w_.zoom_row, 4, 0);
    lv_obj_set_style_radius(w_.zoom_row, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(w_.zoom_row, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(w_.zoom_row, LV_OPA_80, 0);
    lv_obj_align(w_.zoom_row, LV_ALIGN_BOTTOM_MID, 0, -42);
    w_.zoom_one = button(w_.zoom_row, "camera_zoom_one", "1×", action_event, this);
    w_.zoom_two = button(w_.zoom_row, "camera_zoom_two", "2×", action_event, this);
    lv_obj_set_size(w_.zoom_one, 48, 44);
    lv_obj_set_size(w_.zoom_two, 48, 44);
    w_.status = label(w_.preview, "", 14, kMuted);
    lv_obj_set_name_static(w_.status, "camera_status");
    lv_obj_set_width(w_.status, LV_PCT(90));
    lv_label_set_long_mode(w_.status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(w_.status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(w_.status, LV_ALIGN_BOTTOM_MID, 0, -10);

    w_.dock = container(w_.body, "camera_dock");
    lv_obj_set_flex_flow(w_.dock, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(w_.dock, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(w_.dock, 10, 0);
    w_.mode_row = container(w_.dock, "camera_modes");
    lv_obj_set_size(w_.mode_row, LV_SIZE_CONTENT, 48);
    lv_obj_set_style_pad_column(w_.mode_row, 12, 0);
    lv_obj_set_flex_flow(w_.mode_row, LV_FLEX_FLOW_ROW);
    w_.video_mode  = button(w_.mode_row, "camera_mode_video", "Video", action_event, this);
    w_.photo_mode  = button(w_.mode_row, "camera_mode_photo", "Photo", action_event, this);
    w_.capture_row = container(w_.dock, "camera_capture_controls");
    lv_obj_set_size(w_.capture_row, LV_PCT(100), 112);
    lv_obj_set_flex_flow(w_.capture_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w_.capture_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    w_.gallery = button(w_.capture_row, "camera_gallery", "", action_event, this);
    lv_obj_set_size(w_.gallery, 58, 58);
    lv_obj_set_style_radius(w_.gallery, 10, 0);
    lv_obj_set_style_clip_corner(w_.gallery, true, 0);
    lv_obj_set_style_bg_color(w_.gallery, lv_color_hex(kSurface), 0);
    lv_obj_set_style_bg_opa(w_.gallery, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(w_.gallery, lv_color_hex(0x414145), 0);
    lv_obj_set_style_border_width(w_.gallery, 1, 0);
    w_.thumbnail = lv_image_create(w_.gallery);
    lv_obj_set_size(w_.thumbnail, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(w_.thumbnail, LV_IMAGE_ALIGN_COVER);
    w_.gallery_empty = label(w_.gallery, LV_SYMBOL_IMAGE, 22, kMuted);
    lv_obj_center(w_.gallery_empty);
    w_.shutter = button(w_.capture_row, "camera_shutter", "", action_event, this);
    lv_obj_set_size(w_.shutter, 96, 96);
    lv_obj_set_style_border_width(w_.shutter, 4, 0);
    lv_obj_set_style_border_color(w_.shutter, lv_color_hex(kWhite), 0);
    lv_obj_set_style_pad_all(w_.shutter, 6, 0);
    w_.shutter_inner = container(w_.shutter, "camera_shutter_inner");
    lv_obj_set_size(w_.shutter_inner, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_radius(w_.shutter_inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(w_.shutter_inner, LV_OPA_COVER, 0);
    lv_obj_center(w_.shutter_inner);
    w_.count = label(w_.capture_row, "0\nSaved", 14, kMuted);
    lv_obj_set_name_static(w_.count, "camera_saved_count");
    lv_obj_set_size(w_.count, 58, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(w_.count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(w_.count, 6, 0);
    w_.hint = label(w_.dock, "Tap shutter to take a photo", 14, kMuted);

    w_.review = container(w_.surface, "camera_review");
    lv_obj_add_flag(w_.review, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(w_.review, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(w_.review, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(w_.review, lv_color_hex(kCanvas), 0);
    lv_obj_set_style_bg_opa(w_.review, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(w_.review, 24, 0);
    lv_obj_set_style_pad_row(w_.review, 18, 0);
    lv_obj_set_flex_flow(w_.review, LV_FLEX_FLOW_COLUMN);
    w_.review_header = container(w_.review, "camera_review_header");
    lv_obj_set_size(w_.review_header, LV_PCT(100), 48);
    lv_obj_set_flex_flow(w_.review_header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(w_.review_header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    w_.review_title = label(w_.review_header, "Recent capture", 22);
    w_.review_close = button(w_.review_header, "camera_review_close", "Done", action_event, this);
    lv_obj_set_style_text_color(lv_obj_get_child(w_.review_close, 0), lv_color_hex(kYellow), 0);
    w_.review_image = lv_image_create(w_.review);
    lv_obj_set_name_static(w_.review_image, "camera_review_image");
    lv_obj_set_width(w_.review_image, LV_PCT(100));
    lv_obj_set_height(w_.review_image, 0);
    lv_obj_set_flex_grow(w_.review_image, 1);
    lv_image_set_inner_align(w_.review_image, LV_IMAGE_ALIGN_CONTAIN);
    w_.review_filename = label(w_.review, "", 22);
    lv_obj_set_width(w_.review_filename, LV_PCT(100));
    lv_label_set_long_mode(w_.review_filename, LV_LABEL_LONG_DOT);
    w_.review_note = label(w_.review, "", 14, kMuted);
    lv_obj_set_width(w_.review_note, LV_PCT(100));
    lv_label_set_long_mode(w_.review_note, LV_LABEL_LONG_WRAP);

    laying_out_ = false;
    lv_subject_add_observer_obj(view_model_.revision_subject(), revision_changed, w_.surface, this);
    apply_layout();
    refresh();
    return w_.surface;
}

void CameraView::destroy() noexcept
{
    if (w_.surface) lv_obj_delete(w_.surface);
    w_ = {};
    lv_image_cache_drop(&preview_descriptor_);
    lv_image_cache_drop(&thumbnail_descriptor_);
    preview_descriptor_    = {};
    thumbnail_descriptor_  = {};
    preview_sequence_      = 0;
    thumbnail_sequence_    = 0;
    thumbnail_saved_count_ = 0;
    controls_initialized_  = false;
    images_initialized_    = false;
}

void CameraView::delete_event(lv_event_t *event)
{
    static_cast<CameraView *>(lv_event_get_user_data(event))->w_ = {};
}

void CameraView::size_event(lv_event_t *event)
{
    auto *view = static_cast<CameraView *>(lv_event_get_user_data(event));
    if (lv_event_get_target(event) == view->w_.surface)
        view->apply_layout();
    else
        view->fit_preview();
}

void CameraView::key_event(lv_event_t *event)
{
    auto &vm = static_cast<CameraView *>(lv_event_get_user_data(event))->view_model_;
    switch (lv_event_get_key(event)) {
        case LV_KEY_ENTER:
        case ' ':
            vm.shutter();
            break;
        case LV_KEY_LEFT:
            vm.select_mode(CameraMode::Video);
            break;
        case LV_KEY_RIGHT:
            vm.select_mode(CameraMode::Photo);
            break;
        case LV_KEY_ESC:
            if (vm.state().reviewing) vm.toggle_review();
            break;
        default:
            break;
    }
}

void CameraView::action_event(lv_event_t *event)
{
    auto *view   = static_cast<CameraView *>(lv_event_get_user_data(event));
    auto *target = lv_event_get_target_obj(event);
    if (lv_obj_has_state(target, LV_STATE_DISABLED)) return;
    auto &w  = view->w_;
    auto &vm = view->view_model_;
    if (target == w.shutter)
        vm.shutter();
    else if (target == w.photo_mode)
        vm.select_mode(CameraMode::Photo);
    else if (target == w.video_mode)
        vm.select_mode(CameraMode::Video);
    else if (target == w.grid_button)
        vm.toggle_grid();
    else if (target == w.profile_button)
        vm.select_profile(vm.state().profile == CameraProfile::FullHd30 ? CameraProfile::Hd30
                          : vm.state().profile == CameraProfile::Hd30   ? CameraProfile::Hd60
                                                                        : CameraProfile::FullHd30);
    else if (target == w.gallery || target == w.review_close)
        vm.toggle_review();
    else if (target == w.zoom_one)
        vm.set_zoom(1.0);
    else if (target == w.zoom_two)
        vm.set_zoom(2.0);
    else if (target == w.retry)
        vm.retry();
}

void CameraView::revision_changed(lv_observer_t *observer, lv_subject_t *)
{
    static_cast<CameraView *>(lv_observer_get_user_data(observer))->refresh();
}

void CameraView::apply_layout()
{
    if (!w_.dock || laying_out_) return;
    laying_out_        = true;
    const auto width   = lv_obj_get_content_width(w_.surface);
    const auto height  = lv_obj_get_content_height(w_.surface);
    const bool wide    = width > height;
    const bool compact = !wide && height < 600;
    lv_obj_set_height(w_.header, wide ? 60 : compact ? 64 : 76);
    lv_obj_set_flex_flow(w_.body, wide ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(w_.preview, LV_PCT(100));
    lv_obj_set_height(w_.preview, wide ? LV_PCT(100) : 0);
    // Landscape controls overlay the viewfinder so they do not reduce its width.
    if (wide) {
        lv_obj_add_flag(w_.dock, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_add_flag(w_.dock, LV_OBJ_FLAG_FLOATING);
        lv_obj_align(w_.dock, LV_ALIGN_RIGHT_MID, 0, 0);
    } else {
        lv_obj_remove_flag(w_.dock, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_remove_flag(w_.dock, LV_OBJ_FLAG_FLOATING);
        lv_obj_set_align(w_.dock, LV_ALIGN_DEFAULT);
    }
    lv_obj_set_style_bg_color(w_.dock, lv_color_hex(kCanvas), 0);
    lv_obj_set_style_bg_opa(w_.dock, wide ? LV_OPA_50 : LV_OPA_TRANSP, 0);
    lv_obj_set_size(w_.dock, wide ? 210 : LV_PCT(100), wide ? LV_PCT(100) : compact ? 196 : 242);
    lv_obj_set_flex_flow(w_.capture_row, wide ? LV_FLEX_FLOW_COLUMN_REVERSE : LV_FLEX_FLOW_ROW);
    lv_obj_set_height(w_.capture_row, wide ? 272 : compact ? 104 : 112);
    lv_obj_set_style_pad_all(w_.message, compact ? 16 : 24, 0);
    lv_obj_set_style_pad_row(w_.message, compact ? 10 : 14, 0);
    visible(w_.count, !wide);
    lv_obj_set_style_pad_hor(w_.review, wide ? 36 : 24, 0);
    laying_out_ = false;
    fit_preview();
}

void CameraView::fit_preview()
{
    if (!w_.stage || !w_.image || laying_out_) return;
    const int width  = lv_obj_get_content_width(w_.preview);
    const int height = lv_obj_get_content_height(w_.preview);
    if (width <= 0 || height <= 0) return;
    const auto &frame  = view_model_.frame();
    const int source_w = valid_frame(frame) ? frame.width : 640;
    const int source_h = valid_frame(frame) ? frame.height : 360;
    // Keep uniform scaling and round up LVGL's 1/256 scale so neither edge has
    // a thin gap. The stage clips excess height around the image's centre.
    const int scaled_height = (width * source_h + source_w / 2) / source_w;
    lv_obj_set_size(w_.stage, width, std::min(height, scaled_height));
    lv_obj_center(w_.stage);
    lv_image_set_scale(w_.image, (width * LV_SCALE_NONE + source_w - 1) / source_w);
}

void CameraView::refresh()
{
    if (!w_.surface) return;
    refresh_images();
    const auto &state = view_model_.state();
    // Incoming preview frames do not change buttons, labels or the saved-media
    // thumbnail. Avoid invalidating the whole LVGL page at camera frame rate.
    if (controls_initialized_ && same_controls(state, rendered_state_)) return;
    rendered_state_       = state;
    controls_initialized_ = true;
    const bool recording  = state.phase == CameraPhase::Recording;
    const bool ready      = state.phase == CameraPhase::Ready;
    const bool error      = state.phase == CameraPhase::Error;
    const bool busy       = !ready && !recording;
    const bool photo      = state.mode == CameraMode::Photo;
    selection(w_.photo_mode, photo);
    selection(w_.video_mode, !photo);
    selection(w_.zoom_one, state.zoom < 1.5);
    selection(w_.zoom_two, state.zoom >= 1.5);
    lv_obj_set_style_bg_opa(w_.grid_button, state.grid ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    disabled(w_.shutter, busy || state.reviewing);
    disabled(w_.profile_button, !ready || state.reviewing);
    lv_label_set_text(w_.profile_label, camera_profile_settings(state.profile).label);
    disabled(w_.photo_mode, !ready);
    disabled(w_.video_mode, !ready);
    disabled(w_.zoom_one, !ready);
    disabled(w_.zoom_two, !ready);
    disabled(w_.gallery, state.last_media.empty() || !ready);
    lv_obj_set_style_bg_color(w_.shutter_inner, lv_color_hex(photo ? kWhite : kRecord), 0);
    lv_obj_set_size(w_.shutter_inner, recording ? 36 : LV_PCT(100), recording ? 36 : LV_PCT(100));
    lv_obj_set_style_radius(w_.shutter_inner, recording ? 8 : LV_RADIUS_CIRCLE, 0);
    lv_obj_center(w_.shutter_inner);
    lv_label_set_text(w_.hint, error                                  ? "Connect a camera to shoot"
                               : state.phase == CameraPhase::Starting ? "Preparing camera…"
                               : recording                            ? "Tap to stop recording"
                               : photo                                ? "Tap shutter to take a photo"
                                                                      : "Tap to start recording");
    if (state.phase == CameraPhase::Capturing) lv_label_set_text(w_.hint, "Saving photo…");
    if (state.phase == CameraPhase::StartingRecording) lv_label_set_text(w_.hint, "Starting recording…");
    if (state.phase == CameraPhase::Stopping) lv_label_set_text(w_.hint, "Saving video…");
    if (state.phase == CameraPhase::Reconfiguring) lv_label_set_text(w_.hint, "Switching quality…");
    char text[80];
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u", state.elapsed_seconds / 3600, (state.elapsed_seconds / 60) % 60,
                  state.elapsed_seconds % 60);
    lv_label_set_text(w_.timer, text);
    visible(w_.timer, recording || state.phase == CameraPhase::Stopping);
    visible(w_.badge, state.simulated);
    visible(w_.zoom_row, ready || recording);
    visible(w_.message, state.phase == CameraPhase::Starting || error);
    visible(w_.retry, error);
    lv_label_set_text(w_.message_title, (error || !state.error.empty()) ? "Camera unavailable" : "Connecting camera");
    lv_label_set_text(w_.message_body,
                      state.error.empty() ? "You can shoot when the camera is ready" : state.error.c_str());
    const bool instructional = state.status.find("Tap ") == 0;
    visible(w_.status, !error && state.phase != CameraPhase::Starting && (!instructional || !state.error.empty()));
    lv_label_set_text(w_.status, state.error.empty() || error ? state.status.c_str() : state.error.c_str());
    lv_obj_set_style_text_color(w_.status, lv_color_hex(state.error.empty() ? kMuted : kYellow), 0);
    std::snprintf(text, sizeof(text), "%u\nSaved", state.saved_count);
    lv_label_set_text(w_.count, text);
    for (auto *line : w_.grid) visible(line, state.grid && (ready || recording));

    visible(w_.review, state.reviewing);
    if (state.reviewing) {
        const bool video = state.last_kind == "video";
        lv_label_set_text(w_.review_title, video ? "Video saved" : "Photo saved");
        const auto slash = state.last_media.find_last_of('/');
        lv_label_set_text(w_.review_filename,
                          state.last_media.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
        lv_label_set_text(w_.review_note, video ? "Video preview frame · Play the video in your system player"
                                                : "Capture preview frame · Full photo saved to your capture folder");
    }
}

void CameraView::refresh_images()
{
    const auto &frame        = view_model_.frame();
    const bool has_preview   = valid_frame(frame);
    const auto *preview_data = has_preview ? frame.pixels.data() : nullptr;
    const bool size_changed =
        preview_descriptor_.header.w != frame.width || preview_descriptor_.header.h != frame.height;
    if (!images_initialized_ || preview_sequence_ != frame.sequence || preview_descriptor_.data != preview_data ||
        size_changed) {
        visible(w_.image, has_preview);
        lv_image_cache_drop(&preview_descriptor_);
        if (has_preview) {
            image_descriptor(preview_descriptor_, frame);
            lv_image_set_src(w_.image, &preview_descriptor_);
            lv_obj_invalidate(w_.image);
        } else {
            lv_image_set_src(w_.image, nullptr);
            preview_descriptor_ = {};
        }
        preview_sequence_ = frame.sequence;
        if (size_changed) fit_preview();
    }

    const auto &thumbnail      = view_model_.thumbnail();
    const bool has_thumbnail   = valid_frame(thumbnail);
    const auto *thumbnail_data = has_thumbnail ? thumbnail.pixels.data() : nullptr;
    if (!images_initialized_ || thumbnail_saved_count_ != view_model_.state().saved_count ||
        thumbnail_sequence_ != thumbnail.sequence || thumbnail_descriptor_.data != thumbnail_data ||
        thumbnail_descriptor_.header.w != thumbnail.width || thumbnail_descriptor_.header.h != thumbnail.height) {
        visible(w_.thumbnail, has_thumbnail);
        visible(w_.gallery_empty, !has_thumbnail);
        lv_image_cache_drop(&thumbnail_descriptor_);
        if (has_thumbnail) {
            image_descriptor(thumbnail_descriptor_, thumbnail);
            lv_image_set_src(w_.thumbnail, &thumbnail_descriptor_);
            lv_image_set_src(w_.review_image, &thumbnail_descriptor_);
        } else {
            lv_image_set_src(w_.thumbnail, nullptr);
            lv_image_set_src(w_.review_image, nullptr);
            thumbnail_descriptor_ = {};
        }
        thumbnail_sequence_    = thumbnail.sequence;
        thumbnail_saved_count_ = view_model_.state().saved_count;
    }
    images_initialized_ = true;
}
