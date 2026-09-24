#include "pages/camera/camera_view.hpp"

#include <cm0/typography.h>
#include <src/misc/cache/instance/lv_image_cache.h>
#include <algorithm>
#include <cstdio>
#include <string>

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
    w_.surface = container(parent, "camera_surface");
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
    auto *title = label(w_.header, "相机", 22);
    lv_obj_set_style_text_letter_space(title, 3, 0);
    auto *format = label(w_.header, "1080P · 30", 14);
    lv_obj_set_width(format, 92);
    lv_obj_set_style_text_align(format, LV_TEXT_ALIGN_RIGHT, 0);

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
    lv_obj_set_size(w_.image, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(w_.image, LV_IMAGE_ALIGN_CONTAIN);
    for (int i = 0; i < 4; ++i) {
        w_.grid[i] = container(w_.stage, nullptr);
        lv_obj_set_style_bg_color(w_.grid[i], lv_color_hex(kWhite), 0);
        lv_obj_set_style_bg_opa(w_.grid[i], LV_OPA_40, 0);
        lv_obj_set_size(w_.grid[i], i < 2 ? 1 : LV_PCT(100), i < 2 ? LV_PCT(100) : 1);
        lv_obj_set_pos(w_.grid[i], i < 2 ? LV_PCT((i + 1) * 33) : 0, i >= 2 ? LV_PCT((i - 1) * 33) : 0);
    }
    w_.badge = label(w_.preview, "模拟预览", 14, kYellow);
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
    w_.message_title = label(w_.message, "正在连接相机", 28);
    w_.message_body  = label(w_.message, "准备好后即可拍摄", 14, kMuted);
    lv_obj_set_width(w_.message_body, LV_PCT(100));
    lv_label_set_long_mode(w_.message_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(w_.message_body, LV_TEXT_ALIGN_CENTER, 0);
    w_.retry = button(w_.message, "camera_retry", "重试", action_event, this);
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
    w_.video_mode  = button(w_.mode_row, "camera_mode_video", "视频", action_event, this);
    w_.photo_mode  = button(w_.mode_row, "camera_mode_photo", "照片", action_event, this);
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
    w_.count = label(w_.capture_row, "0\n已保存", 14, kMuted);
    lv_obj_set_name_static(w_.count, "camera_saved_count");
    lv_obj_set_size(w_.count, 58, LV_SIZE_CONTENT);
    lv_obj_set_style_text_align(w_.count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(w_.count, 6, 0);
    w_.hint = label(w_.dock, "轻触快门拍照", 14, kMuted);

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
    w_.review_title = label(w_.review_header, "最近拍摄", 22);
    w_.review_close = button(w_.review_header, "camera_review_close", "完成", action_event, this);
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
    lv_obj_set_width(w_.preview, wide ? 0 : LV_PCT(100));
    lv_obj_set_height(w_.preview, wide ? LV_PCT(100) : 0);
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
    if (!w_.stage || laying_out_) return;
    const int width  = lv_obj_get_content_width(w_.preview);
    const int height = lv_obj_get_content_height(w_.preview);
    if (width <= 0 || height <= 0) return;
    const auto &frame   = view_model_.frame();
    const int source_w  = valid_frame(frame) ? frame.width : 640;
    const int source_h  = valid_frame(frame) ? frame.height : 360;
    const double factor = std::min(static_cast<double>(width) / source_w, static_cast<double>(height) / source_h);
    lv_obj_set_size(w_.stage, static_cast<int>(source_w * factor), static_cast<int>(source_h * factor));
    lv_obj_center(w_.stage);
}

void CameraView::refresh()
{
    if (!w_.surface) return;
    const auto &state    = view_model_.state();
    const bool recording = state.phase == CameraPhase::Recording;
    const bool ready     = state.phase == CameraPhase::Ready;
    const bool error     = state.phase == CameraPhase::Error;
    const bool busy      = !ready && !recording;
    const bool photo     = state.mode == CameraMode::Photo;
    selection(w_.photo_mode, photo);
    selection(w_.video_mode, !photo);
    selection(w_.zoom_one, state.zoom < 1.5);
    selection(w_.zoom_two, state.zoom >= 1.5);
    lv_obj_set_style_bg_opa(w_.grid_button, state.grid ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    disabled(w_.shutter, busy || state.reviewing);
    disabled(w_.photo_mode, !ready);
    disabled(w_.video_mode, !ready);
    disabled(w_.zoom_one, !ready);
    disabled(w_.zoom_two, !ready);
    disabled(w_.gallery, state.last_media.empty() || !ready);
    lv_obj_set_style_bg_color(w_.shutter_inner, lv_color_hex(photo ? kWhite : kRecord), 0);
    lv_obj_set_size(w_.shutter_inner, recording ? 36 : LV_PCT(100), recording ? 36 : LV_PCT(100));
    lv_obj_set_style_radius(w_.shutter_inner, recording ? 8 : LV_RADIUS_CIRCLE, 0);
    lv_obj_center(w_.shutter_inner);
    lv_label_set_text(w_.hint, error                                  ? "连接相机后即可拍摄"
                               : state.phase == CameraPhase::Starting ? "相机准备中…"
                               : recording                            ? "轻触停止录像"
                               : photo                                ? "轻触快门拍照"
                                                                      : "轻触开始录像");
    if (state.phase == CameraPhase::Capturing) lv_label_set_text(w_.hint, "正在保存照片…");
    if (state.phase == CameraPhase::StartingRecording) lv_label_set_text(w_.hint, "正在开始录像…");
    if (state.phase == CameraPhase::Stopping) lv_label_set_text(w_.hint, "正在保存视频…");
    char text[80];
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u", state.elapsed_seconds / 3600, (state.elapsed_seconds / 60) % 60,
                  state.elapsed_seconds % 60);
    lv_label_set_text(w_.timer, text);
    visible(w_.timer, recording || state.phase == CameraPhase::Stopping);
    visible(w_.badge, state.simulated);
    visible(w_.zoom_row, ready || recording);
    visible(w_.message, state.phase == CameraPhase::Starting || error);
    visible(w_.retry, error);
    lv_label_set_text(w_.message_title, (error || !state.error.empty()) ? "暂时无法拍摄" : "正在连接相机");
    lv_label_set_text(w_.message_body, state.error.empty() ? "准备好后即可拍摄" : state.error.c_str());
    const bool instructional = state.status.find("轻点") == 0;
    visible(w_.status, !error && state.phase != CameraPhase::Starting && (!instructional || !state.error.empty()));
    lv_label_set_text(w_.status, state.error.empty() || error ? state.status.c_str() : state.error.c_str());
    lv_obj_set_style_text_color(w_.status, lv_color_hex(state.error.empty() ? kMuted : kYellow), 0);
    std::snprintf(text, sizeof(text), "%u\n已保存", state.saved_count);
    lv_label_set_text(w_.count, text);
    for (auto *line : w_.grid) visible(line, state.grid && (ready || recording));

    const auto &frame = view_model_.frame();
    visible(w_.image, valid_frame(frame));
    if (valid_frame(frame)) {
        lv_image_cache_drop(&preview_descriptor_);
        image_descriptor(preview_descriptor_, frame);
        lv_image_set_src(w_.image, &preview_descriptor_);
        lv_obj_invalidate(w_.image);
        fit_preview();
    }
    const auto &thumbnail = view_model_.thumbnail();
    visible(w_.thumbnail, valid_frame(thumbnail));
    visible(w_.gallery_empty, !valid_frame(thumbnail));
    if (valid_frame(thumbnail)) {
        lv_image_cache_drop(&thumbnail_descriptor_);
        image_descriptor(thumbnail_descriptor_, thumbnail);
        lv_image_set_src(w_.thumbnail, &thumbnail_descriptor_);
        lv_image_set_src(w_.review_image, &thumbnail_descriptor_);
    }
    visible(w_.review, state.reviewing);
    if (state.reviewing) {
        const bool video = state.last_kind == "video";
        lv_label_set_text(w_.review_title, video ? "视频已保存" : "照片已保存");
        const auto slash = state.last_media.find_last_of('/');
        lv_label_set_text(w_.review_filename,
                          state.last_media.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
        lv_label_set_text(w_.review_note,
                          video ? "录像预览帧 · 请在系统播放器中播放视频" : "拍摄时的预览帧 · 原图已保存到相册目录");
    }
}
