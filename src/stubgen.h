#pragma once

#include <cstdint>
#include <cstddef>

namespace eu4cjk::stubgen {

// Hardware register numbering (rax=0 .. r15=15), matches x86-64 encoding.
enum class Reg : uint8_t {
    Rax = 0, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi,
    R8, R9, R10, R11, R12, R13, R14, R15,
};

    struct StubSpec {
        struct Arg {
            enum Kind : uint8_t {
                None, Imm32, RegSrc, RbpSlot, FrameSlotLea, RbpLea, IdxSextSum,
            };
            Kind kind = None;
            uint32_t imm = 0;       // Imm32
            Reg src = Reg::Rax;     // RegSrc
            uint16_t rbp_off = 0;   // RbpSlot/RbpLea: [rbp + rbp_off] (rbp = entry_rsp - 8;
                                    //   RbpLea passes the ADDRESS, RbpSlot the value)
            uint8_t frame_off = 0;  // FrameSlotLea: lea arg, [rsp + frame_off]
            // IdxSextSum: arg = base + (int64_t)(int32_t)idx, i.e. a cursor into
            // a string held in a 32-bit index register (engine sign-extends at
            // use; arg must differ from idx/base - satisfied by arg regs).
            Reg idx = Reg::Rax;
            Reg base = Reg::Rax;    // valid when has_base
            uint32_t base_imm = 0;  // absolute base when !has_base
            bool has_base = false;
        };

    // Registers stored into frame slots [rsp+0 .. n_saves*8). 'restore=false'
    // keeps the slot as scratch for tail code without reloading the register
    // afterwards (site A: rax's final value comes from the tail, not a restore).
    struct Save {
        Reg reg;
        bool restore = true;
    };
    Save saves[4] = {};
    size_t n_saves = 0;

    // Optional addend applied to a restore-eligible save slot (cursor advance).
    // conditional=true applies the delta only when the consumed slot
    // ([rsp+advance_frame_off]) equals 3 (escape cursor +2; the engine's own
    // loop increment provides the +1). Used when the live cursor register is
    // rbp itself (the in-frame advance cannot touch the anchor register), so
    // the advance rides on the save slot instead.
    struct WriteBack {
        size_t save_index;
        int8_t delta;
        bool conditional = false;
    };
    WriteBack writebacks[4] = {};
    size_t n_writebacks = 0;

    // Call arguments in SysV order: args[0] -> rdi, args[1] -> rsi, ...,
    // args[5] -> r9. RegSrc args read LIVE registers pre-call; marshal
    // order is 0..5. Unused trailing args must be None (the register keeps
    // whatever it held - pass Imm32 0 explicitly when null is required).
    Arg args[6];

    uintptr_t fn = 0;            // C helper address

    // If set: 'mov result_reg, rax' right after the call (site A wants the
    // glyph in r15; site B leaves the result in rax untouched).
    bool has_result = false;
    Reg result_reg = Reg::Rax;

    // Raw bytes executed INSIDE the frame after the result move: rbp is valid
    // (RbpSlot addressing) and frame slots are live. Used to replicate the
    // register side effects of the replaced instructions (font into rcx, the
    // fetched byte into rax, ...).
    const uint8_t* tail = nullptr;
    size_t tail_len = 0;

    // Conditional cursor advance: if the u32 at [rsp+advance_frame_off] == 3,
    // add 2 to advance_reg (escape consumed 3 bytes; the caller's own loop
    // increment provides the remaining +1). advance2 (optional) applies the
    // same condition to a second register (RTS-1 also walks a copy cursor).
    bool has_advance = false;
    uint8_t advance_frame_off = 0;
    Reg advance_reg = Reg::Rax;
    bool has_advance2 = false;
    Reg advance2_reg = Reg::Rax;

    // Conditional cursor advance on a MEMORY index slot in the engine frame
    // (the measurement loops keep their cursor at engine [rsp+X], reloaded
    // each iteration): if consumed == 3, add 2 to the dword at
    // [rbp+advance_mem_rbp_off] where rbp is the stub anchor (entry_rsp-8),
    // i.e. pass engine_slot_offset + 8. Runs inside the frame, before the
    // saves are restored (unusable together with advance_reg on Rbp).
    bool has_advance_mem = false;
    uint16_t advance_mem_rbp_off = 0;

    // Original site instruction bytes re-executed after the frame is torn down
    // (rsp is back to entry value, so rsp-relative replays stay correct).
    const uint8_t* replay = nullptr;
    size_t replay_len = 0;

    // Raw bytes executed after replay, in full engine context (rbp restored).
    // Needed when the site's destination register is rbp itself, which the
    // in-frame tail cannot touch (frame anchor).
    const uint8_t* post_tail = nullptr;
    size_t post_tail_len = 0;

    // Entry-probe mode: save all volatile GPRs + xmm0-3 around the helper
    // call (at a function entry these hold live arguments). Incompatible
    // with saves/tail/advance/writebacks/result and FrameSlotLea args.
    bool save_volatile = false;

    uintptr_t resume = 0;        // absolute target of the final jmp
};

// Emits the stub into the shared relay page; returns its entry address, 0 on error.
uintptr_t emit(const StubSpec& spec);

// Copies pre-assembled code into the relay page (absolute targets baked in).
uintptr_t emit_raw(const uint8_t* code, size_t len);

// Reserves len bytes on the relay page and returns the address WITHOUT
// writing anything (page is RWX). For callers that must know the stub address
// before assembling rel32/disp32 fields, then fill the bytes in place.
uintptr_t reserve_code(size_t len);

// Reserves n bytes (8-aligned) of RW data on the relay page for stub-side
// rip-relative access (flags etc. shared with the C side). Null on exhaustion.
void* reserve_data(size_t n);

// Patches addr: verifies current bytes against orig[0..len), then writes
// E9 rel32 -> stub followed by NOP padding to len bytes.
bool install_jmp(uintptr_t addr, const uint8_t* orig, size_t len, uintptr_t stub);

} // namespace eu4cjk::stubgen
