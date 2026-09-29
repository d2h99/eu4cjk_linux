#pragma once

#include <cstddef>
#include <cstdint>

namespace eu4cjk::savefix {

// M5 save-system fixes (Linux equivalents of the upstream file_save
// procs, collapsed into boundary hooks):
//   write boundary - CString::RemoveSpecialCharacters entry, SITE-AWARE
//     (RSC is called from 14 sites; the Linux fold is UTF-8-aware and
//     collapses non-CP1252 text to ASCII garbage "a??"):
//       save-name site : escaped widget text -> real UTF-8 (disk name)
//       prefill sites  : UTF-8 default name  -> escaped (CEditBox shows
//                        it through the escaped font pipeline)
//       every site     : escaped / valid-UTF-8 strings are never folded
//     (covers upstream proc1/proc2/proc7/proc8 semantics);
//   read boundary  - CSaveHeaderInfo::ReadFileHeader exit (second pass):
//     UTF-8 meta titles -> escaped for display (covers proc3/4/5/6).
// Switch: EU4CJK_SAVE0=1 disables the family.
bool install();

void log_stats();

// Builds the RemoveSpecialCharacters entry stub for the offline selftest
// (helper = C bool(void* cstr, uintptr_t ret_site); resume = 0x254d060;
// fnret = 0x254d0b7). The stub forwards the caller's return address so
// the helper can dispatch on the call site.
size_t build_savename_stub(uint8_t* out, size_t cap, uintptr_t helper,
                           uintptr_t resume, uintptr_t fnret);

// Call-site return addresses (non-PIE absolute) that pick the conversion
// direction. Evidence: SaveGame(0x1cdcbac) calls RSC at 0x1cdcbfb right
// after reading the CEditBox text; SaveGameSelect(0x1cdd1c4) at
// 0x1cdd249 and Show(0x1cdd2c2) at 0x1cdd63e sanitize the default name
// right before SetText on the widget at this+0xC8. BuildSaveFileFolder /
// BuildFullSavePath (0x128317c / 0x12833cb) re-run RSC on the assembled
// path - the reason valid UTF-8 must be protected at EVERY site.
constexpr uintptr_t kRetSaveName = 0x1CDCC00;
constexpr uintptr_t kRetPrefillA = 0x1CDD24E;
constexpr uintptr_t kRetPrefillB = 0x1CDD643;

// The dispatch behind the write-boundary hook (exposed for the offline
// selftest). Returns true = skip the vanilla fold ("handled").
bool savename_dispatch(void* cstr, uintptr_t ret_site);

// Builds the SetupFromFileHeader post-read stub (helper = void(void*
// info); resume = 0x1d2c00c). Saves rax across the call, takes info from
// rbx, replays mov %eax,%r15d; test %al,%al.
size_t build_saveheader_stub(uint8_t* out, size_t cap, uintptr_t helper,
                              uintptr_t resume);

// Builds the CSavegameItem ctor stub (helper = void(void* name); resume =
// 0x1d2bacd). The ctor's 4th argument (rcx) is the display name that
// lands verbatim in the CStandardlistboxItem row text - every list
// (frontend country-select, in-game, ironman) builds its rows here while
// the filename kept in CBasicSaveInfo(+0x228) stays untouched for file
// ops. Saves rdi/rsi/rdx/rcx around the helper and replays the three
// pushes (rbp, r15, r14).
size_t build_savetitle_stub(uint8_t* out, size_t cap, uintptr_t helper,
                            uintptr_t resume);

// Row-label stub for CLocalSavegameItem's two CInstantTextBox::
// ChangeString sites (the VISIBLE row text is the raw filename, not the
// listbox base text). helper = void(void* widget, void* name); the
// mov_rdi/mov_rsi byte triples select the source registers (site A:
// 48 89 C7 / 4C 89 FE  i.e. rax->rdi, r15->rsi, resume 0x1d2c388;
// site B: 48 89 EF / 4C 89 F6 i.e. rbp->rdi, r14->rsi, resume
// 0x1d2c3d0). Both sites sit at rsp%16==0, hence sub $0x20.
size_t build_rowname_stub(uint8_t* out, size_t cap, uintptr_t helper,
                          uintptr_t resume, const uint8_t mov_rdi[3],
                          const uint8_t mov_rsi[3]);

// Testable core of the row-label fix (real = the injected ChangeString):
// backup -> UTF-8->escaped -> real(widget, name, 0) -> restore. Returns
// true if a conversion happened.
bool rowname_pass(void* widget, void* name, void (*real)(void*, void*, int));

} // namespace eu4cjk::savefix
