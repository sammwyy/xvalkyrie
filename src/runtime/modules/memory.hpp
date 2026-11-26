#pragma once
#include "runtime/modules/kernel.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
#include <windows.h>

// Console memory: reservations in the console's page sizes, and the title's physical pages,
// which are views of one section. ERA titles reach it through kernelx.dll, GDK titles
// through xmem.dll and the API sets.
namespace xv::runtime {
class Registry;
namespace modules {
// by the names kernelx.dll has for them
std::span<const ApiFunction> memory_functions();
// xmem.dll, and VirtualAlloc and its family for every API set
void install_xmem(Registry &registry);
void *allocate_aligned(std::size_t size, std::uint64_t alignment, void *address);
// VirtualAlloc(MEM_COMMIT) as a title's own call does it, physical page reservations included
void *commit_title_memory(void *address, std::size_t size);
// D3DAllocateGraphicsMemory and D3DFreeGraphicsMemory
HRESULT WINAPI allocate_graphics_memory(SIZE_T size, UINT64 alignment, UINT64 address, UINT flags,
                                        void **result);
HRESULT WINAPI free_graphics_memory(void *address);
// Copies `size` bytes to (`write`) or from `data` and the title's physical pages `pages`, by
// the numbers XMemAllocatePhysicalPages gave them, from `offset` into the first: the pages
// need not be mapped. False if one is not a page the title holds.
bool copy_physical_pages(const ULONG_PTR *pages, std::size_t offset, std::uint8_t *data,
                         std::size_t size, bool write);
// Where else the title sees the memory of [start, end): for each of its physical pages that is
// mapped at other addresses as well, those ranges, as many bytes as the part of [start, end)
// the page holds. Empty for memory that is no physical page of the title's.
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> other_views(std::uintptr_t start,
                                                                   std::uintptr_t end);
// The ranges of the title's memory mapped, committed or given another protection since the
// last call, for what waits for memory to be there (placement.cpp's pending textures)
std::vector<std::pair<std::uintptr_t, std::uintptr_t>> take_newly_mapped();
// The physical pages the title has mapped at [from, from + size) mapped at `to` as well, in
// place of what is there, as the console's GPU maps its pages; `from` 0 leaves `to` unmapped.
// False if `to` is not in a reservation of physical pages or a page at `from` is not mapped.
bool map_alias(std::uintptr_t to, std::uintptr_t from, std::uintptr_t size);
} // namespace modules
} // namespace xv::runtime
