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

}  // namespace

int main()
{
    check_all_orientations();
    check_preview_and_export();
    check_invalid_inputs();
    std::cout << "fused frame transform orientation, stride, preview and bounds tests passed\n";
}
