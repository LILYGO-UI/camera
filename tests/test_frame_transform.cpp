#include "pages/camera/frame_transform.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {

struct Image {
    int width;
    int height;
    std::vector<std::uint8_t> bytes;
};

// A deliberately separate reference: materialise each orientation operation,
// then resize it. The production path samples only the final output pixels.
Image rotate_clockwise(Image image, int rotation)
{
    for (int angle = 0; angle < rotation; angle += 90) {
        Image result{image.height, image.width, std::vector<std::uint8_t>(image.bytes.size())};
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                for (int channel = 0; channel < 3; ++channel)
                    result.bytes[(static_cast<std::size_t>(x) * result.width + image.height - y - 1) * 3 + channel] =
                        image.bytes[(static_cast<std::size_t>(y) * image.width + x) * 3 + channel];
        image = std::move(result);
    }
    return image;
}

Image reference(const camera_frame::Source &source, const camera_frame::Orientation &orientation)
{
    Image image{source.width, source.height,
                std::vector<std::uint8_t>(static_cast<std::size_t>(source.width) * source.height * 3)};
    for (int y = 0; y < source.height; ++y)
        for (int x = 0; x < source.width; ++x) {
            const int source_x = orientation.pipeline_mirrored ? source.width - 1 - x : x;
            for (int channel = 0; channel < 3; ++channel)
                image.bytes[(static_cast<std::size_t>(y) * source.width + x) * 3 + channel] =
                    source.data[static_cast<std::size_t>(y) * source.stride + source_x * 3 +
                                (source.swap_red_blue ? 2 - channel : channel)];
        }
    image = rotate_clockwise(std::move(image), (360 - orientation.pipeline_rotation) % 360);
    return rotate_clockwise(std::move(image), orientation.display_rotation);
}

std::vector<std::uint8_t> resize(const Image &image, int width, int height)
{
    std::vector<std::uint8_t> result(static_cast<std::size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const auto source_x = static_cast<std::int64_t>(x) * image.width / width;
            const auto source_y = static_cast<std::int64_t>(y) * image.height / height;
            std::copy_n(image.bytes.data() + (source_y * image.width + source_x) * 3, 3,
                        result.data() + (static_cast<std::size_t>(y) * width + x) * 3);
        }
    return result;
}

std::vector<std::uint8_t> pixels(int width, int height, std::size_t stride)
{
    // The last row intentionally omits padding, just like valid DMA metadata.
    std::vector<std::uint8_t> result(stride * (height - 1) + static_cast<std::size_t>(width) * 3, 0xcd);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            auto *pixel = result.data() + static_cast<std::size_t>(y) * stride + x * 3;
            pixel[0]    = static_cast<std::uint8_t>(x * 17 + y * 19);
            pixel[1]    = static_cast<std::uint8_t>(x * 23 + y * 31 + 83);
            pixel[2]    = static_cast<std::uint8_t>(x * 41 + y * 43 + 157);
        }
    return result;
}

void check_all_orientations()
{
    constexpr int rotations[]{0, 90, 180, 270};
    for (const auto &dimensions : {std::pair<int, int>{7, 4}, {1, 4}, {7, 1}, {1, 1}})
        for (const int padding : {0, 11}) {
            const std::size_t stride = static_cast<std::size_t>(dimensions.first) * 3 + padding;
            const auto bytes         = pixels(dimensions.first, dimensions.second, stride);
            for (const bool rgb : {false, true})
                for (const bool mirror : {false, true})
                    for (const int pipeline : rotations)
                        for (const int display : rotations) {
                            const camera_frame::Source source{bytes.data(),      bytes.size(), dimensions.first,
                                                              dimensions.second, stride,       rgb};
                            const camera_frame::Orientation orientation{pipeline, mirror, display};
                            const auto expected = reference(source, orientation);
                            int width = 0, height = 0;
                            assert(camera_frame::output_size(source, orientation, width, height));
                            assert(width == expected.width && height == expected.height);
                            std::vector<std::uint8_t> output;
                            assert(camera_frame::transform_bgr(source, orientation, width, height, output));
                            assert(output == expected.bytes);
                            assert(camera_frame::transform_bgr(source, orientation, 2, 3, output));
                            assert(output == resize(expected, 2, 3));
                            assert(camera_frame::transform_bgr(source, orientation, 11, 9, output));
                            assert(output == resize(expected, 11, 9));
                        }
        }
}

void check_preview_and_export()
{
    const auto bytes = pixels(1920, 1080, 1920 * 3 + 32);
    const camera_frame::Source source{bytes.data(), bytes.size(), 1920, 1080, 1920 * 3 + 32, false};
    for (const camera_frame::Orientation orientation :
         {camera_frame::Orientation{0, false, 0}, {90, false, 270}, {270, true, 0}, {180, true, 90}}) {
        const auto expected      = reference(source, orientation);
        const int preview_width  = expected.width > expected.height ? 640 : 360;
        const int preview_height = expected.width > expected.height ? 360 : 640;
        std::vector<std::uint8_t> output;
        assert(camera_frame::transform_bgr(source, orientation, preview_width, preview_height, output));
        assert(output == resize(expected, preview_width, preview_height));
        assert(camera_frame::transform_bgr(source, orientation, expected.width, expected.height, output));
        assert(output == expected.bytes);
        const auto *allocation = output.data();
        assert(camera_frame::transform_bgr(source, orientation, preview_width, preview_height, output));
        assert(output.data() == allocation);
        assert(camera_frame::transform_bgr(source, orientation, expected.width, expected.height, output));
        assert(output.data() == allocation && output == expected.bytes);
    }
}

void check_invalid_inputs()
{
    const auto bytes = pixels(7, 4, 24);
    const camera_frame::Source valid{bytes.data(), bytes.size(), 7, 4, 24, false};
    const std::vector<std::uint8_t> untouched{1, 2, 3};
    std::vector<std::uint8_t> output = untouched;
    const auto rejects = [&](camera_frame::Source source, camera_frame::Orientation orientation = {}, int width = 7,
                             int height = 4) {
        assert(!camera_frame::transform_bgr(source, orientation, width, height, output));
        assert(output == untouched);
    };
    auto source = valid;
    source.data = nullptr;
    rejects(source);
    source = valid;
    source.size -= 1;
    rejects(source);
    source       = valid;
    source.width = -1;
    rejects(source);
    source        = valid;
    source.height = 0;
    rejects(source);
    source        = valid;
    source.stride = 20;
    rejects(source);
    source.stride = std::numeric_limits<std::size_t>::max();
    rejects(source);
    source.stride = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    rejects(source);
    rejects(valid, {45, false, 0});
    rejects(valid, {0, false, -90});
    rejects(valid, {}, 0, 4);
    rejects(valid, {}, 7, -1);
    rejects(valid, {}, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
    source = {output.data(), output.size(), 1, 1, 3, false};
    rejects(source);

    // Stride does not imply padding after the final (or only) row.
    source = {bytes.data(), 3, 1, 1, static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()), false};
    assert(camera_frame::transform_bgr(source, {90, true, 0}, 2, 3, output));
    for (std::size_t index = 0; index < output.size(); ++index) assert(output[index] == bytes[index % 3]);
}

std::vector<std::uint8_t> nv_pixels(int width, int height, std::size_t stride, bool nv12)
{
    const auto chroma_base = stride * height;
    const int chroma_rows = nv12 ? height / 2 : height;
    // Include the complete luma plane but omit the last chroma row's padding.
    std::vector<std::uint8_t> result(chroma_base + stride * (chroma_rows - 1) + width, 0xcd);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            result[static_cast<std::size_t>(y) * stride + x] = static_cast<std::uint8_t>(16 + (x * 17 + y * 19) % 220);
    for (int y = 0; y < chroma_rows; ++y)
        for (int x = 0; x < width; x += 2) {
            const auto at = chroma_base + static_cast<std::size_t>(y) * stride + x;
            result[at] = static_cast<std::uint8_t>(32 + (x * 13 + y * 29) % 192);
            result[at + 1] = static_cast<std::uint8_t>(32 + (x * 31 + y * 11) % 192);
        }
    return result;
}

// Keep the former two-stage behavior as an independent mapping reference:
// materialise every cropped NV pixel, rotate that BGR image, then resize it.
Image reference_nv(const camera_frame::NvSource &source, int rotation, int zoom)
{
    int width = std::max(2, source.width / zoom);
    int height = std::max(2, source.height / zoom);
    const int left = ((source.width - width) / 2) & ~1;
    int top = (source.height - height) / 2;
    width &= ~1;
    if (source.nv12) top &= ~1, height &= ~1;
    Image image{width, height, std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 3)};
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const int source_x = left + x, source_y = top + y;
            const auto uv = static_cast<std::size_t>(source.height) * source.stride +
                            static_cast<std::size_t>(source.nv12 ? source_y / 2 : source_y) * source.stride +
                            (source_x / 2) * 2;
            const int yy = source.data[static_cast<std::size_t>(source_y) * source.stride + source_x] - 16;
            const int u = source.data[uv] - 128, v = source.data[uv + 1] - 128;
            const int channels[]{(298 * yy + 516 * u + 128) >> 8,
                                 (298 * yy - 100 * u - 208 * v + 128) >> 8,
                                 (298 * yy + 409 * v + 128) >> 8};
            for (int channel = 0; channel < 3; ++channel)
                image.bytes[(static_cast<std::size_t>(y) * width + x) * 3 + channel] =
                    static_cast<std::uint8_t>(std::clamp(channels[channel], 0, 255));
        }
    return rotate_clockwise(std::move(image), rotation);
}

void check_nv_colors_and_chroma_rows()
{
    // Known limited-range black, white, red, green and blue Y/U/V triplets.
    constexpr int colors[][6]{{16, 128, 128, 0, 0, 0}, {235, 128, 128, 255, 255, 255},
                              {81, 90, 240, 0, 0, 255}, {145, 54, 34, 1, 255, 0},
                              {41, 240, 110, 255, 0, 0}};
    for (const bool nv12 : {false, true}) {
        constexpr int width = 10, height = 4, stride = 14;
        auto bytes = nv_pixels(width, height, stride, nv12);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                const int chroma_y = nv12 ? y / 2 : y;
                const auto &color = colors[(x / 2 + chroma_y) % 5];
                bytes[y * stride + x] = color[0];
                bytes[height * stride + chroma_y * stride + (x & ~1)] = color[1];
                bytes[height * stride + chroma_y * stride + (x & ~1) + 1] = color[2];
            }
        const camera_frame::NvSource source{bytes.data(), bytes.size(), width, height, stride, nv12};
        std::vector<std::uint8_t> output;
        assert(camera_frame::transform_nv_to_bgr(source, 0, 1, width, height, output));
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                for (int channel = 0; channel < 3; ++channel)
                    assert(output[(y * width + x) * 3 + channel] ==
                           colors[(x / 2 + (nv12 ? y / 2 : y)) % 5][channel + 3]);
    }
}

void check_nv_orientation_crop_and_resize()
{
    for (const bool nv12 : {false, true})
        for (const auto &dimensions : {std::pair<int, int>{8, 6}, {10, 6}, {2, 2}, {6, 5}}) {
            if (nv12 && dimensions.second % 2) continue;
            for (const int padding : {0, 7}) {
                const std::size_t stride = dimensions.first + padding;
                const auto bytes = nv_pixels(dimensions.first, dimensions.second, stride, nv12);
                const camera_frame::NvSource source{bytes.data(), bytes.size(), dimensions.first,
                                                     dimensions.second, stride, nv12};
                for (const int zoom : {1, 2})
                    for (const int rotation : {0, 90, 180, 270}) {
                        const auto expected = reference_nv(source, rotation, zoom);
                        std::vector<std::uint8_t> output;
                        assert(camera_frame::transform_nv_to_bgr(source, rotation, zoom, expected.width,
                                                                 expected.height, output));
                        assert(output == expected.bytes);
                        for (const auto &size : {std::pair<int, int>{1, 1}, {3, 5}, {11, 9}}) {
                            assert(camera_frame::transform_nv_to_bgr(source, rotation, zoom, size.first,
                                                                     size.second, output));
                            assert(output == resize(expected, size.first, size.second));
                        }
                    }
            }
        }

    // Exercise fixed 1080P capture with portrait preview/export sizes and reuse
    // the output allocation when switching between those smaller requests.
    for (const bool nv12 : {false, true}) {
        const auto bytes = nv_pixels(1920, 1080, 1952, nv12);
        const camera_frame::NvSource source{bytes.data(), bytes.size(), 1920, 1080, 1952, nv12};
        const auto expected = reference_nv(source, 90, 2);
        std::vector<std::uint8_t> output;
        assert(camera_frame::transform_nv_to_bgr(source, 90, 2, 720, 1280, output));
        assert(output == resize(expected, 720, 1280));
        const auto *allocation = output.data();
        assert(camera_frame::transform_nv_to_bgr(source, 90, 2, 360, 640, output));
        assert(output == resize(expected, 360, 640) && output.data() == allocation);
    }
}

void check_nv_invalid_inputs()
{
    for (const bool nv12 : {false, true}) {
        const auto bytes = nv_pixels(8, 6, 11, nv12);
        const camera_frame::NvSource valid{bytes.data(), bytes.size(), 8, 6, 11, nv12};
        const std::vector<std::uint8_t> untouched{1, 2, 3};
        std::vector<std::uint8_t> output = untouched;
        const auto rejects = [&](camera_frame::NvSource source, int rotation = 0, int zoom = 1,
                                 int width = 8, int height = 6) {
            assert(!camera_frame::transform_nv_to_bgr(source, rotation, zoom, width, height, output));
            assert(output == untouched);
        };
        auto source = valid;
        source.data = nullptr;
        rejects(source);
        source = valid;
        source.size--;
        rejects(source);
        source = valid;
        source.size = source.stride * source.height; // Luma alone is not a complete NV frame.
        rejects(source);
        for (const int width : {-1, 0, 1, 7}) {
            source = valid;
            source.width = width;
            rejects(source);
        }
        for (const int height : {-1, 0, 1}) {
            source = valid;
            source.height = height;
            rejects(source);
        }
        if (nv12) {
            source = valid;
            source.height = 5;
            rejects(source);
        }
        source = valid;
        source.stride = 7;
        rejects(source);
        source.stride = std::numeric_limits<std::size_t>::max();
        rejects(source);
        source.stride = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / 2;
        rejects(source);
        rejects(valid, 45);
        rejects(valid, -90);
        rejects(valid, 0, 0);
        rejects(valid, 0, 3);
        rejects(valid, 0, 1, 0);
        rejects(valid, 0, 1, 8, -1);
        rejects(valid, 0, 1, std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
    }
    std::vector<std::uint8_t> alias(64, 0x80);
    const auto untouched = alias;
    for (const int offset : {0, 2}) {
        const camera_frame::NvSource source{alias.data() + offset, alias.size() - offset, 4, 4, 4, false};
        assert(!camera_frame::transform_nv_to_bgr(source, 90, 2, 8, 8, alias));
        assert(alias == untouched);
    }
}

}  // namespace

int main()
{
    check_all_orientations();
    check_preview_and_export();
    check_invalid_inputs();
    check_nv_colors_and_chroma_rows();
    check_nv_orientation_crop_and_resize();
    check_nv_invalid_inputs();
    std::cout << "fused BGR/NV frame transform colors, orientation, crop, stride, preview and bounds tests passed\n";
}
