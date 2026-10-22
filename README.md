# LILYGO 相机

为 T-DisplayCM0 的 IMX219 摄像头开发的本地相机 APP。界面参考 iPhone 相机：
黑色取景界面、黄色模式选择、圆形拍摄按钮和红色录像状态，支持竖屏与横屏。
应用使用 C++17、LILYGO UI AppKit / LVGL，并通过 libcamera C++ API 访问摄像头。

- 支持 `720P · 30`（默认）、`1080P · 30` 和 `720P · 60` 三档采集配置，点击顶部规格按钮切换。
- JPEG 照片按所选档位保存为 `1920×1080` 或 `1280×720`，旋转 90°/270° 时宽高互换。
- H.264 / MP4 视频按所选档位以 30 fps 或 60 fps 编码，播放方向跟随取景；当前不录制声音。
- 照片/视频模式切换、录像计时、构图网格、1×/2× 数字变焦和最近拍摄预览。
- 主机模拟器提供明确标注的模拟取景，也可以保存有效的 JPEG 和 MP4。

最近拍摄显示本次运行中最后一次拍摄的低分辨率取景快照，不是原图查看器或完整
相册；重启后不会恢复最近拍摄预览。视频只展示缩略图，APP 内暂不提供视频播放。
前后摄像头切换、闪光灯和人像效果不属于当前功能。系统提供 `AfMode` 时自动启用
连续对焦；摄像头驱动未提供该控制时，不假定 FP5510 已具备自动对焦支持。

## 硬件与设备树

依据 `lilygo-linux-dtoverlays/boards/T-DisplayCM0/t-display-cm0-overlay.dts`：

| 项目 | 设备树定义 |
| --- | --- |
| 图像传感器 | Sony IMX219，I2C0 地址 `0x1a` |
| 图像接口 | CSI1，2 lanes，24 MHz 摄像头时钟 |
| 安装方向 | `rotation = <180>`，`orientation = <2>` |
| 镜头马达 | FP5510，I2C0 地址 `0x0c`，通过 `lens-focus` 关联 |
| 主显示屏 | `568×1232`，横屏视口 `1232×568` |

APP 通过 libcamera 获取图像，不直接访问 I2C 寄存器，也不修改设备树。安装旋转
由 libcamera 根据设备树处理，应用不再额外旋转 180°。CM0 相机相对竖屏面板还需
逆时针补偿 90°；应用将这个固定偏角与屏幕的实际旋转角度合并，统一用于取景和保存媒体。
因此真机竖屏预览为 `360×640`，默认保存为 `720×1280`，1080P 档保存为 `1080×1920`；
横屏预览为 `640×360`，保存为 `1920×1080` 或 `1280×720`。SDL 模拟图案不应用这个物理安装偏角。
JPEG 和 MP4 保存方向一致的像素，MP4 由 FFmpeg 编码为 H.264。
FP5510 是否能自动对焦取决于设备的内核驱动、libcamera 控制与调校支持。

IMX219 支持 720P 输出：上游驱动提供至少 1920×1080 的传感器模式，libcamera
可通过管线缩放生成 `1280×720` Viewfinder 流。本项目默认使用 `720P · 30`；
`720P · 60` 是否可用仍取决于 CM0 的 BSP、驱动模式和实际帧率范围。

## 主机开发与模拟

需要 CMake 3.21+、C/C++17 编译器、`pkg-config`、SDL2、FFmpeg、Git 以及可用的
`lpm`。模拟图片和视频编码依赖 `ffmpeg`；真实设备需要 Raspberry Pi OS 提供的
libcamera 运行时和 IPA 模块（当前 BSP 为 `libcamera0.7` 与 `libcamera-ipa`）。

```sh
git submodule update --init --recursive
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
lpm start --no-build --foreground
```

也可以直接启动，指定拍摄目录以方便检查保存结果：

```sh
CAMERA_SIMULATE=1 CAMERA_OUTPUT_DIR="$PWD/build/captures" \
  ./build/host-simulator/lilygo-ui-camera --display sdl \
  --width 568 --height 1232 --orientation portrait
```

将最后一个参数改为 `--orientation landscape` 可验证横屏。AppKit 会交换基础宽高。
SDL 模拟器只改变窗口尺寸，不代表物理屏幕旋转，因此不会额外旋转模拟画面。
模拟拍摄会真实写入指定目录，文件会保留，关闭 APP 不会删除。模拟文件名带有 `SIM_IMG_` 或 `SIM_VID_` 前缀。模拟器不会自动
打开开发机的摄像头；界面会持续显示模拟状态，以免与真实传感器画面混淆。

测试包含纯 C++ 状态与错误处理、ViewModel 绑定、C++ 事件与异步拍摄和模拟媒体，以及
横竖屏渲染。渲染输出位于 `build/host-simulator/camera-portrait.ppm` 与
`camera-landscape.ppm`。主机测试不能代替真实 CM0 的帧率、方向、编码和摄像头
驱动验收；这些数值是配置目标，不代表已经在硬件上测得的性能。

## 设备构建与安装

设备构建需要 AArch64 交叉编译器。preset 首次配置时下载并校验锁定的 CM0 BSP
`0.1.0`，后续复用 `.cache/cm0-bsp/0.1.0`。直接执行：

```sh
lpm pack
```

产物为 `dist/lilygo-ui-camera_0.1.0_arm64.deb`。当前 BSP 只包含 libcamera 运行库；
缺少开发头文件时，配置阶段会从 Raspberry Pi 官方软件源下载匹配的
`libcamera-dev_0.7.2+rpt20260817-1_arm64.deb`，校验固定的 SHA-256 后解包到
`.cache/cm0-bsp/libcamera/0.7.2+rpt20260817-1/arm64`，并链接 BSP 内的同版本运行库。
后续构建复用该缓存；失效的头文件和库路径会在配置时重新查找。不要将依赖长期放在
`/tmp` 或 `/private/tmp`，系统清理临时目录后它们会消失。

`cm0-cross` 会从 BSP sysroot 查找 `libcamera/camera.h`、`libcamera.so` 和
`libcamera-base.so`；使用其他交叉工具链时，sysroot 必须提供等效的 libcamera 开发文件。
使用其他 BSP 或需要离线构建时，可安装匹配版本的 `libcamera-dev:arm64` 开发文件，或将已解包
的开发包目录传给 `-DLIBCAMERA_DEV_ROOT=/path/to/libcamera-dev`；支持包内的 `usr/include`
与 `usr/lib` 目录，也支持直接使用 `include` 与 `lib` 的目录布局。
当前 BSP 使用 `0.7.2+rpt20260817-1`。如切换 libcamera ABI，还需设置
`LIBCAMERA_DEBIAN_PACKAGE` 为目标系统对应的运行库包名。
开发包中的 `.so` 链接需能解析到同版本运行库；单独解包时应将匹配的运行库一起
解包到该目录，或直接指定现有 sysroot 中的 `LIBCAMERA_LIBRARY` 与 `LIBCAMERA_BASE_LIBRARY`。

手动配置（包括指定自定义开发包目录）后，也可以执行 `lpm pack` 复用 CMake 配置。
等价的分步打包命令为：

```sh
cmake --preset cm0-cross
cmake --build --preset cm0-cross --parallel
cpack --config build/cm0-cross/CPackConfig.cmake -B dist
```

如果已有完整 BSP 缓存，可在配置时指定 `-DCM0_BSP_CACHE_DIR=/path/to/cm0-bsp`。
安装包包含可执行文件、Launcher/desktop/AppStream 元数据、图标和字体许可证，并声明
匹配 BSP ABI 的 `libcamera0.7`、`libcamera-ipa`、`ffmpeg` 等运行时依赖。相机进程
直接使用 libcamera C++ API，不安装或调用 Python worker。

在使用 Raspberry Pi OS / CM0 镜像的设备上安装依赖与生成的包：

```sh
sudo apt update
sudo apt install libcamera0.7 libcamera-ipa ffmpeg
sudo apt install ./lilygo-ui-camera_0.1.0_arm64.deb
```

使用 Launcher 打开“相机”。独立运行时，确保当前用户有摄像头、DRM 与输入设备
访问权限，并确保没有另一个进程占用摄像头或 DRM 显示：

```sh
CAMERA_SIMULATE=0 /usr/bin/lilygo-ui-camera --display drm \
  --drm /dev/dri/card0 --orientation portrait
```

`--drm`、`--drm-connector`、`--input` 可根据设备节点覆盖 AppKit 默认值。也可使用
`--display fbdev --fbdev /dev/fb0`，前提是系统提供对应 framebuffer。

## 保存位置与配置

默认保存到当前用户的 `$HOME/Camera`。照片与视频保留在本地，不使用网络
服务；最近拍摄缩略图只驻留内存。请确保目录可写且有足够空间，录像结束后等待
保存完成。取景的 1×/2× 为数字裁切，会同步影响照片与视频，并非光学变焦。

每次启动默认使用 `720P · 30`。在取景页点击顶部规格按钮，可在以下三档之间循环切换，
照片和视频模式共用当前档位：

| 规格 | 采集/保存尺寸（未旋转） | 目标采集帧率 / 视频编码帧率 |
| --- | --- | --- |
| `720P · 30`（默认、低负载录像） | `1280×720` | 30 fps |
| `1080P · 30` | `1920×1080` | 30 fps |
| `720P · 60` | `1280×720` | 60 fps |

切换时会重新配置摄像头，并显示切换状态；变焦、照片/视频模式和最近拍摄保持不变。
录像、拍摄保存、最近拍摄预览及重新配置期间不能切换。所选档位不跨应用重启保存。
设备驱动必须提供支持所需尺寸和帧率的传感器模式；若无法应用新档位，界面提示失败并尝试恢复原档位。
标准 IMX219 驱动提供的模式可能无法达到 720P 60 fps，需以当前 CM0 镜像和真机验证为准；720P·30 通常更适合降低录像编码负载。
编码文件标注的 60 fps 也不能证明传感器每秒采到了 60 个不同画面。

| 环境变量 | 含义 |
| --- | --- |
| `CAMERA_SIMULATE=1` | 使用模拟取景和 FFmpeg 编码，适用于开发演示 |
| `CAMERA_SIMULATE=0` | 使用真实 libcamera C++ 后端；设备构建默认值 |
| `CAMERA_OUTPUT_DIR` | 覆盖照片与视频目录 |
| `CAMERA_FFMPEG` | 覆盖 FFmpeg 可执行文件路径；默认从 `PATH` 查找 |
| `CAMERA_VIDEO_CODEC` | 录像编码器选择：`auto`（默认）、`hardware` 或 `software` |
| `CAMERA_TRACE=1` | 每两秒向标准错误输出采集帧率、预览处理帧率、完整帧处理帧率与转换耗时，便于真机排查卡顿 |

设备模式由 C++ 相机后端创建 `libcamera::CameraManager`，按所选档位配置 `1920×1080`
或 `1280×720` Viewfinder 流与帧时长，并管理帧缓冲区。应用从该流保存照片和录像，
三档均生成 `640×360`（真机竖屏为 `360×640`）UI 预览；媒体编码
由 FFmpeg 完成，不需要设置 Python 解释器或 worker 脚本路径。

采集回调把完成的缓冲区交给后台线程，像素转换不在 libcamera 回调线程进行。日常取景直接从缓冲区生成
`640×360`（真机竖屏为 `360×640`）预览，将缩放和方向补偿合为一次处理；仅拍照和录像才生成完整分辨率图像。
预览和录像模式都会回收过期请求以避免界面延迟；录像只对当前最新帧生成完整分辨率副本，录像时间轴缺少的时刻由最新帧补齐。UI 按约 30 fps 轮询新画面。

开启 `CAMERA_TRACE=1` 后，日志中的 `sensor` 为成功完成的采集请求帧率，`preview` 为
后台处理的预览帧率（不代表屏幕实际刷新率），`full` 为拍照/录像完整帧处理帧率，
`transform` 为每个被处理请求的图像转换平均耗时。日志不会改变相机配置。

录像由独立的 30 fps 或 60 fps 时钟按所选档位向 FFmpeg 提交画面，不受 UI 刷新频率影响，也不会写入
完整的临时原始录像文件。编码拥塞时会保持录像时间轴，并优先使用最新采集帧，编码器异常则中止本次录像并清理临时文件。设备上若同时提供 `h264_v4l2m2m` 和 `/dev/video10`/`/dev/video11`，
录像会优先使用硬件 H.264 编码；否则回退到使用全部可用 CPU 核心的 `libx264`。编码写入在独立线程中进行，
普通编码回压不会阻塞相机回调；拍照和录像保存均在后台执行，结束后确认媒体完整再发布保存事件。

## 摄像头排查

先在设备上确认 libcamera 运行时和 IPA 模块已安装，并能发现 IMX219。若系统提供
相机工具，可用以下命令辅助检查：

```sh
libcamera-hello --list-cameras
```

某些镜像将检查工具命名为 `rpicam-hello`；工具缺失不影响应用直接使用 libcamera
库。若未发现摄像头，请先检查排线、对应 overlay、I2C0 引脚配置以及
内核日志中的 IMX219/Unicam/FP5510 探测结果。该 overlay 要求先启用
`i2c0,pins_44_45`，应按板卡镜像说明配置，避免重复启用另一个 IMX219 overlay。

如果能枚举却无法取景，检查摄像头是否被其他进程占用、`libcamera-ipa` 版本是否与
`libcamera0.x` 匹配，以及设备日志中是否有摄像头管线或缓冲区分配错误。
若录像保存失败，检查 `ffmpeg -version`、输出目录权限与剩余空间。错误会显示在界面中；
设备模式不会悄悄回退到模拟图像。

## 工程结构

- `src/pages/camera/camera_model.*`：独立于 LVGL 的相机状态与命令规则。
- `src/pages/camera/camera_process.*`：相机状态、FFmpeg 媒体编码和取景帧传输。
- `src/pages/camera/libcamera_backend.*`：libcamera 生命周期、Viewfinder 请求和帧缓冲区。
- `src/pages/camera/camera_view_model.*`：展示状态与命令。
- `src/pages/camera/camera_view.*`：响应式 LVGL 页面。
- `lpm.toml`：包名 `lilygo-ui-camera`、应用 ID `cc.lilygo.ui.Camera` 和产品元数据。

架构约束见 [工程总览](docs/00-overview.md)，视觉与布局约束见
[UI 规范](docs/01-user-interface.md)。应用通过 `find_package(LilyGoUI CONFIG REQUIRED)`
消费 SDK，不依赖 Launcher 源码，不单独复制 AppKit 字体。
