#include "stubgen.h"
#include "injector.hpp"
#include "log.h"

#include <sys/mman.h>

#include <cstdio>
#include <cstring>

namespace eu4cjk::stubgen {

namespace {

constexpr size_t kPageSize = 0x10000; // one shared page serves many stubs

struct Emitter {
    uint8_t* p;
    uint8_t* base;
    void b(uint8_t x) { *p++ = x; }
    // Call with p at the rel32 field of a just-written 5-byte E9.
    // Returns false (nothing written) if target is outside rel32 range -
    // a silent truncation here jumps to a wrapped address (found the hard
    // way with a PIE test binary; the game itself is non-PIE and in range).
    bool jmp_rel32(uintptr_t target)
    {
        const int64_t rel = static_cast<int64_t>(target)
            - static_cast<int64_t>(reinterpret_cast<uintptr_t>(base)
                                   + static_cast<size_t>(p - base) + 4);
        if (rel > INT32_MAX || rel < INT32_MIN) return false;
        int32_t rel32 = static_cast<int32_t>(rel);
        std::memcpy(p, &rel32, 4);
        p += 4;
        return true;
    }
    void imm64(uintptr_t v) { std::memcpy(p, &v, 8); p += 8; }
    void imm32(uint32_t v) { std::memcpy(p, &v, 4); p += 4; }
};

void* map_page()
{
    // rel32-reachable from the game image (non-PIE @ 0x400000-0x3520000).
    // M2's glyph-gate relay already owns 0x20000000; MAP_FIXED_NOREPLACE
    // walks us to the next free candidate.
    static const uintptr_t candidates[] = {
        0x20000000, 0x30000000, 0x18000000, 0x60000000, 0x10000000,
    };
    for (uintptr_t c : candidates) {
        void* p = mmap(reinterpret_cast<void*>(c), kPageSize,
                       PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (p != MAP_FAILED) return p;
    }
    return nullptr;
}

struct PageState {
    uint8_t* page;
    size_t used;
    PageState() : page(static_cast<uint8_t*>(map_page())), used(0) {}
};

PageState& page_state()
{
    static PageState st;
    return st;
}

int rc(Reg r) { return static_cast<int>(r); }

void mov_rr(Emitter& e, int dst, int src) // mov dst, src (64-bit)
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 4 : 0) | (src >= 8 ? 1 : 0)));
    e.b(0x8B);
    e.b(static_cast<uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

void mov_slot_to_reg(Emitter& e, int dst, uint8_t off) // mov dst, [rsp+off]
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 4 : 0)));
    e.b(0x8B);
    e.b(static_cast<uint8_t>(0x40 | ((dst & 7) << 3) | 0x04));
    e.b(0x24);
    e.b(off);
}

void mov_reg_to_slot(Emitter& e, int src, uint8_t off) // mov [rsp+off], src
{
    e.b(static_cast<uint8_t>(0x48 | (src >= 8 ? 4 : 0)));
    e.b(0x89);
    e.b(static_cast<uint8_t>(0x40 | ((src & 7) << 3) | 0x04));
    e.b(0x24);
    e.b(off);
}

void add_slot_imm8(Emitter& e, uint8_t off, int8_t imm) // add qword [rsp+off], imm8
{
    e.b(0x48); e.b(0x83);
    e.b(0x40 | 0x04); // mod01 reg=0(add) rm=100(SIB)
    e.b(0x24);
    e.b(off);
    e.b(static_cast<uint8_t>(imm));
}

void mov_r_imm32(Emitter& e, int dst, uint32_t imm) // mov dst, imm32 (zero-extends)
{
    if (dst >= 8) e.b(0x41);
    e.b(static_cast<uint8_t>(0xB8 | (dst & 7)));
    e.imm32(imm);
}

void mov_r_rbpdisp(Emitter& e, int dst, uint8_t off) // mov dst, [rbp+off]
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 4 : 0)));
    e.b(0x8B);
    e.b(static_cast<uint8_t>(0x40 | ((dst & 7) << 3) | 0x05)); // mod01 reg rm=101(rbp)
    e.b(off);
}

void lea_r_rbpdisp(Emitter& e, int dst, uint8_t off) // lea dst, [rbp+off]
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 4 : 0)));
    e.b(0x8D);
    e.b(static_cast<uint8_t>(0x40 | ((dst & 7) << 3) | 0x05)); // mod01 reg rm=101(rbp)
    e.b(off);
}

void lea_r_rspdisp(Emitter& e, int dst, uint8_t off) // lea dst, [rsp+off]
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 4 : 0)));
    e.b(0x8D);
    e.b(static_cast<uint8_t>(0x40 | ((dst & 7) << 3) | 0x04)); // mod01 reg rm=100(SIB)
    e.b(0x24);
    e.b(off);
}

void cmp_dword_slot_imm8(Emitter& e, uint8_t off, uint8_t imm) // cmp dword [rsp+off], imm8
{
    e.b(0x83);
    e.b(0x7C); // mod01 reg=111(cmp) rm=100(SIB)
    e.b(0x24);
    e.b(off);
    e.b(imm);
}

// add dword [rbp+off], imm8. [rbp] addressing must use mod01+disp8
// (mod00/rm=101 would be RIP-relative).
void add_dword_rbpdisp_imm8(Emitter& e, uint8_t off, int8_t imm)
{
    e.b(0x83);
    e.b(0x45); // mod01 reg=000(add) rm=101(rbp)
    e.b(off);
    e.b(static_cast<uint8_t>(imm));
}

void add_r_imm8_64(Emitter& e, int dst, uint8_t imm) // add dst, imm8 (64-bit)
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 1 : 0))); // REX.W | REX.B
    e.b(0x83);
    e.b(static_cast<uint8_t>(0xC0 | (dst & 7))); // mod11 reg=000(add)
    e.b(imm);
}

void movsxd_rr(Emitter& e, int dst, int src) // movsxd dst, src32 (dst = sext src)
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 4 : 0) | (src >= 8 ? 1 : 0)));
    e.b(0x63);
    e.b(static_cast<uint8_t>(0xC0 | ((dst & 7) << 3) | (src & 7)));
}

void add_rr(Emitter& e, int dst, int src) // dst += src (64-bit), opcode 01 /r:
// reg field holds src (needs REX.R), r/m field holds dst (needs REX.B)
{
    e.b(static_cast<uint8_t>(0x48 | (src >= 8 ? 4 : 0) | (dst >= 8 ? 1 : 0)));
    e.b(0x01);
    e.b(static_cast<uint8_t>(0xC0 | ((src & 7) << 3) | (dst & 7)));
}

void add_r_imm32_64(Emitter& e, int dst, uint32_t imm) // dst += imm32 (sign-extended)
{
    e.b(static_cast<uint8_t>(0x48 | (dst >= 8 ? 1 : 0)));
    e.b(0x81);
    e.b(static_cast<uint8_t>(0xC0 | (dst & 7))); // /0 = add
    e.imm32(imm);
}

} // namespace

uintptr_t emit(const StubSpec& spec)
{
    if (!spec.fn || !spec.resume) return 0;
    if (spec.replay_len && !spec.replay) return 0;
    if (spec.tail_len && !spec.tail) return 0;
    if (spec.post_tail_len && !spec.post_tail) return 0;
    if (spec.n_saves > 4 || spec.n_writebacks > 4) return 0;
    if (spec.save_volatile) {
        if (spec.has_advance || spec.has_advance_mem || spec.tail_len
            || spec.has_result || spec.n_saves || spec.n_writebacks)
            return 0;
        for (size_t i = 0; i < 6; ++i)
            if (spec.args[i].kind == StubSpec::Arg::FrameSlotLea) return 0;
    }

    PageState& st = page_state();
    if (!st.page || st.used + 256 > kPageSize) return 0;

    Emitter e{st.page + st.used, st.page};
    const uintptr_t stub_base = reinterpret_cast<uintptr_t>(e.p);

    // Frame: keep 16-byte alignment at the call (M2 lesson: sub must be
    // a multiple of 16 after and rsp,-16). 0x20 covers 4 save slots; the
    // conventional u32 slot for 'consumed' lives at [rsp+0x18].
    e.b(0x55);                                  // push rbp
    e.b(0x48); e.b(0x89); e.b(0xE5);            // mov rbp, rsp  (rbp = entry_rsp - 8)
    e.b(0x48); e.b(0x83); e.b(0xE4); e.b(0xF0); // and rsp, -16
    e.b(0x48); e.b(0x83); e.b(0xEC); e.b(0x20); // sub rsp, 0x20

    if (spec.save_volatile) {
        // push rax rcx rdx rsi rdi r8 r9 r10 r11 (72B), pad 8, xmm0-3 (64B)
        static const uint8_t saves[] = {
            0x50, 0x51, 0x52, 0x56, 0x57,
            0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
            0x48, 0x83, 0xEC, 0x08,
            0x48, 0x83, 0xEC, 0x40,
            0xF2, 0x0F, 0x11, 0x04, 0x24,             // movdqu [rsp],xmm0
            0xF2, 0x0F, 0x11, 0x4C, 0x24, 0x10,       // movdqu [rsp+0x10],xmm1
            0xF2, 0x0F, 0x11, 0x54, 0x24, 0x20,       // movdqu [rsp+0x20],xmm2
            0xF2, 0x0F, 0x11, 0x5C, 0x24, 0x30,       // movdqu [rsp+0x30],xmm3
        };
        for (uint8_t b : saves) e.b(b);
    }

    for (size_t i = 0; i < spec.n_saves; ++i)
        mov_reg_to_slot(e, rc(spec.saves[i].reg), static_cast<uint8_t>(i * 8));

    static const int arg_regs[6] = {7, 6, 2, 1, 8, 9}; // rdi, rsi, rdx, rcx, r8, r9
    for (size_t i = 0; i < 6; ++i) {
        const StubSpec::Arg& a = spec.args[i];
        if (a.kind == StubSpec::Arg::Imm32)
            mov_r_imm32(e, arg_regs[i], a.imm);
        else if (a.kind == StubSpec::Arg::RegSrc)
            mov_rr(e, arg_regs[i], rc(a.src));
        else if (a.kind == StubSpec::Arg::RbpSlot)
            mov_r_rbpdisp(e, arg_regs[i], static_cast<uint8_t>(a.rbp_off));
        else if (a.kind == StubSpec::Arg::FrameSlotLea)
            lea_r_rspdisp(e, arg_regs[i], a.frame_off);
        else if (a.kind == StubSpec::Arg::RbpLea)
            lea_r_rbpdisp(e, arg_regs[i], static_cast<uint8_t>(a.rbp_off));
        else if (a.kind == StubSpec::Arg::IdxSextSum) {
            movsxd_rr(e, arg_regs[i], rc(a.idx));
            if (a.has_base)
                add_rr(e, arg_regs[i], rc(a.base));
            else
                add_r_imm32_64(e, arg_regs[i], a.base_imm);
        }
    }

    e.b(0x48); e.b(0xB8);                       // mov rax, imm64
    e.imm64(spec.fn);
    e.b(0xFF); e.b(0xD0);                       // call rax

    if (spec.save_volatile) {
        // restore xmm0-3, drop pad, pop volatile GPRs (reverse order)
        static const uint8_t restores[] = {
            0xF2, 0x0F, 0x10, 0x04, 0x24,             // movdqu xmm0,[rsp]
            0xF2, 0x0F, 0x10, 0x4C, 0x24, 0x10,       // movdqu xmm1,[rsp+0x10]
            0xF2, 0x0F, 0x10, 0x54, 0x24, 0x20,       // movdqu xmm2,[rsp+0x20]
            0xF2, 0x0F, 0x10, 0x5C, 0x24, 0x30,       // movdqu xmm3,[rsp+0x30]
            0x48, 0x83, 0xC4, 0x40,                   // add rsp,0x40
            0x48, 0x83, 0xC4, 0x08,                   // add rsp,8
            0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, // pop r11 r10 r9 r8
            0x5F, 0x5E, 0x5A, 0x59, 0x58,                   // pop rdi rsi rdx rcx rax
        };
        for (uint8_t b : restores) e.b(b);
    }

    if (spec.has_result)
        mov_rr(e, rc(spec.result_reg), 0);      // mov result_reg, rax

    for (size_t i = 0; i < spec.tail_len; ++i) e.b(spec.tail[i]);

    if (spec.has_advance) {
        // if (u32[rsp+off] == 3) advance_reg += 2
        cmp_dword_slot_imm8(e, spec.advance_frame_off, 3); // 5 bytes
        e.b(0x75); e.b(0x04);                   // jne +4 (skip the add)   2 bytes
        add_r_imm8_64(e, rc(spec.advance_reg), 2);         //              4 bytes
    }
    if (spec.has_advance2) {
        cmp_dword_slot_imm8(e, spec.advance_frame_off, 3);
        e.b(0x75); e.b(0x04);
        add_r_imm8_64(e, rc(spec.advance2_reg), 2);
    }

    if (spec.has_advance_mem) {
        // if (u32[rsp+advance_frame_off] == 3) dword[rbp+mem_off] += 2
        // add dword [rbp+disp8],imm8 is 4 bytes (no SIB for the rbp form);
        // a wrong skip length lands mid-instruction in the teardown mov
        // (found the hard way: +5 decoded its 2nd byte as `mov esp,ebp`)
        cmp_dword_slot_imm8(e, spec.advance_frame_off, 3); // 5 bytes
        e.b(0x75); e.b(0x04);                   // jne +4 (skip the 4-byte add) 2 bytes
        add_dword_rbpdisp_imm8(e, static_cast<uint8_t>(spec.advance_mem_rbp_off), 2);
    }

    for (size_t i = 0; i < spec.n_writebacks; ++i) {
        const uint8_t slot = static_cast<uint8_t>(spec.writebacks[i].save_index * 8);
        if (spec.writebacks[i].conditional) {
            // if (u32[rsp+advance_frame_off] == 3) slot += delta
            cmp_dword_slot_imm8(e, spec.advance_frame_off, 3); // 5 bytes
            e.b(0x75); e.b(0x06);               // jne +6 (skip the 6-byte add) 2 bytes
            add_slot_imm8(e, slot, spec.writebacks[i].delta); // 6 bytes
        } else {
            add_slot_imm8(e, slot, spec.writebacks[i].delta);
        }
    }

    for (size_t i = 0; i < spec.n_saves; ++i) {
        if (!spec.saves[i].restore) continue;
        mov_slot_to_reg(e, rc(spec.saves[i].reg), static_cast<uint8_t>(i * 8));
    }

    e.b(0x48); e.b(0x89); e.b(0xEC);            // mov rsp, rbp
    e.b(0x5D);                                  // pop rbp

    for (size_t i = 0; i < spec.replay_len; ++i) e.b(spec.replay[i]);

    for (size_t i = 0; i < spec.post_tail_len; ++i) e.b(spec.post_tail[i]);

    e.b(0xE9);
    if (!e.jmp_rel32(spec.resume)) return 0; // rel32 overflow: refuse loudly

    st.used += static_cast<size_t>(e.p - (st.page + st.used));
    return stub_base;
}

// Copies pre-assembled machine code into the shared relay page. Absolute
// addresses inside the code must already be baked in by the caller.
uintptr_t emit_raw(const uint8_t* code, size_t len)
{
    if (!code || !len) return 0;
    PageState& st = page_state();
    if (!st.page || st.used + len > kPageSize) return 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(st.page + st.used);
    std::memcpy(st.page + st.used, code, len);
    st.used += len;
    return addr;
}

uintptr_t reserve_code(size_t len)
{
    if (!len) return 0;
    PageState& st = page_state();
    if (!st.page || st.used + len > kPageSize) return 0;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(st.page + st.used);
    st.used += len;
    return addr;
}

void* reserve_data(size_t n)
{
    if (!n) return nullptr;
    PageState& st = page_state();
    if (!st.page) return nullptr;
    st.used = (st.used + 7) & ~static_cast<size_t>(7);
    if (st.used + n > kPageSize) return nullptr;
    void* p = st.page + st.used;
    st.used += n;
    return p;
}

bool install_jmp(uintptr_t addr, const uint8_t* orig, size_t len, uintptr_t stub)
{
    if (!addr || !stub || len < 5) return false;

    uint8_t cur[64];
    if (len > sizeof(cur)) return false;
    Injector::ReadMemoryRaw(Injector::memory_pointer_raw(reinterpret_cast<void*>(addr)),
                            cur, len, true);
    if (std::memcmp(cur, orig, len) != 0) {
        char hex[3 * sizeof(cur) + 1];
        size_t h = 0;
        for (size_t i = 0; i < len && h + 3 < sizeof(hex); ++i)
            h += static_cast<size_t>(snprintf(hex + h, sizeof(hex) - h, "%02x ", cur[i]));
        hex[h] = 0;
        log_line("[eu4cjk] install_jmp: verify mismatch @ 0x%lx: %s\n",
                 static_cast<unsigned long>(addr), hex);
        return false;
    }

    uint8_t patch[64];
    patch[0] = 0xE9;
    const int64_t rel = static_cast<int64_t>(stub) - static_cast<int64_t>(addr + 5);
    if (rel > INT32_MAX || rel < INT32_MIN) return false;
    int32_t rel32 = static_cast<int32_t>(rel);
    std::memcpy(patch + 1, &rel32, 4);
    for (size_t i = 5; i < len; ++i) patch[i] = 0x90;
    Injector::WriteMemoryRaw(Injector::memory_pointer_raw(reinterpret_cast<void*>(addr)),
                             patch, len, true);
    return true;
}

} // namespace eu4cjk::stubgen
