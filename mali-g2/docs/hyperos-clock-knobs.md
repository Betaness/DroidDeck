# HyperOS clock knobs on the Xring O3, without root

What on this HyperOS build can change the CPU or GPU clocks an app gets, who may turn each knob, and what
it does inside. This comes from decompiled system code and read-only adb, not from measurements. Each
knob that might help has a device experiment at the end.

Build: Android 17, OS4.0.16.0.XBMCNXM (CP2A.260605.016). Components: miui-services.jar and services.jar
(system_server), PowerKeeper 4.2.00, SecurityCenter 13.2.9 (Game Turbo, battery), Joyose 2.5.22.
Decompiled with androguard. Class and setting names below are as found; obfuscated classes are named by
their file (e.g. `oh.c0`).

## How the clocks are set on this build

- **CPU.** The `xres` cpufreq governor runs each policy: cores 0-3 C1-Pro up to 3.15 GHz, 4-7 C1-Premium
  up to 3.69 GHz, 8-9 C1-Ultra up to 4.36 GHz. `scaling_min_freq`, `scaling_max_freq` and
  `scaling_available_frequencies` can be read by apps and by the shell; the governor's own tunables cannot.
- **Raising clocks (floors).** The vendor PerfFlinger HAL holds "hints", listed in
  `/vendor/etc/pf_hint_config.xml`. A hint is a set of resources: a cluster's minimum frequency, the GPU's
  minimum frequency, DDR and L3 votes, sched boost, RTG prefer-CPUs. Examples:
  - `HINT_PERFORMANCE_MODE` (1001): every cluster's minimum at its maximum, GPU minimum at its maximum, no
    timeout.
  - `HINT_GPU_BOOST_{LOW_BASIC,LOW,MEDIUM,HIGH}_MODE` (1005-1008): GPU minimum at 279, 461, 749 or
    979 MHz.
  - `HINT_GPU_BOOST_CPU_MODE` (1010): cluster minimums at 1.41, 1.54 and 2.81 GHz.
  - The game-start hints are only for named Tencent and miHoYo games.

  The SELinux policy lets only system_server, Joyose, platform/priv apps, mi_thermald and some HALs call
  PerfFlinger or uniperf. The shell user and untrusted apps can find these services but cannot call
  them.
- **Lowering clocks (ceilings).** These come from thermal control. PowerKeeper computes a thermal scenario
  and writes it to `thermal_message/sconfig`; mi_thermald then picks a table from the encrypted
  `thermal-*.conf` files. PowerKeeper's IPA (power allocator) also writes per-cluster and GPU level
  limits and a sustainable-power budget to `/sys/class/thermal/thermal_message/ipa_*`.
  `dumpsys activity service com.miui.powerkeeper/.thermal.ThermalService` shows the current state:
  - In a G2 session it showed scenario 19, `ForegroundGroup : 16` (PowerKeeper's cloud list maps
    com.droiddeck.launcher.g2 to group 16, ELEMENT_FOREGROUND_OPTIMIZE_GAME) and `PowerMode : 99`
    (balanced).
  - Its "IPA INFO" line shows level limits changing during the session (cpu8/cpu4/cpu0/gpu levels such as
    16/16/12/6).
  - `dumpsys thermalservice` meanwhile showed all AOSP cooling devices at 0.
  - In the same minute `scaling_max_freq` of policy 0/4/8 moved between full and 1.35/1.97/2.20 GHz.
    Another agent was using the device, so these readings are noisy.
- **The AOSP power HAL is a stub.** `android.hardware.power.xring` (in `com.android.hardware.power.xring.apex`,
  86 KB) imports nothing that opens, writes, ioctls or calls another service; it only logs `Power setMode:`
  and `Power setBoost:`. So on this device the following do nothing to clocks:
  - `Mode.GAME`, `GAME_LOADING`, `SUSTAINED_PERFORMANCE`, `FIXED_PERFORMANCE`;
  - every boost;
  - all ADPF hint sessions (`createHintSession`, `reportActualWorkDuration`).

  `dumpsys performance_hint` still lists G2's session, and it reports "CPU/GPU Headroom Supported: false".

## Knobs, one by one

Columns: who may set the knob, what it changes inside, whether the game in G2's child processes gets it,
and the risk.

### 1. Battery → Performance mode (`POWER_PERFORMANCE_MODE_OPEN`)

- **Who:** the owner (Settings → Battery, or the Performance mode tile, which is already in the Quick
  Settings panel: `custom(com.miui.securitycenter/com.miui.powercenter.powersaver.PerformanceModeTileService)`).
  - The shell user can press that tile with
    `cmd statusbar click-tile com.miui.securitycenter/com.miui.powercenter.powersaver.PerformanceModeTileService`.
    `onClick` toggles the mode directly when the device is unlocked: `oh.c0.I0(ctx, true, true)` →
    `PowerSaverProvider.changePerformanceMode`. There is no dialog.
  - Writing `settings put system POWER_PERFORMANCE_MODE_OPEN 1` alone does nothing. The setting is only
    the UI's record. The switch itself is PowerKeeper's `GlobalFeatureTable` `userConfigureStatus`, which
    needs `miui.permission.powerkeeper.HIDDEN_MODE_PROVIDER`. The `com.miui.powercenter.powersaver` provider
    needs `POWER_COMMAND`, which is signature|privileged. Neither is open to the shell or to apps.
- **What it does** (SecurityCenter `PowerSaverProvider.m(true)` and PowerKeeper):
  - SecurityCenter sets `userConfigureStatus=performance` and broadcasts `miui.intent.action.HIDE_MODE_CHANGE`.
  - PowerKeeper's `EventsAggregator.toPowerMode("performance") = 1` reaches:
    - the thermal `PowerModeListener` (element 99 → 98), which changes the thermal scenario id, so
      mi_thermald loads a different, presumably looser, table;
    - `PowerModeHandler` (touch report rate, background residency, `thermal_message/balance_mode` when
      the cloud enables it);
    - `DisplayFrameSetting`, which sets `system speed_mode=1`.
  - `PerfFlingerClient.enableOverClock` is called only if `ro.vendor.persist.hardware.perfflinger.support_overclock=1`,
    which is unset here.
  - No code was found that sends PerfFlinger hint 1001 (`HINT_PERFORMANCE_MODE`). If something does, it is
    native code (mi_thermald or uniperf, both unreadable).
- **Child processes:** yes, it is system-wide.
- **Risk:** low. More heat and battery drain. It is a global owner preference, and it stays on if the app dies.

### 2. `secure speed_mode_enable` and `system speed_mode`

- `speed_mode_enable` is a visibility flag that PowerKeeper copies from cloud config (`CloudUpdateHideMode`).
  Nothing reads it to change clocks.
- `speed_mode` is an output of PowerKeeper:
  - it is 1 only while the power mode is performance, and is reset to 0 when the state machine starts;
  - Joyose reads it to pick its per-game cloud profile key (`...#SPEEDON`);
  - DisplayFrameSetting reads it for refresh rate.

  The shell can write it, but PowerKeeper overwrites it at the next mode change, and it only matters for
  games that Joyose has profiles for.

**Verdict:** no use for G2.

### 3. Game Turbo per-app performance and its GPU tuner

- **`secure gamebooster_perf_level_cross_process_<pkg>`.** SecurityCenter writes it (`GameInfoUtils.w/A`) as a
  copy of the level that Joyose reports back (`PerformancePolicy` in `com.xiaomi.joyose.ACTION_CALLBACK`).
  The only reader is SecurityCenter itself (`g1.i`, default 1). Changing it with `settings put` does
  nothing. This matches the 0/2 test in HANDOFF 5b.
- **Game Turbo "performance" for a game.** This is Joyose's `GPUTunerService.setPerformancePolicy(pkg, level)`
  → the vendor enhance module. It only works for games for which `isSupportPerformancePolicy(pkg)` is
  true (a cloud list). Callers must be uid 1000 or SecurityCenter (`checkCaller`).
- **`GPUTUNER_SWITCH` / GPUTunerService.** These are per-game driver profiles: FPSCap, anisotropy, mipmap
  LOD and Adreno qgl properties. They are not clocks, and the callers are the same restricted set.
- **The rest of Game Turbo's boosting** is Joyose's cloud per-game command lists and the SchedGenius
  game regulator (`setSchedGeniusIfNeed start-game`). That is the machinery HyperOsGameMode and
  FrameSchedOptOut keep away from G2, because it held the mid cores at 0.56-0.84 GHz and the GPU at
  119 MHz.

**Verdict:** no use for G2, and harmful in its current form.

### 4. G2 being in the Game Turbo list (`gb_boosting_pkg_name`, the game list)

- **Who:** the owner (Game Turbo → remove the app). Not the shell.
- **Effects:**
  - Joyose's start-game and FrameSchedPolicy's `topGame` follow from this list.
  - PowerKeeper's thermal `ForegroundGroup` 16 probably does too; the cloud map lists G2 next to other
    Game Turbo games.
  - Removing G2 would stop the regulator at the source. It would also move G2 to another thermal group,
    which could be better or worse: the tables are encrypted.
- **Risk:** low and reversible (add the app back in Game Turbo).

### 5. `cmd game mode performance`, `game_mode_config`, `setGameState`

- **State:** G2's manifest has `appCategory="game"` and `isGame`. `cmd game list-modes com.droiddeck.launcher.g2`
  already reports `current mode: performance`, and there are no interventions.
- **Mode change:** in GameManagerService a mode change only updates interventions and notifies
  `IGameModeListener`s.
- **Foreground game:** when a game is in the foreground, GameManagerService sets `Mode.GAME`.
- **`setGameState(isLoading=true)`:** in performance mode this sets `GAME_LOADING` for up to 8 s.

Both modes go to the stub HAL, so none of this changes clocks.

`game_mode_config` interventions are resolution downscale, an fps cap and ANGLE. None of them is a clock.
GameManager also registers a 60 fps "game default frame rate override" for G2's uid (SurfaceFlinger
shows `{10264, 0 60}`). It is inert because `ro.surface_flinger.enable_frame_rate_override=false`.

**Verdict:** no use.

### 6. ADPF hint sessions (GameThreadHints on the O3)

The sessions reach the stub HAL, so the boost GameThreadHints measured on the Odin 3 does not exist here.
On the O3 the only part of GameThreadHints that acts is ClusterSteer (taskset), which runs inside the same
worker. This fits HANDOFF 5b (F1 against F2): the gain there was moving threads to the little cores.

### 7. Thermal headroom, `cmd thermalservice override-status`, `cmd power set-fixed-performance-mode-enabled`

- **Headroom.** `getThermalHeadroom` and `cmd thermalservice headroom 0` (now 0.774) only report.
- **`override-status`** fakes the AOSP thermal status. Nothing here throttles from that status: the stub
  HAL and mi_thermald ignore it, and PowerKeeper uses its own sensors. A faked status would only change
  what apps see; GameThreadHints, for example, stops at CRITICAL.
- **`set-fixed-performance-mode-enabled`** and `cmd power set-mode` go to the stub HAL. `set-mode 1` is
  AOSP battery saver, which lowers clocks.

**Verdict:** no use.

### 8. `global DYNAMIC_PERFORMANCE_DEFAULT_STATUS` (and `DYNAMIC_PERFORMANCE_STATUS`)

- Joyose writes this from cloud config. It is Joyose's `DynamicPerformanceController`, which changes the
  render resolution of listed games through `/data/system/mcd/dr`.
- It is gated by `persist.sys.smartop.support_dynamic_performance`, which is false on this device.

**Verdict:** not a clock, and inert here.

### 9. Power saving

- **What turns it on:** `system POWER_SAVE_MODE_OPEN`, AOSP `low_power`, and the super/ultimate saving modes.
- **What it does:** it can only lower clocks. PerfFlinger's `HINT_LOWPOWER_MODE` (1002) caps the clusters
  at 1.23/1.54/2.54 GHz and the GPU at 854 MHz, and PowerKeeper writes `thermal/power_save/*`.
- **Now:** all off (`low_power=0`, the settings are unset).
- **Use:** only as a check that none is on during measurements.

### 10. Low-temperature extreme endurance

These are `secure pc_low_temperature_extreme_endurance_switch` (1), `system low_temperature_extreme_endurance_support`
(1, written by PowerKeeper) and `..._temp_mode_value` (4).

- **What it does:** PowerKeeper's `MiawareBcasHandler` (battery-capacity-aware scheduling) writes
  `thermal_message/temp_aware`. That limits peak power from the fuel gauge's `bcl_maxpower` when the
  battery is cold, so the device does not brown out.
- **When it acts:** the battery read 34-42 °C here, so it does not act.
- **Risk of turning it off:** shutdowns in the cold.

**Verdict:** leave on.

### 11. MiBridge, Xiaomi's third-party performance SDK

This is the only system path found through which an untrusted app can raise floors.

**Who.** The app itself, once its package is in `system mibridge_authorized_pkg_list`.

- That list comes from cloud config (PowerKeeper `MiBridgeAuthorizedPkgList`). Today it holds WeChat,
  Weibo, Douyin, the Xiaomi launcher and others.
- It has a second reader: PowerKeeper's `MiSpeedUtils` hands the listed uids to the native
  `miuiboosterservice` (binder transaction 29), and skips that while the list is empty or unset. So when
  the app's package was the only entry, taking it out (`settings delete`) leaves its uid authorized in
  `miuiboosterservice` until a reboot. Being listed is more than leave to ask MirimThirdPartyPolicy.
- The shell user should be able to add a package with `settings put system`. Shell is exempt from the
  system-table restrictions; this is not yet tried here.
- The app can do that through its saved wireless debugging pairing, the same way HyperOsGameMode works.

**Path.**

- `com.miui.performance.MiuiBooster` in `/system_ext/framework/MiuiBooster.jar`. Load it with a
  PathClassLoader, as SecurityCenter does with PerfFlingerClient.jar. Its code then runs with the
  platform hidden-API domain.
- → `miui.scenariorecognition.ScenarioRecognitionManager.method(1816, op, RimParameter)` (service
  `scenariorecognition`, which is an `app_api_service`)
- → system_server `MirimThirdPartyPolicy`
- → `UniperfClient.UniperfSetFreqlevel(type, level)`
- → uniperf HAL → PerfFlinger.

**Operations.**

- `checkPermission(pkg, uid)` grants all bits to a listed uid.
- `requestCpuHighFreq`, `requestGpuHighFreq`, `requestDdrHighFreq` take a level from 1 to 3 and a timeout.
  The timeout is capped at 10 s for level 1, 5 s for level 2 and 1.5 s for level 3, and the request must
  be renewed. MiBridgeBoost and the probe renew halfway through the timeout: renewing 300 ms before it
  left level 3 about 50 ms to spare, and the probe's first version slept past the timeout every time.
- A request returns 0 when granted, -1 when system_server refuses it (a hot board for the CPU, a grant
  gone, uniperf), and -2 when the client library refuses it itself (its static permission flag is unset,
  or the uid is not the caller's).
- CPU requests are refused while `thermal_message/board_sensor_temp` is at or above
  `persist.sys.miuibooster.thermalbreaklimit` (40 °C).
- `requestBindCore` sets affinity, which the app can already do.
- `requestThreadPriority` with level 1 makes the given tids SCHED_FIFO 1; level 2 sets nice -19 and level
  3 nice -10. Tid ownership is not checked.

**State and effect.**

- `dumpsys scenariorecognition` (MirimThirdPartyPolicy) shows `uniperfEnable=true`, the eligible uids, and
  `cpuHandles={} gpuHandles={} ddrHandles={}`.
- **Level 1 is the strongest** (measured with the probe from the shell, 2026-10-26). uniperf's tables cannot be read, but the
  floors can (`/sys/class/devfreq/gpufreq_core/min_freq`, each policy's `scaling_min_freq`, `ddr_devfreq`):

  | Level | GPU floor | CPU floors (cpu0-3 / 4-7 / 8-9) | DDR |
  |---|---|---|---|
  | 1 | 979 MHz | 2.25 / 3.43 / 2.71 GHz | `cur_freq` held at 9600 MHz (`min_freq` unchanged) |
  | 2 | 749 MHz | 2.25 / 2.67 / 2.71 GHz | |
  | 3 | 576 MHz | 1.64 / 1.97 / 2.71 GHz | |

  Without any request the system already held the GPU at 979 MHz and the CPU at 1.07/1.14 GHz during a session,
  so `gpu=1` adds little. A sustained half-resolution session keeps `board_sensor_temp` at 40-42 °C, even with a
  cooler, so CPU requests are mostly refused then. `ddr=1` cut the Duckov title's cycles per frame (main thread
  33.7 → 28.4 M, GPU 19.1 → 14.6 M), while the system then ran the small and middle clusters lower (about 2.1 →
  1.75 GHz). Half-resolution fps: none 56.5 ± 4.5, gpu=1 57.7 ± 4.4, gpu=1,ddr=1 60.2 ± 2.3, cpu=1,gpu=1,ddr=1
  61.1 ± 2.6 (within the spread between sessions); the SoC ran about 5 °C warmer.
- **Child processes:** yes for the frequency floors, which are system-wide. For priority, only for the tids
  passed.
- **Risk.** The list is cloud-managed, so a cloud update can drop the entry; add it again per session (and
  MiBridgeBoost adds it again and re-checks the permission, at most once a minute, when a request is
  refused mid-session). Expect more heat. Leftover requests lapse in 10 s or less. Clearing the app's
  data or uninstalling it leaves the package in the list: take it out by hand
  (`settings get`/`put system mibridge_authorized_pkg_list`). SCHED_FIFO on a busy Wine thread can starve other
  threads on that core, so it is not used here.

**Implemented here:** `MiBridgeBoost` (pref `session/miBridgeBoost`, e.g. `gpu=1`; unset is
`cpu=1,gpu=1,ddr=1` on the O3, `off` turns it off), and the shell probe `mali-g2/tools/mibridge-probe`.

### 12. TurboSched (`system turbo_sched_core_app_list`)

`dumpsys turbosched` reports `TurboEnabled: false`, `SDK ApiEnabled: false` and `metis dev access = false`.

**Verdict:** disabled on this device, so inert.

### 13. `/dev/xr_fbt`

This is a lead for research, not a knob. libxfbt's ioctls (`setQueueBuffer`, `setDoFrame`, `setFpsInfo`,
`mgrSetLimitPid`) are what libmigui uses to put a game's render threads under the kernel frame booster;
`xres` has `rtg_boost_freq`. The node is 0666, so the app could report the game's own frames and threads.
This is the same machinery that FrameSchedOptOut turns off because SchedGenius chose GPU clock pairs from
the wrong layers. The struct layouts are not decoded.

## Ranking (expected gain, then simplicity)

1. **MiBridge GPU/CPU/DDR floors (11).**
   - **Gain:** the only floor an app can get. Level 1 is the strongest, and its GPU floor is 979 MHz, no higher
     than the clock the GPU idles at; the DDR floor is what lowers the cycles per frame (see 11).
   - **Cost:** one setting plus app code, which now exists and is on by default on the O3.
   - **Test:** E1, then E3.
2. **Performance mode (1).**
   - **Gain:** may relax the thermal and IPA ceilings that show up as `scaling_max_freq` drops during a
     session.
   - **Cost:** nothing for the owner, or one tile click through the pairing. The app should not do this on
     its own: it is a global preference that stays on if the app dies.
   - **Test:** E2.
3. **Removing G2 from Game Turbo (4).** This removes the regulator at the source; HyperOsGameMode would
   then not need to stop Joyose. Its thermal effect is unknown. Test: E4.
4. **The rest:** no clock effect on this build (2, 3, 5, 6, 7, 8, 12), or they only lower clocks (9, 10).

## Device experiments

Common baseline:

- Duckov title screen, half-resolution session, setup as in HANDOFF 5c.
- Each run is 120 s, counting fps from the compositor's zero-copy frames.
- Record:
  - The clocks once a second:
    ```
    adb shell 'while :; do for p in 0 4 8; do d=/sys/devices/system/cpu/cpufreq/policy$p; printf "%s/%s/%s " $(cat $d/scaling_min_freq $d/scaling_max_freq $d/scaling_cur_freq); done; echo; sleep 1; done'
    ```
  - Before and after, the thermal scenario and IPA levels:
    `adb shell dumpsys activity service com.miui.powerkeeper/.thermal.ThermalService | sed -n '1,25p;/IPA INFO/,/ThermalService end/p'`
  - The GPU clock, from the perfetto `gpufreq` counters or `mali-g2/tools/mali-counters.py`.
  - The battery temperature (`dumpsys battery`). Runs are only comparable at similar temperatures.
- Check first that nothing lowers clocks: `settings get system POWER_SAVE_MODE_OPEN` and
  `settings get global low_power` must both be 0 or null.

### E1: what MiBridge levels do, from the shell (no new APK)

```
R8_JAR=/path/to/r8.jar mali-g2/tools/mibridge-probe/build.sh     # or use a prebuilt mbprobe.dex
adb push mali-g2/tools/mibridge-probe/mbprobe.dex /data/local/tmp/
adb shell 'settings get system mibridge_authorized_pkg_list > /data/local/tmp/mibridge.orig; cat /data/local/tmp/mibridge.orig'
adb shell 'settings put system mibridge_authorized_pkg_list "$(cat /data/local/tmp/mibridge.orig),com.android.shell"'
P='CLASSPATH=/data/local/tmp/mbprobe.dex:/system_ext/framework/MiuiBooster.jar app_process /system/bin MbProbe'
adb shell "$P check"                      # expect: checkPermission(com.android.shell, 2000) = true
adb shell "$P sample 10"                  # baseline
adb shell "$P hold cpu 1 20"; adb shell "$P hold cpu 3 20"
adb shell "$P hold gpu 1 30"              # in a running session, watch the GPU clock and fps meanwhile
adb shell "$P hold gpu 3 30"; adb shell "$P hold ddr 1 30"
adb shell dumpsys scenariorecognition | sed -n '/MirimThirdPartyPolicy:/,/Local Log/p'   # handles while holding
adb logcat -d -s MirimThirdPartyFreq MirimThirdPartyAuthRegistry MiBridge | tail -40
```

- **Restore:**
  `adb shell 'settings put system mibridge_authorized_pkg_list "$(cat /data/local/tmp/mibridge.orig)"; rm /data/local/tmp/mibridge.orig /data/local/tmp/mbprobe.dex'`.
  If the original value was `null`, use `settings delete system mibridge_authorized_pkg_list` instead.
- **Success** means that while holding, `scaling_min_freq` of some policy rises (CPU), or the GPU clock floor
  rises and the fps of a GPU-bound scene goes up (GPU).
- If `check` is false, the list was probably not reread yet. Wait a second, then look for
  `MirimThirdPartyAuthRegistry: reload pkgs=` in logcat.

### E2: Performance mode

- **On:** with the device unlocked,
  `adb shell cmd statusbar click-tile com.miui.securitycenter/com.miui.powercenter.powersaver.PerformanceModeTileService`.
  The owner can also use Settings → Battery. Then verify:
  - `adb shell settings get system POWER_PERFORMANCE_MODE_OPEN` → 1;
  - `adb shell settings get system speed_mode` → 1;
  - ThermalService `PowerMode : 98` and a different `current ScenarioName`.
- **Run:** A/B: three runs on and three off, alternating, with a cold start each.
- **Compare:** fps, the `scaling_max_freq` distribution of each policy, the IPA levels, and the GPU clock.
- **Restore:** run the same `click-tile` command again (or Settings → Battery → Balanced). Verify
  `POWER_PERFORMANCE_MODE_OPEN` → 0 and `PowerMode : 99`.
- If the tile does not toggle because the shell cannot click tiles on this build, ask the owner to use the
  battery settings.

### E3: MiBridgeBoost in the app (needs an APK with this branch)

```
export DROIDDECK_PACKAGE=com.droiddeck.launcher.g2
tools/droiddeckctl prefs set --type string session miBridgeBoost gpu=1   # then gpu=3, cpu=1, "gpu=1,ddr=1"; "off" for none
# session log / logcat -s MiBridgeBoost: "added to mibridge_authorized_pkg_list", "gpu level 1: granted"
# The session's start line only says what is asked for; granted or refused is in the MiBridgeBoost lines.
```

- **Run:** A/B against `off`, as in E2.
- **Restore:** `tools/droiddeckctl prefs set --type remove session miBridgeBoost` goes back to the default
  (on, on the O3). With `off`, the app takes its package out of `mibridge_authorized_pkg_list` again at the next
  session start (log `taken out of mibridge_authorized_pkg_list again`).
- **Check afterwards (with `off`):** `adb shell settings get system mibridge_authorized_pkg_list` should no
  longer contain `com.droiddeck.launcher.g2`.
- The setting needs the wireless debugging pairing, like HyperOsGameMode. When mDNS does not announce the
  pairing's port (seen on this build: logcat `MiBridgeBoost: no wireless debugging pairing, or wireless
  debugging is off`), the app finds it by trying loopback ports in the kernel's ephemeral range (logcat
  `WirelessAdbFix: loopback ports ... answering as wireless debugging`).
- **Measured** (fullscreen, half-resolution Duckov title, on battery): two 20-minute runs, the default
  `cpu=1,gpu=1,ddr=1` against `off`, gave 82.3±0.8 against 61.4±0.3 fps (per-minute mean ± SE), main thread
  10.64 against 13.85 ms a frame, the same energy per frame (0.141 against 0.139 J), board 42.6 against 37.5 °C.
  GPU and DDR were granted at every renewal (GPU floor and top 979 MHz, DDR 9600 MHz); CPU was refused
  (`thermal break ... deny CPU`) whenever `board_sensor_temp` was 40 °C or more, which is most of a session.

### E4: G2 out of Game Turbo (owner's UI action)

- **Owner:** Game Turbo → its app list → remove DroidDeck G2.
- **Check:**
  - `settings get secure gb_boosting_pkg_name` no longer names G2 after the next launch;
  - ThermalService `ForegroundGroup` is no longer 16 during a session;
  - no `setSchedGeniusIfNeed start-game` for G2 in `logcat -s FrameSchedPolicy`.
- **Run:** A/B against the current setup (in the list, with HyperOsGameMode and FrameSchedOptOut on).
- **Restore:** add G2 back in Game Turbo.

## Sketch, not implemented: performance mode from the app

```kotlin
// At a session's start, only if the owner opted in and the mode is off:
WirelessAdbFix.connectUsingSavedPairing(context)?.use { adb ->
    if (adb.shell("settings get system POWER_PERFORMANCE_MODE_OPEN").output.trim() != "1") {
        adb.shell("cmd statusbar click-tile com.miui.securitycenter/com.miui.powercenter.powersaver.PerformanceModeTileService")
        prefs.edit().putBoolean("perfModeByUs", true).commit() // click again at the session's end
    }
}
```

This is left out until E2 shows a gain. Even then the owner should opt in: the mode is global, visible to
the owner, and stays on if the app dies before its end-of-session click.
