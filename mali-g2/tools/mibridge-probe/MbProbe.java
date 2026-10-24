import java.io.BufferedReader;
import java.io.FileReader;
import java.lang.reflect.Method;

/**
 * Asks Xiaomi's MiBridge (com.miui.performance.MiuiBooster, served by system_server's
 * MirimThirdPartyPolicy through uniperf) for a CPU, GPU or DDR frequency floor from an adb shell,
 * and prints the CPU clock limits meanwhile. Only packages listed in the system setting
 * mibridge_authorized_pkg_list get anything: add com.android.shell there first (see README.md).
 *
 * adb push mbprobe.dex /data/local/tmp/
 * adb shell CLASSPATH=/data/local/tmp/mbprobe.dex:/system_ext/framework/MiuiBooster.jar \
 *     app_process /system/bin MbProbe <command>
 *
 *   check [package]                 checkPermission (default package com.android.shell)
 *   sample <seconds>                CPU policy min/max/cur once a second, no request
 *   hold <cpu|gpu|ddr> <level> <s>  request level 1..3, renewed before its timeout, for s seconds
 */
public final class MbProbe {
    private static final int[] POLICIES = {0, 4, 8};
    /** The longest timeout system_server grants per level (ThirdPartyFreqController.capTimeout). */
    private static final int[] MAX_TIMEOUT_MS = {0, 10_000, 5_000, 1_500};

    public static void main(String[] args) throws Exception {
        if (args.length == 0) {
            System.out.println("usage: check [pkg] | sample <s> | hold <cpu|gpu|ddr> <level> <s>");
            return;
        }
        switch (args[0]) {
            case "sample":
                for (int i = 0; i < Integer.parseInt(args[1]); i++) {
                    System.out.println(clocks());
                    Thread.sleep(1000);
                }
                return;
            case "check": {
                Object booster = booster();
                String pkg = args.length > 1 ? args[1] : "com.android.shell";
                System.out.println("checkPermission(" + pkg + ", " + uid() + ") = " + check(booster, pkg));
                return;
            }
            case "hold": {
                Object booster = booster();
                String kind = args[1];
                int level = Integer.parseInt(args[2]);
                long until = System.currentTimeMillis() + Long.parseLong(args[3]) * 1000;
                if (!check(booster, "com.android.shell")) {
                    System.out.println("not authorized: is com.android.shell in mibridge_authorized_pkg_list?");
                    return;
                }
                String suffix = kind.equals("cpu") ? "Cpu" : kind.equals("gpu") ? "Gpu" : "Ddr";
                Method request = booster.getClass().getMethod("request" + suffix + "HighFreq", int.class, int.class, int.class);
                Method cancel = booster.getClass().getMethod("cancel" + suffix + "HighFreq", int.class);
                int timeout = MAX_TIMEOUT_MS[level];
                long renewAt = 0;
                try {
                    while (System.currentTimeMillis() < until) {
                        if (System.currentTimeMillis() >= renewAt) {
                            Object r = request.invoke(booster, uid(), level, timeout);
                            System.out.println("request" + suffix + "HighFreq(level " + level + ", " + timeout + " ms) = " + r);
                            renewAt = System.currentTimeMillis() + timeout - 300;
                        }
                        System.out.println(clocks());
                        Thread.sleep(Math.min(1000, timeout / 2));
                    }
                } finally {
                    System.out.println("cancel" + suffix + "HighFreq = " + cancel.invoke(booster, uid()));
                }
                return;
            }
            default:
                System.out.println("unknown command " + args[0]);
        }
    }

    private static Object booster() throws Exception {
        return Class.forName("com.miui.performance.MiuiBooster").getConstructor().newInstance();
    }

    private static boolean check(Object booster, String pkg) throws Exception {
        Method m = booster.getClass().getMethod("checkPermission", String.class, int.class);
        return (Boolean) m.invoke(booster, pkg, uid());
    }

    private static int uid() throws Exception {
        return (Integer) Class.forName("android.os.Process").getMethod("myUid").invoke(null);
    }

    private static String clocks() {
        StringBuilder sb = new StringBuilder();
        for (int p : POLICIES) {
            String dir = "/sys/devices/system/cpu/cpufreq/policy" + p + "/";
            sb.append("p").append(p).append(' ')
                .append(read(dir + "scaling_min_freq")).append('/')
                .append(read(dir + "scaling_max_freq")).append(" cur ")
                .append(read(dir + "scaling_cur_freq")).append("  ");
        }
        return sb.toString().trim();
    }

    private static String read(String path) {
        try (BufferedReader r = new BufferedReader(new FileReader(path))) {
            return r.readLine().trim();
        } catch (Exception e) {
            return "?";
        }
    }
}
