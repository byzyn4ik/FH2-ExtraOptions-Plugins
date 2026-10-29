// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The FPS counter and the performance log (from Extra Options PR #106,
// fh2-monitor.dll), as one part of the plugin DLL.
//
// Hooks, all by vtable slot, each keeping what the slot held (dxgi.dll /
// d3d12core.dll, or another overlay's hook) and calling it:
//   IDXGIFactory::CreateSwapChain (10), IDXGIFactory2::CreateSwapChainForHwnd
//     (15): swap chains made after the attach, with their D3D12 queue;
//   IDXGISwapChain::Present (8), IDXGISwapChain1::Present1 (22): frame times,
//     the overlay drawn into the back buffer just before the real Present;
//   IDXGISwapChain3::SetColorSpace1 (38): overlay brightness on HDR;
//   ID3D12CommandQueue::ExecuteCommandLists (10): opens the GPU frame span.
// The vtables come from the attach (attach.h), which makes its own small
// D3D12 objects; swap chains that already exist are found at their next
// Present. DXGI does not tell such a swap chain's queue (GetDevice gives the
// device), so it is the direct queue of that device that submitted command
// lists on the presenting thread since that thread's previous Present.
//
// The counter is one alpha-blended quad in the top-right corner, drawn on the
// presenting queue. The GPU frame span comes from two timestamp queries: one
// before the first command list after a Present, one at the next Present. GPU
// load and video memory come from PDH on a worker thread. Any GPU failure
// turns off the overlay and the GPU time and keeps the CPU measurements.
#pragma once
#include <string>

#include "common.h"

struct IDXGIFactory;
struct IDXGISwapChain;
struct ID3D12CommandQueue;

namespace perf {

enum ShowMode : unsigned { kShowOff = 0, kShowFps = 1, kShowDetailed = 2 };

struct Options {
    unsigned show = kShowFps; // [Performance] ShowFps
    bool log = false;         // [Performance] PerfLog
    bool row = true;          // [Performance] MenuRow
    std::wstring csv;         // [Performance] PerfLogFile, resolved
    std::wstring ini;         // the plugin's INI (watched for changes)
};

// Reads the plugin INI; `folder` is the plugin's folder (the default CSV is
// <folder>\fh2-perf.csv). Values that are not valid keep their default and
// are listed in *warnings.
Options read_options(const std::wstring &ini, const std::wstring &folder, std::string *warnings);
// Writes [Performance] ShowFps (the Video page row); false on failure.
bool write_show(const std::wstring &ini, unsigned mode);

namespace monitor {

// Once, before any hook: the settings, the clock, the CSV.
void configure(const Options &options);
bool configured();

// Live changes (the Video page row, or the INI changing on disk).
void set_show(unsigned mode);
unsigned show();
void set_log(bool on);

// The hooks (attach.cpp installs them). Each returns false when the slot
// could not be written; *chained (when given) tells whether the slot held
// another module's code than the one the object came from.
bool hook_factory(IDXGIFactory *factory, std::string *note);
bool hook_swap_chain(IDXGISwapChain *chain, std::string *note);
bool hook_queue(ID3D12CommandQueue *queue, std::string *note);

// Set while the plugin makes its own DXGI/D3D12 objects: they are not
// measured, hooked as the game's or drawn on.
struct Internal {
    bool saved;
    Internal();
    ~Internal();
};

// What the monitor has seen, for the log and the tests.
struct Stats {
    uint32_t size;
    uint32_t presents;     // Present/Present1 calls seen (not counting DXGI_PRESENT_TEST)
    uint32_t measured;     // frames of the bound chain recorded
    uint32_t chains;       // swap chains known
    uint32_t bound_serial; // serial of the measured chain (1 = first known), 0 none
    uint32_t overlays;     // overlays drawn
    uint32_t gpu_samples;  // GPU frame spans measured
    uint32_t csv_lines;    // performance log lines written
    uint32_t gpu_failed;   // the GPU part gave up
    uint32_t show;         // current ShowFps
    uint32_t ini_reloads;  // INI changes picked up
    uint32_t created;      // swap chains seen through the factory hook
    double fps;            // over the last half second
    double gpu_ms;         // average GPU span over the last half second (0: none)
    char text[4][32];      // the overlay's lines
};
void stats(Stats *out);

} // namespace monitor
} // namespace perf
