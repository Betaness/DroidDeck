/*
 * [mali-g2] Removes ClipDistance / CullDistance stores from SPIR-V. Shared by the Vortek client
 * (droiddeck_maint.c) and the in-process libhybris path (mali-g2/hybris/src/mali_fixes.c).
 *
 * The Mali shader compiler (cmpbe_v2_compile_multiple_shaders) segfaults, taking the whole Vortek
 * server down, on any store to a ClipDistance / CullDistance member of an output block, and in
 * pipeline libraries also to a standalone ClipDistance variable; the driver reports
 * maxClipDistances = maxCullDistances = 0 anyway. DXVK's D3D9 fixed-function vertex shader writes
 * them (A.V.A., 2026-10-23), so do D3D11 SV_ClipDistance shaders. Those stores, and the access
 * chains that only fed them, become OpNop (same length, still valid SPIR-V): user clip planes do
 * nothing, as with the device's limits of 0. VORTEK_KEEP_CLIP_DISTANCE=1 keeps the shaders as they are.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_SPIRV_CLIP_CULL_H
#define DROIDDECK_SPIRV_CLIP_CULL_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { OpNop = 0, OpTypeStruct = 30, OpTypePointer = 32, OpConstant = 43, OpVariable = 59, OpStore = 62,
       OpAccessChain = 65, OpInBoundsAccessChain = 66, OpDecorate = 71, OpMemberDecorate = 72,
       OpCopyObject = 83, OpFunction = 54, OpLoad = 61, OpExtInst = 12, OpSwitch = 251, OpLine = 8, OpNoLine = 317,
       OpName = 5, DecBuiltIn = 11, BuiltInClipDistance = 3, BuiltInCullDistance = 4 };

typedef struct { uint32_t structId, member; } Member;

/* A rewritten copy (free() it) of `words` words of SPIR-V, or NULL when nothing had to change. */
static uint32_t* dd_strip_clip_cull(const uint32_t* code, size_t words, size_t* outWords) {
    if (words < 5 || code[0] != 0x07230203u) return NULL;
    uint32_t bound = code[3];
    Member members[16];
    int nmembers = 0;
    uint32_t vars[16];
    int nvars = 0;
    /* pass 1: clip/cull block members */
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (n == 0 || i + n > words) return NULL;
        if (op == OpMemberDecorate && n >= 5 && code[i + 3] == DecBuiltIn &&
            (code[i + 4] == BuiltInClipDistance || code[i + 4] == BuiltInCullDistance) && nmembers < 16)
            members[nmembers++] = (Member){code[i + 1], code[i + 2]};
        else if (op == OpDecorate && n >= 4 && code[i + 2] == DecBuiltIn &&
                 (code[i + 3] == BuiltInClipDistance || code[i + 3] == BuiltInCullDistance) && nvars < 16)
            vars[nvars++] = code[i + 1];
        i += n;
    }
    if (!nmembers && !nvars) return NULL;

    uint32_t* w = malloc(words * 4);
    uint32_t* constVal = calloc(bound, 4);     /* OpConstant value + 1 (0 = not a constant) */
    uint32_t* pointee = calloc(bound, 4);      /* pointer type -> pointee type */
    uint32_t* varStruct = calloc(bound, 4);    /* variable -> pointee struct type */
    uint8_t* tainted = calloc(bound, 1);       /* pointer into a clip/cull member */
    uint32_t* uses = calloc(bound, 4);
    uint8_t* removed = calloc(bound, 1);
    if (!w || !constVal || !pointee || !varStruct || !tainted || !uses || !removed) goto fail;
    memcpy(w, code, words * 4);
    for (int v = 0; v < nvars; v++) if (vars[v] < bound) tainted[vars[v]] = 1;

    for (size_t i = 5; i < words; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if (op == OpConstant && n >= 4 && w[i + 2] < bound) constVal[w[i + 2]] = w[i + 3] + 1;
        else if (op == OpTypePointer && n >= 4 && w[i + 1] < bound) pointee[w[i + 1]] = w[i + 3];
        else if (op == OpVariable && n >= 4 && w[i + 2] < bound && w[i + 1] < bound) varStruct[w[i + 2]] = pointee[w[i + 1]];
        else if ((op == OpAccessChain || op == OpInBoundsAccessChain) && n >= 4) {
            uint32_t res = w[i + 2], base = w[i + 3];
            if (res >= bound || base >= bound) continue;
            if (tainted[base]) tainted[res] = 1;
            else if (n >= 5 && w[i + 4] < bound && constVal[w[i + 4]]) {
                for (int m = 0; m < nmembers; m++)
                    if (varStruct[base] == members[m].structId && constVal[w[i + 4]] - 1 == members[m].member) tainted[res] = 1;
            }
        }
        else if (op == OpCopyObject && n >= 4 && w[i + 3] < bound && tainted[w[i + 3]] && w[i + 2] < bound) tainted[w[i + 2]] = 1;
    }

    int changed = 0;
    for (size_t i = 5; i < words; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16;
        if ((w[i] & 0xffff) == OpStore && n >= 3 && w[i + 1] < bound && tainted[w[i + 1]]) {
            for (uint32_t j = 0; j < n; j++) w[i + j] = (1u << 16) | OpNop;
            changed = 1;
        }
    }
    /* the access chains (and copies) that fed only those stores: counted as uses are the id
     * operands of instructions inside functions (literal operands are skipped, a literal equal to
     * a chain's id would keep the chain, and a chain left pointing at the built-in still crashes) */
    for (int again = 1; again;) {
        again = 0;
        memset(uses, 0, bound * 4);
        int inFunction = 0;
        for (size_t i = 5; i < words; i += w[i] >> 16) {
            uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
            if (op == OpFunction) inFunction = 1;
            if (!inFunction || op == OpNop || op == OpLine || op == OpNoLine) continue;
            uint32_t first = 1, last = n;                      /* operand words [first, last) to count */
            if (op == OpLoad) { first = 3; last = n < 4 ? n : 4; }
            else if (op == OpStore) { first = 1; last = n < 3 ? n : 3; }
            else if (op == OpExtInst) { first = 3; }           /* word 4 (instruction number) skipped below */
            else if (op == OpSwitch) { first = 1; last = n < 3 ? n : 3; }
            else if (op == OpAccessChain || op == OpInBoundsAccessChain || op == OpCopyObject) first = 3;
            for (uint32_t j = first; j < last; j++) {
                if (op == OpExtInst && j == 4) continue;
                if (w[i + j] < bound) uses[w[i + j]]++;
            }
            if (op == OpSwitch) for (uint32_t j = 4; j < n; j += 2) if (w[i + j] < bound) uses[w[i + j]]++;  /* labels */
        }
        for (size_t i = 5; i < words; i += w[i] >> 16) {
            uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
            if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpCopyObject) && n >= 4 &&
                w[i + 2] < bound && tainted[w[i + 2]] && uses[w[i + 2]] == 0) {
                removed[w[i + 2]] = 1;
                for (uint32_t j = 0; j < n; j++) w[i + j] = (1u << 16) | OpNop;
                again = changed = 1;
            }
        }
    }
    /* decorations of removed ids (NonUniform and the like) would name ids that no longer exist */
    for (size_t i = 5; i < words; i += w[i] >> 16) {
        uint32_t n = w[i] >> 16, op = w[i] & 0xffff;
        if ((op == OpDecorate || op == OpName) && n >= 2 && w[i + 1] < bound && removed[w[i + 1]])
            for (uint32_t j = 0; j < n; j++) w[i + j] = (1u << 16) | OpNop;
    }
    free(constVal); free(pointee); free(varStruct); free(tainted); free(uses); free(removed);
    if (!changed) { free(w); return NULL; }
    *outWords = words;
    return w;
fail:
    free(w); free(constVal); free(pointee); free(varStruct); free(tainted); free(uses); free(removed);
    return NULL;
}

#endif
