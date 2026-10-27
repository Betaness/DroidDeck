/*
 * [mali-g2] Makes constant composites built from specialization constants specializable. Shared by
 * the Vortek client (droiddeck_maint.c) and the in-process libhybris path (mali-g2/hybris).
 *
 * The Mali compiler (r56p0 on the Mali-G2, and older ones) folds an OpConstantComposite whose
 * constituents are spec constants (OpSpecConstantTrue/False/OpSpecConstant/...) at the constituents'
 * default values: the specialization the pipeline gives is lost for the composite, though the same
 * spec constant used on its own is specialized. spirv-val accepts such modules and DXVK's bound
 * checks are built that way (OpSelect of a bvec4 composite of a spec bool between the texel and
 * zero), so a texture read through them comes out as the default. Declaring the composite
 * OpSpecConstantComposite (same operands, same length) keeps the fold away; composites made of such
 * composites are changed too. The rewrite is exact on any driver: the composite's value is what
 * the specialization makes it. tests/mali_quirks_test.c ("spec", "bound") shows the driver's fold.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_SPIRV_SPEC_COMPOSITE_H
#define DROIDDECK_SPIRV_SPEC_COMPOSITE_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define DD_SPV_SPEC_CONSTANT_TRUE 48
#define DD_SPV_SPEC_CONSTANT_FALSE 49
#define DD_SPV_SPEC_CONSTANT 50
#define DD_SPV_SPEC_CONSTANT_COMPOSITE 51
#define DD_SPV_SPEC_CONSTANT_OP 52
#define DD_SPV_CONSTANT_COMPOSITE 44
#define DD_SPV_FUNCTION 54
#define DD_SPV_CONSTANT_COMPOSITE_REPLICATE 4461
#define DD_SPV_SPEC_CONSTANT_COMPOSITE_REPLICATE 4462

/* Counts the composites of `code` (`words` words) that have to become spec-constant composites and,
 * when `out` is not NULL (a copy of `code` or `code` itself), changes their opcodes there.
 * Returns 0 for anything that is not a well-formed module. */
static size_t dd_spec_composites(const uint32_t* code, size_t words, uint32_t* out) {
    if (words < 5 || code[0] != 0x07230203u) return 0;
    uint32_t bound = code[3];
    if (bound == 0 || bound > (1u << 24)) return 0;
    uint8_t* spec = NULL; /* bit per id: a specialization constant */
    size_t changed = 0;
    for (size_t i = 5; i < words;) {
        uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (n == 0 || i + n > words) break;
        if (op == DD_SPV_FUNCTION) break; /* constants are declared before the first function */
        uint32_t result = n >= 3 ? code[i + 2] : 0;
        int isSpec = 0;
        switch (op) {
        case DD_SPV_SPEC_CONSTANT_TRUE: case DD_SPV_SPEC_CONSTANT_FALSE: case DD_SPV_SPEC_CONSTANT:
        case DD_SPV_SPEC_CONSTANT_COMPOSITE: case DD_SPV_SPEC_CONSTANT_OP: case DD_SPV_SPEC_CONSTANT_COMPOSITE_REPLICATE:
            isSpec = 1;
            break;
        case DD_SPV_CONSTANT_COMPOSITE: case DD_SPV_CONSTANT_COMPOSITE_REPLICATE:
            for (uint32_t j = 3; spec && j < n && !isSpec; j++)
                isSpec = code[i + j] < bound && (spec[code[i + j] >> 3] >> (code[i + j] & 7) & 1);
            if (isSpec) {
                changed++;
                if (out) out[i] = (n << 16) | (op == DD_SPV_CONSTANT_COMPOSITE ? DD_SPV_SPEC_CONSTANT_COMPOSITE : DD_SPV_SPEC_CONSTANT_COMPOSITE_REPLICATE);
            }
            break;
        }
        if (isSpec && result < bound) {
            if (!spec && !(spec = (uint8_t*)calloc((bound + 7) / 8, 1))) return 0;
            spec[result >> 3] |= (uint8_t)(1u << (result & 7));
        }
        i += n;
    }
    free(spec);
    return changed;
}

#endif
