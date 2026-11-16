/*
 * [mali-g2] Two questions about SPIR-V that DROIDDECK_HYBRIS_LOAD_SKIP (cmd_stats.c, load_skip.h)
 * asks before it lets a pass skip loading its colour attachment. No Vulkan calls, no state: tests
 * run it on the host.
 *
 *   spv_fs_may_discard   can a fragment shader entry point reach OpKill, OpTerminateInvocation or
 *                        OpDemoteToHelperInvocation, or write the SampleMask built-in, with the
 *                        pipeline's specialization constants? Sparse conditional constant
 *                        propagation over every function reachable from the entry point: a block
 *                        counts as reachable unless every branch to it is decided by constants
 *                        (OpConstant*, OpSpecConstant* with the pipeline's values, and what folds
 *                        from them). Memory loads, inputs and resources are not constants.
 *   spv_vs_run           the vertex shader run on the CPU for given vertices: its inputs are the
 *                        vertex attributes and the index built-ins; anything it reads from memory
 *                        (uniforms, push constants, buffers, images) is unknown, and a branch on an
 *                        unknown value stops the run. The outputs that decide where a triangle is
 *                        drawn are given back: Position, ClipDistance, CullDistance, Layer,
 *                        ViewportIndex. 32-bit integer and float arithmetic in IEEE single precision.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#ifndef DROIDDECK_SPIRV_EVAL_H
#define DROIDDECK_SPIRV_EVAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

typedef struct SpvProgram SpvProgram;

/* A copy of `code` parsed; NULL when it is not SPIR-V this evaluator can index. Shared: one
 * reference per holder. */
SpvProgram* spv_parse(const uint32_t* code, size_t bytes);
void spv_ref(SpvProgram* p);
void spv_unref(SpvProgram* p);

/* Whether the module has an entry point `name` of the execution model (0 vertex, 4 fragment). */
bool spv_has_entry(const SpvProgram* p, const char* name, uint32_t model);

/* Any stage: OpStore / OpCopyMemory / atomics through a pointer that is not Function, Private,
 * Output or Workgroup storage, or OpImageWrite. */
bool spv_writes_memory(const SpvProgram* p);

/* Whether the module may read an attachment's contents: the InputAttachment or tile image read
 * capabilities, or a SubpassData or tile image type. */
bool spv_reads_attachments(const SpvProgram* p);

/* The same for code spv_parse does not take (instructions it does not know are skipped; a store
 * through a pointer whose type is not known counts as a write); also true for what is not SPIR-V. */
bool spv_code_writes_memory(const uint32_t* code, size_t bytes);

enum { SPV_DISCARD_NO, SPV_DISCARD_MAY, SPV_DISCARD_UNKNOWN };
int spv_fs_may_discard(const SpvProgram* p, const char* entry, const VkSpecializationInfo* spec);

/* One attribute location as the vertex input interface hands it to the shader. */
enum { SPV_ATTR_NONE, SPV_ATTR_FLOAT, SPV_ATTR_SINT, SPV_ATTR_UINT };
typedef struct {
    uint32_t value[4];
    uint8_t kind; /* SPV_ATTR_*; NONE: unknown */
} SpvAttr;

#define SPV_LOCATIONS 32
typedef struct {
    SpvAttr location[SPV_LOCATIONS];
    uint32_t vertexIndex, instanceIndex, baseVertex, baseInstance, drawIndex;
} SpvVertexIn;

#define SPV_CLIP_MAX 8
typedef struct {
    float position[4];
    bool positionKnown;
    /* per ClipDistance / CullDistance element: 0 never written, 1 known, 2 unknown */
    uint8_t clipState[SPV_CLIP_MAX], cullState[SPV_CLIP_MAX];
    float clip[SPV_CLIP_MAX], cull[SPV_CLIP_MAX];
    uint32_t clipCount, cullCount; /* elements declared (more than SPV_CLIP_MAX: unknown) */
    uint8_t layerState, viewportState; /* as clipState */
    uint32_t layer, viewport;
} SpvVertexOut;

enum {
    SPV_RUN_OK,
    SPV_RUN_NO_ENTRY,     /* no such vertex entry point */
    SPV_RUN_CONTROL,      /* a branch on an unknown value, a call too deep, OpUnreachable, ... */
    SPV_RUN_BUDGET,       /* too many instructions or too much memory */
    SPV_RUN_UNSUPPORTED,  /* an instruction or type the run cannot follow where it matters */
};
/* Runs `entry` for count vertices. */
int spv_vs_run(const SpvProgram* p, const char* entry, const VkSpecializationInfo* spec, const SpvVertexIn* in, uint32_t count,
               SpvVertexOut* out);

/* Locations the vertex entry point's Input variables use and the scalar kind each expects
 * (SPV_ATTR_*; NONE where not used). 64-bit or 16-bit inputs are NONE too. */
void spv_vs_inputs(const SpvProgram* p, const char* entry, uint8_t kinds[SPV_LOCATIONS]);

#endif
