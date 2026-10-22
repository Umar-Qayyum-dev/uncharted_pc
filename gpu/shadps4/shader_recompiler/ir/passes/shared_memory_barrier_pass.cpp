// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <unordered_set>
#include <vector>
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/ir/passes/ir_passes.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    const IR::Opcode opcode = inst.GetOpcode();
    if (opcode >= IR::Opcode::SharedAtomicIAdd32 && opcode <= IR::Opcode::SharedAtomicCmpSwap64) {
        return !inst.Flags<bool>();
    }
    return opcode == IR::Opcode::WriteSharedU16 || opcode == IR::Opcode::WriteSharedU32 ||
           opcode == IR::Opcode::WriteSharedU64;
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block) {
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        ir.Barrier();
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

static void EmitBarrierAtBlockStart(IR::Block* block) {
    auto insert_point = std::ranges::find_if_not(block->Instructions(), IR::IsPhi);
    IR::IREmitter ir{*block, insert_point};
    ir.Barrier();
}

static bool IsDivergent(const IR::U1& cond) {
    return IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
               if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                   inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
                   return true;
               }
               return std::nullopt;
           }) == true;
}

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth) {
    const IR::U1 cond = data.if_node.cond;
    if (IsDivergent(cond)) {
        if (divergence_depth == 0) {
            EmitBarrierAtBlockStart(data.if_node.merge);
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

// A barrier inside a loop is invalid when different invocations leave on different iterations.
// Mark such loops so their shared-memory synchronization can be deferred to the merge block.
static NodeSet FindDivergentLoops(const IR::AbstractSyntaxList& syntax_list) {
    NodeSet divergent_loops;
    for (const IR::AbstractSyntaxNode& node : syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Repeat:
            if (IsDivergent(node.data.repeat.cond)) {
                divergent_loops.emplace(node.data.repeat.merge);
            }
            break;
        case IR::AbstractSyntaxNode::Type::Break:
            if (IsDivergent(node.data.break_node.cond)) {
                divergent_loops.emplace(node.data.break_node.merge);
            }
            break;
        default:
            break;
        }
    }
    return divergent_loops;
}

static constexpr u32 GcnSubgroupSize = 64;

// bbport: workgroups larger than one wave. Their GCN code has s_barrier wherever more than one
// wave shares data, and none where only lanes of the first wave do: the last steps of tree
// reductions, `if (lane < 32) { lds[lane] += lds[lane + 32]; }` and so on. A barrier is added
// where such a step rejoins: after a top-level if (not inside any other if or loop) whose
// condition depends on the local invocation id and whose body writes shared memory. All
// invocations reach that point. Barriers deeper in the code (the 64-thread rules above) were
// tried and broke other shaders (U3's sun shadows: barriers inside branches whose divergence
// comes from data, which IsDivergent does not see). Shaders that return early from inside a
// branch get none.
static void EmitWaveStepBarriers(IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;
    std::vector<IR::Block*> merges;
    u32 nesting{};
    IR::Block* candidate{};
    bool writes{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::If:
            if (nesting == 0) {
                candidate = IsDivergent(node.data.if_node.cond) ? node.data.if_node.merge : nullptr;
                writes = false;
            }
            ++nesting;
            break;
        case Type::EndIf:
            --nesting;
            if (nesting == 0 && candidate && writes) {
                merges.push_back(node.data.end_if.merge);
            }
            break;
        case Type::Loop:
            if (nesting == 0) {
                candidate = nullptr;
            }
            ++nesting;
            break;
        case Type::Repeat:
            --nesting;
            break;
        case Type::Return:
        case Type::Unreachable:
            if (nesting != 0) {
                return;
            }
            break;
        case Type::Block:
            if (nesting != 0 && candidate && !writes) {
                writes = std::ranges::any_of(node.data.block->Instructions(),
                                             [](const IR::Inst& inst) { return IsWriteShared(inst); });
            }
            break;
        case Type::Break:
            break;
        }
    }
    for (IR::Block* merge : merges) {
        EmitBarrierAtBlockStart(merge);
    }
}

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.hw.cs;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup, or (bbport) in larger workgroups where only the first wave shares data
    // (EmitWaveStepBarriers: U3's exposure sums lost partial sums on 32-lane subgroups, the
    // measured brightness came out too low and the exposure went to its maximum).
    if (shared_memory_size == 0 || !profile.needs_lds_barriers) {
        return;
    }
    if (threadgroup_size > GcnSubgroupSize) {
        if (WaveLdsBarriers()) {
            EmitWaveStepBarriers(program);
        }
        return;
    }
    if (threadgroup_size != GcnSubgroupSize) {
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    NodeSet divergence_end;
    const NodeSet divergent_loops = FindDivergentLoops(program.syntax_list);
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth);
            continue;
        }
        if (node.type == Type::Loop && divergent_loops.contains(node.data.loop.merge)) {
            ++divergence_depth;
            continue;
        }
        if (node.type == Type::Repeat && divergent_loops.contains(node.data.repeat.merge)) {
            ASSERT(divergence_depth > 0);
            --divergence_depth;
            if (divergence_depth == 0) {
                EmitBarrierAtBlockStart(node.data.repeat.merge);
            }
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            EmitBarrierInBlock(node.data.block);
        }
    }
}

} // namespace Shader::Optimization
