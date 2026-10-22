# LILYGO Camera

**English** | [简体中文](README.zh-CN.md)

LILYGO Camera is a local camera app for the T-DisplayCM0 and its Sony IMX219
camera. It is built with C++17, [LILYGO UI AppKit](https://github.com/LILYGO-UI/cm0-appkit),
LVGL, and the libcamera C++ API. The interface supports portrait and landscape
layouts and saves photos and videos on the device.

The project includes an SDL2 desktop simulator for development and testing, and
an AArch64 Linux package for CM0 devices.

## Preview

<p>
  <img src="assets/previews/preview1.png" alt="Camera viewfinder" width="240">
  <img src="assets/previews/camera-landscape.png" alt="Camera in landscape orientation" width="240">
  <img src="assets/previews/camera-recording.png" alt="Video recording state" width="240">
</p>

## Features

- **Photo and video:** capture JPEG photos and silent H.264/MP4 video.
- **Capture profiles:** choose `720P · 30` (default), `1080P · 30`, or
  `720P · 60`. Photos and videos share the selected profile.
- **Camera controls:** switch photo/video mode, show a recording timer, enable a
  composition grid, and choose 1× or 2× digital crop. The crop applies to both
  the viewfinder and saved media; it is not optical zoom.
- **Recent capture:** show a low-resolution viewfinder thumbnail of the latest
  capture from the current app session. It is not a photo viewer or gallery;
  video playback is not provided.
- **Responsive layout:** rearrange controls for portrait and landscape screens.
- **Desktop simulator:** display a clearly marked synthetic viewfinder and save
  valid JPEG and MP4 files through FFmpeg. The simulator does not open the
  development computer's camera.

The app does not currently provide camera switching, flash, portrait effects,
audio recording, or video playback. Autofocus is enabled only when the camera
stack exposes the required `AfMode` control; the presence of an FP5510 lens motor
alone does not guarantee autofocus support.

## Hardware and Camera Orientation

The target T-DisplayCM0 camera setup is defined by its device-tree overlay:

| Component | Configuration |
| --- | --- |
| Image sensor | Sony IMX219, I2C0 address `0x1a` |
| Camera interface | CSI1, 2 lanes, 24 MHz clock |
| Sensor orientation | Device-tree rotation `180`, orientation `2` |
| Lens motor | FP5510, I2C0 address `0x0c`, associated as `lens-focus` |
| Display | `568×1232` portrait; `1232×568` landscape |

The app uses libcamera and does not access sensor registers or modify the device
tree. libcamera applies the sensor's installed orientation. The camera is also
mounted 90° counter-clockwise relative to the portrait display; the app combines
that fixed offset with the screen orientation for the viewfinder and saved
media. As a result, portrait preview is `360×640`; default saved media is
`720×1280` for 720p and `1080×1920` for 1080p. Landscape media uses the
corresponding `1280×720` or `1920×1080` dimensions. The synthetic SDL image does
not use the physical mounting offset.

The profiles describe requested capture and encoding settings, not guaranteed
hardware performance. In particular, whether `720P · 60` is achievable depends
on the CM0 image, sensor mode, libcamera pipeline, and encoder. Verify actual
capture frame rates and output on the target device.

## Development Environment

Run project commands from the repository root. Project identity, version, and
package metadata are defined in [`lpm.toml`](lpm.toml); the
[`CMakePresets.json`](CMakePresets.json) file defines `host-simulator` and
`cm0-cross` builds.

### Get the Source

```sh
git clone --recurse-submodules https://github.com/LILYGO-UI/camera.git
cd camera
```

For an existing checkout, initialize the pinned AppKit dependency:

```sh
git submodule update --init --recursive
```

The project builds against AppKit in `third_party/cm0-appkit`; a separate AppKit
installation is not needed for the simulator.

### Host Dependencies

For the desktop simulator, install CMake 3.21 or later, a C/C++17 compiler,
Git, `pkg-config`, SDL2, FreeType, FFmpeg, Python 3.11 or later, and LPM.
FFmpeg is required to generate simulated capture files. Device builds also need
an AArch64 Linux cross compiler.

#### Debian 12 / Ubuntu 24.04 or Later

```sh
sudo apt update
sudo apt install build-essential cmake git pkg-config patch libsdl2-dev \
  libfreetype6-dev ffmpeg ca-certificates python3 python3-venv python3-pip
```

On older distributions, install versions that meet the minimum CMake and Python
requirements above.

#### macOS

Install the Xcode Command Line Tools if needed, then use Homebrew:

```sh
xcode-select --install
brew install git cmake pkg-config patch sdl2 freetype ffmpeg python
```

Make sure Homebrew and its installed tools are available on `PATH`.

#### LPM

CMake reads project metadata by running `lpm metadata --format cmake`. If LPM is
not already installed, create a project-local Python environment and install
it:

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install "git+https://github.com/LILYGO-UI/lpm.git"
lpm --version
```

Activate `.venv` in each new terminal before running `lpm`. CMake also accepts
an existing LPM executable through `-DLPM_EXECUTABLE=/absolute/path/to/lpm`.

### Device Cross Compiler

Skip this step when building only the desktop simulator.

**Debian / Ubuntu:**

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
```

**Apple Silicon macOS:**

```sh
brew install aarch64-unknown-linux-gnu
```

The BSP toolchain expects `aarch64-linux-gnu-gcc` and
`aarch64-linux-gnu-g++` on `PATH`. The BSP supplies the target sysroot; it does
not install the host cross compiler.

## Build and Test

### Desktop Simulator

Build and launch the app in a graphical desktop session:

```sh
lpm start --foreground
```

To launch an existing build without rebuilding, use
`lpm start --no-build --foreground`. Or build and run the commands separately:

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
lpm start --no-build --foreground
```

To choose a capture directory and test portrait rendering directly:

```sh
CAMERA_SIMULATE=1 CAMERA_OUTPUT_DIR="$PWD/build/captures" \
  ./build/host-simulator/lilygo-ui-camera --display sdl \
  --width 568 --height 1232 --orientation portrait
```

Use `--orientation landscape` to try the landscape layout. The simulator writes
captures to disk and keeps them after the app closes. Simulated filenames have
the `SIM_IMG_` or `SIM_VID_` prefix.

### Tests

Run the host test suite with:

```sh
lpm test
```

The suite covers camera state and error handling, frame transforms, ViewModel
bindings, simulated capture and media handling, and portrait/landscape UI
rendering. Render outputs are written under `build/host-simulator/`, including
`camera-portrait.ppm` and `camera-landscape.ppm`. Host tests do not verify
physical camera orientation, sensor frame rate, or target-device encoding
performance.

### CM0 Device Build and Installation

Build the AArch64 Debian package:

```sh
lpm pack
```

The first device configuration downloads and verifies CM0 BSP `0.1.0` and caches
its sysroot in `.cache/cm0-bsp/0.1.0`. The BSP includes the libcamera runtime
but not its development headers. If needed, CMake downloads and verifies the
matching Raspberry Pi `libcamera-dev` package (`0.7.2+rpt20260817-1`) and
caches it alongside the BSP. The first download requires access to the
corresponding release/package host; later builds reuse the cache.

If you already have matching libcamera development files, provide their root
during CMake configuration with
`-DLIBCAMERA_DEV_ROOT=/path/to/libcamera-dev`. The directory may contain
`usr/include` and `usr/lib`, or directly `include` and `lib`. A custom BSP or
libcamera ABI must also supply matching target libraries and set
`LIBCAMERA_DEBIAN_PACKAGE` to the runtime package name on the device.

The package is written to `dist/lilygo-ui-camera_0.1.0_arm64.deb`. It depends on
`lilygo-ui-appkit-dev >= 0.1.0` for the shared runtime fonts as well as the
camera and media libraries. Make sure the AppKit package is available from the
device's configured package sources. On the CM0 Raspberry Pi OS image, install
the runtime dependencies and package:

```sh
sudo apt update
sudo apt install libcamera0.7 libcamera-ipa ffmpeg
sudo apt install ./lilygo-ui-camera_0.1.0_arm64.deb
```

Launch **Camera** from the app launcher. To run it directly, the user needs
camera, DRM, and input-device access, and no other process may own the camera
or DRM display:

```sh
CAMERA_SIMULATE=0 /usr/bin/lilygo-ui-camera --display drm \
  --drm /dev/dri/card0 --orientation portrait
```

Use `--drm`, `--drm-connector`, or `--input` to override device nodes. A system
with a framebuffer can instead use `--display fbdev --fbdev /dev/fb0`.

## Capture Location and Configuration

Captures are saved to `$HOME/Camera` by default. Set `CAMERA_OUTPUT_DIR` to
choose another writable directory. Files remain on disk after the app exits;
the recent-capture thumbnail is kept only in memory for the current session.
The app does not use network services or record audio.

Each launch starts at `720P · 30`. Tap the profile control at the top of the
viewfinder to cycle through these profiles:

| Profile | Requested capture / saved size before rotation | Target frame rate |
| --- | --- | --- |
| `720P · 30` (default) | `1280×720` | 30 fps |
| `1080P · 30` | `1920×1080` | 30 fps |
| `720P · 60` | `1280×720` | 60 fps |

Changing profiles reconfigures the camera and may fail if the device does not
support the requested mode. The app reports the failure and attempts to restore
the previous profile. The selection lasts for the current session only. A
60-fps MP4 setting does not by itself prove that the sensor supplied 60 unique
frames per second.

| Environment variable | Purpose |
| --- | --- |
| `CAMERA_SIMULATE=1` | Use the synthetic viewfinder and simulated capture backend. |
| `CAMERA_SIMULATE=0` | Use the real libcamera backend; this is the device-build default. |
| `CAMERA_OUTPUT_DIR` | Override the capture directory. |
| `CAMERA_FFMPEG` | Set the FFmpeg executable path; by default it is searched on `PATH`. |
| `CAMERA_VIDEO_CODEC` | Select `auto` (default), `hardware`, or `software` H.264 encoding. |
| `CAMERA_TRACE=1` | Log capture and frame-processing rates and transform time to standard error every two seconds. |

On supported CM0 images, `auto` prefers the `h264_v4l2m2m` hardware encoder
when available and otherwise uses `libx264`. The app writes video in the
background and confirms the output before reporting a successful save.

## Troubleshooting

| Symptom | Check |
| --- | --- |
| `lpm is required to read project metadata` | Activate `.venv`, install LPM, or set `LPM_EXECUTABLE`. |
| AppKit or LVGL sources are missing | Run `git submodule update --init --recursive`. |
| SDL2 or FreeType is not found | Install the host development packages; on macOS, check Homebrew paths. |
| BSP or libcamera development files cannot be downloaded/found | Check network access and disk space, or configure `LIBCAMERA_DEV_ROOT` with matching target files. |
| IMX219 is not detected | Check the camera cable, board overlay, I2C0 pin setup, and kernel camera probe logs. |
| Camera is detected but no preview appears | Check that another process is not using the camera and that `libcamera-ipa` matches `libcamera0.7`. |
| Capture or video save fails | Check `ffmpeg -version`, output-directory permissions, free disk space, and `CAMERA_FFMPEG`. |

Some images provide `rpicam-hello` instead of `libcamera-hello` for camera
diagnostics. These utilities are optional; the app uses the libcamera library
directly. The board overlay requires the documented `i2c0,pins_44_45` setup;
follow the T-DisplayCM0 image instructions and avoid enabling a conflicting
IMX219 overlay.

## Documentation

| Guide | Contents |
| --- | --- |
| [Project overview](docs/00-overview.md) (Chinese) | Feature scope, architecture, dependencies, build and hardware acceptance. |
| [UI guidelines](docs/01-user-interface.md) (Chinese) | Layout, typography, component ownership, and UI validation. |

## Acknowledgments

This app uses [LILYGO UI AppKit](https://github.com/LILYGO-UI/cm0-appkit),
[LVGL](https://lvgl.io/), [libcamera](https://libcamera.org/), and
[FFmpeg](https://ffmpeg.org/). The project package metadata declares the MIT
license.
