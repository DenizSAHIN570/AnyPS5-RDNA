#include "prx/libkernel/DirectMemory/MemoryCensus.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <string>

namespace MemoryCensus {

namespace {

struct Log {
    std::FILE* file = nullptr;
    std::mutex mutex;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
};

Log* open() {
    static Log* log = [] () -> Log* {
        const char* path = std::getenv("APS5_MEMORY_CENSUS");
        if (path == nullptr) return nullptr;
        if (*path == '\0') throw std::runtime_error("APS5_MEMORY_CENSUS is set but empty; it names the census CSV file");
        auto* created = new Log();
        created->file = std::fopen(path, "w");
        if (created->file == nullptr) throw std::runtime_error(std::string("APS5_MEMORY_CENSUS: cannot open ") + path);
        std::fputs("seconds,op,result,requested,address,length,physical,memory_type,prot,flags\n", created->file);
        std::fflush(created->file);
        return created;
    }();
    return log;
}

void write(const char* op, int result, const void* requested, const void* address, std::size_t length, std::int64_t physical, int memoryType, int prot, int flags) {
    Log* log = open();
    if (log == nullptr) return;
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - log->start).count();
    std::lock_guard lock(log->mutex);
    const int written = std::fprintf(log->file, "%.6f,%s,%d,0x%llx,0x%llx,0x%llx,%lld,%d,0x%x,0x%x\n", seconds, op, result,
                                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(requested)),
                                     static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(address)),
                                     static_cast<unsigned long long>(length), static_cast<long long>(physical), memoryType,
                                     static_cast<unsigned>(prot), static_cast<unsigned>(flags));
    if (written < 0 || std::fflush(log->file) != 0) throw std::runtime_error("APS5_MEMORY_CENSUS: cannot write the census file");
}

}

bool Enabled() { return open() != nullptr; }

void Allocate(std::int64_t physical, std::size_t length, int memoryType, int result) {
    write("allocate", result, nullptr, nullptr, length, physical, memoryType, 0, 0);
}

void Release(std::int64_t physical, std::size_t length) {
    write("release", 0, nullptr, nullptr, length, physical, UnknownMemoryType, 0, 0);
}

void MapDirect(const void* requested, const void* mapped, std::size_t length, int prot, int flags, std::int64_t physical, int memoryType, int result) {
    write("map_direct", result, requested, mapped, length, physical, memoryType, prot, flags);
}

void MapFlexible(const void* requested, const void* mapped, std::size_t length, int prot, int flags, int result) {
    write("map_flexible", result, requested, mapped, length, -1, UnknownMemoryType, prot, flags);
}

void Protect(const void* address, std::size_t length, int prot, int result) {
    write("protect", result, nullptr, address, length, -1, UnknownMemoryType, prot, 0);
}

void TypeProtect(const void* address, std::size_t length, int memoryType, int prot, int result) {
    write("type_protect", result, nullptr, address, length, -1, memoryType, prot, 0);
}

void Unmap(const void* address, std::size_t length, int result) {
    write("unmap", result, nullptr, address, length, -1, UnknownMemoryType, 0, 0);
}

}
