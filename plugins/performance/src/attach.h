// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The late attach. Extra Options runs plugins on its loader worker after its
// own hooks, usually after Azimuth has made its D3D12 device and swap chain,
// so the plugin cannot wait for those to be made. Instead it makes its own
// small objects on a thread of its own, takes the vtables from them and hooks
// the slots (monitor.h):
//   - a DXGI factory (CreateDXGIFactory1): CreateSwapChain and
//     CreateSwapChainForHwnd, for swap chains made later;
//   - a D3D12 device on the default adapter (WARP when there is no D3D12
//     GPU). D3D12 devices are one per adapter: when the game's device exists
//     this is the game's device and nothing new is made;
//   - a direct command queue: ExecuteCommandLists;
//   - a 64x64 flip-model swap chain on a hidden window of its own:
//     Present, Present1 and SetColorSpace1.
// All of them are released (and the window destroyed) before the thread ends;
// the vtables live in dxgi.dll and d3d12core.dll, which stay loaded. The
// game's own swap chain is the same DXGI class, so its next Present comes
// through the hook, whether it was made before or after the attach; its
// queue's vtable is hooked too when it is another class than the probe's.
#pragma once
#include <string>

namespace perf {
namespace attach {

// Starts the attach on its own thread, once; later calls do nothing.
void start();
// Waits for the attach to end; true when it ended in time.
bool wait(unsigned milliseconds);
// After the attach: whether Present is hooked, and what was done.
bool succeeded();
const char *report();

// The attach itself, on the calling thread (start() runs it on its thread).
bool run(std::string *report);

} // namespace attach
} // namespace perf
