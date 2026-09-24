# LILYGO 相机工程总览

本项目是 T-DisplayCM0 的独立相机应用，使用 IMX219 与 libcamera C++ API，
LILYGO UI AppKit。公开身份为包/可执行文件 `lilygo-ui-camera`、应用 ID
`cc.lilygo.ui.Camera`、Launcher 标题“相机”，由 `lpm.toml` 统一维护。

## 功能范围

相机页面提供实时取景、JPEG 拍照、无声 MP4 录像、拍摄模式切换、录像计时、构图
网格、1×/2× 数字变焦与最近拍摄预览。照片和视频配置为 1920×1080，视频目标帧率为 30 fps；
UI 取景使用独立的 640×360 帧。屏幕旋转 90°/270° 时，取景与 JPEG 的宽高互换，
MP4 使用方向一致的像素保持相同的播放方向。实际硬件性能需要在 CM0 上测量。视频最近拍摄页
显示缩略图，不提供播放。缩略图来自低分辨率取景，仅保留本次运行中的最后一次
拍摄，不能代替原图或完整相册。当前版本不使用网络或麦克风。

相机通过设备树声明的 IMX219 / CSI1 管线工作，遵循 libcamera 的安装方向处理。
应用启动时将 LVGL 屏幕的实际旋转传给相机后端，由后端同步修正预览、照片和视频。
窗口宽高变化只调整布局，不作为物理方向依据。
FP5510 节点存在并不意味着系统已经提供自动对焦控制；不直接写 I2C 寄存器。

## 架构与数据流

```text
触摸事件 -> CameraView -> CameraViewModel -> CameraModel
                                    |          |
                                LVGL Subject   CameraProcess
                                    |          |
                                UI 绑定       libcamera backend
                                               |
                                    libcamera CameraManager / JPEG / video writer
```

- `camera_model.*` 管理状态、拍摄规则和错误，独立于 LVGL。
- `camera_process.*` 负责拍摄状态、FFmpeg 编码、事件及预览帧。
- `camera_view_model.*` 将模型转换成可观察的展示状态，接收 View 的命令。
- `camera_view.*` 在传入的 parent 下创建对象、布局、绑定和转发事件。
- `app.cpp` 管理创建与销毁，保证 View 先于 ViewModel 销毁。
- `libcamera_backend.*` 在设备模式下直接配置 libcamera 的 `1920×1080` Viewfinder
  流，管理帧缓冲区和 requestCompleted 回调；应用从该流保存 JPEG/录像，并生成
  `640×360` UI 预览。媒体编码使用 FFmpeg，模拟模式生成可验证的媒体文件。

UI 不在 LVGL 回调中执行阻塞拍摄或编码。摄像头初始化、保存失败、设备失联或
后端异常都必须回到明确可见的错误状态。录像中的模式切换需受状态规则约束；
结束录像后等待 MP4 封装完成再标记成功。模拟与真实模式不得隐式互相回退。

拍照与录像保存由后台任务完成，C++ 事件回到 UI 线程后更新状态。录像使用独立的
30 fps 时钟向 FFmpeg 连续送帧，不依赖 UI 的 poll 频率，也不生成完整临时原始录像文件。

## 数据与依赖

默认文件目录是 `~/Pictures/Camera`，可通过 `CAMERA_OUTPUT_DIR` 覆盖。
拍摄输出是用户数据，重启或关闭应用不清理。测试使用临时目录，避免混入真实照片。

设备依赖与 BSP ABI 匹配的 libcamera 运行时、`libcamera-ipa`、FFmpeg 和 AppKit。
构建主机使用合成后端，不需要安装 libcamera；设备模式
由 C++ 直接使用 CameraManager、FrameBuffer 和 Request，无需 Picamera2、NumPy 或
Python worker。

应用只能通过 `find_package(LilyGoUI CONFIG REQUIRED)` 使用 SDK；不引用 Launcher
源码或 AppKit 私有文件。C++ 使用 C++17，UI 使用 SDK 提供的 LVGL 和组合字体。

## 构建与验收

稳定 preset 为 `host-simulator` 与 `cm0-cross`，构建树在 `build/` 下。主机默认模拟
相机，设备默认真实相机，`CAMERA_SIMULATE=0/1` 可显式覆盖。

```sh
cmake --preset host-simulator
cmake --build --preset host-simulator --parallel
ctest --preset host-simulator
```

验收涵盖模型、ViewModel、C++ 事件与异步拍摄、模拟媒体文件以及 568×1232 / 1232×568
横竖屏。UI 修改还需要检查渲染结果，确认中文、控件命中范围、计时和状态提示正常。
修改安装或打包规则时还要构建 `cm0-cross` 并用 CPack 生成 Debian 包。

交叉构建从 CM0 BSP sysroot 查找 `libcamera/camera.h`、`libcamera.so` 和
`libcamera-base.so`；若 BSP 仅包含运行库，需补充匹配版本的 `libcamera-dev:arm64`
开发文件，或设置 `LIBCAMERA_DEV_ROOT` 指向已解包的开发包根目录（`usr/include` / `usr/lib`）
或开发前缀（`include` / `lib`）。主机模拟 preset 不查找或链接这些设备库。

真实设备验收需要检查 IMX219 枚举、取景颜色和方向、JPEG 实际尺寸、MP4 可播放性、
录像结束/关闭时的保存行为、连续拍摄和错误恢复，以及 CM0 上的性能。主机模拟器
测试通过只证明主机测试覆盖的行为，不等同于已经完成这些硬件验证。

`.cache/`、`.lpm/`、`.venv/`、`build/`、`dist/` 都是本地状态，不属于源码。
`data/*.in` 是通用元数据模板，不能重复硬编码产品身份。新应用不声明虚构的
Debian `Provides`、`Conflicts` 或 `Replaces` 迁移关系。
