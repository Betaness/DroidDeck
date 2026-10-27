/*
 * [mali-g2] Row pitch of one-row LINEAR images. Shared by the Vortek client (droiddeck_maint.c) and
 * the in-process libhybris path (mali-g2/hybris).
 *
 * The Mali driver (r56p0 on the Mali-G2) answers vkGetImageSubresourceLayout with rowPitch 0 for any
 * subresource one row high: 1D images, 2D images and mip levels of height 1, 3D images of height 1.
 * Its depthPitch, arrayPitch and size are right, and for such a subresource the smallest non-zero
 * of them is the aligned row (16x1 RGBA8: 64, 300x1 R8: 320 as for 300x2, 3D 16x1x4: depthPitch 64).
 * Apps copy rows by rowPitch (DXVK's mapped D3D11 staging textures give it to the game as RowPitch),
 * so a 0 loses the data. Subresources of more rows have their pitch. tests/mali_quirks_test.c
 * ("linear") shows the driver's answers.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_LINEAR_ROW_PITCH_H
#define DROIDDECK_LINEAR_ROW_PITCH_H

#include <vulkan/vulkan.h>

/* Returns 1 when the layout was changed. */
static int dd_fix_linear_row_pitch(VkSubresourceLayout* l) {
    if (l->rowPitch || !l->size) return 0;
    VkDeviceSize p = l->size;
    if (l->arrayPitch && l->arrayPitch < p) p = l->arrayPitch;
    if (l->depthPitch && l->depthPitch < p) p = l->depthPitch;
    l->rowPitch = p;
    return 1;
}

#endif
