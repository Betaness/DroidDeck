# mibridge-probe

Asks HyperOS's MiBridge (MiuiBooster.jar → system_server MirimThirdPartyPolicy → uniperf) for CPU, GPU or
DDR frequency levels from an adb shell, and prints the CPU clock limits meanwhile. The point is to find
out what the levels do before the app uses them (`MiBridgeBoost`).

For background and the full experiment, including how to restore the setting the probe needs, see E1 in
[../../docs/hyperos-clock-knobs.md](../../docs/hyperos-clock-knobs.md).

```
R8_JAR=/path/to/r8.jar ./build.sh        # javac + D8 → mbprobe.dex
adb push mbprobe.dex /data/local/tmp/
# com.android.shell must be in `settings get system mibridge_authorized_pkg_list` (see E1)
adb shell CLASSPATH=/data/local/tmp/mbprobe.dex:/system_ext/framework/MiuiBooster.jar \
    app_process /system/bin MbProbe hold gpu 1 30
```
