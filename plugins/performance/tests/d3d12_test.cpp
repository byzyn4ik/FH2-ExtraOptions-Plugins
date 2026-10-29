// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// In-process D3D12 test of the plugin's late attach, hooks and overlay. Never
// runs the game: it makes its own device, queue and flip-model swap chains on
// hidden windows and drives the test build (PerformanceTest.dll) as the Extra
// Options loader does.
//
//   d3d12_test <late|early> <PerformanceTest.dll> <work folder> [--warp]
//
//   late   the device, the swap chain and another module's Present hook exist
//          first; then FH2PluginInitialize (the plugin attaches late); the
//          other hook is kept in the chain, the plugin draws the counter
//          before it, a hook added after the plugin is chained too; ShowFps
//          changes in the INI and from the Video page row apply at once;
//          swap chains made after the attach (factory hook) and the choice of
//          the measured chain (Azimuth's game window, the newest, idle).
//   early  FH2PluginInitialize first (the plugin makes its own probe device);
//          the device and the swap chain after it (factory hook).
// Both: the FPS is that of the test's own frames, the GPU frame time is
// measured, the CSV is written (PerfLog=1), no D3D12 error (the D3D12 debug
// layer, when Windows' optional "Graphics Tools" feature is installed; it is
// enabled before any device) and the device is not removed.
// Output: "  ok   ..." / "  FAIL ..." / "  skip ..." lines; exit code 1 on
// any failure. A line "SKIP-D3D12 ...": no D3D12 device and flip-model swap chain here.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fh2_plugin.h"

namespace {

int failures;

void check(bool ok, const std::string &what) {
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    fflush(stdout);
    if (!ok) ++failures;
}
void skip(const std::string &what) {
    printf("  skip %s\n", what.c_str());
    fflush(stdout);
}

// The plugin's statistics (src/monitor.h, monitor::Stats).
struct Stats {
    uint32_t size, presents, measured, chains, bound_serial, overlays, gpu_samples, csv_lines, gpu_failed, show,
        ini_reloads, created;
    double fps, gpu_ms;
    char text[4][32];
};
using StatsFn = void (*)(Stats *);
using WaitFn = int (*)(unsigned);
using ReportFn = const char *(*)();
using RowFn = void (*)(unsigned);
StatsFn plugin_stats;
WaitFn wait_attach;
ReportFn attach_report;
RowFn row_changed;

Stats stats() {
    Stats s{};
    s.size = sizeof(s);
    plugin_stats(&s);
    return s;
}

SRWLOCK log_lock = SRWLOCK_INIT;
std::string log_text;
void log_line(const char *line) {
    AcquireSRWLockExclusive(&log_lock);
    log_text += line;
    printf("    log: %s", line);
    fflush(stdout);
    ReleaseSRWLockExclusive(&log_lock);
}
bool logged(const char *text) {
    AcquireSRWLockExclusive(&log_lock);
    bool found = log_text.find(text) != std::string::npos;
    ReleaseSRWLockExclusive(&log_lock);
    return found;
}

std::string narrow(const std::wstring &text) {
    std::string out;
    for (wchar_t c : text) out += c < 128 ? (char)c : '?';
    return out;
}

std::string read_file(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::string();
    std::string text;
    char buffer[65536];
    DWORD got;
    while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got) text.append(buffer, got);
    CloseHandle(file);
    return text;
}

void write_file(const std::wstring &path, const std::string &text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written;
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, text.data(), (DWORD)text.size(), &written, nullptr);
        CloseHandle(file);
    }
}

// ------------------------------------------------------------- the scene

constexpr UINT kWidth = 640, kHeight = 360;
const float kClear[4] = {0.2f, 0.4f, 0.8f, 1.f};
const unsigned char kClearBgr[3] = {204, 102, 51}; // B, G, R as B8G8R8A8_UNORM stores them

IDXGIFactory4 *factory;
ID3D12Device *device;
ID3D12CommandQueue *queue;
ID3D12Fence *fence;
UINT64 fence_value;
HANDLE fence_event;
ID3D12CommandAllocator *allocator;
ID3D12GraphicsCommandList *list;
ID3D12DescriptorHeap *rtv_heap;
ID3D12Resource *readback;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
bool debug_layer, warp;

void wait_gpu() {
    queue->Signal(fence, ++fence_value);
    if (fence->GetCompletedValue() < fence_value) {
        fence->SetEventOnCompletion(fence_value, fence_event);
        WaitForSingleObject(fence_event, 5000);
    }
}

HWND make_window(const wchar_t *type_name) {
    WNDCLASSEXW type{};
    type.cbSize = sizeof(type);
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = type_name;
    RegisterClassExW(&type);
    return CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, type_name, L"test", WS_POPUP, 0, 0, kWidth, kHeight,
                           nullptr, nullptr, type.hInstance, nullptr);
}

IDXGISwapChain3 *make_chain(HWND window, DXGI_SWAP_EFFECT effect) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.SwapEffect = effect;
    IDXGISwapChain1 *one{};
    IDXGISwapChain3 *third{};
    HRESULT hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &one);
    if (FAILED(hr)) {
        printf("  CreateSwapChainForHwnd failed: 0x%08lx\n", (unsigned long)hr);
        return nullptr;
    }
    one->QueryInterface(IID_PPV_ARGS(&third));
    one->Release();
    return third;
}

// Clears the current back buffer and presents it.
HRESULT frame(IDXGISwapChain3 *chain) {
    ID3D12Resource *buffer{};
    chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer));
    allocator->Reset();
    list->Reset(allocator, nullptr);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(buffer, nullptr, rtv);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {buffer, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                    D3D12_RESOURCE_STATE_RENDER_TARGET};
    list->ResourceBarrier(1, &b);
    list->ClearRenderTargetView(rtv, kClear, 0, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList *lists[] = {list};
    queue->ExecuteCommandLists(1, lists);
    buffer->Release();
    HRESULT hr = chain->Present(0, 0);
    wait_gpu();
    return hr;
}

// ------------------------------------------- other modules' Present hooks

using PresentFn = HRESULT(STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT, UINT);
struct ForeignHook {
    PresentFn original;
    volatile LONG calls;
};
ForeignHook inner, outer; // inner: installed before the plugin; outer: after it
volatile LONG capture_wanted;
std::vector<unsigned char> captured; // B8G8R8A8 rows of footprint.Footprint.RowPitch

// Copies the current back buffer (on the presenting queue) and waits for it.
void capture(IDXGISwapChain *self) {
    IDXGISwapChain3 *chain{};
    ID3D12Resource *buffer{};
    if (FAILED(self->QueryInterface(IID_PPV_ARGS(&chain)))) return;
    chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer));
    chain->Release();
    ID3D12CommandAllocator *a{};
    ID3D12GraphicsCommandList *l{};
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&l));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {buffer, 0, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE};
    l->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION from{}, to{};
    from.pResource = buffer;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = readback;
    to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint = footprint;
    l->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    l->ResourceBarrier(1, &b);
    l->Close();
    ID3D12CommandList *lists[] = {l};
    queue->ExecuteCommandLists(1, lists);
    wait_gpu();
    void *mapped{};
    D3D12_RANGE range{0, (SIZE_T)footprint.Footprint.RowPitch * kHeight};
    if (SUCCEEDED(readback->Map(0, &range, &mapped))) {
        captured.assign((unsigned char *)mapped, (unsigned char *)mapped + range.End);
        D3D12_RANGE none{0, 0};
        readback->Unmap(0, &none);
    }
    l->Release();
    a->Release();
    buffer->Release();
}

HRESULT STDMETHODCALLTYPE inner_present(IDXGISwapChain *self, UINT sync, UINT flags) {
    InterlockedIncrement(&inner.calls);
    if (InterlockedExchange(&capture_wanted, 0)) capture(self);
    return inner.original(self, sync, flags);
}
HRESULT STDMETHODCALLTYPE outer_present(IDXGISwapChain *self, UINT sync, UINT flags) {
    InterlockedIncrement(&outer.calls);
    return outer.original(self, sync, flags);
}

void install_foreign(IDXGISwapChain *chain, ForeignHook &hook, PresentFn callback) {
    void **table = *(void ***)chain;
    DWORD old;
    VirtualProtect(table + 8, sizeof(void *), PAGE_READWRITE, &old);
    hook.original = (PresentFn)table[8];
    table[8] = (void *)callback;
    VirtualProtect(table + 8, sizeof(void *), old, &old);
}

// The counter in the top-right corner: yellow glyph pixels and darkened
// background there, the clear colour elsewhere.
struct Pixels {
    unsigned yellow, clear_in_corner, total;
    bool clear_elsewhere;
};
bool is_clear(const unsigned char *p) {
    for (int i = 0; i < 3; ++i)
        if (std::abs((int)p[i] - (int)kClearBgr[i]) > 1) return false;
    return true;
}
Pixels inspect() {
    Pixels r{};
    if (captured.empty()) return r;
    const UINT pitch = footprint.Footprint.RowPitch;
    auto at = [&](UINT x, UINT y) { return &captured[(size_t)y * pitch + x * 4]; };
    // The box: right edge at width-8, top at 8 (scale 1 below 1080 lines),
    // at most 24 columns of 6 px and 3 lines of 9 px.
    for (UINT y = 8; y < 44; ++y)
        for (UINT x = kWidth - 160; x < kWidth - 8; ++x) {
            const unsigned char *p = at(x, y);
            if (p[2] > 200 && p[1] > 170 && p[0] < 120) ++r.yellow;
        }
    // Its padding (always background) in the top-right corner.
    for (UINT y = 8; y < 10; ++y)
        for (UINT x = kWidth - 14; x < kWidth - 8; ++x) {
            ++r.total;
            if (is_clear(at(x, y))) ++r.clear_in_corner;
        }
    if (getenv("PERF_TEST_DUMP")) {
        for (UINT y = 6; y <= 40; ++y) {
            for (UINT x = kWidth - 160; x < kWidth - 4; ++x) {
                const unsigned char *p = at(x, y);
                putchar(is_clear(p) ? '.' : (p[2] > 200 && p[1] > 170 && p[0] < 120) ? '#' : 'o');
            }
            putchar('\n');
        }
    }
    r.clear_elsewhere = is_clear(at(20, kHeight - 20)) && is_clear(at(kWidth - 10, kHeight - 60)) &&
                        is_clear(at(kWidth / 2, kHeight / 2)) && is_clear(at(5, 5)) &&
                        is_clear(at(kWidth - 4, 20)) && is_clear(at(kWidth - 100, 60));
    return r;
}

Pixels capture_frame(IDXGISwapChain3 *chain) {
    captured.clear();
    InterlockedExchange(&capture_wanted, 1);
    frame(chain);
    return inspect();
}

// Presents for `seconds` at about 1000/(pace+~2) frames a second; returns the test's own FPS.
double run_for(IDXGISwapChain3 *chain, double seconds, DWORD pace = 8, unsigned *count = nullptr) {
    LARGE_INTEGER f, start, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&start);
    unsigned frames = 0;
    LONGLONG last_half = 0;
    std::vector<LONGLONG> times;
    do {
        frame(chain);
        QueryPerformanceCounter(&now);
        times.push_back(now.QuadPart);
        ++frames;
        Sleep(pace);
    } while (double(now.QuadPart - start.QuadPart) / f.QuadPart < seconds);
    if (count) *count = frames;
    // Over the last half second, as the plugin measures.
    LONGLONG end = times.back(), from = end - f.QuadPart / 2;
    unsigned n = 0;
    LONGLONG first = 0;
    for (LONGLONG t : times)
        if (t > from) {
            if (!n) first = t;
            ++n;
        }
    (void)last_half;
    return n > 1 ? (n - 1) * double(f.QuadPart) / double(end - first) : 0;
}

// Before any device (the plugin's probe included), when the debug layer is there.
void enable_debug_layer() {
    ID3D12Debug *debug{};
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
        debug->Release();
        debug_layer = true;
    }
}

bool make_device(bool force_warp) {
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
    HRESULT hr = E_FAIL;
    if (!force_warp) hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        IDXGIAdapter *adapter{};
        if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) {
            hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            adapter->Release();
            warp = SUCCEEDED(hr);
        }
    }
    if (FAILED(hr)) return false;
    IDXGIAdapter1 *adapter{};
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
        adapter->GetDesc1(&desc);
        adapter->Release();
    }
    printf("  device on %s%s; D3D12 debug layer %s\n", narrow(desc.Description).c_str(), warp ? " (WARP)" : "",
           debug_layer ? "on" : "not installed");
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list));
    list->Close();
    D3D12_DESCRIPTOR_HEAP_DESC h{};
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    h.NumDescriptors = 1;
    device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&rtv_heap));
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = kWidth;
    texture.Height = kHeight;
    texture.DepthOrArraySize = 1;
    texture.MipLevels = 1;
    texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    texture.SampleDesc.Count = 1;
    UINT64 total = 0;
    device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&readback)));
}

// Whether a D3D12 device (hardware or WARP) and a flip-model swap chain can be
// made in this session at all; all released again.
bool d3d12_works() {
    IDXGIFactory4 *f{};
    ID3D12Device *d{};
    ID3D12CommandQueue *q{};
    IDXGISwapChain1 *c{};
    IDXGIAdapter *warp_adapter{};
    bool ok = false;
    if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)))) {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d))) &&
            SUCCEEDED(f->EnumWarpAdapter(IID_PPV_ARGS(&warp_adapter))))
            D3D12CreateDevice(warp_adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d));
        D3D12_COMMAND_QUEUE_DESC desc{};
        HWND window = make_window(L"PerfTestCheck");
        if (d && window && SUCCEEDED(d->CreateCommandQueue(&desc, IID_PPV_ARGS(&q)))) {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.Width = 64;
            sd.Height = 64;
            sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd.SampleDesc.Count = 1;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.BufferCount = 2;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ok = SUCCEEDED(f->CreateSwapChainForHwnd(q, window, &sd, nullptr, nullptr, &c));
        }
        if (window) DestroyWindow(window);
    }
    for (IUnknown *u : {(IUnknown *)c, (IUnknown *)q, (IUnknown *)d, (IUnknown *)warp_adapter, (IUnknown *)f})
        if (u) u->Release();
    return ok;
}

// ------------------------------------------------------------- the plugin

HMODULE plugin;
std::wstring work, ini, csv;
FH2PluginContext context;

bool initialize_plugin(const char *ini_text) {
    write_file(ini, ini_text);
    DeleteFileW(csv.c_str());
    static std::wstring path;
    path = work + L"\\Performance.dll";
    const unsigned char *exe = (const unsigned char *)GetModuleHandleW(nullptr);
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    context.size = sizeof(context);
    context.api_version = FH2_PLUGIN_API_VERSION;
    context.adapter = FH2_PLUGIN_AZIMUTH;
    context.game_image = (void *)exe; // not the game: the Video page row must refuse it
    context.game_image_size = nt->OptionalHeader.SizeOfImage;
    context.plugin_module = plugin;
    context.plugins_directory = work.c_str();
    context.plugin_path = path.c_str();
    context.extra_options_version = "0.4.4";
    context.log_utf8 = log_line;
    auto initialize = (FH2PluginInitializeFn)(void *)GetProcAddress(plugin, "FH2PluginInitialize");
    if (!initialize || !initialize(&context)) return false;
    int attached = wait_attach(15000);
    printf("  attach: %s\n", attach_report());
    return attached == 1;
}

void check_debug_messages(const char *label) {
    if (!debug_layer) {
        skip(std::string(label) + ": D3D12 debug layer messages (the debug layer is not installed here)");
        return;
    }
    ID3D12InfoQueue *info{};
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&info)))) {
        skip(std::string(label) + ": no ID3D12InfoQueue");
        return;
    }
    unsigned errors = 0;
    UINT64 count = info->GetNumStoredMessages();
    for (UINT64 i = 0; i < count; ++i) {
        SIZE_T size = 0;
        info->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size);
        auto *message = (D3D12_MESSAGE *)bytes.data();
        if (SUCCEEDED(info->GetMessage(i, message, &size)) &&
            (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR || message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
            ++errors;
            printf("    D3D12: %s\n", message->pDescription);
        }
    }
    info->Release();
    check(errors == 0, std::string(label) + ": no D3D12 debug layer errors (" + std::to_string(count) + " messages)");
}

std::vector<std::string> csv_lines() {
    std::vector<std::string> lines;
    std::string text = read_file(csv);
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find("\r\n", at);
        if (end == std::string::npos) end = text.size();
        lines.push_back(text.substr(at, end - at));
        at = end + 2;
    }
    return lines;
}

void check_common(const char *label, double own_fps, bool row) {
    Stats s = stats();
    check(s.fps > own_fps * 0.75 && s.fps < own_fps * 1.33,
          std::string(label) + ": the plugin's FPS " + std::to_string(s.fps) + " is the test's own " +
              std::to_string(own_fps));
    check(s.gpu_failed == 0, std::string(label) + ": the GPU part stays on");
    check(s.gpu_samples > 0 && s.gpu_ms > 0 && s.gpu_ms < 100,
          std::string(label) + ": GPU frame times measured (" + std::to_string(s.gpu_samples) + ", " +
              std::to_string(s.gpu_ms) + " ms)");
    check(s.csv_lines >= 1, std::string(label) + ": PerfLog=1 writes the log (" + std::to_string(s.csv_lines) +
                                " lines)");
    std::vector<std::string> lines = csv_lines();
    check(lines.size() >= 2 && lines[0].rfind("time,session,elapsed_s,interval_s,frames,avg_fps", 0) == 0,
          std::string(label) + ": " + narrow(csv) + " has the header and data lines (" +
              std::to_string(lines.size()) + ")");
    if (lines.size() >= 2) {
        // time,session,elapsed_s,interval_s,frames,avg_fps,...
        std::vector<std::string> fields;
        size_t at = 0, comma;
        const std::string &last = lines.back();
        while ((comma = last.find(',', at)) != std::string::npos) {
            fields.push_back(last.substr(at, comma - at));
            at = comma + 1;
        }
        fields.push_back(last.substr(at));
        double fps = fields.size() == 13 ? atof(fields[5].c_str()) : 0;
        check(fields.size() == 13 && fps > own_fps * 0.6 && fps < own_fps * 1.6,
              std::string(label) + ": the log's avg_fps " + std::to_string(fps) + " (13 columns: " +
                  std::to_string(fields.size()) + ")");
    }
    if (row)
        check(logged("is not the supported size (0x3a18000); no Video page row"),
              std::string(label) + ": the Video page row refuses an image that is not the game's");
    else
        check(!logged("Video page row ready") && !logged("no Video page row"),
              std::string(label) + ": MenuRow=0 leaves the Video page alone");
}

int late() {
    HWND window = make_window(L"PerfTestWindow");
    IDXGISwapChain3 *chain = make_chain(window, DXGI_SWAP_EFFECT_FLIP_DISCARD);
    if (!chain) return 1;
    install_foreign(chain, inner, inner_present);
    for (int i = 0; i < 10; ++i) frame(chain);
    check(inner.calls == 10, "another module's Present hook sees the frames before the plugin");

    check(initialize_plugin("[Performance]\r\nShowFps=2\r\nPerfLog=1\r\nMenuRow=1\r\n"),
          "FH2PluginInitialize after the swap chain exists: the late attach hooks Present");
    check(logged("chained onto") || logged("Present (dxgi.dll)"), "the log names the hooked slots");
    Stats before = stats();
    check(before.presents == 0 && before.chains == 0, "nothing measured before the next Present");
    LONG inner_before = inner.calls;
    unsigned frames = 0;
    double own = run_for(chain, 1.6, 8, &frames);
    Stats s = stats();
    check(s.presents == frames && s.chains == 1 && s.bound_serial == 1,
          "the plugin sees every Present of the swap chain made before it (" + std::to_string(s.presents) + "/" +
              std::to_string(frames) + ")");
    check(inner.calls - inner_before == (LONG)frames, "the earlier hook stays in the chain (called for every frame)");
    check(logged("found a swap chain made before the attach"), "the log tells the swap chain was found late");
    check(s.overlays * 2 >= frames, "the counter is drawn on the frames from the first half second on (" +
                                       std::to_string(s.overlays) + " of " + std::to_string(frames) + ")");
    check(std::string(s.text[0]).find(" FPS ") != std::string::npos &&
              std::string(s.text[1]).rfind("1% LOW", 0) == 0,
          std::string("ShowFps=2 shows the detailed lines (\"") + s.text[0] + "\", \"" + s.text[1] + "\", \"" +
              s.text[2] + "\")");
    check_common("late", own, true);
    Pixels p = capture_frame(chain);
    check(p.total && p.yellow >= 40 && p.clear_in_corner == 0,
          "the back buffer has the counter in the top-right corner when the next hook gets it (" +
              std::to_string(p.yellow) + " glyph pixels; its background over " + std::to_string(p.total) + " pixels)");
    check(p.clear_elsewhere, "the rest of the frame is untouched");

    // A hook added after the plugin (another overlay starting later).
    install_foreign(chain, outer, outer_present);
    LONG a = inner.calls, b = outer.calls;
    uint32_t seen = stats().presents;
    for (int i = 0; i < 5; ++i) frame(chain);
    check(outer.calls - b == 5 && inner.calls - a == 5 && stats().presents - seen == 5,
          "a hook added after the plugin chains onto it: all three see every frame");

    // ShowFps=0 written to the INI: off at once.
    Sleep(50);
    WritePrivateProfileStringW(L"Performance", L"ShowFps", L"0", ini.c_str());
    run_for(chain, 1.3);
    s = stats();
    check(s.show == 0 && s.ini_reloads >= 1, "ShowFps=0 written to the INI applies within a second");
    p = capture_frame(chain);
    check(p.total && p.yellow == 0 && p.clear_in_corner == p.total && p.clear_elsewhere,
          "with ShowFps=0 nothing is drawn");
    uint32_t csv_before = s.csv_lines;

    // The Video page row: On.
    row_changed(1);
    wchar_t value[8];
    GetPrivateProfileStringW(L"Performance", L"ShowFps", L"", value, 8, ini.c_str());
    run_for(chain, 1.2);
    s = stats();
    check(s.show == 1 && !wcscmp(value, L"1"), "the Video page row's On writes ShowFps=1 and applies at once");
    check(std::string(s.text[0]).find(" FPS GPU ") != std::string::npos && s.text[2][0] == 0,
          std::string("ShowFps=1 shows the short lines (\"") + s.text[0] + "\", \"" + s.text[1] + "\")");
    p = capture_frame(chain);
    check(p.yellow >= 20 && p.clear_in_corner == 0 && p.clear_elsewhere, "and the counter is drawn again");
    check(s.csv_lines > csv_before, "the log goes on meanwhile (" + std::to_string(csv_before) + " -> " + std::to_string(s.csv_lines) + " lines)");

    // Swap chains made after the attach; Azimuth's game window is measured first.
    HWND game_window = make_window(L"Azimuth.Game.Win32");
    IDXGISwapChain3 *game = make_chain(game_window, DXGI_SWAP_EFFECT_FLIP_DISCARD);
    s = stats();
    check(game && s.created == 1 && s.chains == 2, "a swap chain made after the attach comes through the factory hook");
    for (int i = 0; i < 20; ++i) {
        frame(chain);
        frame(game);
    }
    check(stats().bound_serial == 2, "Azimuth's game window is measured while both present");
    HWND other_window = make_window(L"PerfTestOther");
    IDXGISwapChain3 *other = make_chain(other_window, DXGI_SWAP_EFFECT_FLIP_DISCARD);
    for (int i = 0; i < 20; ++i) {
        frame(chain);
        frame(other);
        frame(game);
    }
    check(stats().bound_serial == 2 && stats().chains == 3, "a newer swap chain does not take over from the game window");
    for (int i = 0; i < 20; ++i) frame(other);
    run_for(other, 1.2);
    check(stats().bound_serial == 3, "once the game window is idle a second, the presenting chain is measured");
    for (int i = 0; i < 5; ++i) frame(game);
    check(stats().bound_serial == 2, "the game window takes over again at once");
    check(stats().gpu_failed == 0, "the GPU part follows the chains without failing");

    wait_gpu();
    check_debug_messages("late");
    check(device->GetDeviceRemovedReason() == S_OK, "late: the device is not removed");
    for (IDXGISwapChain3 *c : {chain, game, other})
        if (c) c->Release();
    return 0;
}

int early(bool force_warp) {
    check(initialize_plugin("[Performance]\r\nShowFps=1\r\nPerfLog=1\r\nMenuRow=0\r\n"),
          "FH2PluginInitialize before any device: the attach makes its own probe device");
    check(logged("probe device on"), "the log names the probe's adapter");
    if (!make_device(force_warp)) {
        check(false, "a device after the attach");
        return 1;
    }
    HWND window = make_window(L"PerfTestWindow");
    IDXGISwapChain3 *chain = make_chain(window, DXGI_SWAP_EFFECT_FLIP_DISCARD);
    if (!chain) return 1;
    Stats s = stats();
    check(s.created == 1 && s.chains == 1, "the swap chain made after the attach comes through the factory hook");
    install_foreign(chain, inner, inner_present); // this time after the plugin: it runs before the plugin's hook
    unsigned frames = 0;
    double own = run_for(chain, 1.6, 8, &frames);
    s = stats();
    check(s.presents == frames && inner.calls == (LONG)frames && s.bound_serial == 1,
          "the plugin and a later hook see every Present (" + std::to_string(s.presents) + "/" +
              std::to_string(frames) + ")");
    check(!logged("found a swap chain made before the attach"), "the swap chain was known from its creation");
    check(s.overlays * 2 >= frames, "the counter is drawn from the first half second on (" +
                                       std::to_string(s.overlays) + " of " + std::to_string(frames) + ")");
    check(std::string(s.text[0]).find(" FPS GPU ") != std::string::npos,
          std::string("ShowFps=1: \"") + s.text[0] + "\"");
    check_common("early", own, false);
    wait_gpu();
    check_debug_messages("early");
    check(device->GetDeviceRemovedReason() == S_OK, "early: the device is not removed");
    chain->Release();
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc < 4) {
        printf("usage: d3d12_test <late|early> <PerformanceTest.dll> <work folder> [--warp]\n");
        return 2;
    }
    std::wstring mode = argv[1];
    enable_debug_layer();
    bool force_warp = argc > 4 && !wcscmp(argv[4], L"--warp");
    work = argv[3];
    ini = work + L"\\Performance.ini";
    csv = work + L"\\fh2-perf.csv";
    plugin = LoadLibraryW(argv[2]);
    if (!plugin) {
        printf("  FAIL the test DLL does not load (%lu)\n", GetLastError());
        return 1;
    }
    plugin_stats = (StatsFn)(void *)GetProcAddress(plugin, "PerfTestStats");
    wait_attach = (WaitFn)(void *)GetProcAddress(plugin, "PerfTestWaitAttach");
    attach_report = (ReportFn)(void *)GetProcAddress(plugin, "PerfTestReport");
    row_changed = (RowFn)(void *)GetProcAddress(plugin, "PerfTestRowChanged");
    if (!plugin_stats || !wait_attach || !attach_report || !row_changed) {
        printf("  FAIL the test DLL lacks the test entry points\n");
        return 1;
    }
    if (mode == L"late") {
        if (!d3d12_works() || !make_device(force_warp)) {
            printf("SKIP-D3D12 (no D3D12 device and flip-model swap chain here)\n");
            return 0;
        }
        late();
    } else if (mode == L"early") {
        // No D3D12 device or swap chain here at all (a CI runner): the
        // plugin's attach would fail too; say so instead. Everything the
        // check makes is released before the plugin starts.
        if (!d3d12_works()) {
            printf("SKIP-D3D12 (no D3D12 device and flip-model swap chain here)\n");
            return 0;
        }
        early(force_warp);
    } else {
        printf("unknown mode\n");
        return 2;
    }
    printf("  %d failure(s)\n", failures);
    fflush(stdout);
    // The plugin's hooks stay in DXGI's vtables until the process ends, as in the game.
    TerminateProcess(GetCurrentProcess(), failures ? 1 : 0);
    return failures ? 1 : 0;
}
