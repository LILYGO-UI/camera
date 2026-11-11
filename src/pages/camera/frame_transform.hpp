#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camera_frame {

struct Source {
    const std::uint8_t *data = nullptr;
    std::size_t size         = 0;
    int width                = 0;
    int height               = 0;
    std::size_t stride       = 0;
    // Set for RGB memory. libcamera RGB888 uses BGR memory and leaves this false.
    bool swap_red_blue = false;
};

struct NvSource {
    const std::uint8_t *data = nullptr;
    std::size_t size         = 0;
    int width                = 0;
    int height               = 0;
    std::size_t stride       = 0;
    // Both formats store interleaved U/V after stride * height luma bytes.
    // NV12 shares each chroma row between two luma rows; NV16 does not.
    bool nv12 = false;
};

struct Orientation {
    int pipeline_rotation  = 0;
    bool pipeline_mirrored = false;
    int display_rotation   = 0;
};

// Dimensions after undoing the pipeline orientation and applying the clockwise
// display rotation. Rotations must be 0, 90, 180 or 270 degrees.
bool output_size(const Source &source, const Orientation &orientation, int &width, int &height);

// Nearest-neighbour resize, orientation correction and BGR conversion in one
// pass over the output. The output's allocation is reused when possible. Source
// memory must not overlap output. Invalid dimensions/buffer bounds return false
// without changing output. For full-resolution export, use output_size().
bool transform_bgr(const Source &source, const Orientation &orientation, int output_width, int output_height,
                   std::vector<std::uint8_t> &output);

// Sample NV16/NV12 directly into the requested BGR output using BT.601 limited
// range, a clockwise display rotation, and 1x/2x chroma-aligned centre cropping.
// This avoids a full-size intermediate BGR image for a small preview. Width
// must be even and dimensions at least 2; NV12 also requires even height.
// Source memory must not overlap output. Invalid inputs leave output unchanged.
bool transform_nv_to_bgr(const NvSource &source, int display_rotation, int zoom, int output_width,
                         int output_height, std::vector<std::uint8_t> &output);

}  // namespace camera_frame
