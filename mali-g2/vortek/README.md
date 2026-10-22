# Vortek：Mali 设备上的 Vulkan 转发（DroidDeck G2）

这个目录是 `mali-g2` 分支新增的部分，让没有 Adreno、也没有可用 DRM render node 的
Mali 设备（例如 Mali-G2 / v15，Android 17）也能在 DroidDeck 的 Linux 环境里用上 GPU：

- **客户端** `libvulkan_vortek.so`：glibc aarch64 的 Vulkan ICD，放在 guest 的
  `/usr/local/lib/droiddeck-vortek/`，清单 `vortek_icd.aarch64.json`。它把 Vulkan 调用经
  unix socket + 共享内存环形缓冲区发给服务端；`vkQueuePresentKHR` 时把画面拷进共享缓冲区，
  再用 MIT-SHM 写进 X 窗口（软件呈现，见 `src/client_x11_present.c`）。
- **服务端** `libvortekserver.so`：bionic 可执行文件（名字是 `lib*.so`，安装时会被解压到
  App 的 `nativeLibraryDir`，App 可以直接执行）。它通过导出的 `sphal` 链接器命名空间
  加载 `/vendor/lib64/hw/vulkan.mali.so`（HAL 的 `HMI` → `open("vk0")`），执行客户端发来的调用。
  必须从 `/data` 下运行（`nativeLibraryDir` 满足），链接器才会给它 `sphal` 命名空间。

源码来自上游，按固定提交拉取再打补丁（上游代码不放进本仓库）：

| 仓库 | 提交 |
|---|---|
| https://github.com/brunodev85/vortek | `b1730c5def9b575672e671aee11d79ae7adc63d1` |
| https://github.com/brunodev85/winlator-app | `8077f374ef2d54127b088de9e8cd7b4af14d9c8b`（只稀疏检出 `vortekrenderer/`、`winlator/`） |

## 文件

| 文件 | 作用 |
|---|---|
| `fetch.sh` | 拉取上游到 `work/` 并应用 `patches/` |
| `patches/` | 对上游的修改：socket 路径可配置、X11 呈现钩子、去掉 JNI 的独立服务端钩子、Mali 上 `vkWaitForFences` 的修复、`VK_EXT_graphics_pipeline_library` 的线上结构（特性/属性、库创建信息、库句柄转换，`vortek-0008`/`winlator-0010`；服务端在链接出的管线还在时推迟销毁它用到的库，绕开 Mali 的问题；测试 `tests/gpl_test.c`）；`winlator-0011`：`ResourceMemory_allocate` 每种内存路径（opaque_fd / dmabuf / ahb / ahb 回退 / plain / native image）第一次分配时打日志，外部内存能力查询改用合法的 buffer usage 并打一次日志；实验开关 `VORTEK_NATIVE_IMAGES=1` 让专用于 OPTIMAL 图像的内存走普通 `vkAllocateMemory`（不可映射，`vkMapMemory` 返回 `VK_ERROR_MEMORY_MAP_FAILED`；带 `VkImportMemoryFdInfoKHR` 或 `VkExportMemoryAllocateInfo` 的分配仍走外部内存路径，pNext 只转发 dedicated/allocate flags/opaque capture address 的副本），测试 `tests/dedicated_image_test.c`；`src/headless_swapchain.c` 的 `VORTEK_MIN_IMAGES`（1..6）覆盖交换链 minImageCount）；`vortek-0009`/`winlator-0012`：命令流式发送，命令缓冲录制的命令超过 `VORTEK_CMD_CHUNK` 字节（客户端环境变量，默认关闭；Duckov 上 A/B 开关没有可测差别，所以不默认开启；可写 `32k`）时先用 DroidDeck 请求 `REQUEST_CODE_DD_CMD_CHUNK`（91，见 `src/droiddeck_requests.h`）发给服务端重放，`vkEndCommandBuffer` 只发剩下的部分，服务端的重放和应用的录制重叠；分块发送只尝试拿客户端调用锁（锁忙就继续攒，超过 1 MiB 才等锁）；客户端请求的头和数据一次发布到环形缓冲区（服务端睡着时只唤醒一次），命令批缓冲按 1.5 倍增长；命令缓冲 id 为空的整批丢弃，非空但已失效的 id 和以前一样检测不到；服务端没有 `textureCompressionBC` 时会建纹理解码器（Mali-G2 有 BC，不会建），它推迟的 BC 拷贝在分块重放时就进了队列，可能在该命令缓冲 End/提交之前、甚至被重置后仍被别的线程的 `vkQueueSubmit` 上传，这种设备上应设 `VORTEK_CMD_CHUNK=0`（服务端会打一次日志提醒）；测试 `tests/cmd_stream_test.c`。在 PalmCode 的 proot 里测得每次唤醒约 38 µs，但那个 proot 的 kompat 会拦截 futex，DroidDeck 自己的 proot（`tools/proot` 的 0011）不拦，所以那里测出的分块开销和总收益不代表游戏里的情况，默认值要以 DroidDeck 客户机里的测量为准，例如用 Duckov 对比 `VORTEK_CMD_CHUNK=0` 和默认值） |
| `src/standalone_main.c` | 服务端入口（代替 JNI 的 `main.c` 和 Java 的 `VortekRendererComponent`）：监听 socket、每个连接一个线程、经 `sphal` 加载厂商 HAL |
| `src/headless_swapchain.c` | 代替依赖 Winlator X server 的 `xwindow_swapchain.c` |
| `src/client_x11_present.c/.h` | 客户端的软件 X11 呈现（MIT-SHM / PutImage） |
| `src/droiddeck_defaults.h` | 客户端默认 socket 路径 `/run/droiddeck/vortek/V0` |
| `build-client.sh` | 在 aarch64 Linux 上编译客户端和 ICD 清单（CI：`ubuntu-24.04-arm`） |
| `build-server.sh` | 用 Android NDK 编译服务端（CI：`build` 任务里的 NDK） |
| `NOTICE`、`LICENSE.LGPL-2.1` | 许可说明，随 APK 放进 guest 的 `/usr/local/share/licenses/droiddeck-vortek/` |

## CI

`.github/workflows/build.yml`：

1. `vortek` 任务（`ubuntu-24.04-arm`）：`fetch.sh` + `build-client.sh`，上传 `vortek-client`。
2. `build` 任务：下载 `vortek-client` 到 `app/src/main/assets/linuxfs/usr/local/lib/droiddeck-vortek/`，
   用 NDK 编译服务端到 `app/src/main/jniLibs/arm64-v8a/libvortekserver.so`（`.gitignore` 里），再照常编 APK。

本地编译：

```sh
./fetch.sh
./build-client.sh out/client                       # aarch64 Linux，需要 cmake libx11-dev libxcb1-dev libxcb-shm0-dev libvulkan-dev
NDK=/path/to/ndk ./build-server.sh out/libvortekserver.so
```

## App 里怎么用（`[mali-g2]` 标记的改动）

- `session/VortekServerComponent.kt`：没有 Adreno、`/vendor/lib64/hw/` 里有 Vulkan HAL、APK 里有服务端时，
  会话开始前作为 `SessionPart` 用 `HostProcess` 启动服务端，socket 在 `filesDir/.wayland-rt/vortek/V0`
  （guest 里是 `/run/droiddeck/vortek/V0`），输出写进会话日志目录的 `vortek.log`，会话结束时停止。
  droiddeck-env 里的 `VORTEK_*` 变量（`VORTEK_VK_MAX_VERSION`、`VORTEK_RESOURCE_MEMORY_TYPE` 等）也传给服务端。
- `session/SessionService.kt`：给 guest 设 `BL_GPU_MODE=vortek`、`VORTEK_SERVER_PATH`、
  `BL_VK_DRIVER`/`VK_ICD_FILENAMES`/`VK_DRIVER_FILES`（指向 Vortek 的 ICD）。droiddeck-env 里写
  `BL_GPU_MODE=` 加别的值可以关掉。
- `session/SessionFiles.kt`：每次会话把客户端、清单和许可文件放进 rootfs。
- `droiddeck-session`：`BL_GPU_MODE=vortek` 时不启动 gamescope，改为启动 rootful Xwayland
  （`-shm`，不用 glamor；支持时加 `-fullscreen`；`BL_XWAYLAND_ARGS` 可追加参数），
  直接作为 App 合成器的客户端；`WAYLAND_DISPLAY` 清空，所有程序走 X11。Steam 的 CEF 用软件渲染
  （`BL_CEF_GPU=1` 可改回 GPU）。Xwayland 的日志在会话日志目录的 `xwayland.log`。
- `gpu/TurnipDriver.java`：没有 Adreno 但有厂商 Vulkan HAL 时，合成器用系统 Vulkan 驱动。
- `core/DeviceSupport.kt`、`MainActivity.kt`：这类设备不再弹「不是 Adreno」的对话框。
- `app/build.gradle`：包名 `com.droiddeck.launcher.g2`，应用名「DroidDeck G2」，可以和官方版并存。

## 限制

- 呈现是软件路径（GPU → 共享缓冲区 → MIT-SHM → Xwayland → wl_shm → 合成器），多次拷贝，没有垂直同步。
- 没有 gamescope：没有 Steam 游戏模式的合成、缩放和性能浮层。
- Vortek 客户端只支持 X11（xlib/xcb）surface，不支持 Wayland surface。
- Steam Linux Runtime 容器（pressure-vessel）里能不能看到 `/run/droiddeck/vortek/V0` 和这个 ICD 还没验证。

## 许可

Vortek 客户端和 vortekrenderer 由 brunodev85 以 LGPL-2.1 发布。`patches/` 是对上游的修改，`src/` 里的新文件
同样以 LGPL-2.1（或更新版本）发布，文件头保留了相应声明。全文见 `LICENSE.LGPL-2.1`，来源说明见 `NOTICE`。
