# QueryPerformanceCounter 测量（调研第 2 项的门槛）

问题：游戏主线程每秒调多少次 `QueryPerformanceCounter`（N），每次在 ARM64 Proton 上花多少时间（t）？
只有 N×t 达到主线程时间的 1%（大约每秒 4–10 万次）才值得改 wine（PE 侧和 unix 侧都直接读 `cntvct_el0`）。

不用重编 wine，也不用新的信号量池包：

- ARM64 Proton 上 QPC 的路径是 kernel32 → ntdll（PE）`RtlQueryPerformanceCounter` → `NtQueryPerformanceCounter`
  系统调用 → unix 侧 `ntdll.so` 的 `monotonic_counter()`，也就是 `clock_gettime(CLOCK_MONOTONIC_RAW)`。在 Valve 的
  wine（experimental_11.0）里，`ntdll.so` 只有这一处用 `CLOCK_MONOTONIC_RAW`。glibc 从 vDSO 读时钟，不进内核，所以
  系统调用跟踪（perfetto、strace）看不到；FEX 的统计对 ARM64EC 代码也不数系统调用。
- 所以 `libqpcprobe.so` 经 `LD_PRELOAD` 进游戏进程，按线程、时钟、调用点（返回地址）数 `clock_gettime`。主线程
  （tid == pid，Wine 把 Windows 主线程放在进程的第一个线程上）从 `ntdll.so` 来的 `CLOCK_MONOTONIC_RAW` 调用就是它的 QPC。
- `qpcbench.exe`（x86_64 Windows 程序）用游戏自己的 Wine、FEX 和环境量每次调用的耗时：`qpc`
  （QueryPerformanceCounter）、`ntqpc`（直接调 NtQueryPerformanceCounter）、`tick64`（GetTickCount64：只进 Wine 的 PE
  代码、不进 unix 侧，相当于调用本身的开销）、`rdtsc`（FEX 把它变成读 `cntvct_el0`）、`empty`（空循环）。在用户态读
  计数器的 QPC 仍要付 `tick64 + rdtsc` 左右，能省下的是 `qpc` 减去它。

## 文件

| 文件 | 作用 |
|---|---|
| `qpcprobe.c` | 预加载库。不设 `QPCPROBE_DIR` 时只是直通。设了就计数：按 `pthread_self()` 的无锁表（不用线程局部存储：预加载了 libhybris-common 时静态 TLS 余量很紧），每线程最多 16 个调用点；每 `QPCPROBE_INTERVAL_MS`（默认 5000）由读 `CLOCK_MONOTONIC_RAW` 的线程顺手把快照追加到 `$QPCPROBE_DIR/qpc-<pid>.log`（命令行含 `QPCPROBE_MATCH` 的进程才写），格式见文件头。只用 `write(2)`，不调加载器。每次调用多几 ns（本机 proot 里 203 → 221 ns，proot 的 `clock_gettime` 不走 vDSO） |
| `qpcbench.c` | 测速程序，见上；`qpcbench.exe [N] [重复次数]`，各种调用交错测，输出每种的最快和中位 ns/次 |
| `build.sh` | 编出 `libqpcprobe.so`（只用 GLIBC_2.17 的符号，guest 是 Debian bullseye）和 `qpcbench.exe` |
| `qpc-guest.sh` | 在 guest 里跑（`droiddeckctl guest`，或测试工具的 g.sh）：`install`（从 App 的 agent-inbox 拷文件，写进 `game-environment.json` 的 `games[appid]`：`LD_PRELOAD` 会替换继承的值，所以写成 G2 会话里鸭科夫继承的那串（libblsession、libfakeinput、Steam 的三项叠加层）再加探针，droiddeck-game-env 仍把 libhybris-common 放第一；App 每次开会话都重写这个文件）、`check`、`bench`（复制游戏进程的环境，去掉探针和继承的 `WINESERVERSOCKET`、`WINELOADERNOEXEC`（否则 wine 报 Bad server socket 直接退出）、`WINEDEBUG=-all`，用游戏的 Wine 跑 qpcbench.exe，绑到主线程同簇的另一个核上，记下那个核测前测后的频率）、`report`、`dump`、`restore` |
| `qpc-report.py` | 分析：选进程（`--pid`，或命令行含 `--match` 的调用最多的那个）、按最后 `--last` 秒的快照算每秒次数，列出主线程的调用点（在 guest 里跑时从映射的文件读 ELF 符号）、各线程的 QPC 次数；给了 `--bench` 就算主线程占比 N×t、可省的占比和门槛结论；`--fps` 换算成每帧次数，`--bench-ghz`/`--main-ghz` 把测速核的耗时换算到主线程的频率 |

## 设备上怎么做

1. `build.sh <out>`，把 `qpc-guest.sh`、`qpc-report.py` 也放进 `<out>`；`adb push` `libqpcprobe.so`、`qpcbench.exe`、
   `qpc-report.py` 到 `/sdcard/Android/data/com.droiddeck.launcher.g2/files/agent-inbox/`。
2. 开 Steam 会话，开游戏之前 `qpc-guest.sh install`；开鸭科夫，到标题画面后等测量时长（例如 200 秒）。
3. 游戏还开着时：`qpc-guest.sh check`（游戏进程映射了探针）、`qpc-guest.sh bench`、`qpc-guest.sh report 200 --fps <帧率> --main-ghz <主线程 GHz> --bench-ghz <测速核 GHz>`、
   `qpc-guest.sh dump`（原始日志）。
4. `qpc-guest.sh restore`，删掉 agent-inbox 里的文件。

结论看 report 最后一行：`main thread share` 不到 1% 就不改 wine；到了再做 RESEARCH-2026-11-02 第 2 项的实现（PE 和 unix
两侧同一公式、保持 10 MHz）。
