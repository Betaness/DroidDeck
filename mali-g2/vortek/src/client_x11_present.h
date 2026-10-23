/*
 * Software X11 presentation for the standalone Vortek client (glibc side).
 * Hooked into vortek/src/vulkan_calls.c under #ifdef VORTEK_STANDALONE
 * (patches/vortek-0002-x11-present.patch). See client_x11_present.c.
 *
 * Copyright (C) 2026 mali-v15-bringup contributors.
 * Based on Vortek (C) brunodev85, LGPL-2.1.
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef VORTEK_CLIENT_X11_PRESENT_H
#define VORTEK_CLIENT_X11_PRESENT_H

#include "vortek.h"

/* Surfaces: the VkObject id stays the X window id (what the server sees);
 * the X connection is kept in the object's tag. */
extern void X11Present_initXlibSurface(VkObject* surfaceObject, Display* dpy, Window window);
extern VkResult vt_call_vkCreateXcbSurfaceKHR(VkInstance instance, const VkXcbSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
extern VkBool32 vt_call_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, xcb_connection_t* connection, xcb_visualid_t visual_id);
extern void X11Present_destroySurface(VkObject* surfaceObject);

/* Report the X window's current size instead of the server's fixed one. */
extern void X11Present_fixSurfaceCapabilities(VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* capabilities);

/* Bookkeeping: device -> physical device, queue -> family. */
extern void X11Present_onCreateDevice(VkPhysicalDevice physicalDevice, VkDevice device);
extern void X11Present_onGetDeviceQueue(VkQueue queue, uint32_t queueFamilyIndex);

/* Swapchain lifetime; must be called without the vt_call lock held. */
extern void X11Present_onCreateSwapchain(VkDevice device, const VkSwapchainCreateInfoKHR* createInfo, VkSwapchainKHR swapchain);
extern void X11Present_onDestroySwapchain(VkDevice device, VkSwapchainKHR swapchain);

/* Called at the start of vkQueuePresentKHR (lock not held): copies every
 * presented image to its X window. Consumes the wait semaphores. Returns the
 * overall result; per-swapchain results go to results[] (may be NULL). */
extern VkResult X11Present_present(VkQueue queue, const VkPresentInfoKHR* presentInfo, VkResult* results);

#endif
