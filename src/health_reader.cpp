#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Opaque IL2CPP types: no game-specific addresses or object offsets.
struct Domain;
struct Assembly;
struct Image;
struct Class;
struct Object;
struct Field;
struct Method;
struct Type;
struct Thread;

struct Api {
    Domain* (*domain_get)();
    const Assembly** (*domain_get_assemblies)(Domain*, size_t*);
    const Image* (*assembly_get_image)(const Assembly*);
    Class* (*class_from_name)(const Image*, const char*, const char*);
    Field* (*class_get_field_from_name)(Class*, const char*);
    Class* (*class_get_parent)(Class*);
    const Method* (*class_get_method_from_name)(Class*, const char*, int);
    bool (*class_is_assignable_from)(Class*, Class*);
    Class* (*object_get_class)(Object*);
    const Method* (*object_get_virtual_method)(Object*, const Method*);
    void* (*object_unbox)(Object*);
    void (*field_static_get_value)(Field*, void*);
    void (*field_get_value)(Object*, Field*, void*);
    const Type* (*field_get_type)(Field*);
    int (*type_get_type)(const Type*);
    const Type* (*method_get_return_type)(const Method*);
    Object* (*runtime_invoke)(const Method*, void*, void**, Object**);
    Thread* (*thread_attach)(Domain*);
    void (*thread_detach)(Thread*);
    uint32_t (*gchandle_new)(Object*, bool);
    void (*gchandle_free)(uint32_t);
    uint32_t (*array_length)(Object*);
    uint32_t (*array_object_header_size)();
    int (*array_element_size)(Class*);

    bool Load(HMODULE module, char* missing, size_t capacity) {
#define LOAD(name) \
        { FARPROC address = GetProcAddress(module, "il2cpp_" #name); \
          static_assert(sizeof(name) == sizeof(address)); \
          std::memcpy(&name, &address, sizeof(name)); } \
        if (!name) { std::snprintf(missing, capacity, "Missing export: il2cpp_%s", #name); return false; }
        LOAD(domain_get)
        LOAD(domain_get_assemblies)
        LOAD(assembly_get_image)
        LOAD(class_from_name)
        LOAD(class_get_field_from_name)
        LOAD(class_get_parent)
        LOAD(class_get_method_from_name)
        LOAD(class_is_assignable_from)
        LOAD(object_get_class)
        LOAD(object_get_virtual_method)
        LOAD(object_unbox)
        LOAD(field_static_get_value)
        LOAD(field_get_value)
        LOAD(field_get_type)
        LOAD(type_get_type)
        LOAD(method_get_return_type)
        LOAD(runtime_invoke)
        LOAD(thread_attach)
        LOAD(thread_detach)
        LOAD(gchandle_new)
        LOAD(gchandle_free)
        LOAD(array_length)
        LOAD(array_object_header_size)
        LOAD(array_element_size)
#undef LOAD
        return true;
    }
};

static HANDLE stop_event = nullptr;
static HANDLE worker = nullptr;
// Pack health and validity in a single atomic snapshot.
static std::atomic<uint64_t> latest{0};
static std::atomic<bool> running{false};
static_assert(sizeof(float) == sizeof(uint32_t));
static_assert(sizeof(void*) == 8, "Build for Windows x64");

static void Log(const char* message) {
    char path[MAX_PATH];
    DWORD length = GetTempPathA(MAX_PATH, path);
    if (!length || length >= MAX_PATH || length + sizeof("SCPInject-health.log") > MAX_PATH) return;
    std::strcat(path, "SCPInject-health.log");
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME time;
    GetLocalTime(&time);
    char line[512];
    int length_written = std::snprintf(line, sizeof(line),
        "[%04u-%02u-%02u %02u:%02u:%02u] %s\r\n", time.wYear, time.wMonth, time.wDay,
        time.wHour, time.wMinute, time.wSecond, message);
    DWORD written;
    if (length_written > 0 && length_written < static_cast<int>(sizeof(line)))
        WriteFile(file, line, static_cast<DWORD>(length_written), &written, nullptr);
    CloseHandle(file);
}

static Field* FindField(Api& api, Class* klass, const char* name) {
    for (; klass; klass = api.class_get_parent(klass))
        if (Field* field = api.class_get_field_from_name(klass, name)) return field;
    return nullptr;
}

static const Method* FindMethod(Api& api, Class* klass, const char* name) {
    for (; klass; klass = api.class_get_parent(klass))
        if (const Method* method = api.class_get_method_from_name(klass, name, 0)) return method;
    return nullptr;
}

struct Pin {
    Api& api;
    uint32_t handle;
    Pin(Api& a, Object* object) : api(a), handle(a.gchandle_new(object, true)) {}
    ~Pin() { if (handle) api.gchandle_free(handle); }
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
};

static const char* ReadHealth(Api& api, Field* local_hub, Class* health_class, float& value) {
    Object* hub = nullptr;
    api.field_static_get_value(local_hub, &hub);
    if (!hub) return "Waiting for local player";
    Pin hub_pin(api, hub);
    if (!hub_pin.handle) return "Cannot retain local player";
    Field* stats_field = FindField(api, api.object_get_class(hub), "playerStats");
    if (!stats_field || api.type_get_type(api.field_get_type(stats_field)) != 0x12)
        return "Unsupported playerStats field";
    Object* stats = nullptr;
    api.field_get_value(hub, stats_field, &stats);
    if (!stats) return "Waiting for player stats";
    Pin stats_pin(api, stats);
    if (!stats_pin.handle) return "Cannot retain player stats";
    Field* modules_field = FindField(api, api.object_get_class(stats), "_statModules");
    if (!modules_field || api.type_get_type(api.field_get_type(modules_field)) != 0x1d)
        return "Unsupported _statModules array";
    Object* modules = nullptr;
    api.field_get_value(stats, modules_field, &modules);
    if (!modules) return "Waiting for stat modules";
    Pin modules_pin(api, modules);
    if (!modules_pin.handle) return "Cannot retain stat modules";
    uint32_t count = api.array_length(modules);
    uint32_t header = api.array_object_header_size();
    if (count > 128 || header < 16 || header > 256 ||
        api.array_element_size(api.object_get_class(modules)) != static_cast<int>(sizeof(Object*)))
        return "Unsupported IL2CPP array layout";
    auto elements = reinterpret_cast<Object**>(reinterpret_cast<unsigned char*>(modules) + header);
    for (uint32_t index = 0; index < count; ++index) {
        Object* stat = elements[index];
        if (!stat || !api.class_is_assignable_from(health_class, api.object_get_class(stat))) continue;
        Pin stat_pin(api, stat);
        if (!stat_pin.handle) return "Cannot retain health module";
        const Method* getter = FindMethod(api, api.object_get_class(stat), "get_CurValue");
        if (!getter) return "CurValue getter unavailable";
        getter = api.object_get_virtual_method(stat, getter);
        if (!getter || api.type_get_type(api.method_get_return_type(getter)) != 0x0c)
            return "Unsupported health return type";
        Object* exception = nullptr;
        Object* boxed = api.runtime_invoke(getter, stat, nullptr, &exception);
        if (exception) return "Health getter raised a managed exception";
        if (!boxed) return "Health getter returned no value";
        Pin value_pin(api, boxed);
        if (!value_pin.handle) return "Cannot retain health value";
        void* data = api.object_unbox(boxed);
        if (!data) return "Cannot unbox health value";
        std::memcpy(&value, data, sizeof(value));
        if (!std::isfinite(value)) return "Health value is not finite";
        return nullptr;
    }
    return "Health module unavailable";
}

static DWORD WINAPI Worker(void*) {
    running.store(true);
    HMODULE retained = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                           reinterpret_cast<LPCSTR>(&Worker), &retained)) {
        running.store(false);
        return 1;
    }
    Log("Health reader started; waiting for IL2CPP");
    Api api{};
    Thread* attached = nullptr;
    char error[256]{};
    while (WaitForSingleObject(stop_event, 250) == WAIT_TIMEOUT) {
        HMODULE game = GetModuleHandleA("GameAssembly.dll");
        if (!game) continue;
        if (!api.Load(game, error, sizeof(error))) { Log(error); break; }
        Domain* domain = api.domain_get();
        if (!domain) continue;
        attached = api.thread_attach(domain);
        if (!attached) { Log("Cannot attach IL2CPP thread"); break; }
        size_t count = 0;
        const Assembly** assemblies = api.domain_get_assemblies(domain, &count);
        Class* hub_class = nullptr;
        Class* health_class = nullptr;
        for (size_t i = 0; assemblies && i < count; ++i) {
            const Image* image = api.assembly_get_image(assemblies[i]);
            if (!image) continue;
            if (!hub_class) hub_class = api.class_from_name(image, "", "ReferenceHub");
            if (!health_class) health_class = api.class_from_name(image, "PlayerStatsSystem", "HealthStat");
        }
        if (!hub_class || !health_class) {
            api.thread_detach(attached);
            attached = nullptr;
            continue;
        }
        Field* local_hub = FindField(api, hub_class, "_localHub");
        if (!local_hub || api.type_get_type(api.field_get_type(local_hub)) != 0x12) {
            Log("Unsupported ReferenceHub._localHub field");
            break;
        }
        char previous[256]{};
        while (WaitForSingleObject(stop_event, 250) == WAIT_TIMEOUT) {
            float health = 0;
            const char* status = ReadHealth(api, local_hub, health_class, health);
            char message[256];
            if (status) {
                latest.store(0);
                std::snprintf(message, sizeof(message), "%s", status);
            } else {
                uint32_t bits;
                std::memcpy(&bits, &health, sizeof(bits));
                latest.store((uint64_t{1} << 32) | bits);
                std::snprintf(message, sizeof(message), "Local player HP: %.2f", health);
            }
            if (std::strcmp(message, previous) != 0) {
                Log(message);
                std::strcpy(previous, message);
            }
        }
        break;
    }
    latest.store(0);
    if (attached) api.thread_detach(attached);
    Log("Health reader stopped");
    running.store(false);
    FreeLibraryAndExitThread(retained, 0);
    return 0;
}

extern "C" __declspec(dllexport) BOOL GetLatestHealth(float* result) {
    if (!result) return FALSE;
    uint64_t snapshot = latest.load();
    if (!(snapshot >> 32)) return FALSE;
    uint32_t bits = static_cast<uint32_t>(snapshot);
    std::memcpy(result, &bits, sizeof(bits));
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL StopHealthReader() {
    if (!stop_event || !worker) return FALSE;
    SetEvent(stop_event);
    return WaitForSingleObject(worker, 5000) == WAIT_OBJECT_0;
}

extern "C" __declspec(dllexport) BOOL IsHealthReaderRunning() {
    return running.load() ? TRUE : FALSE;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event) return FALSE;
        worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (!worker) { CloseHandle(stop_event); stop_event = nullptr; return FALSE; }
    } else if (reason == DLL_PROCESS_DETACH) {
        // Never wait for a worker while holding the loader lock.
        if (stop_event) SetEvent(stop_event);
        if (worker) CloseHandle(worker);
        if (stop_event) CloseHandle(stop_event);
    }
    (void)module;
    return TRUE;
}
