// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/set.hpp>
#include <boost/container/small_vector.hpp>
#include <algorithm>
#include <cstdlib>
#include "common/types.h"

namespace Serialization {
struct Archive;
}

namespace Shader {

using PFN_SrtWalker = void PS4_SYSV_ABI (*)(const u32* /*user_data*/, u32* /*flat_dst*/);
PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size);

// bbport: a ReadConst whose offset varies per lane (a constant block's pointer plus a per-pixel
// offset, as addr64 buffer loads compute it) reads a window of the block's first dwords that the
// walker copies to the flat buffer; its flags are the window's start with this bit.
constexpr u32 ReadConstWindowFlag = 1u << 16;
/// bbport: a window of ReadConstSmallWindowDwords only (the flat buffer had no room for more).
constexpr u32 ReadConstSmallWindowFlag = 1u << 17;
constexpr u32 ReadConstSmallWindowDwords = 256;

/// bbport: dwords of a ReadConst window. 256 held ~4 lights of U2's tiled lighting compute
/// shader (56 dwords each, up to 128 lights): the others read the window's last dword, passed
/// culling everywhere and lit every pixel (too bright, and ~26 ms a frame at a lamp-heavy view
/// instead of ~2). 8192 dwords (32 KiB) hold them all; few shaders use windows (7 binds a frame
/// in U2). BB_READCONST_WINDOW_DWORDS=N sets it (256: as before); the shader cache is rebuilt
/// when it changes (ShaderBinaryVersion).
inline u32 ReadConstWindowDwords() {
    static const u32 dwords = [] {
        const char* env = std::getenv("BB_READCONST_WINDOW_DWORDS");
        const u32 n = env ? u32(std::strtoul(env, nullptr, 10)) : 8192u;
        return n < ReadConstSmallWindowDwords ? ReadConstSmallWindowDwords
                                              : std::min<u32>(n, 32768u);
    }();
    return dwords;
}

struct PersistentSrtInfo {
    // Special case when fetch shader uses step rates.
    struct SrtSharpReservation {
        u32 sgpr_base;
        u32 dword_offset;
        u32 num_dwords;
    };

    PFN_SrtWalker walker_func{};
    size_t walker_func_size{};
    u32 flattened_bufsize_dw = 16; // NumUserDataRegs

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

} // namespace Shader
