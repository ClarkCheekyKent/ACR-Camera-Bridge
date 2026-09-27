#pragma once
#include <windows.h>
#include "BridgeCore.hpp"

namespace bridge {
template <class T> bool read(const void *p, T &value) {
    __try {
        value = *static_cast<const T *>(p);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
template <class T> bool write(void *p, const T &value) {
    __try {
        *static_cast<T *>(p) = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
inline bool copy(void *dest, const void *src, std::size_t size) {
    __try {
        std::memcpy(dest, src, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct TextSection {
    std::uintptr_t address{};
    std::size_t size{};
};
inline std::optional<TextSection> text_section() {
    auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    IMAGE_DOS_HEADER dos{};
    IMAGE_NT_HEADERS64 nt{};
    if (!read(reinterpret_cast<void *>(base), dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        dos.e_lfanew > 0x100000 || !read(reinterpret_cast<void *>(base + dos.e_lfanew), nt) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt.FileHeader.NumberOfSections > 96)
        return {};
    auto table = base + dos.e_lfanew + 24 + nt.FileHeader.SizeOfOptionalHeader;
    for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER section{};
        if (!read(reinterpret_cast<void *>(table + i * sizeof(section)), section))
            return {};
        if (std::memcmp(section.Name, ".text", 6) != 0 || !(section.Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;
        if (section.VirtualAddress >= nt.OptionalHeader.SizeOfImage ||
            section.Misc.VirtualSize > nt.OptionalHeader.SizeOfImage - section.VirtualAddress)
            return {};
        return TextSection{base + section.VirtualAddress, section.Misc.VirtualSize};
    }
    return {};
}
struct HookTarget {
    void *function{};
    int slot{-1};
};
inline std::optional<HookTarget> resolve(void *pcm, std::uint32_t controller_field, void *already_hooked) {
    const auto section = text_section();
    std::uintptr_t vt{};
    if (!section || !read(pcm, vt) || !vt || controller_field > 0x10000)
        return {};
    std::vector<std::uint8_t> code(section->size);
    if (!copy(code.data(), reinterpret_cast<void *>(section->address), code.size()))
        return {};
    std::vector<int> slots;
    for (auto hit : forwarders(code, controller_field)) {
        std::uintptr_t fn{};
        if (!read(reinterpret_cast<void *>(vt + hit.slot * sizeof(void *)), fn))
            continue;
        if (fn < section->address || fn >= section->address + section->size ||
            section->address + section->size - fn < 256)
            continue;
        const auto bytes = std::span(code).subspan(fn - section->address, 256);
        if (reinterpret_cast<void *>(fn) == already_hooked || update_markers(bytes))
            slots.push_back(hit.slot);
    }
    const auto slot = unique_slot(slots);
    if (!slot)
        return {};
    void *fn{};
    if (!read(reinterpret_cast<void *>(vt + *slot * sizeof(void *)), fn))
        return {};
    return HookTarget{fn, *slot};
}
} // namespace bridge
