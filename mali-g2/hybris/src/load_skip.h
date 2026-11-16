/*
 * [mali-g2] DROIDDECK_HYBRIS_LOAD_SKIP (cmd_stats.c): the pure parts, no state, run on the host by
 * tests/load_skip_test.c too.
 *
 * ls_cover decides whether the triangles of one draw cover every pixel centre of the render area.
 * The vertices are the vertex shader's clip-space positions (spirv_eval.h runs it on the CPU). It
 * accepts two shapes it can prove:
 *   - one triangle that contains the four corner pixel centres, or
 *   - two triangles sharing an edge (the same two clip-space positions, bit for bit) whose union is
 *     a convex quadrilateral that contains them. A pixel centre on the shared edge belongs to exactly
 *     one of the two (Vulkan's rule for polygons with a common edge).
 * Each corner must lie at least LS_MARGIN pixels inside every outer edge, so that the GPU's
 * sub-pixel snapping and arithmetic that differs from the CPU's cannot move an edge past a pixel
 * centre; a corner closer than that (or on the edge) gives LS_COVER_EDGE. Every vertex needs
 * w > 0 and 0 <= z <= w (-w <= z <= w with negativeOneToOne), so no triangle is clipped by the near
 * or far plane. Culled and zero-area triangles do not count.
 *
 * ls_viewport_covers: the viewport (after the y flip of a negative height) contains every pixel
 * centre of the area, a centre on its left or top edge included. DXVK's D3D9 viewports start half
 * a pixel in, on the first column's and row's centres.
 *
 * ls_decode: one vertex attribute in memory as the vertex input interface hands it to the shader.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_LOAD_SKIP_H
#define DROIDDECK_LOAD_SKIP_H

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan_core.h>

#include "spirv_eval.h"

#define LS_MARGIN (1.0 / 16.0)

enum { LS_COVER_YES, LS_COVER_EDGE, LS_COVER_NO, LS_COVER_CLIP };

typedef struct {
    double x, y;
} LsPoint;

/* without libm (the ICD does not link it): a first guess from the exponent, then Newton */
static inline double ls_sqrt(double x) {
    if (!(x > 0)) return 0;
    uint64_t bits;
    memcpy(&bits, &x, 8);
    bits = (bits >> 1) + (1023ull << 51);
    double r;
    memcpy(&r, &bits, 8);
    for (int i = 0; i < 6; i++) r = 0.5 * (r + x / r);
    return r;
}

static inline double ls_cross(LsPoint o, LsPoint a, LsPoint b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

/* The smallest distance, in pixels, by which the four points lie inside the convex polygon
 * (negative: outside). The polygon's vertices are in order, either winding. */
static inline double ls_inside(const LsPoint* poly, int n, const LsPoint* pts, int m) {
    double area = 0;
    for (int i = 0; i < n; i++) area += ls_cross((LsPoint){0, 0}, poly[i], poly[(i + 1) % n]);
    double s = area > 0 ? 1.0 : -1.0, best = 1e300;
    for (int i = 0; i < n; i++) {
        LsPoint a = poly[i], b = poly[(i + 1) % n];
        double len2 = (b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y);
        if (len2 <= 0) return -1e300;
        double len = ls_sqrt(len2);
        for (int j = 0; j < m; j++) {
            double d = s * ls_cross(a, b, pts[j]) / len;
            if (d < best) best = d;
        }
    }
    return best;
}

static inline bool ls_convex(const LsPoint* poly, int n) {
    int sign = 0;
    for (int i = 0; i < n; i++) {
        double c = ls_cross(poly[i], poly[(i + 1) % n], poly[(i + 2) % n]);
        int s = c > 0 ? 1 : c < 0 ? -1 : 0;
        if (!s) return false;
        if (sign && s != sign) return false;
        sign = s;
    }
    return true;
}

static inline bool ls_same(const float* a, const float* b) {
    return !memcmp(a, b, 4 * sizeof(float));
}

/* positions: count clip-space vec4; tris: triCount triples of indices into them, in the order the
 * rasterizer sees their vertices (strips already alternated). */
static inline int ls_cover(const float (*positions)[4], uint32_t count, const uint32_t (*tris)[3], uint32_t triCount,
                           const VkViewport* vp, VkRect2D area, VkCullModeFlags cull, VkFrontFace front, bool negativeOneToOne) {
    enum { MAX = 16 };
    if (!triCount || triCount > MAX || area.extent.width == 0 || area.extent.height == 0) return LS_COVER_NO;
    LsPoint fb[3 * MAX];
    uint32_t live[MAX], liveCount = 0;
    for (uint32_t t = 0; t < triCount; t++) {
        for (int k = 0; k < 3; k++) {
            uint32_t v = tris[t][k];
            if (v >= count) return LS_COVER_NO;
            const float* p = positions[v];
            double x = p[0], y = p[1], z = p[2], w = p[3];
            /* finite and well inside any guard band; the comparisons are false for NaN */
            if (!(w > 1e-20) || !(w < 1e30) || !(fabs(x) <= 1e6 * w) || !(fabs(y) <= 1e6 * w) || !(z <= w) ||
                !(negativeOneToOne ? z >= -w : z >= 0))
                return LS_COVER_CLIP;
            fb[3 * t + k] = (LsPoint){vp->x + vp->width * 0.5 * (x / w + 1.0), vp->y + vp->height * 0.5 * (y / w + 1.0)};
        }
        /* Vulkan: a = -1/2 sum(x_i y_{i+1} - x_{i+1} y_i); front facing when a > 0 with
         * VK_FRONT_FACE_COUNTER_CLOCKWISE, a < 0 with CLOCKWISE */
        double a = -0.5 * ls_cross(fb[3 * t], fb[3 * t + 1], fb[3 * t + 2]);
        if (a == 0) continue;
        bool frontFacing = front == VK_FRONT_FACE_COUNTER_CLOCKWISE ? a > 0 : a < 0;
        if ((cull & VK_CULL_MODE_FRONT_BIT) && frontFacing) continue;
        if ((cull & VK_CULL_MODE_BACK_BIT) && !frontFacing) continue;
        live[liveCount++] = t;
    }
    double x0 = area.offset.x + 0.5, x1 = area.offset.x + area.extent.width - 0.5;
    double y0 = area.offset.y + 0.5, y1 = area.offset.y + area.extent.height - 0.5;
    const LsPoint corners[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    double best = -1e300;
    for (uint32_t i = 0; i < liveCount; i++) {
        double d = ls_inside(&fb[3 * live[i]], 3, corners, 4);
        if (d > best) best = d;
    }
    for (uint32_t i = 0; i < liveCount; i++)
        for (uint32_t j = i + 1; j < liveCount; j++) {
            const uint32_t* a = tris[live[i]];
            const uint32_t* b = tris[live[j]];
            /* shared vertices by position */
            int ai[2], bi[2], shared = 0;
            for (int u = 0; u < 3; u++)
                for (int v = 0; v < 3; v++)
                    if (ls_same(positions[a[u]], positions[b[v]])) {
                        if (shared < 2) ai[shared] = u, bi[shared] = v;
                        shared++;
                    }
            if (shared != 2) continue;
            int ao = 3 - ai[0] - ai[1], bo = 3 - bi[0] - bi[1];
            LsPoint A = fb[3 * live[i] + ai[0]], C = fb[3 * live[i] + ai[1]];
            LsPoint B = fb[3 * live[i] + ao], D = fb[3 * live[j] + bo];
            double sb = ls_cross(A, C, B), sd = ls_cross(A, C, D);
            if (!((sb > 0 && sd < 0) || (sb < 0 && sd > 0))) continue;
            const LsPoint quad[4] = {A, B, C, D};
            if (!ls_convex(quad, 4)) continue;
            double d = ls_inside(quad, 4, corners, 4);
            if (d > best) best = d;
        }
    if (best >= LS_MARGIN) return LS_COVER_YES;
    if (best > -LS_MARGIN) return LS_COVER_EDGE;
    return LS_COVER_NO;
}

static inline bool ls_viewport_covers(const VkViewport* vp, VkRect2D area) {
    double vx0 = vp->x, vx1 = vp->x + vp->width;
    double vy0 = vp->height < 0 ? vp->y + vp->height : vp->y, vy1 = vp->height < 0 ? vp->y : vp->y + vp->height;
    double cx0 = area.offset.x + 0.5, cx1 = area.offset.x + area.extent.width - 0.5;
    double cy0 = area.offset.y + 0.5, cy1 = area.offset.y + area.extent.height - 0.5;
    return vp->width > 0 && vx0 <= cx0 && cx1 < vx1 && vy0 <= cy0 && cy1 < vy1;
}

static inline bool ls_scissor_covers(const VkRect2D* s, VkRect2D area) {
    int64_t sx1 = (int64_t)s->offset.x + s->extent.width, sy1 = (int64_t)s->offset.y + s->extent.height;
    int64_t ax1 = (int64_t)area.offset.x + area.extent.width, ay1 = (int64_t)area.offset.y + area.extent.height;
    return s->offset.x <= area.offset.x && s->offset.y <= area.offset.y && sx1 >= ax1 && sy1 >= ay1;
}

static inline float ls_half(uint16_t h) {
    uint32_t sign = (uint32_t)(h >> 15) << 31, exp = (h >> 10) & 0x1f, man = h & 0x3ff, bits;
    if (exp == 0x1f) {
        bits = sign | 0x7f800000u | (man << 13);
    } else if (exp) {
        bits = sign | ((exp + 112) << 23) | (man << 13);
    } else if (man) {
        int e = -1;
        do {
            man <<= 1;
            e++;
        } while (!(man & 0x400));
        bits = sign | ((uint32_t)(112 - e) << 23) | ((man & 0x3ff) << 13);
    } else {
        bits = sign;
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static inline uint32_t ls_fbits(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

/* Bytes the format takes and *out filled as the shader sees it ((0, 0, 0, 1) for missing
 * components); 0 for a format not decoded here. */
static inline uint32_t ls_decode(VkFormat format, const uint8_t* src, SpvAttr* out) {
    uint32_t comps = 0, size = 0, kind = SPV_ATTR_FLOAT;
    enum { F32, I32, F16, UN8, SN8, US8, SS8, UI8, SI8, UN16, SN16, US16, SS16, UI16, SI16 } enc;
    bool bgra = false;
    switch (format) {
    case VK_FORMAT_R32_SFLOAT: enc = F32, comps = 1; break;
    case VK_FORMAT_R32G32_SFLOAT: enc = F32, comps = 2; break;
    case VK_FORMAT_R32G32B32_SFLOAT: enc = F32, comps = 3; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: enc = F32, comps = 4; break;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32A32_UINT:
        enc = I32, kind = SPV_ATTR_UINT, comps = 1 + (format - VK_FORMAT_R32_UINT) / 3;
        break;
    case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        enc = I32, kind = SPV_ATTR_SINT, comps = 1 + (format - VK_FORMAT_R32_SINT) / 3;
        break;
    case VK_FORMAT_R16_SFLOAT: enc = F16, comps = 1; break;
    case VK_FORMAT_R16G16_SFLOAT: enc = F16, comps = 2; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: enc = F16, comps = 4; break;
    case VK_FORMAT_B8G8R8A8_UNORM: enc = UN8, comps = 4, bgra = true; break;
    case VK_FORMAT_R8G8B8A8_UNORM: enc = UN8, comps = 4; break;
    case VK_FORMAT_R8G8B8A8_SNORM: enc = SN8, comps = 4; break;
    case VK_FORMAT_R8G8B8A8_USCALED: enc = US8, comps = 4; break;
    case VK_FORMAT_R8G8B8A8_SSCALED: enc = SS8, comps = 4; break;
    case VK_FORMAT_R8G8B8A8_UINT: enc = UI8, kind = SPV_ATTR_UINT, comps = 4; break;
    case VK_FORMAT_R8G8B8A8_SINT: enc = SI8, kind = SPV_ATTR_SINT, comps = 4; break;
    case VK_FORMAT_R16G16_UNORM: enc = UN16, comps = 2; break;
    case VK_FORMAT_R16G16_SNORM: enc = SN16, comps = 2; break;
    case VK_FORMAT_R16G16_USCALED: enc = US16, comps = 2; break;
    case VK_FORMAT_R16G16_SSCALED: enc = SS16, comps = 2; break;
    case VK_FORMAT_R16G16_UINT: enc = UI16, kind = SPV_ATTR_UINT, comps = 2; break;
    case VK_FORMAT_R16G16_SINT: enc = SI16, kind = SPV_ATTR_SINT, comps = 2; break;
    case VK_FORMAT_R16G16B16A16_UNORM: enc = UN16, comps = 4; break;
    case VK_FORMAT_R16G16B16A16_SNORM: enc = SN16, comps = 4; break;
    case VK_FORMAT_R16G16B16A16_USCALED: enc = US16, comps = 4; break;
    case VK_FORMAT_R16G16B16A16_SSCALED: enc = SS16, comps = 4; break;
    case VK_FORMAT_R16G16B16A16_UINT: enc = UI16, kind = SPV_ATTR_UINT, comps = 4; break;
    case VK_FORMAT_R16G16B16A16_SINT: enc = SI16, kind = SPV_ATTR_SINT, comps = 4; break;
    default: return 0;
    }
    uint32_t one = kind == SPV_ATTR_FLOAT ? ls_fbits(1.0f) : 1;
    out->kind = (uint8_t)kind;
    out->value[0] = out->value[1] = out->value[2] = 0;
    out->value[3] = one;
    for (uint32_t i = 0; i < comps; i++) {
        uint32_t s = bgra && i < 3 ? 2 - i : i;
        uint32_t v = 0;
        switch (enc) {
        case F32: case I32: memcpy(&v, src + 4 * s, 4); size = 4 * comps; break;
        case F16: {
            uint16_t h;
            memcpy(&h, src + 2 * s, 2);
            v = ls_fbits(ls_half(h));
            size = 2 * comps;
            break;
        }
        case UN8: v = ls_fbits(src[s] / 255.0f); size = 4; break;
        case SN8: {
            float f = (int8_t)src[s] / 127.0f;
            v = ls_fbits(f < -1.0f ? -1.0f : f);
            size = 4;
            break;
        }
        case US8: v = ls_fbits((float)src[s]); size = 4; break;
        case SS8: v = ls_fbits((float)(int8_t)src[s]); size = 4; break;
        case UI8: v = src[s]; size = 4; break;
        case SI8: v = (uint32_t)(int32_t)(int8_t)src[s]; size = 4; break;
        default: {
            uint16_t h;
            memcpy(&h, src + 2 * s, 2);
            size = 2 * comps;
            if (enc == UN16) v = ls_fbits(h / 65535.0f);
            else if (enc == SN16) {
                float f = (int16_t)h / 32767.0f;
                v = ls_fbits(f < -1.0f ? -1.0f : f);
            } else if (enc == US16) v = ls_fbits((float)h);
            else if (enc == SS16) v = ls_fbits((float)(int16_t)h);
            else if (enc == UI16) v = h;
            else v = (uint32_t)(int32_t)(int16_t)h;
            break;
        }
        }
        out->value[i] = v;
    }
    return size;
}

#endif
