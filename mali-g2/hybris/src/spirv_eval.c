/*
 * [mali-g2] See spirv_eval.h.
 *
 * Every value is a row of 32-bit cells (scalars, vector and matrix components, array elements and
 * struct members flattened in order), each known or not. Pointers are one cell naming a variable
 * and a cell offset in it. Only 32-bit integers, 32-bit floats and booleans are computed; other
 * widths are unknown. Both questions share the constant folding (evalPure).
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1 or later.
 */
#include "spirv_eval.h"

#include "spirv_ops.h"

#include <stdlib.h>
#include <string.h>

/* opcodes */
enum {
    OpUndef = 1, OpExtInstImport = 11, OpExtInst = 12, OpEntryPoint = 15,
    OpTypeVoid = 19, OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeMatrix = 24,
    OpTypeArray = 28, OpTypeRuntimeArray = 29, OpTypeStruct = 30, OpTypePointer = 32, OpTypeFunction = 33,
    OpConstantTrue = 41, OpConstantFalse = 42, OpConstant = 43, OpConstantComposite = 44, OpConstantNull = 46,
    OpSpecConstantTrue = 48, OpSpecConstantFalse = 49, OpSpecConstant = 50, OpSpecConstantComposite = 51, OpSpecConstantOp = 52,
    OpFunction = 54, OpFunctionParameter = 55, OpFunctionEnd = 56, OpFunctionCall = 57, OpVariable = 59,
    OpLoad = 61, OpStore = 62, OpCopyMemory = 63, OpCopyMemorySized = 64, OpAccessChain = 65, OpInBoundsAccessChain = 66,
    OpPtrAccessChain = 67, OpInBoundsPtrAccessChain = 70, OpDecorate = 71, OpMemberDecorate = 72, OpGroupDecorate = 74,
    OpGroupMemberDecorate = 75, OpVectorExtractDynamic = 77, OpVectorInsertDynamic = 78, OpVectorShuffle = 79,
    OpCompositeConstruct = 80, OpCompositeExtract = 81, OpCompositeInsert = 82, OpCopyObject = 83, OpTranspose = 84,
    OpImageWrite = 99,
    OpConvertFToU = 109, OpConvertFToS = 110, OpConvertSToF = 111, OpConvertUToF = 112, OpUConvert = 113, OpSConvert = 114,
    OpFConvert = 115, OpBitcast = 124, OpSNegate = 126, OpFNegate = 127, OpIAdd = 128, OpFAdd = 129, OpISub = 130,
    OpFSub = 131, OpIMul = 132, OpFMul = 133, OpUDiv = 134, OpSDiv = 135, OpFDiv = 136, OpUMod = 137, OpSRem = 138,
    OpSMod = 139, OpFRem = 140, OpFMod = 141, OpVectorTimesScalar = 142, OpMatrixTimesScalar = 143,
    OpVectorTimesMatrix = 144, OpMatrixTimesVector = 145, OpMatrixTimesMatrix = 146, OpDot = 148, OpAny = 154,
    OpAll = 155, OpIsNan = 156, OpIsInf = 157, OpLogicalEqual = 164, OpLogicalNotEqual = 165, OpLogicalOr = 166,
    OpLogicalAnd = 167, OpLogicalNot = 168, OpSelect = 169, OpIEqual = 170, OpINotEqual = 171, OpUGreaterThan = 172,
    OpSGreaterThan = 173, OpUGreaterThanEqual = 174, OpSGreaterThanEqual = 175, OpULessThan = 176, OpSLessThan = 177,
    OpULessThanEqual = 178, OpSLessThanEqual = 179, OpFOrdEqual = 180, OpFUnordEqual = 181, OpFOrdNotEqual = 182,
    OpFUnordNotEqual = 183, OpFOrdLessThan = 184, OpFUnordLessThan = 185, OpFOrdGreaterThan = 186,
    OpFUnordGreaterThan = 187, OpFOrdLessThanEqual = 188, OpFUnordLessThanEqual = 189, OpFOrdGreaterThanEqual = 190,
    OpFUnordGreaterThanEqual = 191, OpShiftRightLogical = 194, OpShiftRightArithmetic = 195, OpShiftLeftLogical = 196,
    OpBitwiseOr = 197, OpBitwiseXor = 198, OpBitwiseAnd = 199, OpNot = 200, OpBitFieldInsert = 201,
    OpBitFieldSExtract = 202, OpBitFieldUExtract = 203, OpAtomicLoad = 227, OpAtomicStore = 228, OpAtomicFlagClear = 244,
    OpPhi = 245, OpLoopMerge = 246, OpSelectionMerge = 247, OpLabel = 248, OpBranch = 249, OpBranchConditional = 250,
    OpSwitch = 251, OpKill = 252, OpReturn = 253, OpReturnValue = 254, OpUnreachable = 255, OpCopyLogical = 400,
    OpTerminateInvocation = 4416, OpDemoteToHelperInvocation = 5380, OpAtomicFMinEXT = 5614, OpAtomicFMaxEXT = 5615,
    OpAtomicFAddEXT = 6035,
};
/* GLSL.std.450 */
enum {
    G_Round = 1, G_RoundEven = 2, G_Trunc = 3, G_FAbs = 4, G_SAbs = 5, G_FSign = 6, G_SSign = 7, G_Floor = 8, G_Ceil = 9,
    G_Fract = 10, G_FMin = 37, G_UMin = 38, G_SMin = 39, G_FMax = 40, G_UMax = 41, G_SMax = 42, G_FClamp = 43, G_UClamp = 44,
    G_SClamp = 45, G_FMix = 46, G_Step = 48, G_Fma = 50, G_NMin = 79, G_NMax = 80, G_NClamp = 81,
};
enum { DecSpecId = 1, DecBuiltIn = 11, DecLocation = 30, DecComponent = 31 };
enum {
    BiPosition = 0, BiClipDistance = 3, BiCullDistance = 4, BiVertexId = 5, BiInstanceId = 6, BiLayer = 9,
    BiViewportIndex = 10, BiSampleMask = 20, BiVertexIndex = 42, BiInstanceIndex = 43, BiBaseVertex = 4424,
    BiBaseInstance = 4425, BiDrawIndex = 4426,
};
enum { ScInput = 1, ScOutput = 3, ScWorkgroup = 4, ScPrivate = 6, ScFunction = 7 };

enum { T_NONE, T_VOID, T_BOOL, T_INT, T_FLOAT, T_VECTOR, T_MATRIX, T_ARRAY, T_RARRAY, T_STRUCT, T_POINTER, T_FUNCTION, T_OPAQUE };

#define MAX_IDS (1u << 22)
#define MAX_TYPE_CELLS (1u << 16)
#define MAX_SLOT_CELLS (1u << 22)
#define NONE UINT32_MAX

typedef struct {
    uint32_t at;          /* word offset of the defining instruction, 0 none */
    uint32_t type;        /* its result type */
    uint32_t slot;        /* first cell of the value, NONE without one */
    uint32_t cells;       /* types: cells of a value (0: not representable); labels: offset of the terminator */
    uint32_t elem, count; /* vector, matrix, array: element type and count; pointer: pointee; struct: members */
    uint32_t sc;          /* pointer types: storage class */
    int32_t builtin, location, component, specId;
    uint8_t kind, width, sign, cellsDone;
} IdInfo;

typedef struct {
    uint32_t id, member;
    int32_t builtin;
} MemberBuiltin;

typedef struct {
    uint32_t model, fn, name; /* name: word offset of the string */
} Entry;

typedef struct {
    uint32_t id, at, end, firstLabel, params;
} Fn;

struct SpvProgram {
    int refs;
    uint32_t* code;
    uint32_t words, bound, firstFunction, slotCells, maxCells;
    IdInfo* ids;
    MemberBuiltin* mb;
    uint32_t mbCount;
    Entry* entries;
    uint32_t entryCount;
    Fn* fns;
    uint32_t fnCount;
    uint32_t glsl;
    bool writes;
};

static bool knownOp(uint32_t op, bool* hasResult, bool* hasType) {
    *hasResult = *hasType = false;
    size_t lo = 0, hi = sizeof spv_known_ops / sizeof spv_known_ops[0];
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        uint32_t o = spv_known_ops[mid] >> 2;
        if (o == op) {
            *hasResult = spv_known_ops[mid] & 2;
            *hasType = spv_known_ops[mid] & 1;
            return true;
        }
        if (o < op) lo = mid + 1;
        else hi = mid;
    }
    return false;
}

static const uint32_t* insn(const SpvProgram* p, uint32_t at, uint32_t* n, uint32_t* op) {
    const uint32_t* w = p->code + at;
    *n = w[0] >> 16;
    *op = w[0] & 0xffff;
    return w;
}

static uint32_t typeCells(SpvProgram* p, uint32_t t, int depth);

static uint32_t memberCells(SpvProgram* p, uint32_t st, uint32_t member, int depth) {
    uint32_t n, op, off = 0;
    const uint32_t* w = insn(p, p->ids[st].at, &n, &op);
    for (uint32_t i = 0; i < member && 2 + i < n; i++) {
        uint32_t c = typeCells(p, w[2 + i], depth + 1);
        if (!c) return NONE;
        off += c;
    }
    return off;
}

static uint32_t typeCells(SpvProgram* p, uint32_t t, int depth) {
    if (t >= p->bound || depth > 32) return 0;
    IdInfo* id = &p->ids[t];
    if (id->cellsDone) return id->cells;
    uint32_t c = 0;
    switch (id->kind) {
    case T_BOOL: case T_INT: case T_FLOAT: case T_POINTER: c = 1; break;
    case T_VECTOR: case T_MATRIX: case T_ARRAY: {
        uint64_t e = typeCells(p, id->elem, depth + 1);
        uint64_t total = e * id->count;
        c = e && total <= MAX_TYPE_CELLS ? (uint32_t)total : 0;
        break;
    }
    case T_STRUCT: {
        uint64_t total = 0;
        uint32_t n, op;
        const uint32_t* w = insn(p, id->at, &n, &op);
        for (uint32_t i = 2; i < n && total <= MAX_TYPE_CELLS; i++) {
            uint32_t e = typeCells(p, w[i], depth + 1);
            if (!e) {
                total = 0;
                break;
            }
            total += e;
        }
        c = total && total <= MAX_TYPE_CELLS ? (uint32_t)total : 0;
        break;
    }
    default: c = 0; break;
    }
    id->cells = c;
    id->cellsDone = 1;
    return c;
}

static bool decorate(SpvProgram* p, uint32_t target, uint32_t dec, const uint32_t* lit, uint32_t litCount) {
    if (target >= p->bound) return false;
    IdInfo* id = &p->ids[target];
    if (!litCount) return true;
    switch (dec) {
    case DecSpecId: id->specId = (int32_t)lit[0]; break;
    case DecBuiltIn: id->builtin = (int32_t)lit[0]; break;
    case DecLocation: id->location = (int32_t)lit[0]; break;
    case DecComponent: id->component = (int32_t)lit[0]; break;
    default: break;
    }
    return true;
}

SpvProgram* spv_parse(const uint32_t* code, size_t bytes) {
    if (!code || bytes % 4 || bytes < 20 || code[0] != 0x07230203u) return NULL;
    size_t words = bytes / 4;
    if (words > (1u << 26) || code[3] == 0 || code[3] > MAX_IDS) return NULL;
    SpvProgram* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->refs = 1;
    p->words = (uint32_t)words;
    p->bound = code[3];
    p->code = malloc(bytes);
    p->ids = calloc(p->bound, sizeof *p->ids);
    if (!p->code || !p->ids) goto fail;
    memcpy(p->code, code, bytes);
    for (uint32_t i = 0; i < p->bound; i++) {
        p->ids[i].slot = NONE;
        p->ids[i].builtin = p->ids[i].location = p->ids[i].component = p->ids[i].specId = -1;
    }
    uint32_t mbCap = 0, entryCap = 0, fnCap = 0;
    Fn* fn = NULL;
    uint32_t lastLabel = 0;
    for (uint32_t pos = 5; pos < p->words;) {
        uint32_t n, op;
        const uint32_t* w = insn(p, pos, &n, &op);
        if (!n || pos + n > p->words) goto fail;
        bool hasResult, hasType;
        if (!knownOp(op, &hasResult, &hasType)) goto fail;
        uint32_t need = (hasType ? 1 : 0) + (hasResult ? 1 : 0);
        if (n < 1 + need) goto fail;
        uint32_t res = hasResult ? w[hasType ? 2 : 1] : 0;
        if (hasResult) {
            if (!res || res >= p->bound || p->ids[res].at) goto fail;
            p->ids[res].at = pos;
            p->ids[res].type = hasType ? w[1] : 0;
            if (hasType && w[1] >= p->bound) goto fail;
        }
        IdInfo* r = hasResult ? &p->ids[res] : NULL;
        switch (op) {
        case OpExtInstImport:
            if (n >= 3 && (n - 2) * 4 >= 12 && !memcmp(&w[2], "GLSL.std.450", 12)) p->glsl = res;
            break;
        case OpEntryPoint:
            if (n < 4) goto fail;
            if (p->entryCount == entryCap) {
                entryCap = entryCap ? entryCap * 2 : 4;
                Entry* e = realloc(p->entries, entryCap * sizeof *e);
                if (!e) goto fail;
                p->entries = e;
            }
            p->entries[p->entryCount++] = (Entry){w[1], w[2], pos + 3};
            break;
        case OpTypeVoid: r->kind = T_VOID; break;
        case OpTypeBool: r->kind = T_BOOL; break;
        case OpTypeInt:
            if (n < 4) goto fail;
            r->kind = T_INT, r->width = (uint8_t)(w[2] > 255 ? 255 : w[2]), r->sign = (uint8_t)w[3];
            break;
        case OpTypeFloat:
            if (n < 3) goto fail;
            r->kind = T_FLOAT, r->width = (uint8_t)(w[2] > 255 ? 255 : w[2]);
            break;
        case OpTypeVector: case OpTypeMatrix:
            if (n < 4) goto fail;
            r->kind = op == OpTypeVector ? T_VECTOR : T_MATRIX, r->elem = w[2], r->count = w[3];
            break;
        case OpTypeArray: {
            if (n < 4 || w[3] >= p->bound) goto fail;
            r->kind = T_ARRAY, r->elem = w[2];
            uint32_t ln, lop;
            const IdInfo* len = &p->ids[w[3]];
            const uint32_t* lw = len->at ? insn(p, len->at, &ln, &lop) : NULL;
            /* the length must be a plain constant; a specialized length is not followed */
            r->count = lw && lop == OpConstant && ln >= 4 ? lw[3] : 0;
            break;
        }
        case OpTypeRuntimeArray: r->kind = T_RARRAY; break;
        case OpTypeStruct: r->kind = T_STRUCT, r->count = n - 2; break;
        case OpTypePointer:
            if (n < 4) goto fail;
            r->kind = T_POINTER, r->sc = w[2], r->elem = w[3];
            break;
        case OpTypeFunction: r->kind = T_FUNCTION; break;
        case OpDecorate:
            if (n < 3 || !decorate(p, w[1], w[2], w + 3, n - 3)) goto fail;
            break;
        case OpMemberDecorate:
            if (n < 4) goto fail;
            if (w[3] == DecBuiltIn && n >= 5) {
                if (p->mbCount == mbCap) {
                    mbCap = mbCap ? mbCap * 2 : 8;
                    MemberBuiltin* m = realloc(p->mb, mbCap * sizeof *m);
                    if (!m) goto fail;
                    p->mb = m;
                }
                p->mb[p->mbCount++] = (MemberBuiltin){w[1], w[2], (int32_t)w[4]};
            }
            break;
        case OpGroupDecorate: case OpGroupMemberDecorate: goto fail;
        case OpFunction:
            if (fn) goto fail;
            if (!p->firstFunction) p->firstFunction = pos;
            if (p->fnCount == fnCap) {
                fnCap = fnCap ? fnCap * 2 : 8;
                Fn* f = realloc(p->fns, fnCap * sizeof *f);
                if (!f) goto fail;
                p->fns = f;
            }
            fn = &p->fns[p->fnCount++];
            *fn = (Fn){res, pos, 0, 0, 0};
            break;
        case OpFunctionParameter:
            if (!fn || fn->firstLabel) goto fail;
            fn->params++;
            break;
        case OpLabel:
            if (!fn) goto fail;
            if (!fn->firstLabel) fn->firstLabel = res;
            lastLabel = res;
            break;
        case OpBranch: case OpBranchConditional: case OpSwitch: case OpReturn: case OpReturnValue: case OpKill:
        case OpUnreachable: case OpTerminateInvocation:
            if (!fn || !lastLabel) goto fail;
            p->ids[lastLabel].cells = pos;
            lastLabel = 0;
            break;
        case OpFunctionEnd:
            if (!fn || lastLabel) goto fail;
            fn->end = pos;
            fn = NULL;
            break;
        default: break;
        }
        pos += n;
    }
    if (fn || !p->firstFunction) goto fail;
    /* value slots: every id with a result type, and functions (their return value) */
    uint64_t total = 0;
    for (uint32_t i = 1; i < p->bound; i++) {
        IdInfo* id = &p->ids[i];
        if (!id->at || !id->type) continue;
        if (p->ids[id->type].kind == T_NONE) continue;
        uint32_t c = typeCells(p, id->type, 0);
        if (!c) c = 1;
        if (c > p->maxCells) p->maxCells = c;
        id->slot = (uint32_t)total;
        total += c;
        if (total > MAX_SLOT_CELLS) goto fail;
    }
    p->slotCells = (uint32_t)total;
    for (uint32_t i = 1; i < p->bound; i++) {
        /* labels' terminator offsets live in `cells`; types keep theirs */
        if (p->ids[i].kind != T_NONE) typeCells(p, i, 0);
    }
    /* memory writes */
    for (uint32_t pos = 5; pos < p->words;) {
        uint32_t n, op;
        const uint32_t* w = insn(p, pos, &n, &op);
        uint32_t ptr = 0;
        bool check = false;
        if (op == OpImageWrite) p->writes = true;
        else if (op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == OpAtomicStore) {
            if (n < 2) p->writes = true;
            else ptr = w[1], check = true;
        }
        else if ((op > OpAtomicLoad && op <= OpAtomicFlagClear) || op == OpAtomicFMinEXT || op == OpAtomicFMaxEXT || op == OpAtomicFAddEXT)
            ptr = n > 3 ? w[3] : 0, check = true;
        if (check) {
            uint32_t pt = ptr < p->bound ? p->ids[ptr].type : 0;
            uint32_t sc = pt < p->bound && p->ids[pt].kind == T_POINTER ? p->ids[pt].sc : NONE;
            if (sc != ScFunction && sc != ScPrivate && sc != ScOutput && sc != ScWorkgroup) p->writes = true;
        }
        pos += n;
    }
    return p;
fail:
    spv_unref(p);
    return NULL;
}

void spv_ref(SpvProgram* p) {
    if (p) __atomic_add_fetch(&p->refs, 1, __ATOMIC_RELAXED);
}

void spv_unref(SpvProgram* p) {
    if (!p || __atomic_sub_fetch(&p->refs, 1, __ATOMIC_ACQ_REL)) return;
    free(p->code);
    free(p->ids);
    free(p->mb);
    free(p->entries);
    free(p->fns);
    free(p);
}

bool spv_writes_memory(const SpvProgram* p) {
    return !p || p->writes;
}

bool spv_reads_attachments(const SpvProgram* p) {
    if (!p) return true;
    for (uint32_t pos = 5; pos < p->words;) {
        uint32_t n, op;
        const uint32_t* w = insn(p, pos, &n, &op);
        /* InputAttachment, TileImage{Color,Depth,Stencil}ReadAccessEXT; Dim SubpassData, TileImageDataEXT */
        if (op == 17 /* OpCapability */ && n >= 2 && (w[1] == 40 || (w[1] >= 4166 && w[1] <= 4168))) return true;
        if (op == 25 /* OpTypeImage */ && n >= 4 && (w[3] == 6 || w[3] == 4173)) return true;
        pos += n;
    }
    return false;
}

bool spv_code_writes_memory(const uint32_t* code, size_t bytes) {
    if (!code || bytes % 4 || bytes < 20 || code[0] != 0x07230203u || code[3] == 0 || code[3] > MAX_IDS) return true;
    uint32_t words = (uint32_t)(bytes / 4), bound = code[3];
    /* per id: its result type; per type id: 1 + storage class of a pointer type */
    uint32_t* typeOf = calloc(bound, sizeof *typeOf);
    uint32_t* pointer = calloc(bound, sizeof *pointer);
    bool writes = !typeOf || !pointer;
    for (uint32_t pos = 5; pos < words && !writes;) {
        uint32_t n = code[pos] >> 16, op = code[pos] & 0xffff;
        if (!n || pos + n > words) {
            writes = true;
            break;
        }
        const uint32_t* w = code + pos;
        bool hasResult, hasType;
        if (knownOp(op, &hasResult, &hasType) && hasResult && hasType && n >= 3 && w[2] < bound) typeOf[w[2]] = w[1];
        if (op == OpTypePointer && n >= 4 && w[1] < bound) pointer[w[1]] = 1 + w[2];
        uint32_t ptr = 0;
        bool check = false;
        if (op == OpImageWrite) writes = true;
        else if (op == OpStore || op == OpCopyMemory || op == OpCopyMemorySized || op == OpAtomicStore) ptr = n > 1 ? w[1] : 0, check = true;
        else if ((op > OpAtomicLoad && op <= OpAtomicFlagClear) || op == OpAtomicFMinEXT || op == OpAtomicFMaxEXT || op == OpAtomicFAddEXT)
            ptr = n > 3 ? w[3] : 0, check = true;
        if (check) {
            uint32_t t = ptr < bound ? typeOf[ptr] : 0;
            uint32_t sc = t && t < bound && pointer[t] ? pointer[t] - 1 : NONE;
            if (sc != ScFunction && sc != ScPrivate && sc != ScOutput && sc != ScWorkgroup) writes = true;
        }
        pos += n;
    }
    free(typeOf);
    free(pointer);
    return writes;
}

static const Entry* findEntry(const SpvProgram* p, const char* name, uint32_t model) {
    if (!p || !name) return NULL;
    size_t len = strlen(name);
    for (uint32_t i = 0; i < p->entryCount; i++) {
        const Entry* e = &p->entries[i];
        uint32_t n, op;
        const uint32_t* w = insn(p, p->entries[i].name - 3, &n, &op);
        size_t room = (size_t)(n - 3) * 4;
        if (e->model != model || room <= len) continue;
        if (!memcmp(&w[3], name, len) && ((const char*)&w[3])[len] == 0) return e;
    }
    return NULL;
}

bool spv_has_entry(const SpvProgram* p, const char* name, uint32_t model) {
    return findEntry(p, name, model) != NULL;
}

static const Fn* findFn(const SpvProgram* p, uint32_t id) {
    for (uint32_t i = 0; i < p->fnCount; i++)
        if (p->fns[i].id == id) return &p->fns[i];
    return NULL;
}

/* ---- values ---- */

#define F_K 1 /* known */
#define F_P 2 /* a pointer: v variable, p cell offset (exact with F_K) */
#define F_W 4 /* memory: written */

typedef struct {
    uint32_t v, p;
    uint8_t f;
} Cell;

typedef struct {
    uint32_t off, n, sc;
    int32_t builtin; /* of the variable (not of members) */
    uint32_t type;   /* pointee */
} Var;

enum { M_SCCP, M_RUN };

typedef struct {
    const SpvProgram* p;
    int mode;
    const VkSpecializationInfo* spec;
    Cell* cells;
    uint8_t* defined; /* M_SCCP: per id */
    Cell* tmp;
    bool changed;
    /* M_RUN memory */
    Cell* mem;
    uint32_t memUsed, memCap;
    Var* vars;
    uint32_t varCount, varCap;
    uint32_t globalVars, globalMem;
    uint64_t steps;
} Ctx;

static const IdInfo* idOf(const Ctx* c, uint32_t id) {
    return id < c->p->bound ? &c->p->ids[id] : NULL;
}

static const Cell* V(const Ctx* c, uint32_t id) {
    const IdInfo* i = idOf(c, id);
    if (!i || i->slot == NONE) return NULL;
    if (c->mode == M_SCCP && !c->defined[id]) return NULL;
    return &c->cells[i->slot];
}

static uint32_t cellsOfType(const Ctx* c, uint32_t type) {
    const IdInfo* t = idOf(c, type);
    return t && t->cellsDone && t->cells ? t->cells : 1;
}

static uint32_t valueCells(const Ctx* c, uint32_t id) {
    const IdInfo* i = idOf(c, id);
    return i ? cellsOfType(c, i->type) : 0;
}

static void commit(Ctx* c, uint32_t id, const Cell* v, uint32_t n) {
    const IdInfo* i = idOf(c, id);
    if (!i || i->slot == NONE) return;
    uint32_t want = cellsOfType(c, i->type);
    Cell* dst = &c->cells[i->slot];
    if (c->mode == M_RUN) {
        for (uint32_t k = 0; k < want; k++) dst[k] = k < n ? v[k] : (Cell){0, 0, 0};
        return;
    }
    if (!c->defined[id]) {
        for (uint32_t k = 0; k < want; k++) dst[k] = k < n ? v[k] : (Cell){0, 0, 0};
        c->defined[id] = 1;
        c->changed = true;
        return;
    }
    for (uint32_t k = 0; k < want; k++) {
        Cell nv = k < n ? v[k] : (Cell){0, 0, 0};
        if ((dst[k].f & F_K) && !((nv.f & F_K) && nv.v == dst[k].v && nv.p == dst[k].p && nv.f == dst[k].f)) {
            dst[k] = (Cell){0, 0, 0};
            c->changed = true;
        }
    }
}

static void commitUnknown(Ctx* c, uint32_t id) {
    commit(c, id, NULL, 0);
}

/* the scalar type of a scalar, vector or matrix type */
static const IdInfo* scalarOf(const Ctx* c, uint32_t type) {
    const IdInfo* t = idOf(c, type);
    for (int i = 0; t && i < 3 && (t->kind == T_VECTOR || t->kind == T_MATRIX); i++) t = idOf(c, t->elem);
    return t;
}

enum { C_X, C_F32, C_I32, C_BOOL };
static int classOf(const Ctx* c, uint32_t type) {
    const IdInfo* s = scalarOf(c, type);
    if (!s) return C_X;
    if (s->kind == T_BOOL) return C_BOOL;
    if (s->kind == T_FLOAT && s->width == 32) return C_F32;
    if (s->kind == T_INT && s->width == 32) return C_I32;
    return C_X;
}

static float asF(uint32_t v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static uint32_t asU(float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}
static bool isNan(float f) {
    return f != f;
}
static bool isInf(float f) {
    return !isNan(f) && isNan(f - f);
}

/* floor, ceil, trunc without libm: |f| >= 2^23 is already integral */
static float fTrunc(float f) {
    if (isNan(f) || isInf(f) || (f >= 8388608.0f || f <= -8388608.0f)) return f;
    return (float)(int32_t)f;
}
static float fFloor(float f) {
    float t = fTrunc(f);
    return t > f ? t - 1.0f : t;
}
static float fCeil(float f) {
    float t = fTrunc(f);
    return t < f ? t + 1.0f : t;
}
static float fRoundEven(float f) {
    float fl = fFloor(f), d = f - fl;
    if (d > 0.5f) return fl + 1.0f;
    if (d < 0.5f) return fl;
    return fTrunc(fl * 0.5f) * 2.0f == fl ? fl : fl + 1.0f;
}

#define K(x) ((Cell){(x), 0, F_K})

/* One component of a componentwise operation; false: no value. */
static bool scalar1(uint32_t op, int cls, const Cell* a, Cell* r) {
    if (!(a->f & F_K) || (a->f & F_P)) return false;
    uint32_t x = a->v;
    switch (op) {
    case OpFNegate: if (cls != C_F32) return false; *r = K(x ^ 0x80000000u); return true;
    case OpSNegate: if (cls != C_I32) return false; *r = K(0u - x); return true;
    case OpNot: if (cls != C_I32) return false; *r = K(~x); return true;
    case OpLogicalNot: if (cls != C_BOOL) return false; *r = K(!x); return true;
    case OpIsNan: *r = K(isNan(asF(x))); return true;
    case OpIsInf: *r = K(isInf(asF(x))); return true;
    default: return false;
    }
}

static bool cmpF(uint32_t op, float x, float y, uint32_t* r) {
    bool un = isNan(x) || isNan(y);
    switch (op) {
    case OpFOrdEqual: *r = !un && x == y; return true;
    case OpFUnordEqual: *r = un || x == y; return true;
    case OpFOrdNotEqual: *r = !un && x != y; return true;
    case OpFUnordNotEqual: *r = un || x != y; return true;
    case OpFOrdLessThan: *r = !un && x < y; return true;
    case OpFUnordLessThan: *r = un || x < y; return true;
    case OpFOrdGreaterThan: *r = !un && x > y; return true;
    case OpFUnordGreaterThan: *r = un || x > y; return true;
    case OpFOrdLessThanEqual: *r = !un && x <= y; return true;
    case OpFUnordLessThanEqual: *r = un || x <= y; return true;
    case OpFOrdGreaterThanEqual: *r = !un && x >= y; return true;
    case OpFUnordGreaterThanEqual: *r = un || x >= y; return true;
    default: return false;
    }
}

/* operand class `cls` (comparisons: the operands', not the boolean result's) */
static bool scalar2(uint32_t op, int cls, const Cell* a, const Cell* b, Cell* r) {
    if (!(a->f & F_K) || !(b->f & F_K) || ((a->f | b->f) & F_P)) return false;
    uint32_t x = a->v, y = b->v, out;
    int32_t sx = (int32_t)x, sy = (int32_t)y;
    float fx = asF(x), fy = asF(y);
    if (op >= OpFOrdEqual && op <= OpFUnordGreaterThanEqual) {
        if (cls != C_F32 || !cmpF(op, fx, fy, &out)) return false;
        *r = K(out);
        return true;
    }
    if (op == OpLogicalEqual || op == OpLogicalNotEqual || op == OpLogicalOr || op == OpLogicalAnd) {
        if (cls != C_BOOL) return false;
        bool bx = x != 0, by = y != 0;
        out = op == OpLogicalEqual ? bx == by : op == OpLogicalNotEqual ? bx != by : op == OpLogicalOr ? (bx || by) : (bx && by);
        *r = K(out);
        return true;
    }
    if (cls == C_F32) {
        float f;
        switch (op) {
        case OpFAdd: f = fx + fy; break;
        case OpFSub: f = fx - fy; break;
        case OpFMul: f = fx * fy; break;
        case OpFDiv: f = fx / fy; break;
        default: return false;
        }
        *r = K(asU(f));
        return true;
    }
    if (cls != C_I32) return false;
    switch (op) {
    case OpIAdd: out = x + y; break;
    case OpISub: out = x - y; break;
    case OpIMul: out = x * y; break;
    case OpUDiv: if (!y) return false; out = x / y; break;
    case OpUMod: if (!y) return false; out = x % y; break;
    case OpSDiv: if (!y || (sx == INT32_MIN && sy == -1)) return false; out = (uint32_t)(sx / sy); break;
    case OpSRem: if (!y || (sx == INT32_MIN && sy == -1)) return false; out = (uint32_t)(sx % sy); break;
    case OpSMod: {
        if (!y || (sx == INT32_MIN && sy == -1)) return false;
        int32_t m = sx % sy;
        if (m && ((m < 0) != (sy < 0))) m += sy;
        out = (uint32_t)m;
        break;
    }
    case OpShiftRightLogical: if (y >= 32) return false; out = x >> y; break;
    case OpShiftRightArithmetic: if (y >= 32) return false; out = (uint32_t)(sx >> y); break;
    case OpShiftLeftLogical: if (y >= 32) return false; out = x << y; break;
    case OpBitwiseOr: out = x | y; break;
    case OpBitwiseXor: out = x ^ y; break;
    case OpBitwiseAnd: out = x & y; break;
    case OpIEqual: out = x == y; break;
    case OpINotEqual: out = x != y; break;
    case OpUGreaterThan: out = x > y; break;
    case OpSGreaterThan: out = sx > sy; break;
    case OpUGreaterThanEqual: out = x >= y; break;
    case OpSGreaterThanEqual: out = sx >= sy; break;
    case OpULessThan: out = x < y; break;
    case OpSLessThan: out = sx < sy; break;
    case OpULessThanEqual: out = x <= y; break;
    case OpSLessThanEqual: out = sx <= sy; break;
    default: return false;
    }
    *r = K(out);
    return true;
}

static bool glsl1(uint32_t inst, int cls, const Cell* a, Cell* r) {
    if (!(a->f & F_K) || (a->f & F_P)) return false;
    float f = asF(a->v);
    int32_t s = (int32_t)a->v;
    if (cls == C_I32) {
        if (inst == G_SAbs) { *r = K(s < 0 ? 0u - a->v : a->v); return true; }
        if (inst == G_SSign) { *r = K((uint32_t)(s > 0 ? 1 : s < 0 ? -1 : 0)); return true; }
        return false;
    }
    if (cls != C_F32) return false;
    switch (inst) {
    case G_FAbs: *r = K(a->v & 0x7fffffffu); return true;
    case G_FSign: *r = K(asU(f > 0.0f ? 1.0f : f < 0.0f ? -1.0f : f)); return true;
    case G_Trunc: *r = K(asU(fTrunc(f))); return true;
    case G_Floor: *r = K(asU(fFloor(f))); return true;
    case G_Ceil: *r = K(asU(fCeil(f))); return true;
    case G_Fract: *r = K(asU(f - fFloor(f))); return true;
    case G_RoundEven: case G_Round: *r = K(asU(fRoundEven(f))); return true; /* Round: either way at .5 */
    default: return false;
    }
}

static bool glsl2(uint32_t inst, int cls, const Cell* a, const Cell* b, Cell* r) {
    if (!(a->f & F_K) || !(b->f & F_K) || ((a->f | b->f) & F_P)) return false;
    uint32_t x = a->v, y = b->v;
    float fx = asF(x), fy = asF(y);
    switch (inst) {
    case G_FMin: case G_NMin:
        if (cls != C_F32 || isNan(fx) || isNan(fy)) return false;
        *r = K(asU(fy < fx ? fy : fx));
        return true;
    case G_FMax: case G_NMax:
        if (cls != C_F32 || isNan(fx) || isNan(fy)) return false;
        *r = K(asU(fy > fx ? fy : fx));
        return true;
    case G_Step:
        if (cls != C_F32 || isNan(fx) || isNan(fy)) return false;
        *r = K(asU(fy < fx ? 0.0f : 1.0f));
        return true;
    case G_UMin: if (cls != C_I32) return false; *r = K(x < y ? x : y); return true;
    case G_UMax: if (cls != C_I32) return false; *r = K(x > y ? x : y); return true;
    case G_SMin: if (cls != C_I32) return false; *r = K((int32_t)x < (int32_t)y ? x : y); return true;
    case G_SMax: if (cls != C_I32) return false; *r = K((int32_t)x > (int32_t)y ? x : y); return true;
    default: return false;
    }
}

static bool glsl3(uint32_t inst, int cls, const Cell* a, const Cell* b, const Cell* d, Cell* r) {
    if (!(a->f & F_K) || !(b->f & F_K) || !(d->f & F_K) || ((a->f | b->f | d->f) & F_P)) return false;
    uint32_t x = a->v, y = b->v, z = d->v;
    float fx = asF(x), fy = asF(y), fz = asF(z);
    switch (inst) {
    case G_FClamp: case G_NClamp: {
        if (cls != C_F32 || isNan(fx) || isNan(fy) || isNan(fz)) return false;
        float m = fy > fx ? fy : fx;
        *r = K(asU(fz < m ? fz : m));
        return true;
    }
    case G_UClamp: {
        if (cls != C_I32) return false;
        uint32_t m = y > x ? y : x;
        *r = K(z < m ? z : m);
        return true;
    }
    case G_SClamp: {
        if (cls != C_I32) return false;
        int32_t m = (int32_t)y > (int32_t)x ? (int32_t)y : (int32_t)x;
        *r = K((uint32_t)((int32_t)z < m ? (int32_t)z : m));
        return true;
    }
    case G_FMix: if (cls != C_F32) return false; *r = K(asU(fx * (1.0f - fz) + fy * fz)); return true;
    case G_Fma: if (cls != C_F32) return false; *r = K(asU(fx * fy + fz)); return true;
    default: return false;
    }
}

/* The cell offset and type reached from `type` by `count` indices (literals, or ids whose values are
 * looked up when `ids`). */
static bool walk(const Ctx* c, uint32_t type, const uint32_t* idx, uint32_t count, bool ids, uint32_t* off, uint32_t* outType) {
    uint32_t o = 0;
    for (uint32_t i = 0; i < count; i++) {
        const IdInfo* t = idOf(c, type);
        if (!t) return false;
        uint32_t index;
        if (ids) {
            const Cell* v = V(c, idx[i]);
            if (!v || !(v->f & F_K) || (v->f & F_P)) return false;
            index = v->v;
        } else {
            index = idx[i];
        }
        if (t->kind == T_STRUCT) {
            uint32_t mo = memberCells((SpvProgram*)c->p, type, index, 0);
            uint32_t n, op;
            const uint32_t* w = insn(c->p, t->at, &n, &op);
            if (mo == NONE || index >= n - 2) return false;
            o += mo;
            type = w[2 + index];
        } else if (t->kind == T_VECTOR || t->kind == T_MATRIX || t->kind == T_ARRAY) {
            if (index >= t->count) return false;
            uint32_t e = idOf(c, t->elem) ? c->p->ids[t->elem].cells : 0;
            if (!e) return false;
            o += index * e;
            type = t->elem;
        } else {
            return false;
        }
    }
    *off = o;
    *outType = type;
    return true;
}

/* A pure instruction: its result computed from its operands into the result's slot. 1 done,
 * 0 not a pure instruction, -1 an operand has no value yet (M_SCCP). */
static int evalPure(Ctx* c, const uint32_t* w, uint32_t n, uint32_t op) {
    if (n < 3) return 0;
    uint32_t rt = w[1], res = w[2];
    uint32_t rn = cellsOfType(c, rt);
    Cell* r = c->tmp;
    for (uint32_t k = 0; k < rn; k++) r[k] = (Cell){0, 0, 0};
    int rcls = classOf(c, rt);
#define OPND(i, var)                         \
    const Cell* var = NULL;                  \
    if (n <= (i)) return 0;                  \
    var = V(c, w[i]);                        \
    if (!var) return -1;
    switch (op) {
    case OpUndef:
        break;
    case OpCopyObject: case OpCopyLogical: {
        OPND(3, a);
        if (valueCells(c, w[3]) == rn)
            for (uint32_t k = 0; k < rn; k++) r[k] = a[k];
        break;
    }
    case OpCompositeConstruct: {
        uint32_t o = 0;
        bool fits = true;
        for (uint32_t i = 3; i < n; i++) {
            OPND(i, a);
            uint32_t an = valueCells(c, w[i]);
            if (o + an > rn) {
                fits = false;
                break;
            }
            for (uint32_t k = 0; k < an; k++) r[o + k] = a[k];
            o += an;
        }
        if (!fits || o != rn)
            for (uint32_t k = 0; k < rn; k++) r[k] = (Cell){0, 0, 0};
        break;
    }
    case OpCompositeExtract: {
        OPND(3, a);
        uint32_t off, t;
        if (walk(c, idOf(c, w[3])->type, w + 4, n - 4, false, &off, &t) && off + rn <= valueCells(c, w[3]))
            for (uint32_t k = 0; k < rn; k++) r[k] = a[off + k];
        break;
    }
    case OpCompositeInsert: {
        OPND(3, obj);
        OPND(4, comp);
        uint32_t off, t, on = valueCells(c, w[3]);
        if (valueCells(c, w[4]) != rn) break;
        for (uint32_t k = 0; k < rn; k++) r[k] = comp[k];
        if (walk(c, rt, w + 5, n - 5, false, &off, &t) && off + on <= rn) {
            for (uint32_t k = 0; k < on; k++) r[off + k] = obj[k];
        } else {
            for (uint32_t k = 0; k < rn; k++) r[k] = (Cell){0, 0, 0};
        }
        break;
    }
    case OpVectorShuffle: {
        OPND(3, a);
        OPND(4, b);
        uint32_t an = valueCells(c, w[3]), bn = valueCells(c, w[4]);
        for (uint32_t k = 0; k < rn && 5 + k < n; k++) {
            uint32_t s = w[5 + k];
            if (s < an) r[k] = a[s];
            else if (s - an < bn) r[k] = b[s - an];
        }
        break;
    }
    case OpVectorExtractDynamic: {
        OPND(3, a);
        OPND(4, i);
        if ((i->f & F_K) && i->v < valueCells(c, w[3])) r[0] = a[i->v];
        break;
    }
    case OpVectorInsertDynamic: {
        OPND(3, a);
        OPND(4, comp);
        OPND(5, i);
        if ((i->f & F_K) && i->v < rn) {
            for (uint32_t k = 0; k < rn; k++) r[k] = a[k];
            r[i->v] = comp[0];
        }
        break;
    }
    case OpSelect: {
        OPND(3, cond);
        OPND(4, a);
        OPND(5, b);
        uint32_t cn = valueCells(c, w[3]);
        for (uint32_t k = 0; k < rn; k++) {
            const Cell* ck = cn == 1 ? cond : k < cn ? &cond[k] : NULL;
            if (ck && (ck->f & F_K)) r[k] = ck->v ? a[k] : b[k];
        }
        break;
    }
    case OpFNegate: case OpSNegate: case OpNot: case OpLogicalNot: case OpIsNan: case OpIsInf: {
        OPND(3, a);
        int cls = op == OpIsNan || op == OpIsInf ? classOf(c, idOf(c, w[3])->type) : rcls;
        if ((op == OpIsNan || op == OpIsInf) && cls != C_F32) break;
        if (valueCells(c, w[3]) != rn) break;
        for (uint32_t k = 0; k < rn; k++) scalar1(op, cls, &a[k], &r[k]);
        break;
    }
    case OpIAdd: case OpFAdd: case OpISub: case OpFSub: case OpIMul: case OpFMul: case OpUDiv: case OpSDiv: case OpFDiv:
    case OpUMod: case OpSRem: case OpSMod: case OpShiftRightLogical: case OpShiftRightArithmetic: case OpShiftLeftLogical:
    case OpBitwiseOr: case OpBitwiseXor: case OpBitwiseAnd: case OpLogicalEqual: case OpLogicalNotEqual: case OpLogicalOr:
    case OpLogicalAnd: case OpIEqual: case OpINotEqual: case OpUGreaterThan: case OpSGreaterThan: case OpUGreaterThanEqual:
    case OpSGreaterThanEqual: case OpULessThan: case OpSLessThan: case OpULessThanEqual: case OpSLessThanEqual:
    case OpFOrdEqual: case OpFUnordEqual: case OpFOrdNotEqual: case OpFUnordNotEqual: case OpFOrdLessThan:
    case OpFUnordLessThan: case OpFOrdGreaterThan: case OpFUnordGreaterThan: case OpFOrdLessThanEqual:
    case OpFUnordLessThanEqual: case OpFOrdGreaterThanEqual: case OpFUnordGreaterThanEqual: {
        OPND(3, a);
        OPND(4, b);
        int cls = classOf(c, idOf(c, w[3])->type);
        uint32_t an = valueCells(c, w[3]), bn = valueCells(c, w[4]);
        /* shifts may have a differently signed but equally wide shift operand */
        if (an != rn || bn != rn || cls != classOf(c, idOf(c, w[4])->type)) break;
        for (uint32_t k = 0; k < rn; k++) scalar2(op, cls, &a[k], &b[k], &r[k]);
        break;
    }
    case OpBitFieldInsert: {
        OPND(3, base);
        OPND(4, ins);
        OPND(5, off);
        OPND(6, cnt);
        if (rcls != C_I32 || !(off->f & F_K) || !(cnt->f & F_K) || off->v > 32 || cnt->v > 32 - off->v) break;
        uint32_t mask = cnt->v == 32 ? 0xffffffffu : ((1u << cnt->v) - 1u) << off->v;
        for (uint32_t k = 0; k < rn; k++)
            if ((base[k].f & F_K) && (ins[k].f & F_K)) r[k] = K((base[k].v & ~mask) | ((ins[k].v << off->v) & mask));
        break;
    }
    case OpBitFieldSExtract: case OpBitFieldUExtract: {
        OPND(3, base);
        OPND(4, off);
        OPND(5, cnt);
        if (rcls != C_I32 || !(off->f & F_K) || !(cnt->f & F_K) || off->v > 32 || cnt->v > 32 - off->v) break;
        for (uint32_t k = 0; k < rn; k++) {
            if (!(base[k].f & F_K)) continue;
            if (!cnt->v) {
                r[k] = K(0);
                continue;
            }
            uint32_t v = base[k].v >> off->v;
            if (cnt->v < 32) {
                v &= (1u << cnt->v) - 1u;
                if (op == OpBitFieldSExtract && (v >> (cnt->v - 1)) & 1) v |= ~((1u << cnt->v) - 1u);
            }
            r[k] = K(v);
        }
        break;
    }
    case OpConvertFToU: case OpConvertFToS: case OpConvertSToF: case OpConvertUToF: case OpUConvert: case OpSConvert:
    case OpFConvert: case OpBitcast: {
        OPND(3, a);
        int scls = classOf(c, idOf(c, w[3])->type);
        if (valueCells(c, w[3]) != rn) break;
        for (uint32_t k = 0; k < rn; k++) {
            if (!(a[k].f & F_K) || (a[k].f & F_P)) continue;
            uint32_t x = a[k].v;
            float f = asF(x);
            switch (op) {
            case OpConvertFToU:
                if (scls == C_F32 && rcls == C_I32 && f > -1.0f && f < 4294967296.0f) r[k] = K((uint32_t)f);
                break;
            case OpConvertFToS:
                if (scls == C_F32 && rcls == C_I32 && f > -2147483649.0f && f < 2147483648.0f) r[k] = K((uint32_t)(int32_t)f);
                break;
            case OpConvertSToF: if (scls == C_I32 && rcls == C_F32) r[k] = K(asU((float)(int32_t)x)); break;
            case OpConvertUToF: if (scls == C_I32 && rcls == C_F32) r[k] = K(asU((float)x)); break;
            case OpUConvert: case OpSConvert: if (scls == C_I32 && rcls == C_I32) r[k] = K(x); break;
            case OpFConvert: if (scls == C_F32 && rcls == C_F32) r[k] = K(x); break;
            case OpBitcast: if (scls != C_X && scls != C_BOOL && rcls != C_X && rcls != C_BOOL) r[k] = K(x); break;
            }
        }
        break;
    }
    case OpVectorTimesScalar: case OpMatrixTimesScalar: {
        OPND(3, a);
        OPND(4, s);
        if (rcls != C_F32 || valueCells(c, w[3]) != rn) break;
        for (uint32_t k = 0; k < rn; k++) scalar2(OpFMul, C_F32, &a[k], s, &r[k]);
        break;
    }
    case OpDot: {
        OPND(3, a);
        OPND(4, b);
        uint32_t an = valueCells(c, w[3]);
        if (rcls != C_F32 || an != valueCells(c, w[4])) break;
        Cell acc = K(asU(0.0f)), m;
        bool ok = true;
        for (uint32_t k = 0; k < an && ok; k++)
            ok = scalar2(OpFMul, C_F32, &a[k], &b[k], &m) && scalar2(OpFAdd, C_F32, k ? &acc : &(Cell){asU(-0.0f), 0, F_K}, &m, &acc);
        if (ok) r[0] = acc;
        break;
    }
    case OpMatrixTimesVector: case OpVectorTimesMatrix: case OpMatrixTimesMatrix: {
        /* column-major: M[col][row] at col * rows + row */
        OPND(3, a);
        OPND(4, b);
        if (rcls != C_F32) break;
        const IdInfo* ta = idOf(c, idOf(c, w[3])->type);
        const IdInfo* tb = idOf(c, idOf(c, w[4])->type);
        if (!ta || !tb) break;
        uint32_t inner, outer, rows;
        if (op == OpMatrixTimesVector) { /* rows x cols  times cols */
            if (ta->kind != T_MATRIX) break;
            rows = cellsOfType(c, ta->elem), inner = ta->count, outer = 1;
        } else if (op == OpVectorTimesMatrix) { /* rows  times rows x cols */
            if (tb->kind != T_MATRIX) break;
            rows = 1, inner = cellsOfType(c, tb->elem), outer = tb->count;
        } else {
            if (ta->kind != T_MATRIX || tb->kind != T_MATRIX) break;
            rows = cellsOfType(c, ta->elem), inner = ta->count, outer = tb->count;
        }
        if (rows * outer != rn) break;
        for (uint32_t o = 0; o < outer; o++)
            for (uint32_t row = 0; row < rows; row++) {
                Cell acc = (Cell){asU(-0.0f), 0, F_K}, m;
                bool ok = true;
                for (uint32_t i = 0; i < inner && ok; i++) {
                    const Cell *x, *y;
                    if (op == OpMatrixTimesVector) x = &a[i * rows + row], y = &b[i];
                    else if (op == OpVectorTimesMatrix) x = &a[i], y = &b[o * inner + i];
                    else x = &a[i * rows + row], y = &b[o * inner + i];
                    ok = scalar2(OpFMul, C_F32, x, y, &m) && scalar2(OpFAdd, C_F32, &acc, &m, &acc);
                }
                if (ok) r[o * rows + row] = acc;
            }
        break;
    }
    case OpTranspose: {
        OPND(3, a);
        const IdInfo* ta = idOf(c, idOf(c, w[3])->type);
        const IdInfo* tr = idOf(c, rt);
        if (!ta || !tr || ta->kind != T_MATRIX || tr->kind != T_MATRIX) break;
        uint32_t rows = cellsOfType(c, ta->elem), cols = ta->count;
        if (rows * cols != rn) break;
        for (uint32_t col = 0; col < cols; col++)
            for (uint32_t row = 0; row < rows; row++) r[row * cols + col] = a[col * rows + row];
        break;
    }
    case OpAny: case OpAll: {
        OPND(3, a);
        uint32_t an = valueCells(c, w[3]);
        bool all = true, any = false, unknown = false;
        for (uint32_t k = 0; k < an; k++) {
            if (!(a[k].f & F_K)) unknown = true;
            else if (a[k].v) any = true;
            else all = false;
        }
        if (op == OpAny && (any || !unknown)) r[0] = K(any);
        if (op == OpAll && (!all || !unknown)) r[0] = K(all);
        break;
    }
    case OpExtInst: {
        if (n < 5) return 0;
        if (!c->p->glsl || w[3] != c->p->glsl) break;
        uint32_t inst = w[4], args = n - 5;
        const Cell* x[3] = {NULL, NULL, NULL};
        for (uint32_t i = 0; i < args && i < 3; i++) {
            x[i] = V(c, w[5 + i]);
            if (!x[i]) return -1;
            if (valueCells(c, w[5 + i]) != rn) return 1 + (commitUnknown(c, res), 0);
        }
        for (uint32_t k = 0; k < rn; k++) {
            if (args == 1) glsl1(inst, rcls, &x[0][k], &r[k]);
            else if (args == 2) glsl2(inst, rcls, &x[0][k], &x[1][k], &r[k]);
            else if (args == 3) glsl3(inst, rcls, &x[0][k], &x[1][k], &x[2][k], &r[k]);
        }
        break;
    }
    default:
        return 0;
    }
#undef OPND
    commit(c, res, r, rn);
    return 1;
}

/* ---- module-level constants with the pipeline's specialization ---- */

static bool specValue(const VkSpecializationInfo* spec, int32_t specId, uint32_t* out) {
    if (!spec || specId < 0) return false;
    for (uint32_t i = 0; i < spec->mapEntryCount && spec->pMapEntries; i++) {
        const VkSpecializationMapEntry* e = &spec->pMapEntries[i];
        if (e->constantID != (uint32_t)specId) continue;
        if (!spec->pData || e->offset > spec->dataSize || e->size > spec->dataSize - e->offset || !e->size || e->size > 8) return false;
        uint64_t v = 0;
        memcpy(&v, (const uint8_t*)spec->pData + e->offset, e->size); /* little endian */
        *out = (uint32_t)v;
        return true;
    }
    return false;
}

/* Values of the constants (and pointers to the global variables in M_RUN, set by the caller). */
static void initGlobals(Ctx* c) {
    const SpvProgram* p = c->p;
    for (uint32_t pos = 5; pos < p->firstFunction;) {
        uint32_t n, op;
        const uint32_t* w = insn(p, pos, &n, &op);
        pos += n;
        if (n < 3) continue;
        uint32_t rt = w[1], res = w[2];
        const IdInfo* t = idOf(c, rt);
        Cell* r = c->tmp;
        uint32_t rn = cellsOfType(c, rt);
        switch (op) {
        case OpConstantTrue: case OpConstantFalse: case OpSpecConstantTrue: case OpSpecConstantFalse: {
            uint32_t v = op == OpConstantTrue || op == OpSpecConstantTrue;
            uint32_t s;
            if ((op == OpSpecConstantTrue || op == OpSpecConstantFalse) && specValue(c->spec, p->ids[res].specId, &s)) v = s != 0;
            r[0] = K(v);
            commit(c, res, r, 1);
            break;
        }
        case OpConstant: case OpSpecConstant: {
            uint32_t v = n > 3 ? w[3] : 0, s;
            bool known = t && (t->kind == T_INT || t->kind == T_FLOAT) && t->width == 32 && n == 4;
            if (op == OpSpecConstant && specValue(c->spec, p->ids[res].specId, &s)) v = s;
            r[0] = known ? K(v) : (Cell){0, 0, 0};
            commit(c, res, r, 1);
            break;
        }
        case OpConstantNull:
            for (uint32_t k = 0; k < rn; k++) r[k] = K(0);
            commit(c, res, r, rn);
            break;
        case OpConstantComposite: case OpSpecConstantComposite: {
            uint32_t o = 0;
            bool fits = true;
            for (uint32_t i = 3; i < n && fits; i++) {
                const Cell* a = V(c, w[i]);
                uint32_t an = valueCells(c, w[i]);
                if (!a || o + an > rn) {
                    fits = false;
                    break;
                }
                for (uint32_t k = 0; k < an; k++) r[o + k] = a[k];
                o += an;
            }
            if (!fits || o != rn) commitUnknown(c, res);
            else commit(c, res, r, rn);
            break;
        }
        case OpSpecConstantOp: {
            /* the inner instruction as if it were in a function: opcode, result type, result, operands */
            uint32_t inner[64];
            if (n < 4 || n - 1 > 64) {
                commitUnknown(c, res);
                break;
            }
            inner[0] = ((n - 1) << 16) | (w[3] & 0xffff);
            inner[1] = rt;
            inner[2] = res;
            for (uint32_t i = 4; i < n; i++) inner[i - 1] = w[i];
            if (evalPure(c, inner, n - 1, w[3]) != 1) commitUnknown(c, res);
            break;
        }
        case OpUndef:
            commitUnknown(c, res);
            break;
        default:
            break;
        }
    }
}

static bool ctxInit(Ctx* c, const SpvProgram* p, int mode, const VkSpecializationInfo* spec) {
    memset(c, 0, sizeof *c);
    c->p = p;
    c->mode = mode;
    c->spec = spec;
    c->cells = calloc(p->slotCells ? p->slotCells : 1, sizeof *c->cells);
    c->tmp = calloc(p->maxCells ? p->maxCells : 1, sizeof *c->tmp);
    c->defined = mode == M_SCCP ? calloc(p->bound, 1) : NULL;
    if (!c->cells || !c->tmp || (mode == M_SCCP && !c->defined)) return false;
    initGlobals(c);
    return true;
}

static void ctxFree(Ctx* c) {
    free(c->cells);
    free(c->tmp);
    free(c->defined);
    free(c->mem);
    free(c->vars);
}

/* ---- fragment shaders: can a discard be reached ---- */

static uint32_t rootVariable(const SpvProgram* p, uint32_t id) {
    for (int i = 0; i < 64 && id && id < p->bound && p->ids[id].at; i++) {
        uint32_t n, op;
        const uint32_t* w = insn(p, p->ids[id].at, &n, &op);
        if (op == OpVariable) return id;
        if ((op == OpAccessChain || op == OpInBoundsAccessChain || op == OpPtrAccessChain || op == OpInBoundsPtrAccessChain ||
             op == OpCopyObject) && n >= 4)
            id = w[3];
        else
            return 0;
    }
    return 0;
}

/* Words per OpSwitch case literal for selector `sel`; 0 when its width is not known. */
static uint32_t switchLiteralWords(const Ctx* c, uint32_t sel) {
    const IdInfo* i = idOf(c, sel);
    const IdInfo* t = i ? idOf(c, i->type) : NULL;
    if (!t || t->kind != T_INT) return 0;
    return t->width <= 32 ? 1 : t->width == 64 ? 2 : 0;
}

/* Whether the edge from block `from` (executable) to `to` is taken with what is known now. */
static bool edgeFeasible(Ctx* c, const uint8_t* exec, uint32_t from, uint32_t to) {
    const IdInfo* f = idOf(c, from);
    if (!f || !exec[from] || !f->cells) return false;
    uint32_t n, op;
    const uint32_t* w = insn(c->p, f->cells, &n, &op);
    if (op == OpBranch) return n >= 2 && w[1] == to;
    if (op == OpBranchConditional) {
        if (n < 4) return false;
        const Cell* cond = V(c, w[1]);
        if (!cond) return false;
        if (!(cond->f & F_K)) return w[2] == to || w[3] == to;
        return (cond->v ? w[2] : w[3]) == to;
    }
    if (op == OpSwitch) {
        if (n < 3) return false;
        const Cell* sel = V(c, w[1]);
        if (!sel) return false;
        uint32_t lw = switchLiteralWords(c, w[1]);
        if (!lw) return true;
        bool known = (sel->f & F_K) && lw == 1 && valueCells(c, w[1]) == 1 && classOf(c, idOf(c, w[1])->type) == C_I32;
        if (!known) {
            if (w[2] == to) return true;
            for (uint32_t i = 3; i + lw < n; i += lw + 1)
                if (w[i + lw] == to) return true;
            return false;
        }
        for (uint32_t i = 3; i + 1 < n; i += 2)
            if (w[i] == sel->v) return w[i + 1] == to;
        return w[2] == to;
    }
    return false;
}

static void markBlock(Ctx* c, uint8_t* exec, uint32_t label) {
    if (label < c->p->bound && !exec[label]) {
        exec[label] = 1;
        c->changed = true;
    }
}

int spv_fs_may_discard(const SpvProgram* p, const char* entry, const VkSpecializationInfo* spec) {
    const Entry* e = findEntry(p, entry, 4);
    if (!e) return SPV_DISCARD_UNKNOWN;
    Ctx ctx;
    Ctx* c = &ctx;
    uint8_t* exec = calloc(p->bound, 1);    /* labels and functions */
    int result = SPV_DISCARD_UNKNOWN;
    if (!exec || !ctxInit(c, p, M_SCCP, spec)) goto out;
    const Fn* main = findFn(p, e->fn);
    if (!main || !main->firstLabel) goto out;
    exec[main->id] = 1;
    exec[main->firstLabel] = 1;
    for (int round = 0; round < 200; round++) {
        c->changed = false;
        for (uint32_t fi = 0; fi < p->fnCount; fi++) {
            const Fn* f = &p->fns[fi];
            if (!exec[f->id]) continue;
            uint32_t block = 0;
            bool live = false;
            for (uint32_t pos = f->at; pos < f->end;) {
                uint32_t n, op;
                const uint32_t* w = insn(p, pos, &n, &op);
                pos += n;
                if (op == OpLabel) {
                    block = w[1];
                    live = exec[block];
                    continue;
                }
                if (!live) continue;
                switch (op) {
                case OpKill: case OpTerminateInvocation: case OpDemoteToHelperInvocation:
                    result = SPV_DISCARD_MAY;
                    goto out;
                case OpStore: case OpCopyMemory: case OpCopyMemorySized: {
                    if (n < 2) {
                        result = SPV_DISCARD_UNKNOWN;
                        goto out;
                    }
                    uint32_t var = rootVariable(p, w[1]);
                    if (!var && n >= 2) { /* a store through a pointer not traced to a variable */
                        uint32_t pt = idOf(c, w[1]) ? p->ids[w[1]].type : 0;
                        if (pt < p->bound && p->ids[pt].kind == T_POINTER && p->ids[pt].sc == ScOutput) {
                            result = SPV_DISCARD_UNKNOWN;
                            goto out;
                        }
                    }
                    if (var && p->ids[var].builtin == BiSampleMask) {
                        result = SPV_DISCARD_MAY;
                        goto out;
                    }
                    break;
                }
                case OpPhi: {
                    if (n < 3) break;
                    uint32_t rn = cellsOfType(c, w[1]);
                    Cell* r = c->tmp;
                    bool any = false;
                    Cell merged[64];
                    if (rn > 64) {
                        commitUnknown(c, w[2]);
                        break;
                    }
                    for (uint32_t i = 3; i + 1 < n; i += 2) {
                        if (!edgeFeasible(c, exec, w[i + 1], block)) continue;
                        const Cell* v = V(c, w[i]);
                        if (!v) continue;
                        if (!any) {
                            for (uint32_t k = 0; k < rn; k++) merged[k] = v[k];
                            any = true;
                        } else {
                            for (uint32_t k = 0; k < rn; k++)
                                if (!((merged[k].f & F_K) && (v[k].f & F_K) && merged[k].v == v[k].v && merged[k].f == v[k].f))
                                    merged[k] = (Cell){0, 0, 0};
                        }
                    }
                    if (any) {
                        for (uint32_t k = 0; k < rn; k++) r[k] = merged[k];
                        commit(c, w[2], r, rn);
                    }
                    break;
                }
                case OpBranch:
                    if (n >= 2) markBlock(c, exec, w[1]);
                    break;
                case OpBranchConditional: case OpSwitch: {
                    if (n < 3) break;
                    uint32_t targets[2] = {w[2], op == OpBranchConditional && n >= 4 ? w[3] : 0};
                    for (int i = 0; i < 2; i++)
                        if (targets[i] && edgeFeasible(c, exec, block, targets[i])) markBlock(c, exec, targets[i]);
                    uint32_t lw = op == OpSwitch ? switchLiteralWords(c, w[1]) : 0;
                    if (op == OpSwitch && !lw) {
                        result = SPV_DISCARD_UNKNOWN;
                        goto out;
                    }
                    for (uint32_t i = 3; op == OpSwitch && i + lw < n; i += lw + 1)
                        if (edgeFeasible(c, exec, block, w[i + lw])) markBlock(c, exec, w[i + lw]);
                    break;
                }
                case OpReturnValue: {
                    const Cell* v = n >= 2 ? V(c, w[1]) : NULL;
                    if (v) commit(c, f->id, v, valueCells(c, w[1]));
                    break;
                }
                case OpFunctionCall: {
                    if (n < 4) break;
                    const Fn* g = findFn(p, w[3]);
                    if (!g || !g->firstLabel) {
                        result = SPV_DISCARD_UNKNOWN;
                        goto out;
                    }
                    if (!exec[g->id]) {
                        exec[g->id] = 1;
                        c->changed = true;
                    }
                    markBlock(c, exec, g->firstLabel);
                    /* arguments into the callee's parameters */
                    uint32_t ppos = g->at + (p->code[g->at] >> 16);
                    for (uint32_t i = 4; i < n && ppos < g->end; i++) {
                        uint32_t pn, pop;
                        const uint32_t* pw = insn(p, ppos, &pn, &pop);
                        if (pop != OpFunctionParameter) break;
                        const Cell* a = V(c, w[i]);
                        if (a) commit(c, pw[2], a, valueCells(c, w[i]));
                        ppos += pn;
                    }
                    const Cell* rv = V(c, g->id);
                    if (rv) commit(c, w[2], rv, valueCells(c, w[2]));
                    break;
                }
                default: {
                    bool hasResult, hasType;
                    knownOp(op, &hasResult, &hasType);
                    if (!hasResult || !hasType) break;
                    int r = evalPure(c, w, n, op);
                    if (r == 0) commitUnknown(c, w[2]); /* loads, images, ...: not constants */
                    break;
                }
                }
            }
        }
        if (!c->changed) {
            result = SPV_DISCARD_NO;
            goto out;
        }
    }
out:
    ctxFree(c);
    free(exec);
    return result;
}

/* ---- vertex shaders on the CPU ---- */

#define RUN_STEPS 200000
#define RUN_MEM (1u << 20)
#define RUN_DEPTH 32

static bool opaqueClass(uint32_t sc) {
    return sc != ScInput && sc != ScOutput && sc != ScPrivate && sc != ScFunction;
}

/* A variable of `cells` cells: its index, or NONE when out of room. */
static uint32_t newVar(Ctx* c, uint32_t type, uint32_t sc, int32_t builtin) {
    uint32_t cells = cellsOfType(c, type);
    if (c->memUsed + cells > RUN_MEM) return NONE;
    if (c->memUsed + cells > c->memCap) {
        uint32_t cap = c->memCap ? c->memCap : 1024;
        while (cap < c->memUsed + cells) cap *= 2;
        Cell* m = realloc(c->mem, cap * sizeof *m);
        if (!m) return NONE;
        c->mem = m;
        c->memCap = cap;
    }
    if (c->varCount == c->varCap) {
        uint32_t cap = c->varCap ? c->varCap * 2 : 64;
        Var* v = realloc(c->vars, cap * sizeof *v);
        if (!v) return NONE;
        c->vars = v;
        c->varCap = cap;
    }
    c->vars[c->varCount] = (Var){c->memUsed, cells, sc, builtin, type};
    for (uint32_t k = 0; k < cells; k++) c->mem[c->memUsed + k] = (Cell){0, 0, 0};
    c->memUsed += cells;
    return c->varCount++;
}

static void forgetVar(Ctx* c, uint32_t var) {
    Var* v = &c->vars[var];
    for (uint32_t k = 0; k < v->n; k++) c->mem[v->off + k] = (Cell){0, 0, F_W};
}

static void forgetAll(Ctx* c) {
    for (uint32_t i = 0; i < c->varCount; i++)
        if (!opaqueClass(c->vars[i].sc) && c->vars[i].sc != ScInput) forgetVar(c, i);
}

/* An Input variable's value from the vertex. */
static void fillInput(Ctx* c, uint32_t var, uint32_t id, const SpvVertexIn* in) {
    const SpvProgram* p = c->p;
    Var* v = &c->vars[var];
    Cell* m = &c->mem[v->off];
    int32_t b = p->ids[id].builtin;
    if (b >= 0) {
        uint32_t value;
        switch (b) {
        case BiVertexIndex: case BiVertexId: value = in->vertexIndex; break;
        case BiInstanceIndex: case BiInstanceId: value = in->instanceIndex; break;
        case BiBaseVertex: value = in->baseVertex; break;
        case BiBaseInstance: value = in->baseInstance; break;
        case BiDrawIndex: value = in->drawIndex; break;
        default: return;
        }
        if (v->n == 1 && classOf(c, v->type) == C_I32) m[0] = K(value);
        return;
    }
    int32_t loc = p->ids[id].location, comp = p->ids[id].component < 0 ? 0 : p->ids[id].component;
    if (loc < 0 || loc >= SPV_LOCATIONS || v->n + (uint32_t)comp > 4) return;
    const IdInfo* t = idOf(c, v->type);
    if (!t || (t->kind != T_VECTOR && t->kind != T_INT && t->kind != T_FLOAT)) return;
    const SpvAttr* a = &in->location[loc];
    int cls = classOf(c, v->type);
    const IdInfo* s = scalarOf(c, v->type);
    bool fits = (cls == C_F32 && a->kind == SPV_ATTR_FLOAT) ||
                (cls == C_I32 && s && (a->kind == SPV_ATTR_SINT || a->kind == SPV_ATTR_UINT));
    if (!fits) return;
    for (uint32_t k = 0; k < v->n; k++) m[k] = K(a->value[comp + k]);
}

typedef struct {
    uint32_t retPos, result, fnIndex, prevLabel, curLabel;
} Frame;

/* the phis at the head of the block at `pos`, all read before any is written */
static bool enterBlock(Ctx* c, uint32_t* pos, uint32_t prev) {
    const SpvProgram* p = c->p;
    uint32_t n, op;
    insn(p, *pos, &n, &op);
    if (op != OpLabel) return false;
    uint32_t at = *pos + n;
    /* first pass: values into a scratch area past the slots of every phi */
    uint32_t count = 0, scratch = 0;
    for (uint32_t q = at; q < p->words; count++) {
        const uint32_t* w = insn(p, q, &n, &op);
        if (op != OpPhi) break;
        scratch += cellsOfType(c, w[1]);
        q += n;
    }
    if (!count) {
        *pos = at;
        return true;
    }
    Cell* buf = malloc((scratch ? scratch : 1) * sizeof *buf);
    if (!buf) return false;
    uint32_t o = 0, q = at;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t* w = insn(p, q, &n, &op);
        uint32_t rn = cellsOfType(c, w[1]);
        bool found = false;
        for (uint32_t j = 3; j + 1 < n; j += 2) {
            if (w[j + 1] != prev) continue;
            const Cell* v = V(c, w[j]);
            for (uint32_t k = 0; k < rn; k++) buf[o + k] = v && k < valueCells(c, w[j]) ? v[k] : (Cell){0, 0, 0};
            found = true;
            break;
        }
        if (!found)
            for (uint32_t k = 0; k < rn; k++) buf[o + k] = (Cell){0, 0, 0};
        o += rn;
        q += n;
    }
    o = 0, q = at;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t* w = insn(p, q, &n, &op);
        uint32_t rn = cellsOfType(c, w[1]);
        commit(c, w[2], &buf[o], rn);
        o += rn;
        q += n;
    }
    free(buf);
    *pos = q;
    return true;
}

static int runVertex(Ctx* c, const Fn* main, const SpvVertexIn* in, SpvVertexOut* out) {
    const SpvProgram* p = c->p;
    /* fresh memory: global variables */
    c->memUsed = 0;
    c->varCount = 0;
    for (uint32_t pos = 5; pos < p->firstFunction;) {
        uint32_t n, op;
        const uint32_t* w = insn(p, pos, &n, &op);
        pos += n;
        if (op != OpVariable || n < 4) continue;
        const IdInfo* pt = idOf(c, w[1]);
        if (!pt || pt->kind != T_POINTER) return SPV_RUN_UNSUPPORTED;
        uint32_t var = newVar(c, pt->elem, w[3], p->ids[w[2]].builtin);
        if (var == NONE) return SPV_RUN_BUDGET;
        Var* v = &c->vars[var];
        if (w[3] == ScInput) fillInput(c, var, w[2], in);
        if (w[3] == ScPrivate && n >= 5) {
            const Cell* init = V(c, w[4]);
            for (uint32_t k = 0; init && k < v->n && k < valueCells(c, w[4]); k++) c->mem[v->off + k] = init[k];
        }
        Cell ptr = {var, 0, F_K | F_P};
        commit(c, w[2], &ptr, 1);
    }
    Frame stack[RUN_DEPTH];
    int depth = 0;
    uint32_t pos = 0;
    {
        uint32_t n, op;
        pos = main->at;
        insn(p, pos, &n, &op);
        pos += n;
        /* the entry point has no parameters */
        uint32_t label = main->firstLabel;
        pos = p->ids[label].at;
        if (!enterBlock(c, &pos, 0)) return SPV_RUN_UNSUPPORTED;
        stack[0] = (Frame){0, 0, (uint32_t)(main - p->fns), 0, label};
        depth = 1;
    }
    while (depth > 0) {
        if (++c->steps > RUN_STEPS) return SPV_RUN_BUDGET;
        if (pos >= p->words) return SPV_RUN_UNSUPPORTED;
        uint32_t n, op;
        const uint32_t* w = insn(p, pos, &n, &op);
        Frame* fr = &stack[depth - 1];
        uint32_t next = pos + n;
        switch (op) {
        case OpLoopMerge: case OpSelectionMerge: case 8 /* OpLine */: case 317 /* OpNoLine */: case 0: case 256: case 257:
            break;
        case OpBranch: case OpBranchConditional: case OpSwitch: {
            uint32_t target = 0;
            if (op == OpBranch) {
                target = n >= 2 ? w[1] : 0;
            } else if (op == OpBranchConditional) {
                const Cell* cond = n >= 4 ? V(c, w[1]) : NULL;
                if (!cond || !(cond->f & F_K)) return SPV_RUN_CONTROL;
                target = cond->v ? w[2] : w[3];
            } else {
                const Cell* sel = n >= 3 ? V(c, w[1]) : NULL;
                if (!sel || !(sel->f & F_K) || valueCells(c, w[1]) != 1 || classOf(c, idOf(c, w[1])->type) != C_I32)
                    return SPV_RUN_CONTROL;
                target = w[2];
                for (uint32_t i = 3; i + 1 < n; i += 2)
                    if (w[i] == sel->v) {
                        target = w[i + 1];
                        break;
                    }
            }
            const IdInfo* t = idOf(c, target);
            if (!t || !t->at) return SPV_RUN_UNSUPPORTED;
            uint32_t from = fr->curLabel;
            next = t->at;
            if (!enterBlock(c, &next, from)) return SPV_RUN_UNSUPPORTED;
            fr->prevLabel = from;
            fr->curLabel = target;
            break;
        }
        case OpReturn: case OpReturnValue: {
            Frame done = *fr;
            depth--;
            if (depth > 0 && op == OpReturnValue && n >= 2 && done.result) {
                const Cell* v = V(c, w[1]);
                commit(c, done.result, v, v ? valueCells(c, w[1]) : 0);
            } else if (depth > 0 && done.result) {
                commitUnknown(c, done.result);
            }
            next = done.retPos;
            break;
        }
        case OpFunctionCall: {
            if (n < 4 || depth == RUN_DEPTH) return SPV_RUN_CONTROL;
            const Fn* g = findFn(p, w[3]);
            if (!g || !g->firstLabel) return SPV_RUN_UNSUPPORTED;
            uint32_t ppos = g->at + (p->code[g->at] >> 16);
            for (uint32_t i = 4; i < n; i++) {
                uint32_t pn, pop;
                const uint32_t* pw = insn(p, ppos, &pn, &pop);
                if (pop != OpFunctionParameter) return SPV_RUN_UNSUPPORTED;
                const Cell* a = V(c, w[i]);
                commit(c, pw[2], a, a ? valueCells(c, w[i]) : 0);
                ppos += pn;
            }
            stack[depth++] = (Frame){next, w[2], (uint32_t)(g - p->fns), 0, g->firstLabel};
            next = p->ids[g->firstLabel].at;
            if (!enterBlock(c, &next, 0)) return SPV_RUN_UNSUPPORTED;
            break;
        }
        case OpVariable: {
            const IdInfo* pt = idOf(c, w[1]);
            if (n < 4 || !pt || pt->kind != T_POINTER) return SPV_RUN_UNSUPPORTED;
            uint32_t var = newVar(c, pt->elem, w[3], -1);
            if (var == NONE) return SPV_RUN_BUDGET;
            if (n >= 5) {
                const Cell* init = V(c, w[4]);
                Var* v = &c->vars[var];
                for (uint32_t k = 0; init && k < v->n && k < valueCells(c, w[4]); k++) c->mem[v->off + k] = init[k];
            }
            Cell ptr = {var, 0, F_K | F_P};
            commit(c, w[2], &ptr, 1);
            break;
        }
        case OpAccessChain: case OpInBoundsAccessChain: {
            if (n < 4) return SPV_RUN_UNSUPPORTED;
            const Cell* base = V(c, w[3]);
            Cell ptr = {0, 0, 0};
            if (base && (base->f & F_P)) {
                ptr = (Cell){base->v, NONE, F_P};
                const Var* v = &c->vars[base->v];
                uint32_t bt = idOf(c, idOf(c, w[3])->type)->elem, off, t;
                if ((base->f & F_K) && walk(c, bt, w + 4, n - 4, true, &off, &t) && base->p + off < v->n) {
                    ptr.p = base->p + off;
                    ptr.f |= F_K;
                }
            }
            commit(c, w[2], &ptr, 1);
            break;
        }
        case OpLoad: {
            if (n < 4) return SPV_RUN_UNSUPPORTED;
            const Cell* ptr = V(c, w[3]);
            uint32_t rn = cellsOfType(c, w[1]);
            Cell* r = c->tmp;
            for (uint32_t k = 0; k < rn; k++) r[k] = (Cell){0, 0, 0};
            if (ptr && (ptr->f & F_P) && (ptr->f & F_K)) {
                const Var* v = &c->vars[ptr->v];
                if (!opaqueClass(v->sc) && ptr->p + rn <= v->n)
                    for (uint32_t k = 0; k < rn; k++) {
                        r[k] = c->mem[v->off + ptr->p + k];
                        r[k].f &= ~F_W;
                    }
            }
            commit(c, w[2], r, rn);
            break;
        }
        case OpStore: case OpCopyMemory: {
            if (n < 3) return SPV_RUN_UNSUPPORTED;
            const Cell* ptr = V(c, w[1]);
            const Cell* src = NULL;
            uint32_t sn = 0;
            Cell* loaded = c->tmp;
            if (op == OpStore) {
                src = V(c, w[2]);
                sn = valueCells(c, w[2]);
            } else {
                const Cell* sp = V(c, w[2]);
                const IdInfo* spt = idOf(c, idOf(c, w[2])->type);
                sn = spt ? cellsOfType(c, spt->elem) : 0;
                for (uint32_t k = 0; k < sn; k++) loaded[k] = (Cell){0, 0, 0};
                if (sp && (sp->f & F_P) && (sp->f & F_K)) {
                    const Var* sv = &c->vars[sp->v];
                    if (!opaqueClass(sv->sc) && sp->p + sn <= sv->n)
                        for (uint32_t k = 0; k < sn; k++) loaded[k] = c->mem[sv->off + sp->p + k];
                }
                src = loaded;
            }
            if (!ptr || !(ptr->f & F_P)) {
                forgetAll(c);
                break;
            }
            Var* v = &c->vars[ptr->v];
            if (opaqueClass(v->sc)) break;
            if (!(ptr->f & F_K) || ptr->p + sn > v->n || !src) {
                forgetVar(c, ptr->v);
                break;
            }
            for (uint32_t k = 0; k < sn; k++) {
                c->mem[v->off + ptr->p + k] = src[k];
                c->mem[v->off + ptr->p + k].f |= F_W;
            }
            break;
        }
        case OpKill: case OpTerminateInvocation: case OpDemoteToHelperInvocation: case OpUnreachable: case OpFunctionEnd:
        case OpLabel: case OpPhi: case OpFunction: case OpFunctionParameter:
            return SPV_RUN_CONTROL;
        default: {
            bool hasResult, hasType;
            knownOp(op, &hasResult, &hasType);
            if (hasResult && hasType) {
                if (evalPure(c, w, n, op) != 1) commitUnknown(c, w[2]);
                /* pointers this run does not follow: what they may write is forgotten */
                const IdInfo* rt = idOf(c, w[1]);
                if (rt && rt->kind == T_POINTER && op != OpCopyObject && op != OpSelect) {
                    Cell none = {0, 0, 0};
                    commit(c, w[2], &none, 1);
                }
            } else if (op == OpAtomicStore) {
                forgetAll(c);
            }
            break;
        }
        }
        pos = next;
    }
    /* outputs */
    memset(out, 0, sizeof *out);
    for (uint32_t i = 0; i < c->varCount; i++) {
        const Var* v = &c->vars[i];
        if (v->sc != ScOutput) continue;
        const Cell* m = &c->mem[v->off];
        /* a variable that is the built-in, or a block with built-in members */
        uint32_t members[8][3]; /* builtin, cell offset, cells */
        uint32_t count = 0;
        if (v->builtin >= 0) {
            members[count][0] = (uint32_t)v->builtin, members[count][1] = 0, members[count][2] = v->n;
            count++;
        } else {
            const IdInfo* t = idOf(c, v->type);
            for (uint32_t j = 0; t && t->kind == T_STRUCT && j < p->mbCount && count < 8; j++) {
                if (p->mb[j].id != v->type) continue;
                uint32_t off = memberCells((SpvProgram*)p, v->type, p->mb[j].member, 0);
                uint32_t mn, mop;
                const uint32_t* tw = insn(p, t->at, &mn, &mop);
                if (off == NONE || p->mb[j].member >= mn - 2) continue;
                members[count][0] = (uint32_t)p->mb[j].builtin;
                members[count][1] = off;
                members[count][2] = cellsOfType(c, tw[2 + p->mb[j].member]);
                count++;
            }
        }
        for (uint32_t j = 0; j < count; j++) {
            uint32_t b = members[j][0], off = members[j][1], cn = members[j][2];
            if (off + cn > v->n) continue;
            const Cell* x = m + off;
            switch (b) {
            case BiPosition: {
                bool known = cn == 4;
                for (uint32_t k = 0; k < cn && known; k++) known = (x[k].f & F_K) && !(x[k].f & F_P);
                if (known)
                    for (int k = 0; k < 4; k++) out->position[k] = asF(x[k].v);
                out->positionKnown = known;
                break;
            }
            case BiClipDistance: case BiCullDistance: {
                uint8_t* st = b == BiClipDistance ? out->clipState : out->cullState;
                float* val = b == BiClipDistance ? out->clip : out->cull;
                if (b == BiClipDistance) out->clipCount = cn;
                else out->cullCount = cn;
                for (uint32_t k = 0; k < cn && k < SPV_CLIP_MAX; k++) {
                    st[k] = !(x[k].f & F_W) ? 0 : (x[k].f & F_K) ? 1 : 2;
                    val[k] = asF(x[k].v);
                }
                break;
            }
            case BiLayer: case BiViewportIndex: {
                uint8_t s = cn != 1 ? 2 : !(x[0].f & F_W) ? 0 : (x[0].f & F_K) ? 1 : 2;
                if (b == BiLayer) out->layerState = s, out->layer = x[0].v;
                else out->viewportState = s, out->viewport = x[0].v;
                break;
            }
            default: break;
            }
        }
    }
    return SPV_RUN_OK;
}

int spv_vs_run(const SpvProgram* p, const char* entry, const VkSpecializationInfo* spec, const SpvVertexIn* in, uint32_t count,
               SpvVertexOut* out) {
    const Entry* e = findEntry(p, entry, 0);
    const Fn* main = e ? findFn(p, e->fn) : NULL;
    if (!main || !main->firstLabel) return SPV_RUN_NO_ENTRY;
    Ctx ctx;
    int result = SPV_RUN_BUDGET;
    if (ctxInit(&ctx, p, M_RUN, spec)) {
        result = SPV_RUN_OK;
        for (uint32_t i = 0; i < count && result == SPV_RUN_OK; i++) result = runVertex(&ctx, main, &in[i], &out[i]);
    }
    ctxFree(&ctx);
    return result;
}

void spv_vs_inputs(const SpvProgram* p, const char* entry, uint8_t kinds[SPV_LOCATIONS]) {
    memset(kinds, 0, SPV_LOCATIONS);
    if (!findEntry(p, entry, 0)) return;
    for (uint32_t pos = 5; pos < p->firstFunction;) {
        uint32_t n, op;
        const uint32_t* w = insn(p, pos, &n, &op);
        pos += n;
        if (op != OpVariable || n < 4 || w[3] != ScInput) continue;
        const IdInfo* id = &p->ids[w[2]];
        if (id->location < 0 || id->location >= SPV_LOCATIONS) continue;
        const IdInfo* pt = w[1] < p->bound ? &p->ids[w[1]] : NULL;
        const IdInfo* t = pt && pt->elem < p->bound ? &p->ids[pt->elem] : NULL;
        for (int i = 0; t && i < 2 && t->kind == T_VECTOR; i++) t = t->elem < p->bound ? &p->ids[t->elem] : NULL;
        if (!t || t->width != 32) continue;
        kinds[id->location] = t->kind == T_FLOAT ? SPV_ATTR_FLOAT : t->kind == T_INT ? (t->sign ? SPV_ATTR_SINT : SPV_ATTR_UINT) : SPV_ATTR_NONE;
    }
}
