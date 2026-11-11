#include "pages/camera/frame_transform.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace camera_frame {
namespace {

bool valid_rotation(int rotation)
{
    return rotation == 0 || rotation == 90 || rotation == 180 || rotation == 270;
}

struct Point {
    int x;
    int y;
};

Point rotate(Point point, int width, int height, int rotation)
{
    switch (rotation) {
        case 90:
            return {height - 1 - point.y, point.x};
        case 180:
            return {width - 1 - point.x, height - 1 - point.y};
        case 270:
            return {point.y, width - 1 - point.x};
        default:
            return point;
    }
}

Point source_point(Point point, int display_width, int display_height, int upright_width, int upright_height,
                   int source_width, const Orientation &orientation)
{
    point = rotate(point, display_width, display_height, (360 - orientation.display_rotation) % 360);
    point = rotate(point, upright_width, upright_height, orientation.pipeline_rotation);
    if (orientation.pipeline_mirrored) point.x = source_width - 1 - point.x;
    return point;
}

}  // namespace

bool output_size(const Source &source, const Orientation &orientation, int &width, int &height)
{
    if (source.width <= 0 || source.height <= 0 || !valid_rotation(orientation.pipeline_rotation) ||
        !valid_rotation(orientation.display_rotation))
        return false;
    const bool swapped = (orientation.pipeline_rotation % 180 != 0) != (orientation.display_rotation % 180 != 0);
    width              = swapped ? source.height : source.width;
    height             = swapped ? source.width : source.height;
    return true;
}

bool transform_bgr(const Source &source, const Orientation &orientation, int output_width, int output_height,
                   std::vector<std::uint8_t> &output)
{
    int display_width, display_height;
    if (!output_size(source, orientation, display_width, display_height) || !source.data || output_width <= 0 ||
        output_height <= 0)
        return false;

    // All sampled offsets fit ptrdiff_t so signed, negative rotation steps are
    // safe as well as the usual tightly packed and padded camera buffers.
    constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    if (static_cast<std::size_t>(source.width) > limit / 3 ||
        static_cast<std::size_t>(output_width) > output.max_size() / 3)
        return false;
    const auto row_bytes = static_cast<std::size_t>(source.width) * 3;
    if (row_bytes > limit || source.stride < row_bytes || source.stride > limit ||
        static_cast<std::size_t>(source.height - 1) > (limit - row_bytes) / source.stride)
        return false;
    const auto required = static_cast<std::size_t>(source.height - 1) * source.stride + row_bytes;
    if (source.size < required) return false;
    const auto output_row_bytes = static_cast<std::size_t>(output_width) * 3;
    if (output_row_bytes > output.max_size() ||
        static_cast<std::size_t>(output_height) > output.max_size() / output_row_bytes)
        return false;

    // Reject aliasing before resize can invalidate source.data.
    if (output.capacity() != 0) {
        const auto source_address = reinterpret_cast<std::uintptr_t>(source.data);
        const auto output_address = reinterpret_cast<std::uintptr_t>(output.data());
        if ((source_address >= output_address && source_address - output_address < output.capacity()) ||
            (output_address >= source_address && output_address - source_address < required))
            return false;
    }

    const int upright_width  = orientation.pipeline_rotation % 180 ? source.height : source.width;
    const int upright_height = orientation.pipeline_rotation % 180 ? source.width : source.height;
    const auto map_point     = [&](Point point) {
        return source_point(point, display_width, display_height, upright_width, upright_height, source.width,
                                orientation);
    };
    const Point origin = map_point({0, 0});
    const Point next_x = map_point({1, 0});
    const Point next_y = map_point({0, 1});
    const Point step_x{next_x.x - origin.x, next_x.y - origin.y};
    const Point step_y{next_y.x - origin.x, next_y.y - origin.y};
    output.resize(output_row_bytes * static_cast<std::size_t>(output_height));

    if (output_width == source.width && output_height == source.height && origin.x == 0 && origin.y == 0 &&
        step_x.x == 1 && step_x.y == 0 && step_y.x == 0 && step_y.y == 1 && !source.swap_red_blue) {
        if (source.stride == row_bytes) {
            std::memcpy(output.data(), source.data, output.size());
        } else {
            for (int y = 0; y < source.height; ++y)
                std::memcpy(output.data() + static_cast<std::size_t>(y) * row_bytes,
                            source.data + static_cast<std::size_t>(y) * source.stride, row_bytes);
        }
        return true;
    }

    const auto stride          = static_cast<std::ptrdiff_t>(source.stride);
    const auto pixel_step      = step_x.x * std::ptrdiff_t{3} + step_x.y * stride;
    const int sample_step      = display_width / output_width;
    const int sample_remainder = display_width % output_width;
    // Only needed if a row has a second pixel. This also avoids an overflowing
    // one-past-image step for a one-pixel output from an enormous source.
    const auto base_step = output_width > 1 ? pixel_step * sample_step : 0;
    const int blue       = source.swap_red_blue ? 2 : 0;
    const int red        = source.swap_red_blue ? 0 : 2;
    for (int y = 0; y < output_height; ++y) {
        const int sample_y = static_cast<int>(static_cast<std::int64_t>(y) * display_height / output_height);
        const int source_x = origin.x + step_y.x * sample_y;
        const int source_y = origin.y + step_y.y * sample_y;
        auto offset        = static_cast<std::ptrdiff_t>(source_y) * stride + static_cast<std::ptrdiff_t>(source_x) * 3;
        auto *destination  = output.data() + static_cast<std::size_t>(y) * output_row_bytes;
        std::int64_t remainder = 0;
        for (int x = 0; x < output_width; ++x) {
            const auto *pixel = source.data + offset;
            destination[0]    = pixel[blue];
            destination[1]    = pixel[1];
            destination[2]    = pixel[red];
            destination += 3;
            if (x + 1 < output_width) {
                offset += base_step;
                remainder += sample_remainder;
                if (remainder >= output_width) {
                    remainder -= output_width;
                    offset += pixel_step;
                }
            }
        }
    }
    return true;
}

bool transform_nv_to_bgr(const NvSource &source, int display_rotation, int zoom, int output_width,
                         int output_height, std::vector<std::uint8_t> &output)
{
    if (!source.data || source.width < 2 || source.height < 2 || source.width % 2 ||
        (source.nv12 && source.height % 2) || !valid_rotation(display_rotation) || (zoom != 1 && zoom != 2) ||
        output_width <= 0 || output_height <= 0)
        return false;

    constexpr auto limit = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max());
    const auto row_bytes = static_cast<std::size_t>(source.width);
    const auto chroma_rows = static_cast<std::size_t>(source.nv12 ? source.height / 2 : source.height);
    const auto rows = static_cast<std::size_t>(source.height) + chroma_rows;
    // Luma includes padding through the start of the chroma plane. Only the
    // final chroma row may omit its trailing padding.
    if (row_bytes > limit || source.stride < row_bytes || source.stride > limit ||
        rows - 1 > (limit - row_bytes) / source.stride)
        return false;
    const auto required = (rows - 1) * source.stride + row_bytes;
    if (source.size < required || static_cast<std::size_t>(output_width) > output.max_size() / 3)
        return false;
    const auto output_row_bytes = static_cast<std::size_t>(output_width) * 3;
    if (static_cast<std::size_t>(output_height) > output.max_size() / output_row_bytes)
        return false;

    if (output.capacity() != 0) {
        const auto source_address = reinterpret_cast<std::uintptr_t>(source.data);
        const auto output_address = reinterpret_cast<std::uintptr_t>(output.data());
        if ((source_address >= output_address && source_address - output_address < output.capacity()) ||
            (output_address >= source_address && output_address - source_address < required))
            return false;
    }

    // Preserve the existing K230 crop rounding before mapping output pixels.
    int crop_width = std::max(2, source.width / zoom);
    int crop_height = std::max(2, source.height / zoom);
    const int crop_x = ((source.width - crop_width) / 2) & ~1;
    int crop_y = (source.height - crop_height) / 2;
    crop_width &= ~1;
    if (source.nv12) {
        crop_y &= ~1;
        crop_height &= ~1;
    }
    const int display_width = display_rotation % 180 ? crop_height : crop_width;
    const int display_height = display_rotation % 180 ? crop_width : crop_height;
    const auto map_point = [&](Point point) {
        return rotate(point, display_width, display_height, (360 - display_rotation) % 360);
    };
    const Point origin = map_point({0, 0});
    const Point next_x = map_point({1, 0});
    const Point next_y = map_point({0, 1});
    const Point step_x{next_x.x - origin.x, next_x.y - origin.y};
    const Point step_y{next_y.x - origin.x, next_y.y - origin.y};
    const int sample_step = display_width / output_width;
    const int sample_remainder = display_width % output_width;
    const auto chroma_base = static_cast<std::size_t>(source.height) * source.stride;
    output.resize(output_row_bytes * static_cast<std::size_t>(output_height));

    for (int y = 0; y < output_height; ++y) {
        const int sample_y = static_cast<int>(static_cast<std::int64_t>(y) * display_height / output_height);
        int source_x = crop_x + origin.x + step_y.x * sample_y;
        int source_y = crop_y + origin.y + step_y.y * sample_y;
        auto *destination = output.data() + static_cast<std::size_t>(y) * output_row_bytes;
        std::int64_t remainder = 0;
        for (int x = 0; x < output_width; ++x) {
            const auto luma_offset = static_cast<std::size_t>(source_y) * source.stride + source_x;
            const auto chroma_y = static_cast<std::size_t>(source.nv12 ? source_y / 2 : source_y);
            const auto chroma_offset = chroma_base + chroma_y * source.stride + (source_x & ~1);
            const int c = static_cast<int>(source.data[luma_offset]) - 16;
            const int d = static_cast<int>(source.data[chroma_offset]) - 128;
            const int e = static_cast<int>(source.data[chroma_offset + 1]) - 128;
            destination[0] = static_cast<std::uint8_t>(std::clamp((298 * c + 516 * d + 128) >> 8, 0, 255));
            destination[1] = static_cast<std::uint8_t>(std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8, 0, 255));
            destination[2] = static_cast<std::uint8_t>(std::clamp((298 * c + 409 * e + 128) >> 8, 0, 255));
            destination += 3;
            if (x + 1 < output_width) {
                source_x += step_x.x * sample_step;
                source_y += step_x.y * sample_step;
                remainder += sample_remainder;
                if (remainder >= output_width) {
                    remainder -= output_width;
                    source_x += step_x.x;
                    source_y += step_x.y;
                }
            }
        }
    }
    return true;
}

}  // namespace camera_frame
