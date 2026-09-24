#include "pages/camera/camera_view.hpp"
#include <cm0/typography.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr int kPartialRows = 80;
std::vector<lv_color32_t> framebuffer;
int display_width   = 0;
lv_indev_t *pointer = nullptr;
lv_point_t pointer_position{};
bool pointer_pressed = false;

void read_pointer(lv_indev_t *, lv_indev_data_t *data)
{
    data->point = pointer_position;
    data->state = pointer_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void flush_display(lv_display_t *display, const lv_area_t *area, std::uint8_t *pixels)
{
    const int width    = area->x2 - area->x1 + 1;
    const auto *source = reinterpret_cast<const lv_color32_t *>(pixels);
    for (int y = area->y1; y <= area->y2; ++y) {
        const auto source_offset      = static_cast<std::size_t>(y - area->y1) * width;
        const auto destination_offset = static_cast<std::size_t>(y) * display_width + area->x1;
        std::copy_n(source + source_offset, width, framebuffer.begin() + destination_offset);
    }
    lv_display_flush_ready(display);
}

bool write_ppm(const char *path, int width, int height)
{
    std::ofstream file(path, std::ios::binary);
    if (!file) return false;
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (const auto &pixel : framebuffer) {
        file.put(static_cast<char>(pixel.red));
        file.put(static_cast<char>(pixel.green));
        file.put(static_cast<char>(pixel.blue));
    }
    return file.good();
}

// Deterministic synthetic landscape; camera simulation is explicitly labelled.
CameraFrame sample_frame()
{
    CameraFrame frame;
    frame.width    = 640;
    frame.height   = 360;
    frame.sequence = 1;
    frame.pixels.resize(640 * 360 * 3);
    for (int y = 0; y < 360; ++y) {
        for (int x = 0; x < 640; ++x) {
            const double sky = std::min(1.0, y / 230.0);
            int r            = static_cast<int>(108 + sky * 128);
            int g            = static_cast<int>(155 + sky * 45);
            int b            = static_cast<int>(179 - sky * 23);
            if ((x - 454) * (x - 454) + (y - 96) * (y - 96) < 25 * 25) {
                r = 255;
                g = 225;
                b = 177;
            }
            const double back  = 174 + 25 * std::sin(x / 68.0) - 19 * std::sin(x / 26.0);
            const double front = 214 + 28 * std::sin(x / 81.0 + 1.0) - 8 * std::cos(x / 30.0);
            if (y > back) {
                r = 107;
                g = 126;
                b = 133;
            }
            if (y > front) {
                r = 54;
                g = 86;
                b = 99;
            }
            if (y > 251) {
                const double water = (y - 251) / 109.0;
                const int ripple   = static_cast<int>(4 * std::sin(x / 13.0 + y * 1.8));
                r                  = static_cast<int>(117 - 43 * water) + ripple;
                g                  = static_cast<int>(157 - 35 * water) + ripple;
                b                  = static_cast<int>(166 - 29 * water) + ripple;
            }
            // A small sailboat provides a recognizable subject and detail.
            if (y >= 192 && y < 282 && x >= 315 && x <= 315 + (y - 192) / 2) {
                r = 244;
                g = 234;
                b = 210;
            }
            if (y > 209 && y < 282 && x < 312 && x > 312 - (y - 209) / 3) {
                r = 219;
                g = 210;
                b = 192;
            }
            if (x >= 312 && x <= 315 && y >= 187 && y <= 292) {
                r = 56;
                g = 53;
                b = 48;
            }
            if (y >= 289 && y <= 296 && x > 285 + (y - 289) * 2 && x < 360 - (y - 289) * 2) {
                r = 45;
                g = 56;
                b = 61;
            }
            auto offset              = static_cast<std::size_t>(y * 640 + x) * 3;
            frame.pixels[offset]     = static_cast<std::uint8_t>(b);
            frame.pixels[offset + 1] = static_cast<std::uint8_t>(g);
            frame.pixels[offset + 2] = static_cast<std::uint8_t>(r);
        }
    }
    return frame;
}

lv_obj_t *find(lv_obj_t *parent, const char *name)
{
    auto *object = lv_obj_find_by_name(parent, name);
    assert(object);
    return object;
}

void click(lv_obj_t *parent, const char *name)
{
    lv_obj_update_layout(parent);
    lv_area_t area;
    lv_obj_get_coords(find(parent, name), &area);
    pointer_position = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pointer_pressed  = true;
    lv_tick_inc(25);
    lv_indev_read(pointer);
    pointer_pressed = false;
    lv_tick_inc(25);
    lv_indev_read(pointer);
}

void inside(lv_obj_t *object, int width, int height)
{
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    assert(area.x1 >= 0 && area.y1 >= 0 && area.x2 < width && area.y2 < height);
}

std::uint64_t now_ms()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
}  // namespace

int main(int argc, char **argv)
{
    assert(argc == 4 || argc == 5);
    const int width           = std::atoi(argv[1]);
    const int height          = std::atoi(argv[2]);
    const std::string variant = argc == 5 ? argv[4] : "photo";
    assert(width >= 320 && height >= 240);
    display_width = width;
    framebuffer.resize(static_cast<std::size_t>(width) * height);
    std::vector<lv_color32_t> draw_buffer(static_cast<std::size_t>(width) * kPartialRows);
    lv_init();
    auto *display = lv_display_create(width, height);
    assert(display);
    lv_display_set_buffers(display, draw_buffer.data(), nullptr,
                           static_cast<std::uint32_t>(draw_buffer.size() * sizeof(lv_color32_t)),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_display);
    lv_display_set_default(display);
    pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(pointer, display);
    lv_indev_set_read_cb(pointer, read_pointer);

    {
        CameraViewModel vm(false);
        CameraView view(vm);
        auto *parent = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(parent);
        lv_obj_set_size(parent, LV_PCT(100), LV_PCT(100));
        auto *surface = view.create(parent);
        lv_obj_update_layout(parent);
        auto *shutter = find(surface, "camera_shutter");
        assert(lv_obj_has_state(shutter, LV_STATE_DISABLED));
        click(surface, "camera_shutter");
        assert(vm.state().phase == CameraPhase::Starting);
        CameraEvent ready;
        ready.event     = "ready";
        ready.simulated = true;
        vm.receive(ready, now_ms());
        vm.set_frame(sample_frame());
        assert(!lv_obj_has_state(shutter, LV_STATE_DISABLED));
        assert(!lv_obj_has_flag(find(surface, "camera_simulator_badge"), LV_OBJ_FLAG_HIDDEN));
        assert(lv_color_eq(lv_obj_get_style_bg_color(surface, LV_PART_MAIN), lv_color_hex(0x000000)));
        click(surface, "camera_grid");
        assert(vm.state().grid);
        click(surface, "camera_zoom_two");
        assert(vm.state().zoom == 2.0);
        click(surface, "camera_zoom_one");
        assert(vm.state().zoom == 1.0);
        click(surface, "camera_shutter");
        assert(vm.state().phase == CameraPhase::Capturing);
        assert(lv_obj_has_state(shutter, LV_STATE_DISABLED));
        CameraEvent media;
        media.event = "media";
        media.kind  = "photo";
        media.path  = "/Pictures/Camera/IMG_20260922_164208.jpg";
        vm.receive(media, now_ms());
        CameraEvent idle;
        idle.event = "state";
        idle.state = "ready";
        vm.receive(idle, now_ms());
        assert(vm.state().saved_count == 1);
        click(surface, "camera_gallery");
        assert(vm.state().reviewing);
        assert(!lv_obj_has_flag(find(surface, "camera_review"), LV_OBJ_FLAG_HIDDEN));
        click(surface, "camera_review_close");
        assert(!vm.state().reviewing);
        std::uint32_t key = LV_KEY_LEFT;
        lv_obj_send_event(surface, LV_EVENT_KEY, &key);
        assert(vm.state().mode == CameraMode::Video);
        key = LV_KEY_RIGHT;
        lv_obj_send_event(surface, LV_EVENT_KEY, &key);
        assert(vm.state().mode == CameraMode::Photo);
        click(surface, "camera_mode_video");
        assert(vm.state().mode == CameraMode::Video);
        click(surface, "camera_shutter");
        assert(vm.state().phase == CameraPhase::StartingRecording);
        CameraEvent recording;
        recording.event = "state";
        recording.state = "recording";
        vm.receive(recording, now_ms() - 65000);
        vm.poll();
        assert(vm.state().phase == CameraPhase::Recording);
        assert(std::string(lv_label_get_text(find(surface, "camera_timer"))) == "00:01:05");
        click(surface, "camera_mode_photo");
        assert(vm.state().mode == CameraMode::Video);
        click(surface, "camera_shutter");
        assert(vm.state().phase == CameraPhase::Stopping);
        assert(lv_obj_has_state(shutter, LV_STATE_DISABLED));
        media.kind = "video";
        media.path = "/Pictures/Camera/VID_20260922_164245.mp4";
        vm.receive(media, now_ms());
        vm.receive(idle, now_ms());
        assert(vm.state().saved_count == 2);

        // Reflow this exact page twice, retaining mode, count, and grid state.
        lv_obj_set_size(parent, height, width);
        lv_obj_update_layout(parent);
        lv_obj_set_size(parent, width, height);
        lv_obj_update_layout(parent);
        assert(vm.state().mode == CameraMode::Video && vm.state().saved_count == 2 && vm.state().grid);
        for (const char *name : {"camera_shutter", "camera_gallery", "camera_modes", "camera_grid", "camera_zoom"})
            inside(find(surface, name), width, height);
        auto *stage = find(surface, "camera_image_stage");
        assert(std::abs(lv_obj_get_width(stage) * 9 - lv_obj_get_height(stage) * 16) <= 16);

        CameraEvent failure;
        failure.event   = "error";
        failure.fatal   = true;
        failure.message = "未找到可用相机，请检查连接后重试。";
        vm.receive(failure, now_ms());
        assert(!lv_obj_has_flag(find(surface, "camera_message"), LV_OBJ_FLAG_HIDDEN));
        click(surface, "camera_retry");
        assert(vm.state().phase == CameraPhase::Starting);
        vm.receive(ready, now_ms());
        vm.set_frame(sample_frame());
        click(surface, "camera_grid");
        click(surface, "camera_mode_photo");
        if (variant == "recording") {
            click(surface, "camera_mode_video");
            click(surface, "camera_shutter");
            vm.receive(recording, now_ms() - 65000);
            vm.poll();
        } else if (variant == "error") {
            vm.receive(failure, now_ms());
        } else if (variant == "review") {
            click(surface, "camera_gallery");
        }
        lv_obj_update_layout(parent);
        lv_tick_inc(20);
        lv_refr_now(display);
        const bool has_contrast = std::any_of(framebuffer.cbegin() + 1, framebuffer.cend(), [](const auto &pixel) {
            return pixel.red != framebuffer[0].red || pixel.green != framebuffer[0].green ||
                   pixel.blue != framebuffer[0].blue;
        });
        assert(has_contrast);
        assert(write_ppm(argv[3], width, height));
        // Parent-driven teardown must detach the Subject observer too.
        lv_obj_delete(parent);
        vm.toggle_grid();
        view.destroy();
        view.destroy();
    }
    lv_indev_delete(pointer);
    pointer = nullptr;
    lv_display_delete(display);
    lilygo_ui_fonts_deinit();
    lv_deinit();
    return 0;
}
