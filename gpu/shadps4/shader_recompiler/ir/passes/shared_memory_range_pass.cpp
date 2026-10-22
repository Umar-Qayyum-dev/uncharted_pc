// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

// bbport: GCN discards LDS writes beyond the workgroup's allocation and returns 0 for such
// reads. Vulkan leaves out-of-range shared memory accesses undefined: past the allocation this
// port keeps its wave64 scratch, past that the GPU returns whatever is there. Uncharted 3's
// cloth solver in its 1024-thread variant (cs 1c957b01, Sully's shirt in the Cartagena
// flashback) allocates 2 KiB and sums with shared[t] += shared[t + 512]: the upper half reads
// past the allocation, 0 on PS4. Here it read the last wave64 ballot (0xffffffff, a NaN) and
// garbage: NaN cloth vertices (the piece missing on NVIDIA, the "vortex" elsewhere).
// Each shared memory access is checked against the allocation: reads out of range return 0,
// writes and atomics go to an 8-byte sink after it.
static u32 AccessBytes(IR::Opcode opcode) {
    switch (opcode) {
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::WriteSharedU16:
        return 2;
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicISub32:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicCmpSwap32:
        return 4;
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::WriteSharedU64:
    case IR::Opcode::SharedAtomicIAdd64:
    case IR::Opcode::SharedAtomicISub64:
    case IR::Opcode::SharedAtomicSMin64:
    case IR::Opcode::SharedAtomicUMin64:
    case IR::Opcode::SharedAtomicSMax64:
    case IR::Opcode::SharedAtomicUMax64:
    case IR::Opcode::SharedAtomicInc64:
    case IR::Opcode::SharedAtomicDec64:
    case IR::Opcode::SharedAtomicAnd64:
    case IR::Opcode::SharedAtomicOr64:
    case IR::Opcode::SharedAtomicXor64:
    case IR::Opcode::SharedAtomicCmpSwap64:
        return 8;
    default:
        return 0;
    }
}

static bool IsSharedWrite(IR::Opcode opcode) {
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

static bool IsSharedLoad(IR::Opcode opcode) {
    return opcode == IR::Opcode::LoadSharedU16 || opcode == IR::Opcode::LoadSharedU32 ||
           opcode == IR::Opcode::LoadSharedU64;
}

void SharedMemoryRangePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    const u32 size = runtime_info.hw.cs.shared_memory_size;
    if (!LdsOutOfRange() || program.info.hw_stage != HwStage::Compute || size == 0) {
        return;
    }
    const u32 sink = Common::AlignUp(size, sizeof(u64));
    bool any = false;
    for (IR::Block* const block : program.blocks) {
        for (auto it = block->Instructions().begin(); it != block->Instructions().end();) {
            IR::Inst& inst = *it++;
            const IR::Opcode opcode = inst.GetOpcode();
            const u32 bytes = AccessBytes(opcode);
            if (bytes == 0) {
                continue;
            }
            const IR::Value address = inst.Arg(0);
            if (address.IsImmediate() && address.U32() + bytes <= size) {
                continue;
            }
            any = true;
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::U1 in_range =
                size >= bytes ? ir.ILessThan(IR::U32{address}, ir.Imm32(size - bytes + 1), false)
                              : ir.Imm1(false);
            if (IsSharedWrite(opcode)) {
                inst.SetArg(0, ir.Select(in_range, address, ir.Imm32(sink)));
                continue;
            }
            // Loads read address 0 and atomics work on the sink instead; their result is 0.
            const IR::Value safe =
                ir.Select(in_range, address, ir.Imm32(IsSharedLoad(opcode) ? 0 : sink));
            IR::Inst* const copy = &*block->PrependNewInst(
                IR::Block::InstructionList::s_iterator_to(inst), inst);
            copy->SetArg(0, safe);
            if (bytes == 2) {
                // No 16-bit select: through 32 bits.
                const IR::U32 wide{ir.UConvert(32, IR::U16{IR::Value{copy}})};
                inst.ReplaceUsesWithAndRemove(
                    ir.UConvert(16, IR::U32{ir.Select(in_range, wide, ir.Imm32(0u))}));
                continue;
            }
            const IR::Value zero =
                bytes == 8 ? IR::Value{ir.Imm64(u64{0})} : IR::Value{ir.Imm32(0u)};
            inst.ReplaceUsesWithAndRemove(ir.Select(in_range, IR::Value{copy}, zero));
        }
    }
    if (any) {
        program.info.shared_memory_scratch_size = sink + sizeof(u64) - size;
    }
}

} // namespace Shader::Optimization
