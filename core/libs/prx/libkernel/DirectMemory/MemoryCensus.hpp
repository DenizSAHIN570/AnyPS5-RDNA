#ifndef CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_MEMORYCENSUS_HPP
#define CORE_LIBS_PRX_LIBKERNEL_DIRECTMEMORY_MEMORYCENSUS_HPP

#include <cstddef>
#include <cstdint>

namespace MemoryCensus {

inline constexpr int UnknownMemoryType = -2;

bool Enabled();
void Allocate(std::int64_t physical, std::size_t length, int memoryType, int result);
void Release(std::int64_t physical, std::size_t length);
void MapDirect(const void* requested, const void* mapped, std::size_t length, int prot, int flags, std::int64_t physical, int memoryType, int result);
void MapFlexible(const void* requested, const void* mapped, std::size_t length, int prot, int flags, int result);
void Protect(const void* address, std::size_t length, int prot, int result);
void TypeProtect(const void* address, std::size_t length, int memoryType, int prot, int result);
void Unmap(const void* address, std::size_t length, int result);

}

#endif
