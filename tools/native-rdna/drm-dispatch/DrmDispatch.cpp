#include "KernelBinary.hpp"
#include "Pm4.hpp"

#include <drm/amdgpu_drm.h>
#include <drm/drm.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::uint32_t ThreadsPerGroup = 64;
constexpr std::uint32_t Rsrc1 = 0x400c0041u;
constexpr std::uint32_t Rsrc2 = (2u << 1) | (1u << 7);
constexpr std::uint64_t DefaultVaBase = 0x200000000ull;
constexpr std::uint64_t UserLowRangeEnd = 0x800000000000ull;
constexpr std::uint64_t WaitNanoseconds = 2000000000ull;

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

struct Options {
    std::string device = "/dev/dri/renderD128";
    bool gfxRing = false;
    std::uint32_t groups = 4;
    std::uint64_t vaBase = DefaultVaBase;
    bool vramDestination = false;
    bool anyGeneration = false;
};

class Device {
public:
    explicit Device(const std::string& path) : path(path) {
        fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0) fail("open " + path + ": " + std::strerror(errno));
    }
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    ~Device() {
        if (fd >= 0) close(fd);
    }

    void Ioctl(unsigned long request, void* argument, const char* what) const {
        int result;
        do {
            result = ioctl(fd, request, argument);
        } while (result == -1 && (errno == EINTR || errno == EAGAIN));
        if (result == -1) fail(std::string(what) + " failed: " + std::strerror(errno));
    }

    drm_amdgpu_info_hw_ip HwIp(std::uint32_t type) const {
        drm_amdgpu_info_hw_ip info{};
        drm_amdgpu_info request{};
        request.return_pointer = reinterpret_cast<std::uintptr_t>(&info);
        request.return_size = sizeof(info);
        request.query = AMDGPU_INFO_HW_IP_INFO;
        request.query_hw_ip.type = type;
        request.query_hw_ip.ip_instance = 0;
        Ioctl(DRM_IOCTL_AMDGPU_INFO, &request, "AMDGPU_INFO_HW_IP_INFO");
        return info;
    }

    int Fd() const { return fd; }
    const std::string& Path() const { return path; }

private:
    std::string path;
    int fd = -1;
};

class Buffer {
public:
    Buffer(const Device& device, std::uint64_t bytes, std::uint32_t domain, std::uint64_t flags, std::uint64_t va, std::uint32_t vaFlags) : device(device), bytes(bytes), va(va) {
        union drm_amdgpu_gem_create create{};
        create.in.bo_size = bytes;
        create.in.alignment = 4096;
        create.in.domains = domain;
        create.in.domain_flags = flags;
        device.Ioctl(DRM_IOCTL_AMDGPU_GEM_CREATE, &create, "AMDGPU_GEM_CREATE");
        handle = create.out.handle;
        drm_amdgpu_gem_va map{};
        map.handle = handle;
        map.operation = AMDGPU_VA_OP_MAP;
        map.flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_WRITEABLE | AMDGPU_VM_PAGE_EXECUTABLE | vaFlags;
        map.va_address = va;
        map.offset_in_bo = 0;
        map.map_size = bytes;
        device.Ioctl(DRM_IOCTL_AMDGPU_GEM_VA, &map, "AMDGPU_GEM_VA (map)");
        mapped = true;
        union drm_amdgpu_gem_mmap offset{};
        offset.in.handle = handle;
        device.Ioctl(DRM_IOCTL_AMDGPU_GEM_MMAP, &offset, "AMDGPU_GEM_MMAP");
        cpu = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, device.Fd(), static_cast<off_t>(offset.out.addr_ptr));
        if (cpu == MAP_FAILED) {
            cpu = nullptr;
            fail(std::string("mmap of the buffer object failed: ") + std::strerror(errno));
        }
    }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    ~Buffer() {
        if (cpu != nullptr) munmap(cpu, bytes);
        if (mapped) {
            drm_amdgpu_gem_va unmap{};
            unmap.handle = handle;
            unmap.operation = AMDGPU_VA_OP_UNMAP;
            unmap.va_address = va;
            unmap.map_size = bytes;
            ioctl(device.Fd(), DRM_IOCTL_AMDGPU_GEM_VA, &unmap);
        }
        if (handle != 0) {
            drm_gem_close close{};
            close.handle = handle;
            ioctl(device.Fd(), DRM_IOCTL_GEM_CLOSE, &close);
        }
    }
    std::uint32_t Handle() const { return handle; }
    std::uint64_t Va() const { return va; }
    void* Cpu() const { return cpu; }
    std::uint64_t Bytes() const { return bytes; }

private:
    const Device& device;
    std::uint32_t handle = 0;
    std::uint64_t bytes;
    std::uint64_t va;
    void* cpu = nullptr;
    bool mapped = false;
};

class Context {
public:
    explicit Context(const Device& device) : device(device) {
        union drm_amdgpu_ctx request{};
        request.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
        device.Ioctl(DRM_IOCTL_AMDGPU_CTX, &request, "AMDGPU_CTX (alloc)");
        id = request.out.alloc.ctx_id;
    }
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    ~Context() {
        union drm_amdgpu_ctx request{};
        request.in.op = AMDGPU_CTX_OP_FREE_CTX;
        request.in.ctx_id = id;
        ioctl(device.Fd(), DRM_IOCTL_AMDGPU_CTX, &request);
    }
    std::uint32_t Id() const { return id; }

private:
    const Device& device;
    std::uint32_t id = 0;
};

std::uint64_t submit(const Device& device, const Context& context, std::uint32_t ip, const Buffer& commands, std::uint32_t dwords, const std::vector<std::uint32_t>& handles) {
    std::vector<drm_amdgpu_bo_list_entry> entries;
    for (const auto handle : handles) entries.push_back({handle, 0});
    drm_amdgpu_bo_list_in list{};
    list.operation = ~0u;
    list.list_handle = ~0u;
    list.bo_number = static_cast<std::uint32_t>(entries.size());
    list.bo_info_size = sizeof(drm_amdgpu_bo_list_entry);
    list.bo_info_ptr = reinterpret_cast<std::uintptr_t>(entries.data());
    drm_amdgpu_cs_chunk_ib ib{};
    ib.flags = AMDGPU_IB_FLAG_EMIT_MEM_SYNC;
    ib.va_start = commands.Va();
    ib.ib_bytes = dwords * 4;
    ib.ip_type = ip;
    ib.ip_instance = 0;
    ib.ring = 0;
    drm_amdgpu_cs_chunk chunks[2]{};
    chunks[0].chunk_id = AMDGPU_CHUNK_ID_BO_HANDLES;
    chunks[0].length_dw = sizeof(list) / 4;
    chunks[0].chunk_data = reinterpret_cast<std::uintptr_t>(&list);
    chunks[1].chunk_id = AMDGPU_CHUNK_ID_IB;
    chunks[1].length_dw = sizeof(ib) / 4;
    chunks[1].chunk_data = reinterpret_cast<std::uintptr_t>(&ib);
    std::uint64_t pointers[2] = {reinterpret_cast<std::uintptr_t>(&chunks[0]), reinterpret_cast<std::uintptr_t>(&chunks[1])};
    union drm_amdgpu_cs cs{};
    cs.in.ctx_id = context.Id();
    cs.in.num_chunks = 2;
    cs.in.chunks = reinterpret_cast<std::uintptr_t>(pointers);
    device.Ioctl(DRM_IOCTL_AMDGPU_CS, &cs, "AMDGPU_CS");
    return cs.out.handle;
}

bool wait(const Device& device, const Context& context, std::uint32_t ip, std::uint64_t sequence) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    union drm_amdgpu_wait_cs request{};
    request.in.handle = sequence;
    request.in.timeout = static_cast<std::uint64_t>(now.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(now.tv_nsec) + WaitNanoseconds;
    request.in.ip_type = ip;
    request.in.ip_instance = 0;
    request.in.ring = 0;
    request.in.ctx_id = context.Id();
    device.Ioctl(DRM_IOCTL_AMDGPU_WAIT_CS, &request, "AMDGPU_WAIT_CS");
    return request.out.status == 0;
}

std::uint64_t parseNumber(const char* text, const char* option) {
    char* end = nullptr;
    errno = 0;
    const auto value = std::strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') fail(std::string(option) + " needs a number, got \"" + text + "\"");
    return value;
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto value = [&]() -> const char* {
            if (++i == argc) fail(argument + " needs a value");
            return argv[i];
        };
        if (argument == "--device") options.device = value();
        else if (argument == "--ring") {
            const std::string ring = value();
            if (ring != "compute" && ring != "gfx") fail("--ring is compute or gfx");
            options.gfxRing = ring == "gfx";
        } else if (argument == "--groups") options.groups = static_cast<std::uint32_t>(parseNumber(value(), "--groups"));
        else if (argument == "--va-base") options.vaBase = parseNumber(value(), "--va-base");
        else if (argument == "--vram-destination") options.vramDestination = true;
        else if (argument == "--any-generation") options.anyGeneration = true;
        else fail("unknown option " + argument);
    }
    if (options.groups == 0 || options.groups > 65536) fail("--groups must be 1..65536");
    if (options.vaBase % 65536 != 0 || options.vaBase < 0x10000 || options.vaBase >= UserLowRangeEnd - 0x10000000ull) fail("--va-base must be 64 KiB aligned inside the user low range [0x10000, 0x800000000000)");
    return options;
}

int run(const Options& options) {
    Device device(options.device);
    const std::uint32_t ip = options.gfxRing ? AMDGPU_HW_IP_GFX : AMDGPU_HW_IP_COMPUTE;
    const auto info = device.HwIp(ip);
    std::printf("%s: %s IP %u.%u, rings 0x%x\n", device.Path().c_str(), options.gfxRing ? "GFX" : "COMPUTE", info.hw_ip_version_major, info.hw_ip_version_minor, info.available_rings);
    if (info.hw_ip_version_major != 12 && !options.anyGeneration) fail("this sample encodes gfx12 (RDNA4) state; the device reports IP major " + std::to_string(info.hw_ip_version_major));
    if ((info.available_rings & 1u) == 0) fail("ring 0 of the requested type is not available");

    const std::uint64_t outputBytes = (static_cast<std::uint64_t>(options.groups) * ThreadsPerGroup * 4 + 65535) / 65536 * 65536;
    Buffer code(device, 65536, AMDGPU_GEM_DOMAIN_VRAM, AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED, options.vaBase, 0);
    Buffer output(device, outputBytes, options.vramDestination ? AMDGPU_GEM_DOMAIN_VRAM : AMDGPU_GEM_DOMAIN_GTT, options.vramDestination ? AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED : 0, options.vaBase + 0x100000, 0);
    Buffer commands(device, 65536, AMDGPU_GEM_DOMAIN_GTT, AMDGPU_GEM_CREATE_CPU_GTT_USWC, options.vaBase + 0x100000 + outputBytes, AMDGPU_VM_MTYPE_UC);

    std::memcpy(code.Cpu(), KernelBinary.data(), sizeof(KernelBinary));
    std::memset(output.Cpu(), 0, output.Bytes());

    Gfx12Pm4::Builder builder;
    if (options.gfxRing) builder.ContextControl();
    builder.ComputePreamble();
    Gfx12Pm4::ComputeProgram program;
    program.codeAddress = code.Va();
    program.rsrc1 = Rsrc1;
    program.rsrc2 = Rsrc2;
    program.threadsX = ThreadsPerGroup;
    program.userData = {static_cast<std::uint32_t>(output.Va()), static_cast<std::uint32_t>(output.Va() >> 32)};
    builder.Program(program);
    builder.DispatchDirect(options.groups, 1, 1, true);
    builder.PadTo(8);
    const auto& words = builder.Words();
    if (words.size() * 4 > commands.Bytes()) fail("the IB does not fit its buffer");
    std::memcpy(commands.Cpu(), words.data(), words.size() * 4);
    std::printf("code at 0x%llx, output at 0x%llx, IB at 0x%llx (%zu dwords)\n", static_cast<unsigned long long>(code.Va()), static_cast<unsigned long long>(output.Va()), static_cast<unsigned long long>(commands.Va()), words.size());

    Context context(device);
    const auto sequence = submit(device, context, ip, commands, static_cast<std::uint32_t>(words.size()), {code.Handle(), output.Handle(), commands.Handle()});
    if (!wait(device, context, ip, sequence)) fail("the dispatch did not finish within 2 s");

    const auto* values = static_cast<const std::uint32_t*>(output.Cpu());
    const std::uint32_t total = options.groups * ThreadsPerGroup;
    std::uint32_t wrong = 0;
    for (std::uint32_t i = 0; i < total; ++i) {
        const std::uint32_t expected = i * 3u + 7u;
        if (values[i] != expected) {
            if (wrong < 8) std::printf("  output[%u] = 0x%08x, expected 0x%08x\n", i, values[i], expected);
            ++wrong;
        }
    }
    if (wrong != 0) {
        std::printf("FAIL: %u of %u values wrong\n", wrong, total);
        return 1;
    }
    std::printf("PASS: %u threads in %u groups wrote the expected values\n", total, options.groups);
    return 0;
}

}

int main(int argc, char** argv) {
    try {
        return run(parse(argc, argv));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "drm_dispatch: %s\nusage: drm_dispatch [--device /dev/dri/renderD128] [--ring compute|gfx] [--groups N] [--va-base 0xADDR] [--vram-destination] [--any-generation]\n", error.what());
        return 2;
    }
}
