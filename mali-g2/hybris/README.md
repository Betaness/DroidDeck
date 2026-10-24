# 进程内 Mali Vulkan（DroidDeck G2，libhybris）

Vortek（`../vortek`）把游戏的每个 Vulkan 调用经 socket 转发给 App 进程里的服务端执行。这个目录是另一条路：
游戏进程（glibc，aarch64，比如 Proton ARM64 里 Wine 的 unix 侧和其中的 DXVK / vkd3d-proton）通过
[libhybris](https://github.com/taowen/libhybris)（arlinux 分支）在**自己进程里**加载设备的
`/vendor/lib64/hw/vulkan.mali.so`，没有转发和服务端。原型在设备上测得每 4 万条命令（录制 + 提交等待）省约 1.8 ms，
另外省下 Vortek 服务端约 16% 个核。

默认关闭。会话设置 `gpuPath` 设为 `hybris` 时只有**游戏**走这条路，Steam 客户端（网页界面）仍走 Vortek。

## 组成

| 文件 | 作用 |
|---|---|
| `build.sh` | 按固定提交拉取 libhybris 和 Halium android-headers，打 `patches/`，编出 libhybris（common、hardware、Vulkan ICD、q 链接器）和 `libdroiddeck_hybris_vk.so`，生成 guest 的目录树和 ICD 清单。在 aarch64 glibc 环境里跑（PalmCode 的 Ubuntu 24.04、CI 的 ubuntu-24.04-arm），glibc 不高于 guest（Arch Linux ARM）即可 |
| `patches/libhybris-0001-static-tls-4096.patch` | `BIONIC_STATIC_TLS_SIZE` 1024 → 4096：`libGLES_mali.so` 的 TLS 段 832 字节、偏移 400，放不进原来的静态 TLS |
| `src/icd.c`, `src/icd.h` | guest 加载器看到的 ICD。dlopen 同目录 `lib/libhybris-vulkan-icd.so.0`（libhybris 的 glibc ICD：实例由安卓的 libvulkan 创建，设备和命令直接进 HAL），只拦截需要的入口，其余原样交给它；不包装句柄。有交换链的设备在图形队列族里多要一个队列给 acquire 用，没有空余时用应用的队列并加锁 |
| `src/wsi.c` | X11 WSI：`VK_KHR_surface`、`VK_KHR_xcb_surface`、`VK_KHR_xlib_surface`、`VK_KHR_get_surface_capabilities2`、`VK_KHR_swapchain`。交换链图像是 AHardwareBuffer（`VK_ANDROID_external_memory_android_hardware_buffer` 导入），经 `$VORTEK_PRESENT_SOCKET` 用 Vortek 的零拷贝协议（`../vortek/src/zero_copy.c`，原样编译）交给合成器显示；acquire 把合成器的释放栅栏导入应用的信号量/栅栏（`headless_swapchain.c` 的做法）。是否能走合成器图层由 X 窗口树决定（`client_x11_present.c` 的逻辑：被别的窗口挡住、被父窗口裁剪、带 alpha 时拷贝进窗口，完全被挡时按 10 fps 节流），两份代码合在一个进程里 |
| `src/mali_fixes.c` | Vortek 客户端里属于驱动问题的补丁：去掉 ClipDistance/CullDistance 写入（`../vortek/src/spirv_clip_cull.h`，和 Vortek 共用；不去掉 Mali 编译器在本进程里段错误，见测试）、vkd3d-proton 的 storage texel buffer 对齐、`VK_EXT_vertex_attribute_divisor` 别名。maintenance5/6、GPL、扩展过滤表（`ext_commands.h`）都是 Vortek 协议的需要，原生驱动不用 |
| `src/ahb.c`, `src/compat/android/hardware_buffer.h` | AHardwareBuffer 调用经 libhybris 转到设备的 `libnativewindow.so`（分配走 gralloc 的 binder 服务，和普通 App 一样） |
| `tests/` | 见下 |

环境变量沿用 Vortek 的同名开关：`VORTEK_PRESENT_SOCKET`、`VORTEK_ZERO_COPY=0`、`VORTEK_AHB_SWAPCHAIN=0`、`VORTEK_MIN_IMAGES`、
`VORTEK_RELEASE_WAIT`、`VORTEK_X11_SHM=0`、`VORTEK_X11_STATS=N`、`VORTEK_KEEP_CLIP_DISTANCE=1`、`VORTEK_STRICT_TEXEL_ALIGNMENT=1`。
日志前缀 `hybris-icd:`、`hybris-wsi:`、`hybris-x11:`（和 `vortek-x11:` 的格式相同）；连接合成器那行沿用 `vortek-server:`。

## guest 里的文件（`build.sh` 的输出）

```
/usr/local/lib/droiddeck-hybris/libdroiddeck_hybris_vk.so
/usr/local/lib/droiddeck-hybris/droiddeck_hybris_icd.aarch64.json
/usr/local/lib/droiddeck-hybris/lib/libhybris-common.so.1      ← 必须 LD_PRELOAD（静态 TLS）
/usr/local/lib/droiddeck-hybris/lib/libhardware.so.2
/usr/local/lib/droiddeck-hybris/lib/libhybris-vulkan-icd.so.0
/usr/local/lib/droiddeck-hybris/lib/libhybris/linker/q.so
/usr/local/share/licenses/droiddeck-hybris/{NOTICE,LGPL-2.1,Apache-2.0,BSD-2-Clause,SOURCES}
```

都是按 soname 命名的实文件（APK 的 assets 存不了符号链接）。`GUEST_DIR` 编进了 libhybris（链接器目录、RUNPATH），
别处测试用 `tests/run.sh`（设 `HYBRIS_LINKER_DIR`、`LD_LIBRARY_PATH`、`LD_PRELOAD`、`VK_ICD_FILENAMES`）。

## App 里怎么接（默认关）

- `session/HybrisGpu.kt`、`SessionPrefs.gpuPath`（`droiddeckctl prefs set session gpuPath hybris`，`vortek` 或删掉即关）。
- 打开时 `SessionService` 挂入安卓系统目录（和 `androidSystemBinds` 相同），guest 环境加 `DROIDDECK_GPU_PATH=hybris`（rootfs 里有 ICD 清单时）。
  droiddeck-env 里写 `DROIDDECK_GPU_PATH=vortek` 可整体关掉，game-environment.json 里给单个游戏写也行。
- `droiddeck-game-env` 的 `hybris_gpu`：只对游戏启动（`waitforexitandrun`）、`BL_GPU_MODE=vortek` 时生效，设
  `VK_ICD_FILENAMES`/`VK_DRIVER_FILES` 为本 ICD，把 `libhybris-common.so.1` 放到 `LD_PRELOAD` 最前，
  `VORTEK_PRESENT_SOCKET=$XDG_RUNTIME_DIR/vortek-present`。缺文件或 `/system/lib64/libvulkan.so` 时退回 Vortek。
- `SessionFiles` 每次会话从 APK 放进 rootfs（有才放）；CI 的 `hybris` 任务（`continue-on-error`）编出后放进 APK。
- 合成器（`waylandcomp/src/vortek_present.c`）现在同时接受最多 7 个零拷贝连接（Vortek 服务端 + 各游戏进程），
  链 id 按连接区分，释放消息发回所属连接。

## 测试（PalmCode 的 Ubuntu 里能跑；需要能读到 /system、/vendor、/apex）

```sh
sh build.sh                        # 第一次要联网
sh tests/run.sh vulkaninfo --summary
sh tests/run.sh /path/to/compute_glibc        # 私有原型的计算测试，结果和 /opt/hybris 原型一致
sh tests/wsi_test.sh               # Xvfb 上：X11 拷贝（BGRA/RGBA）、零拷贝（mock 合成器验像素）、改窗口大小、
                                   # 部分/完全遮挡切换、vkcube 零拷贝（最后一帧在 out/tests/vkcube.ppm）
sh tests/compositor_test.sh        # 合成器自己的 vortek_present.c（SurfaceControl 打桩）同时服务 3 个进程
glslangValidator -V --vn clip_vert -o out/tests/clip_vert.h tests/clip.vert && cc -Iout/tests -o out/tests/clip_test tests/clip_test.c -lvulkan
sh tests/run.sh out/tests/clip_test           # 写 ClipDistance 的管线；VORTEK_KEEP_CLIP_DISTANCE=1 时驱动段错误
```

## 只能在设备上测的

- DroidDeck 会话里（DroidDeck 自己的 proot、guest 是 Arch）游戏进程能否加载：Proton/Wine 进程里 `LD_PRELOAD` libhybris-common、
  wine 的地址空间预留和 bionic 链接器能否共存，WoW64 32 位游戏，FEX 在同一进程里。
- 真合成器的显示（SurfaceControl 图层、AFBC 缓冲、释放栅栏），和 Steam 客户端（Vortek）同时出图时的图层叠放和遮挡。
- 帧率和 CPU：和 Vortek 的 A/B。
- 驱动崩溃现在直接带走游戏进程（不再是整个 Vortek 服务端）。

## 许可

`libdroiddeck_hybris_vk.so`：LGPL-2.1 或更高（含 Vortek 的代码）。libhybris：Apache-2.0，链接器（q.so）来自 bionic，BSD-2-Clause（部分 Apache-2.0）。
android-headers：Apache-2.0。运行时加载的安卓系统库和驱动来自设备本身，不随 APK 分发。详见 `NOTICE`。
