#pragma once

#include "victory_abi.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace victory::hook {

struct PatchRecord {
    std::uintptr_t site = 0;
    std::vector<std::uint8_t> original_bytes;
    void* trampoline = nullptr;
    std::size_t trampoline_size = 0;
    bool applied = false;
};

struct DescriptorSiteDef {
    std::size_t window_rva = 0;
    std::size_t store_offset = 0;
    std::size_t store_len = 0;
    std::vector<std::uint8_t> expected_window;
};

/// Allocates executable memory within +/-2 GiB of `near_addr` when possible.
void* alloc_near_executable(std::uintptr_t near_addr, std::size_t bytes);

/// Verifies `expected` matches `[site, site + expected.size())`, then overwrites
/// with a 5-byte `jmp rel32` to `target` (padded with `0x90` NOPs). Requires
/// `expected.size() >= 5` and `|target - (site + 5)| <= INT32_MAX`.
bool patch_rel32_jump(
    std::uintptr_t site,
    const std::vector<std::uint8_t>& expected,
    std::uintptr_t target,
    PatchRecord& out_record,
    std::string& err);

/// Restores original bytes from `record`.
bool restore_patch(PatchRecord& record, std::string& err);

/// Parses a `vmovups [base+index], ymm0` store instruction and emits `lea rax, [base+index]`.
bool parse_vmovups_lea(const std::uint8_t* store, std::size_t len, std::uint8_t (&out_lea)[4]) noexcept;

/// Builds a stub at `stub_addr` that executes `store[0..store_len]`, checks `marks`
/// bounds, sets `slots[slot]=1` and `chunks[slot>>7]=1`, and jumps back to `site_addr + store_len`.
bool build_descriptor_mark_stub(
    const std::uint8_t* store,
    std::size_t store_len,
    std::uintptr_t site_addr,
    std::uintptr_t stub_addr,
    const VictoryDescriptorMarks* marks,
    std::vector<std::uint8_t>& out_stub);

/// Returns the 6 known GTA VI 1.0.2.1 descriptor-table record stores (`descriptor_sites::SITES`).
const std::vector<DescriptorSiteDef>& known_descriptor_sites();

/// Applies `sites` inside `[image_base, image_base + image_size)`. Returns number of sites patched.
std::size_t apply_descriptor_sites(
    std::uintptr_t image_base,
    std::size_t image_size,
    const VictoryDescriptorMarks* marks,
    const std::vector<DescriptorSiteDef>& sites,
    std::vector<PatchRecord>* out_records = nullptr);

/// Parses an `or byte ptr [mem], imm8` instruction (>= 5 bytes) and builds a look-first stub
/// that skips the memory write if the bit(s) are already set while preserving CPU flags.
bool build_or_byte_guard_stub(
    const std::uint8_t* code,
    std::size_t avail_len,
    std::uintptr_t site_addr,
    std::uintptr_t stub_addr,
    std::size_t& out_insn_len,
    std::vector<std::uint8_t>& out_stub);

/// Self-tests for near trampoline patching, descriptor-mark stubs, and write-site look-first stubs.
bool run_self_tests(std::string& report);

} // namespace victory::hook
