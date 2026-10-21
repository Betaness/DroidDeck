package com.droiddeck.launcher.core

import android.content.Context
import android.os.Build
import com.droiddeck.launcher.R
import java.io.File

object DeviceSupport {
    fun adreno(): Boolean =
        File("/sys/class/kgsl/kgsl-3d0").exists() || File("/vendor/lib64/hw/vulkan.adreno.so").exists()

    /** [mali-g2] Adreno, or a device whose vendor Vulkan driver this build forwards to the guest with Vortek. */
    fun supported(context: Context): Boolean =
        adreno() || com.droiddeck.launcher.session.VortekServerComponent.available(context)

    /** [mali-g2] No Adreno, but a vendor Vulkan HAL (Mali): the compositor must use the system driver. */
    fun vendorVulkanOnly(): Boolean =
        !adreno() && com.droiddeck.launcher.session.VortekServerComponent.halDriver() != null

    fun gpuName(context: Context): String {
        val soc = if (Build.VERSION.SDK_INT >= 31) Build.SOC_MODEL.takeIf { it.isNotBlank() && it != Build.UNKNOWN } else null
        return soc?.let { "$it (${Build.HARDWARE})" } ?: Build.HARDWARE.ifBlank { context.getString(R.string.gpu_this_gpu) }
    }

    fun lowLatencyWifiSafe(): Boolean = Build.VERSION.SDK_INT < 31 || Build.SOC_MODEL != "CQ8725S"
}
