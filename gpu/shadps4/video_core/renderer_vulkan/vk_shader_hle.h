// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/unique_function.h"

namespace AmdGpu {
struct ComputeProgram;
union Regs;
} // namespace AmdGpu

namespace Shader {
struct Info;
}

namespace Vulkan {

class Rasterizer;

/// bbport: shaders whose HLE writes guest memory. They run where the dispatch is processed,
/// not on the draw recording thread: the SRT walkers of the draws after them read that memory.
bool IsGuestMemoryShaderHLE(unsigned long long pgm_hash);

/// After the dispatch of such a shader was recorded (copies it queued behind it).
void AfterGuestMemoryShaderDispatch(Rasterizer& rasterizer);

/// Before an end-of-pipe fence the guest sees: what those copies wrote goes into guest memory
/// first. True when `signal` (the fence) was taken, to run once that is done.
bool DeferGuestFence(Rasterizer& rasterizer, Common::UniqueFunction<void>& signal);

/// Attempts to execute a shader using HLE if possible.
bool ExecuteShaderHLE(const Shader::Info& info, const AmdGpu::Regs& regs,
                      const AmdGpu::ComputeProgram& cs_program, Rasterizer& rasterizer);

} // namespace Vulkan
