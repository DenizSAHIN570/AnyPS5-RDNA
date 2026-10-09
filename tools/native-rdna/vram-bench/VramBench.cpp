#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <linux/futex.h>
#include <linux/udmabuf.h>
#include <numeric>
#include <optional>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t MiB = 1024 * 1024;

struct Options {
    std::string command;
    std::string gpu;
    std::string json;
    std::size_t sizeMiB = 256;
    std::uint64_t address = 0;
    std::uint32_t allocations = 1000;
    std::uint32_t submits = 200;
    bool unplaced = false;
};

[[noreturn]] void fail(const std::string& what) { throw std::runtime_error(what); }

void check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) fail(std::string(what) + " failed: VkResult " + std::to_string(static_cast<int>(result)));
}

#define VRAM_BENCH_INSTANCE_FUNCTIONS(X) \
    X(vkDestroyInstance) \
    X(vkEnumeratePhysicalDevices) \
    X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) \
    X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkEnumerateDeviceExtensionProperties) \
    X(vkCreateDevice) \
    X(vkGetDeviceProcAddr)

#define VRAM_BENCH_DEVICE_FUNCTIONS(X) \
    X(vkDestroyDevice) \
    X(vkDeviceWaitIdle) \
    X(vkGetDeviceQueue) \
    X(vkCreateCommandPool) \
    X(vkDestroyCommandPool) \
    X(vkCreateBuffer) \
    X(vkDestroyBuffer) \
    X(vkGetBufferMemoryRequirements) \
    X(vkAllocateMemory) \
    X(vkFreeMemory) \
    X(vkBindBufferMemory) \
    X(vkMapMemory) \
    X(vkUnmapMemory) \
    X(vkAllocateCommandBuffers) \
    X(vkFreeCommandBuffers) \
    X(vkCreateFence) \
    X(vkDestroyFence) \
    X(vkCreateQueryPool) \
    X(vkDestroyQueryPool) \
    X(vkResetCommandBuffer) \
    X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) \
    X(vkCmdResetQueryPool) \
    X(vkCmdWriteTimestamp) \
    X(vkCmdCopyBuffer) \
    X(vkQueueSubmit) \
    X(vkWaitForFences) \
    X(vkResetFences) \
    X(vkGetQueryPoolResults)

#define VRAM_BENCH_DECLARE(name) PFN_##name name = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
VRAM_BENCH_INSTANCE_FUNCTIONS(VRAM_BENCH_DECLARE)
VRAM_BENCH_DEVICE_FUNCTIONS(VRAM_BENCH_DECLARE)
#undef VRAM_BENCH_DECLARE

void loadGlobalFunctions() {
    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (loader == nullptr) loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (loader == nullptr) fail(std::string("cannot load the Vulkan loader: ") + dlerror());
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(loader, "vkGetInstanceProcAddr"));
    if (vkGetInstanceProcAddr == nullptr) fail("the Vulkan loader has no vkGetInstanceProcAddr");
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (vkCreateInstance == nullptr) fail("the Vulkan loader has no vkCreateInstance");
}

void loadInstanceFunctions(VkInstance instance) {
#define VRAM_BENCH_LOAD(name) \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name)); \
    if (name == nullptr) fail("missing instance function " #name);
    VRAM_BENCH_INSTANCE_FUNCTIONS(VRAM_BENCH_LOAD)
#undef VRAM_BENCH_LOAD
}

void loadDeviceFunctions(VkDevice device) {
#define VRAM_BENCH_LOAD(name) \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name)); \
    if (name == nullptr) fail("missing device function " #name);
    VRAM_BENCH_DEVICE_FUNCTIONS(VRAM_BENCH_LOAD)
#undef VRAM_BENCH_LOAD
}

std::string hex(std::uint64_t value) {
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out + "\"";
}

std::string flagsText(VkMemoryPropertyFlags flags) {
    std::string text;
    const auto add = [&](VkMemoryPropertyFlags bit, const char* name) {
        if ((flags & bit) == 0) return;
        if (!text.empty()) text += '|';
        text += name;
    };
    add(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DEVICE_LOCAL");
    add(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HOST_VISIBLE");
    add(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HOST_COHERENT");
    add(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HOST_CACHED");
    add(VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, "LAZILY_ALLOCATED");
    add(VK_MEMORY_PROPERTY_PROTECTED_BIT, "PROTECTED");
    add(VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD, "DEVICE_COHERENT_AMD");
    add(VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD, "DEVICE_UNCACHED_AMD");
    return text.empty() ? "0" : text;
}

class JsonObject {
public:
    void Add(const std::string& key, const std::string& rawValue) {
        if (!body.empty()) body += ", ";
        body += quoted(key) + ": " + rawValue;
    }
    void Text(const std::string& key, const std::string& value) { Add(key, quoted(value)); }
    void Number(const std::string& key, double value) {
        char text[64];
        std::snprintf(text, sizeof(text), "%.6g", value);
        Add(key, text);
    }
    void Integer(const std::string& key, std::uint64_t value) { Add(key, std::to_string(value)); }
    void Bool(const std::string& key, bool value) { Add(key, value ? "true" : "false"); }
    std::string Str() const { return "{" + body + "}"; }

private:
    std::string body;
};

struct Extensions {
    bool mapMemory2 = false;
    bool mapMemoryPlaced = false;
    bool externalMemoryHost = false;
    bool externalMemoryFd = false;
    bool externalMemoryDmaBuf = false;
};

struct PlacedSupport {
    bool memoryMapPlaced = false;
    bool memoryMapRangePlaced = false;
    bool memoryUnmapReserve = false;
    VkDeviceSize minPlacedMemoryMapAlignment = 0;
};

class Context {
public:
    explicit Context(const std::string& gpuName) {
        loadGlobalFunctions();
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "vram_bench";
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        instanceInfo.pApplicationInfo = &app;
        check(vkCreateInstance(&instanceInfo, nullptr, &instance), "vkCreateInstance");
        loadInstanceFunctions(instance);
        std::uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> devices(count);
        check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "vkEnumeratePhysicalDevices");
        if (devices.empty()) fail("no Vulkan physical device");
        physical = pick(devices, gpuName);
        vkGetPhysicalDeviceProperties(physical, &properties);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        readDriverProperties();
        readExtensions();
        createDevice();
    }

    ~Context() {
        if (device != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
            if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance != VK_NULL_HANDLE) vkDestroyInstance(instance, nullptr);
    }

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    template <typename TFunction>
    TFunction Function(const char* name) const {
        const auto function = reinterpret_cast<TFunction>(vkGetDeviceProcAddr(device, name));
        if (function == nullptr) fail(std::string("missing device function ") + name);
        return function;
    }

    std::optional<std::uint32_t> FindType(std::uint32_t allowed, VkMemoryPropertyFlags required, VkMemoryPropertyFlags excluded, VkMemoryPropertyFlags preferred = 0) const {
        std::optional<std::uint32_t> found;
        for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            const auto flags = memory.memoryTypes[i].propertyFlags;
            if ((allowed & (1u << i)) == 0 || (flags & required) != required || (flags & excluded) != 0) continue;
            if ((flags & preferred) == preferred) return i;
            if (!found) found = i;
        }
        return found;
    }

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::uint32_t queueFamily = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memory{};
    std::string driverName;
    std::string driverInfo;
    Extensions extensions;
    PlacedSupport placed;
    VkDeviceSize minImportedHostPointerAlignment = 0;
    std::uint32_t timestampValidBits = 0;

private:
    static VkPhysicalDevice pick(const std::vector<VkPhysicalDevice>& devices, const std::string& gpuName) {
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        std::optional<VkPhysicalDevice> discrete;
        for (const auto candidate : devices) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(candidate, &props);
            if (!gpuName.empty() && lower(props.deviceName).find(lower(gpuName)) != std::string::npos) return candidate;
            if (!discrete && props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) discrete = candidate;
        }
        if (!gpuName.empty()) fail("no Vulkan device name contains \"" + gpuName + "\"");
        return discrete.value_or(devices.front());
    }

    void readDriverProperties() {
        VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props.pNext = &driver;
        vkGetPhysicalDeviceProperties2(physical, &props);
        driverName = driver.driverName;
        driverInfo = driver.driverInfo;
    }

    void readExtensions() {
        std::uint32_t count = 0;
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> available(count);
        check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
        const auto has = [&](const char* name) {
            return std::any_of(available.begin(), available.end(), [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        extensions.mapMemory2 = has(VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
        extensions.mapMemoryPlaced = has(VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && extensions.mapMemory2;
        extensions.externalMemoryHost = has(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        extensions.externalMemoryFd = has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        extensions.externalMemoryDmaBuf = has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) && extensions.externalMemoryFd;

        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placedFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        if (extensions.mapMemoryPlaced) features.pNext = &placedFeatures;
        vkGetPhysicalDeviceFeatures2(physical, &features);
        placed.memoryMapPlaced = placedFeatures.memoryMapPlaced == VK_TRUE;
        placed.memoryMapRangePlaced = placedFeatures.memoryMapRangePlaced == VK_TRUE;
        placed.memoryUnmapReserve = placedFeatures.memoryUnmapReserve == VK_TRUE;

        VkPhysicalDeviceMapMemoryPlacedPropertiesEXT placedProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT};
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostProperties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        void** chain = &props.pNext;
        if (extensions.mapMemoryPlaced) {
            *chain = &placedProperties;
            chain = &placedProperties.pNext;
        }
        if (extensions.externalMemoryHost) *chain = &hostProperties;
        vkGetPhysicalDeviceProperties2(physical, &props);
        placed.minPlacedMemoryMapAlignment = placedProperties.minPlacedMemoryMapAlignment;
        minImportedHostPointerAlignment = hostProperties.minImportedHostPointerAlignment;
    }

    void createDevice() {
        std::uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
        bool found = false;
        for (std::uint32_t i = 0; i < count; ++i) {
            if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0 && families[i].timestampValidBits != 0) {
                queueFamily = i;
                timestampValidBits = families[i].timestampValidBits;
                found = true;
                break;
            }
        }
        if (!found) fail("no graphics or compute queue with timestamps");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;
        std::vector<const char*> enabled;
        if (extensions.mapMemory2) enabled.push_back(VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
        if (extensions.mapMemoryPlaced) enabled.push_back(VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
        if (extensions.externalMemoryHost) enabled.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
        if (extensions.externalMemoryFd) enabled.push_back(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        if (extensions.externalMemoryDmaBuf) enabled.push_back(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placedFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT};
        placedFeatures.memoryMapPlaced = placed.memoryMapPlaced ? VK_TRUE : VK_FALSE;
        placedFeatures.memoryUnmapReserve = placed.memoryUnmapReserve ? VK_TRUE : VK_FALSE;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        if (extensions.mapMemoryPlaced) deviceInfo.pNext = &placedFeatures;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(enabled.size());
        deviceInfo.ppEnabledExtensionNames = enabled.data();
        check(vkCreateDevice(physical, &deviceInfo, nullptr, &device), "vkCreateDevice");
        loadDeviceFunctions(device);
        vkGetDeviceQueue(device, queueFamily, 0, &queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "vkCreateCommandPool");
    }
};

struct Allocation {
    const Context* context = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;

    Allocation() = default;
    Allocation(const Allocation&) = delete;
    Allocation& operator=(const Allocation&) = delete;
    Allocation(Allocation&& other) noexcept { *this = std::move(other); }
    Allocation& operator=(Allocation&& other) noexcept {
        std::swap(context, other.context);
        std::swap(memory, other.memory);
        std::swap(buffer, other.buffer);
        return *this;
    }
    ~Allocation() {
        if (context == nullptr) return;
        if (buffer != VK_NULL_HANDLE) vkDestroyBuffer(context->device, buffer, nullptr);
        if (memory != VK_NULL_HANDLE) vkFreeMemory(context->device, memory, nullptr);
    }
};

VkBuffer createBuffer(const Context& context, VkDeviceSize size, VkExternalMemoryHandleTypeFlags external) {
    VkExternalMemoryBufferCreateInfo externalInfo{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    externalInfo.handleTypes = external;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    if (external != 0) info.pNext = &externalInfo;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buffer = VK_NULL_HANDLE;
    check(vkCreateBuffer(context.device, &info, nullptr, &buffer), "vkCreateBuffer");
    return buffer;
}

Allocation allocateBuffer(const Context& context, VkDeviceSize size, VkMemoryPropertyFlags required, VkMemoryPropertyFlags excluded, VkMemoryPropertyFlags preferred, std::uint32_t* typeOut = nullptr) {
    Allocation allocation;
    allocation.context = &context;
    allocation.buffer = createBuffer(context, size, 0);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, allocation.buffer, &requirements);
    const auto type = context.FindType(requirements.memoryTypeBits, required, excluded, preferred);
    if (!type) fail("no memory type with " + flagsText(required) + " and without " + flagsText(excluded));
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = *type;
    check(vkAllocateMemory(context.device, &info, nullptr, &allocation.memory), "vkAllocateMemory");
    check(vkBindBufferMemory(context.device, allocation.buffer, allocation.memory, 0), "vkBindBufferMemory");
    if (typeOut != nullptr) *typeOut = *type;
    return allocation;
}

struct HostRegion {
    void* address = MAP_FAILED;
    std::size_t bytes = 0;
    HostRegion() = default;
    HostRegion(const HostRegion&) = delete;
    HostRegion& operator=(const HostRegion&) = delete;
    HostRegion(HostRegion&& other) noexcept { *this = std::move(other); }
    HostRegion& operator=(HostRegion&& other) noexcept {
        std::swap(address, other.address);
        std::swap(bytes, other.bytes);
        return *this;
    }
    ~HostRegion() {
        if (address != MAP_FAILED) munmap(address, bytes);
    }
};

HostRegion anonymous(std::size_t bytes) {
    HostRegion region;
    region.bytes = bytes;
    region.address = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (region.address == MAP_FAILED) fail("mmap of " + std::to_string(bytes) + " bytes failed: " + std::strerror(errno));
    return region;
}

Allocation importHostPointer(const Context& context, void* pointer, VkDeviceSize size) {
    const auto properties = context.Function<PFN_vkGetMemoryHostPointerPropertiesEXT>("vkGetMemoryHostPointerPropertiesEXT");
    VkMemoryHostPointerPropertiesEXT pointerProperties{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    check(properties(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, pointer, &pointerProperties), "vkGetMemoryHostPointerPropertiesEXT");
    Allocation allocation;
    allocation.context = &context;
    allocation.buffer = createBuffer(context, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, allocation.buffer, &requirements);
    const auto type = context.FindType(requirements.memoryTypeBits & pointerProperties.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
    if (!type) fail("no memory type accepts the imported host pointer");
    VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    import.pHostPointer = pointer;
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.pNext = &import;
    info.allocationSize = size;
    info.memoryTypeIndex = *type;
    check(vkAllocateMemory(context.device, &info, nullptr, &allocation.memory), "vkAllocateMemory (host pointer import)");
    check(vkBindBufferMemory(context.device, allocation.buffer, allocation.memory, 0), "vkBindBufferMemory");
    return allocation;
}

class FileDescriptor {
public:
    explicit FileDescriptor(int fd = -1) : fd(fd) {}
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    ~FileDescriptor() {
        if (fd >= 0) close(fd);
    }
    int Get() const { return fd; }
    int Release() {
        const int out = fd;
        fd = -1;
        return out;
    }

private:
    int fd;
};

bool udmabufAvailable() { return access("/dev/udmabuf", R_OK | W_OK) == 0; }

Allocation importUdmabuf(const Context& context, VkDeviceSize size) {
    FileDescriptor memfd(static_cast<int>(memfd_create("vram_bench", MFD_ALLOW_SEALING)));
    if (memfd.Get() < 0) fail(std::string("memfd_create failed: ") + std::strerror(errno));
    if (ftruncate(memfd.Get(), static_cast<off_t>(size)) != 0) fail(std::string("ftruncate failed: ") + std::strerror(errno));
    if (fcntl(memfd.Get(), F_ADD_SEALS, F_SEAL_SHRINK) != 0) fail(std::string("F_ADD_SEALS failed: ") + std::strerror(errno));
    FileDescriptor device(open("/dev/udmabuf", O_RDWR | O_CLOEXEC));
    if (device.Get() < 0) fail(std::string("open /dev/udmabuf failed: ") + std::strerror(errno));
    udmabuf_create create{};
    create.memfd = static_cast<std::uint32_t>(memfd.Get());
    create.flags = UDMABUF_FLAGS_CLOEXEC;
    create.offset = 0;
    create.size = size;
    FileDescriptor dmabuf(ioctl(device.Get(), UDMABUF_CREATE, &create));
    if (dmabuf.Get() < 0) fail(std::string("UDMABUF_CREATE failed: ") + std::strerror(errno));
    const auto properties = context.Function<PFN_vkGetMemoryFdPropertiesKHR>("vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR fdProperties{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    check(properties(context.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dmabuf.Get(), &fdProperties), "vkGetMemoryFdPropertiesKHR");
    Allocation allocation;
    allocation.context = &context;
    allocation.buffer = createBuffer(context, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(context.device, allocation.buffer, &requirements);
    const auto type = context.FindType(requirements.memoryTypeBits & fdProperties.memoryTypeBits, 0, 0);
    if (!type) fail("no memory type accepts the udmabuf");
    VkImportMemoryFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    import.fd = dmabuf.Get();
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.pNext = &import;
    info.allocationSize = size;
    info.memoryTypeIndex = *type;
    check(vkAllocateMemory(context.device, &info, nullptr, &allocation.memory), "vkAllocateMemory (udmabuf import)");
    dmabuf.Release();
    check(vkBindBufferMemory(context.device, allocation.buffer, allocation.memory, 0), "vkBindBufferMemory");
    return allocation;
}

class OneShot {
public:
    explicit OneShot(const Context& context) : context(context) {
        VkCommandBufferAllocateInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        info.commandPool = context.pool;
        info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        info.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(context.device, &info, &commands), "vkAllocateCommandBuffers");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context.device, &fenceInfo, nullptr, &fence), "vkCreateFence");
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = 2;
        check(vkCreateQueryPool(context.device, &queryInfo, nullptr, &queries), "vkCreateQueryPool");
    }
    OneShot(const OneShot&) = delete;
    OneShot& operator=(const OneShot&) = delete;
    ~OneShot() {
        vkDestroyQueryPool(context.device, queries, nullptr);
        vkDestroyFence(context.device, fence, nullptr);
        vkFreeCommandBuffers(context.device, context.pool, 1, &commands);
    }

    double TimedCopyNs(VkBuffer source, VkBuffer destination, VkDeviceSize bytes) {
        check(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
        vkCmdResetQueryPool(commands, queries, 0, 2);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, 0);
        const VkBufferCopy region{0, 0, bytes};
        vkCmdCopyBuffer(commands, source, destination, 1, &region);
        vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, 1);
        check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        submitAndWait(commands);
        std::uint64_t stamps[2] = {};
        check(vkGetQueryPoolResults(context.device, queries, 0, 2, sizeof(stamps), stamps, sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        const auto mask = context.timestampValidBits >= 64 ? ~0ull : ((1ull << context.timestampValidBits) - 1);
        const auto ticks = (stamps[1] - stamps[0]) & mask;
        return static_cast<double>(ticks) * context.properties.limits.timestampPeriod;
    }

    VkCommandBuffer Empty() {
        check(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
        check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        return commands;
    }

    void submitAndWait(VkCommandBuffer buffer) {
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &buffer;
        check(vkQueueSubmit(context.queue, 1, &submit, fence), "vkQueueSubmit");
        check(vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
        check(vkResetFences(context.device, 1, &fence), "vkResetFences");
    }

private:
    const Context& context;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
};

double seconds(Clock::duration duration) { return std::chrono::duration<double>(duration).count(); }

double bestOf(int passes, const std::function<void()>& body) {
    double best = 1e30;
    for (int pass = 0; pass < passes; ++pass) {
        const auto start = Clock::now();
        body();
        best = std::min(best, seconds(Clock::now() - start));
    }
    return best;
}

double chaseNs(std::byte* base, std::size_t bytes, std::uint32_t hops) {
    constexpr std::size_t Line = 64;
    const std::size_t slots = bytes / Line;
    if (slots < 2) fail("region too small for a pointer chase");
    std::vector<std::size_t> order(slots);
    std::iota(order.begin(), order.end(), 0);
    std::mt19937_64 random(12345);
    std::shuffle(order.begin() + 1, order.end(), random);
    for (std::size_t i = 0; i < slots; ++i) {
        const std::uint64_t next = order[(i + 1) % slots] * Line;
        std::memcpy(base + order[i] * Line, &next, sizeof(next));
    }
    std::uint64_t cursor = 0;
    const auto start = Clock::now();
    for (std::uint32_t i = 0; i < hops; ++i) {
        const volatile std::uint64_t* slot = reinterpret_cast<const volatile std::uint64_t*>(base + cursor);
        cursor = *slot;
    }
    const auto elapsed = seconds(Clock::now() - start);
    if (cursor >= bytes) fail("pointer chase left the region");
    return elapsed * 1e9 / hops;
}

double gbPerSecond(std::size_t bytes, double secondsTaken) { return static_cast<double>(bytes) / secondsTaken / 1e9; }

std::string runInfo(const Context& context) {
    std::printf("device: %s (%s, %s), Vulkan %u.%u.%u\n", context.properties.deviceName, context.driverName.c_str(), context.driverInfo.c_str(), VK_API_VERSION_MAJOR(context.properties.apiVersion), VK_API_VERSION_MINOR(context.properties.apiVersion), VK_API_VERSION_PATCH(context.properties.apiVersion));
    JsonObject json;
    json.Text("device", context.properties.deviceName);
    json.Text("driverName", context.driverName);
    json.Text("driverInfo", context.driverInfo);
    json.Text("deviceType", context.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : context.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated" : context.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? "cpu" : "other");
    std::string heaps = "[";
    VkDeviceSize deviceLocal = 0;
    VkDeviceSize visibleDeviceLocal = 0;
    for (std::uint32_t i = 0; i < context.memory.memoryHeapCount; ++i) {
        const auto& heap = context.memory.memoryHeaps[i];
        bool hostVisible = false;
        for (std::uint32_t t = 0; t < context.memory.memoryTypeCount; ++t)
            if (context.memory.memoryTypes[t].heapIndex == i && (context.memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) hostVisible = true;
        const bool local = (heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0;
        if (local) deviceLocal = std::max(deviceLocal, heap.size);
        if (local && hostVisible) visibleDeviceLocal = std::max(visibleDeviceLocal, heap.size);
        std::printf("  heap %u: %8.0f MiB%s%s\n", i, static_cast<double>(heap.size) / MiB, local ? " DEVICE_LOCAL" : "", hostVisible ? " (host-visible types)" : "");
        JsonObject entry;
        entry.Integer("index", i);
        entry.Integer("bytes", heap.size);
        entry.Bool("deviceLocal", local);
        entry.Bool("hostVisibleTypes", hostVisible);
        heaps += (i == 0 ? "" : ", ") + entry.Str();
    }
    json.Add("heaps", heaps + "]");
    std::string types = "[";
    for (std::uint32_t i = 0; i < context.memory.memoryTypeCount; ++i) {
        const auto& type = context.memory.memoryTypes[i];
        std::printf("  type %2u: heap %u %s\n", i, type.heapIndex, flagsText(type.propertyFlags).c_str());
        JsonObject entry;
        entry.Integer("index", i);
        entry.Integer("heap", type.heapIndex);
        entry.Text("flags", flagsText(type.propertyFlags));
        types += (i == 0 ? "" : ", ") + entry.Str();
    }
    json.Add("types", types + "]");
    const bool discrete = context.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
    const bool fullRebar = discrete && deviceLocal != 0 && visibleDeviceLocal * 10 >= deviceLocal * 9;
    const char* verdict = !discrete ? "not a discrete GPU: Resizable BAR does not apply" : fullRebar ? "full Resizable BAR" : "no full Resizable BAR";
    std::printf("  CPU-visible VRAM heap: %.0f MiB of %.0f MiB device-local (%s)\n", static_cast<double>(visibleDeviceLocal) / MiB, static_cast<double>(deviceLocal) / MiB, verdict);
    json.Integer("deviceLocalHeapBytes", deviceLocal);
    json.Integer("hostVisibleDeviceLocalHeapBytes", visibleDeviceLocal);
    json.Bool("fullResizableBar", fullRebar);
    std::printf("  VK_EXT_map_memory_placed: %s (memoryMapPlaced %d, memoryMapRangePlaced %d, memoryUnmapReserve %d, minPlacedMemoryMapAlignment %llu)\n", context.extensions.mapMemoryPlaced ? "yes" : "no", context.placed.memoryMapPlaced, context.placed.memoryMapRangePlaced, context.placed.memoryUnmapReserve, static_cast<unsigned long long>(context.placed.minPlacedMemoryMapAlignment));
    std::printf("  VK_EXT_external_memory_host: %s (alignment %llu), VK_EXT_external_memory_dma_buf: %s, /dev/udmabuf: %s\n", context.extensions.externalMemoryHost ? "yes" : "no", static_cast<unsigned long long>(context.minImportedHostPointerAlignment), context.extensions.externalMemoryDmaBuf ? "yes" : "no", udmabufAvailable() ? "usable" : "not usable");
    json.Bool("mapMemoryPlaced", context.extensions.mapMemoryPlaced && context.placed.memoryMapPlaced);
    json.Bool("memoryMapRangePlaced", context.placed.memoryMapRangePlaced);
    json.Bool("memoryUnmapReserve", context.placed.memoryUnmapReserve);
    json.Integer("minPlacedMemoryMapAlignment", context.placed.minPlacedMemoryMapAlignment);
    json.Bool("externalMemoryHost", context.extensions.externalMemoryHost);
    json.Integer("minImportedHostPointerAlignment", context.minImportedHostPointerAlignment);
    json.Bool("externalMemoryDmaBuf", context.extensions.externalMemoryDmaBuf);
    json.Bool("udmabuf", udmabufAvailable());
    return json.Str();
}

class MappedVram {
public:
    MappedVram(const Context& context, const Options& options) : context(context), placed(!options.unplaced) {
        if (placed && (!context.extensions.mapMemoryPlaced || !context.placed.memoryMapPlaced)) fail("VK_EXT_map_memory_placed with memoryMapPlaced is not supported on this device (--unplaced maps without placement)");
        if (!placed && options.address != 0) fail("--address needs a placed map");
        const auto alignment = std::max<VkDeviceSize>(context.placed.minPlacedMemoryMapAlignment, static_cast<VkDeviceSize>(sysconf(_SC_PAGESIZE)));
        bytes = (options.sizeMiB * MiB + alignment - 1) / alignment * alignment;
        if (options.address % alignment != 0) fail("--address must be a multiple of minPlacedMemoryMapAlignment (" + std::to_string(alignment) + ")");
        vram = allocateBuffer(context, bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &type);
        if (placed) {
            reservedBytes = static_cast<std::size_t>(bytes + alignment);
            const auto reserveFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (options.address != 0 ? MAP_FIXED_NOREPLACE : 0);
            reserved = mmap(reinterpret_cast<void*>(options.address), reservedBytes, PROT_NONE, reserveFlags, -1, 0);
            if (reserved == MAP_FAILED) fail(std::string("reserving the placed range failed: ") + std::strerror(errno));
            auto placedAddress = (reinterpret_cast<std::uintptr_t>(reserved) + alignment - 1) / alignment * alignment;
            if (options.address != 0) placedAddress = options.address;
            VkMemoryMapPlacedInfoEXT placedInfo{VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT};
            placedInfo.pPlacedAddress = reinterpret_cast<void*>(placedAddress);
            VkMemoryMapInfoKHR mapInfo{VK_STRUCTURE_TYPE_MEMORY_MAP_INFO_KHR};
            mapInfo.pNext = &placedInfo;
            mapInfo.flags = VK_MEMORY_MAP_PLACED_BIT_EXT;
            mapInfo.memory = vram.memory;
            mapInfo.offset = 0;
            mapInfo.size = VK_WHOLE_SIZE;
            check(context.Function<PFN_vkMapMemory2KHR>("vkMapMemory2KHR")(context.device, &mapInfo, &mapped), "vkMapMemory2KHR (placed)");
            if (reinterpret_cast<std::uintptr_t>(mapped) != placedAddress) fail("placed map landed at " + hex(reinterpret_cast<std::uintptr_t>(mapped)) + ", requested " + hex(placedAddress));
        } else {
            check(vkMapMemory(context.device, vram.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        }
        std::printf("%s map: %zu MiB of memory type %u (%s) at %s\n", placed ? "placed" : "unplaced", static_cast<std::size_t>(bytes / MiB), type, flagsText(context.memory.memoryTypes[type].propertyFlags).c_str(), hex(Address()).c_str());
    }

    MappedVram(const MappedVram&) = delete;
    MappedVram& operator=(const MappedVram&) = delete;

    ~MappedVram() {
        if (mapped != nullptr) {
            if (placed && context.placed.memoryUnmapReserve) {
                VkMemoryUnmapInfoKHR unmapInfo{VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO_KHR};
                unmapInfo.flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT;
                unmapInfo.memory = vram.memory;
                context.Function<PFN_vkUnmapMemory2KHR>("vkUnmapMemory2KHR")(context.device, &unmapInfo);
            } else {
                vkUnmapMemory(context.device, vram.memory);
            }
        }
        if (reserved != MAP_FAILED) munmap(reserved, reservedBytes);
    }

    void Describe(JsonObject& results) const {
        results.Bool("placed", placed);
        results.Text("mapAddress", hex(Address()));
        results.Integer("mapBytes", bytes);
        results.Integer("memoryType", type);
        results.Text("memoryFlags", flagsText(context.memory.memoryTypes[type].propertyFlags));
    }

    std::byte* Base() const { return static_cast<std::byte*>(mapped); }
    std::uintptr_t Address() const { return reinterpret_cast<std::uintptr_t>(mapped); }
    VkDeviceSize Bytes() const { return bytes; }
    VkBuffer Buffer() const { return vram.buffer; }
    bool Placed() const { return placed; }

private:
    const Context& context;
    bool placed;
    Allocation vram;
    std::uint32_t type = 0;
    VkDeviceSize bytes = 0;
    void* reserved = MAP_FAILED;
    std::size_t reservedBytes = 0;
    void* mapped = nullptr;
};

std::string runPlaced(const Context& context, const Options& options, JsonObject& results) {
    MappedVram mapping(context, options);
    mapping.Describe(results);
    const auto bytes = mapping.Bytes();
    auto* base = mapping.Base();
    std::vector<std::byte> host(static_cast<std::size_t>(bytes), std::byte{0x5a});
    const auto writeSeconds = bestOf(3, [&] { std::memcpy(base, host.data(), host.size()); });
    const auto readBytes = std::min<std::size_t>(static_cast<std::size_t>(bytes), 64 * MiB);
    const auto readSeconds = bestOf(2, [&] { std::memcpy(host.data(), base, readBytes); });
    const auto latencyBytes = std::min<std::size_t>(static_cast<std::size_t>(bytes), 64 * MiB);
    const auto mappedLatency = chaseNs(base, latencyBytes, 20000);
    const auto ramLatency = chaseNs(host.data(), latencyBytes, 2000000);
    std::printf("  CPU write to mapped memory:  %7.2f GB/s\n", gbPerSecond(host.size(), writeSeconds));
    std::printf("  CPU read from mapped memory: %7.3f GB/s\n", gbPerSecond(readBytes, readSeconds));
    std::printf("  CPU dependent load: %8.1f ns from mapped memory, %6.1f ns from system RAM (%zu MiB region)\n", mappedLatency, ramLatency, latencyBytes / MiB);
    results.Number("cpuWriteGBps", gbPerSecond(host.size(), writeSeconds));
    results.Number("cpuReadGBps", gbPerSecond(readBytes, readSeconds));
    results.Number("cpuLoadLatencyMappedNs", mappedLatency);
    results.Number("cpuLoadLatencyRamNs", ramLatency);

    std::string gpu = "[";
    {
        OneShot shot(context);
        const bool deviceOnly = context.FindType(~0u, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT).has_value();
        Allocation destination = deviceOnly ? allocateBuffer(context, bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0)
                                            : allocateBuffer(context, bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, 0);
        const auto measure = [&](const char* name, VkBuffer source) {
            static_cast<void>(shot.TimedCopyNs(source, destination.buffer, bytes));
            double best = 1e30;
            for (int pass = 0; pass < 3; ++pass) best = std::min(best, shot.TimedCopyNs(source, destination.buffer, bytes));
            const auto rate = static_cast<double>(bytes) / best;
            std::printf("  GPU read (copy) from %-28s %7.2f GB/s\n", name, rate);
            JsonObject entry;
            entry.Text("source", name);
            entry.Number("GBps", rate);
            gpu += (gpu.size() == 1 ? "" : ", ") + entry.Str();
        };
        measure(mapping.Placed() ? "placed VRAM:" : "mapped VRAM:", mapping.Buffer());
        if (deviceOnly) {
            Allocation local = allocateBuffer(context, bytes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0);
            measure("device-only VRAM:", local.buffer);
        }
        if (context.FindType(~0u, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            Allocation system = allocateBuffer(context, bytes, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            measure("system memory (GTT):", system.buffer);
        }
        if (context.extensions.externalMemoryHost) {
            auto region = anonymous(static_cast<std::size_t>(bytes));
            Allocation imported = importHostPointer(context, region.address, bytes);
            measure("host-pointer import:", imported.buffer);
        }
        if (context.extensions.externalMemoryDmaBuf && udmabufAvailable()) {
            Allocation imported = importUdmabuf(context, bytes);
            measure("udmabuf import:", imported.buffer);
        }
    }
    results.Add("gpuRead", gpu + "]");
    return results.Str();
}

struct AccessResult {
    std::string name;
    std::string status;
    std::string detail;
};

std::string errnoText(int error) { return std::string(std::strerror(error)) + " (errno " + std::to_string(error) + ")"; }

class TempFile {
public:
    explicit TempFile(std::size_t bytes) {
        char name[] = "/tmp/vram_bench_XXXXXX";
        fd = mkstemp(name);
        if (fd < 0) fail(std::string("mkstemp failed: ") + std::strerror(errno));
        path = name;
        std::vector<std::byte> pattern(bytes);
        for (std::size_t i = 0; i < bytes; ++i) pattern[i] = static_cast<std::byte>((i * 131u + 7u) & 0xffu);
        if (pwrite(fd, pattern.data(), bytes, 0) != static_cast<ssize_t>(bytes)) fail(std::string("writing the temporary file failed: ") + std::strerror(errno));
        expected = std::move(pattern);
    }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    ~TempFile() {
        close(fd);
        unlink(path.c_str());
    }
    const std::string& Path() const { return path; }
    int Fd() const { return fd; }
    const std::vector<std::byte>& Expected() const { return expected; }

private:
    int fd = -1;
    std::string path;
    std::vector<std::byte> expected;
};

AccessResult testAtomics(std::byte* base) {
    constexpr int Threads = 4;
    constexpr std::uint64_t Increments = 200000;
    auto* counter = reinterpret_cast<std::uint64_t*>(base);
    auto* exchange = reinterpret_cast<std::uint64_t*>(base + 64);
    *counter = 0;
    *exchange = 0;
    std::vector<std::thread> workers;
    const auto start = Clock::now();
    for (int t = 0; t < Threads; ++t) {
        workers.emplace_back([&] {
            std::atomic_ref<std::uint64_t> add(*counter);
            std::atomic_ref<std::uint64_t> swap(*exchange);
            for (std::uint64_t i = 0; i < Increments; ++i) {
                add.fetch_add(1, std::memory_order_seq_cst);
                auto expected = swap.load(std::memory_order_relaxed);
                while (!swap.compare_exchange_weak(expected, expected + 1, std::memory_order_seq_cst)) {
                }
            }
        });
    }
    for (auto& worker : workers) worker.join();
    const auto elapsed = seconds(Clock::now() - start);
    const auto total = Threads * Increments;
    const bool correct = *counter == total && *exchange == total;
    char detail[160];
    std::snprintf(detail, sizeof(detail), "%d threads x %llu fetch_add + CAS: add=%llu cas=%llu, %.2f M op pairs/s", Threads, static_cast<unsigned long long>(Increments), static_cast<unsigned long long>(*counter), static_cast<unsigned long long>(*exchange), static_cast<double>(total) / elapsed / 1e6);
    return {"locked atomics", correct ? "pass" : "FAIL", detail};
}

AccessResult testRead(std::byte* base, const TempFile& file, std::size_t bytes) {
    std::memset(base, 0, bytes);
    const auto got = pread(file.Fd(), base, bytes, 0);
    if (got < 0) return {"read() into mapping", "FAIL", errnoText(errno)};
    if (static_cast<std::size_t>(got) != bytes || std::memcmp(base, file.Expected().data(), bytes) != 0) return {"read() into mapping", "FAIL", "short read or wrong data"};
    return {"read() into mapping", "pass", std::to_string(bytes) + " bytes"};
}

AccessResult testWrite(std::byte* base, std::size_t bytes) {
    char name[] = "/tmp/vram_bench_out_XXXXXX";
    const int fd = mkstemp(name);
    if (fd < 0) return {"write() from mapping", "skip", errnoText(errno)};
    for (std::size_t i = 0; i < bytes; ++i) base[i] = static_cast<std::byte>(i & 0xffu);
    const auto written = pwrite(fd, base, bytes, 0);
    const int error = errno;
    std::vector<std::byte> back(bytes);
    const auto readBack = written == static_cast<ssize_t>(bytes) ? pread(fd, back.data(), bytes, 0) : -1;
    close(fd);
    unlink(name);
    if (written < 0) return {"write() from mapping", "FAIL", errnoText(error)};
    if (readBack != static_cast<ssize_t>(bytes) || std::memcmp(back.data(), base, bytes) != 0) return {"write() from mapping", "FAIL", "short write or wrong data"};
    return {"write() from mapping", "pass", std::to_string(bytes) + " bytes"};
}

AccessResult testDirectRead(std::byte* base, const TempFile& file, std::size_t bytes) {
    const int fd = open(file.Path().c_str(), O_RDONLY | O_DIRECT);
    if (fd < 0) return {"O_DIRECT read into mapping", "skip", "O_DIRECT open failed: " + errnoText(errno)};
    void* control = nullptr;
    if (posix_memalign(&control, 4096, bytes) != 0) {
        close(fd);
        fail("posix_memalign failed");
    }
    const auto controlGot = pread(fd, control, bytes, 0);
    const int controlError = errno;
    std::free(control);
    if (controlGot != static_cast<ssize_t>(bytes)) {
        close(fd);
        return {"O_DIRECT read into mapping", "skip", "O_DIRECT does not work on this file system even into RAM: " + errnoText(controlError)};
    }
    std::memset(base, 0, bytes);
    const auto got = pread(fd, base, bytes, 0);
    const int error = errno;
    close(fd);
    if (got < 0) return {"O_DIRECT read into mapping", "FAIL", errnoText(error)};
    if (static_cast<std::size_t>(got) != bytes || std::memcmp(base, file.Expected().data(), bytes) != 0) return {"O_DIRECT read into mapping", "FAIL", "short read or wrong data"};
    return {"O_DIRECT read into mapping", "pass", std::to_string(bytes) + " bytes"};
}

AccessResult testFutex(std::byte* base, bool shared) {
    const char* name = shared ? "shared futex on mapping" : "private futex on mapping";
    auto* word = reinterpret_cast<std::uint32_t*>(base + 128);
    *word = 1;
    timespec timeout{0, 1000000};
    const long result = syscall(SYS_futex, word, shared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE, 2u, &timeout, nullptr, 0);
    const int error = errno;
    if (result == -1 && error == EAGAIN) return {name, "pass", "kernel read the futex word (EAGAIN on value mismatch)"};
    if (result == -1) return {name, "FAIL", errnoText(error)};
    return {name, "FAIL", "futex wait returned without a value mismatch"};
}

AccessResult testProcessVmRead(std::byte* base, std::size_t bytes) {
    for (std::size_t i = 0; i < bytes; ++i) base[i] = static_cast<std::byte>((i * 7u) & 0xffu);
    std::vector<std::byte> local(bytes);
    iovec localVector{local.data(), bytes};
    iovec remoteVector{base, bytes};
    const auto got = process_vm_readv(getpid(), &localVector, 1, &remoteVector, 1, 0);
    if (got < 0) return {"process_vm_readv of mapping", "FAIL", errnoText(errno)};
    if (static_cast<std::size_t>(got) != bytes || std::memcmp(local.data(), base, bytes) != 0) return {"process_vm_readv of mapping", "FAIL", "short read or wrong data"};
    return {"process_vm_readv of mapping", "pass", std::to_string(bytes) + " bytes"};
}

AccessResult testMprotect(std::byte* base, std::size_t bytes) {
    if (mprotect(base, bytes, PROT_READ) != 0) return {"mprotect of mapping", "FAIL", "PROT_READ: " + errnoText(errno)};
    const volatile std::byte value = base[0];
    static_cast<void>(value);
    if (mprotect(base, bytes, PROT_READ | PROT_WRITE) != 0) return {"mprotect of mapping", "FAIL", "PROT_READ|PROT_WRITE: " + errnoText(errno)};
    base[0] = std::byte{1};
    return {"mprotect of mapping", "pass", "read-only and back to read-write"};
}

std::string runAccess(const Context& context, const Options& options) {
    MappedVram mapping(context, options);
    JsonObject json;
    mapping.Describe(json);
    constexpr std::size_t IoBytes = 1 * MiB;
    if (mapping.Bytes() < 4 * IoBytes) fail("access tests need at least 4 MiB (--size-mib)");
    auto* base = mapping.Base();
    TempFile file(IoBytes);
    std::vector<AccessResult> results;
    results.push_back(testAtomics(base));
    results.push_back(testRead(base + IoBytes, file, IoBytes));
    results.push_back(testWrite(base + IoBytes, IoBytes));
    results.push_back(testDirectRead(base + 2 * IoBytes, file, IoBytes));
    results.push_back(testFutex(base, false));
    results.push_back(testFutex(base, true));
    results.push_back(testProcessVmRead(base + 3 * IoBytes, IoBytes));
    results.push_back(testMprotect(base, static_cast<std::size_t>(mapping.Bytes())));
    std::string list = "[";
    for (const auto& result : results) {
        std::printf("  %-28s %-5s %s\n", result.name.c_str(), result.status.c_str(), result.detail.c_str());
        JsonObject entry;
        entry.Text("test", result.name);
        entry.Text("status", result.status);
        entry.Text("detail", result.detail);
        list += (list.size() == 1 ? "" : ", ") + entry.Str();
    }
    json.Add("tests", list + "]");
    return json.Str();
}

double submitMicroseconds(const Context& context, std::uint32_t submits) {
    OneShot shot(context);
    const auto commands = shot.Empty();
    for (int i = 0; i < 10; ++i) shot.submitAndWait(commands);
    std::vector<double> samples;
    samples.reserve(submits);
    for (std::uint32_t i = 0; i < submits; ++i) {
        const auto start = Clock::now();
        shot.submitAndWait(commands);
        samples.push_back(seconds(Clock::now() - start) * 1e6);
    }
    std::nth_element(samples.begin(), samples.begin() + samples.size() / 2, samples.end());
    return samples[samples.size() / 2];
}

std::string runSubmits(const Context& context, const Options& options) {
    constexpr VkDeviceSize Small = 64 * 1024;
    JsonObject json;
    const auto baseline = submitMicroseconds(context, options.submits);
    std::printf("submit + wait, median of %u (empty command buffer):\n", options.submits);
    std::printf("  no extra allocations:          %8.1f us\n", baseline);
    json.Number("baselineUs", baseline);
    {
        std::vector<Allocation> allocations;
        allocations.reserve(options.allocations);
        for (std::uint32_t i = 0; i < options.allocations; ++i) allocations.push_back(allocateBuffer(context, Small, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT));
        const auto value = submitMicroseconds(context, options.submits);
        std::printf("  %5u ordinary allocations:     %8.1f us\n", options.allocations, value);
        json.Number("ordinaryAllocationsUs", value);
    }
    if (context.extensions.externalMemoryHost) {
        std::vector<HostRegion> regions;
        std::vector<Allocation> allocations;
        regions.reserve(options.allocations);
        allocations.reserve(options.allocations);
        for (std::uint32_t i = 0; i < options.allocations; ++i) {
            regions.push_back(anonymous(static_cast<std::size_t>(Small)));
            allocations.push_back(importHostPointer(context, regions.back().address, Small));
        }
        const auto value = submitMicroseconds(context, options.submits);
        std::printf("  %5u host-pointer imports:     %8.1f us\n", options.allocations, value);
        json.Number("hostPointerImportsUs", value);
    }
    if (context.extensions.externalMemoryDmaBuf && udmabufAvailable()) {
        std::vector<Allocation> allocations;
        allocations.reserve(options.allocations);
        for (std::uint32_t i = 0; i < options.allocations; ++i) allocations.push_back(importUdmabuf(context, Small));
        const auto value = submitMicroseconds(context, options.submits);
        std::printf("  %5u udmabuf imports:          %8.1f us\n", options.allocations, value);
        json.Number("udmabufImportsUs", value);
    }
    json.Integer("allocations", options.allocations);
    json.Integer("submits", options.submits);
    return json.Str();
}

std::uint64_t parseNumber(const std::string& text, const char* option) {
    try {
        std::size_t used = 0;
        const auto value = std::stoull(text, &used, 0);
        if (used != text.size()) throw std::invalid_argument(text);
        return value;
    } catch (const std::exception&) {
        fail(std::string(option) + " needs a number, got \"" + text + "\"");
    }
}

Options parse(int argc, char** argv) {
    if (argc < 2) fail("usage");
    Options options;
    options.command = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto value = [&]() -> std::string {
            if (++i == argc) fail(argument + " needs a value");
            return argv[i];
        };
        if (argument == "--gpu") options.gpu = value();
        else if (argument == "--json") options.json = value();
        else if (argument == "--size-mib") options.sizeMiB = static_cast<std::size_t>(parseNumber(value(), "--size-mib"));
        else if (argument == "--address") options.address = parseNumber(value(), "--address");
        else if (argument == "--allocations") options.allocations = static_cast<std::uint32_t>(parseNumber(value(), "--allocations"));
        else if (argument == "--submits") options.submits = static_cast<std::uint32_t>(parseNumber(value(), "--submits"));
        else if (argument == "--unplaced") options.unplaced = true;
        else fail("unknown option " + argument);
    }
    if (options.command != "info" && options.command != "placed" && options.command != "access" && options.command != "submits" && options.command != "all") fail("unknown command " + options.command);
    if (options.sizeMiB == 0 || options.allocations == 0 || options.submits == 0) fail("sizes and counts must be positive");
    return options;
}

}

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse(argc, argv);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\nusage: vram_bench info|placed|access|submits|all [--gpu NAME] [--json FILE] [--size-mib N] [--address 0xADDR] [--allocations N] [--submits N] [--unplaced]\n", error.what());
        return 2;
    }
    try {
        Context context(options.gpu);
        JsonObject report;
        report.Text("tool", "vram_bench");
        report.Add("info", runInfo(context));
        if (options.command == "placed" || options.command == "all") {
            JsonObject placed;
            report.Add("placed", runPlaced(context, options, placed));
        }
        if (options.command == "access" || options.command == "all") report.Add("access", runAccess(context, options));
        if (options.command == "submits" || options.command == "all") report.Add("submits", runSubmits(context, options));
        if (!options.json.empty()) {
            std::ofstream file(options.json, std::ios::trunc);
            if (!file) fail("cannot open " + options.json);
            file << report.Str() << "\n";
            if (!file) fail("cannot write " + options.json);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "vram_bench: %s\n", error.what());
        return 1;
    }
}
