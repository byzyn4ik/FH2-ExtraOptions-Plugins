// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
#include "attach.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include "common.h"
#include "monitor.h"

namespace perf {
namespace attach {

namespace {

volatile LONG started;
HANDLE done_event;
volatile LONG result_ok;
std::string result_text;

template <class T> void release(T *&object) {
    if (object) {
        object->Release();
        object = nullptr;
    }
}

const wchar_t kProbeClass[] = L"FH2PerformanceProbe";

HMODULE self_module() {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)(void *)&self_module, &module);
    return module;
}

DWORD WINAPI attach_thread(void *) {
    std::string text;
    bool ok = run(&text);
    result_text = text;
    InterlockedExchange(&result_ok, ok ? 1 : 0);
    say(text);
    SetEvent(done_event);
    return 0;
}

std::string adapter_text(ID3D12Device *device) {
    IDXGIFactory4 *factory{};
    IDXGIAdapter1 *adapter{};
    std::string name = "?";
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)))) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))) name = narrow(desc.Description);
    }
    release(adapter);
    release(factory);
    return name;
}

} // namespace

bool run(std::string *report) {
    monitor::Internal scope; // the probe objects are not the game's
    std::string hooked, failed;
    auto fail = [&](const std::string &what, HRESULT hr) {
        failed += (failed.empty() ? "" : "; ") + what + " (" + hex((uint32_t)hr) + ")";
    };
    // Callbacks into this DLL stay in DXGI's and D3D12's vtables: keep it loaded.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)(void *)&run, &pinned)) {
        if (report) *report = "the plugin could not keep itself loaded; nothing is hooked";
        return false;
    }
    IDXGIFactory2 *factory{};
    ID3D12Device *device{};
    ID3D12CommandQueue *queue{};
    IDXGISwapChain1 *chain{};
    HWND window = nullptr;
    bool present_hooked = false, warp = false;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        fail("CreateDXGIFactory1", hr);
    } else {
        if (!monitor::hook_factory(factory, &hooked)) failed += failed.empty() ? "factory slots" : "; factory slots";
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr)) {
            IDXGIFactory4 *four{};
            IDXGIAdapter *adapter{};
            if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&four))) &&
                SUCCEEDED(four->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))
                hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
            warp = SUCCEEDED(hr);
            release(adapter);
            release(four);
        }
        if (FAILED(hr)) fail("D3D12CreateDevice", hr);
    }
    if (device) {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue));
        if (FAILED(hr))
            fail("CreateCommandQueue", hr);
        else if (!monitor::hook_queue(queue, &hooked))
            failed += failed.empty() ? "queue slot" : "; queue slot";
    }
    if (queue) {
        WNDCLASSEXW type{};
        type.cbSize = sizeof(type);
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = self_module();
        type.lpszClassName = kProbeClass;
        ATOM atom = RegisterClassExW(&type);
        if (atom || GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
            window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kProbeClass, L"", WS_POPUP, 0, 0, 64, 64,
                                     nullptr, nullptr, type.hInstance, nullptr);
        if (!window) {
            fail("CreateWindowExW", HRESULT_FROM_WIN32(GetLastError()));
        } else {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = 64;
            desc.Height = 64;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.Scaling = DXGI_SCALING_STRETCH;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            desc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
            hr = factory->CreateSwapChainForHwnd(queue, window, &desc, nullptr, nullptr, &chain);
            if (FAILED(hr))
                fail("CreateSwapChainForHwnd", hr);
            else if (!(present_hooked = monitor::hook_swap_chain(chain, &hooked)))
                failed += failed.empty() ? "swap chain slots" : "; swap chain slots";
        }
    }
    std::string adapter = device ? adapter_text(device) : std::string();
    release(chain);
    release(queue);
    release(device);
    release(factory);
    if (window) DestroyWindow(window);
    UnregisterClassW(kProbeClass, self_module());
    if (report) {
        std::string text = present_hooked ? "attached: " : "not attached: ";
        text += hooked.empty() ? std::string("nothing hooked") : "hooked " + hooked;
        if (!adapter.empty()) text += "; probe device on " + adapter + (warp ? " (WARP)" : "");
        if (!failed.empty()) text += "; failed: " + failed;
        if (present_hooked) text += "; the game's swap chain is measured from its next Present";
        *report = text;
    }
    return present_hooked;
}

void start() {
    if (InterlockedCompareExchange(&started, 1, 0)) return;
    done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE thread = CreateThread(nullptr, 0, attach_thread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    } else {
        result_text = "the attach thread could not start (error " + std::to_string(GetLastError()) + ")";
        say(result_text);
        if (done_event) SetEvent(done_event);
    }
}

bool wait(unsigned milliseconds) {
    return started && done_event && WaitForSingleObject(done_event, milliseconds) == WAIT_OBJECT_0;
}

bool succeeded() { return wait(0) && result_ok; }

const char *report() { return wait(0) ? result_text.c_str() : "the attach has not ended"; }

} // namespace attach
} // namespace perf
