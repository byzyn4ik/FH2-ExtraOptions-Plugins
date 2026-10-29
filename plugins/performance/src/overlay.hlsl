// Performance for Forza Horizon 2 (Extra Options plugin).
// SPDX-License-Identifier: GPL-3.0-or-later
//
// The counter: one alpha-blended quad (a 4-vertex strip, no vertex buffer)
// whose pixel shader draws up to 96 glyphs of a 5x7 font from 40 root
// constants. build.py compiles "vs" (vs_5_0) and "ps" (ps_5_0) with
// d3dcompiler_47 into obj/overlay_shaders.h; nothing is compiled in the game.
cbuffer Overlay : register(b0) { float4 Rect; float4 Target; float4 Color; uint4 Layout; uint4 Text[6]; };
struct V { float4 position : SV_Position; };
V vs(uint id : SV_VertexID) {
    float2 pixel = Rect.xy + float2(id & 1, id >> 1) * Rect.zw;
    V o; o.position = float4(pixel.x / Target.x * 2 - 1, 1 - pixel.y / Target.y * 2, 0, 1); return o;
}
// 5x7 glyphs: rows 0-5 in x (5 bits each, leftmost column high), row 6 in y.
// Order: " 0123456789.%:/-ABCDEFGHIJKLMNOPQRSTUVWXYZ" (monitor.cpp, kGlyphs).
static const uint2 Font[42] = {
    {0x00000000,0x00},{0x239ace2e,0x0e},{0x08421184,0x0e},{0x1041062e,0x1f},{0x2211105f,0x0e},{0x05f928c2,0x02},
    {0x2210fa1f,0x0e},{0x231f4106,0x0e},{0x1082083f,0x08},{0x2317462e,0x0e},{0x0417c62e,0x0c},{0x18000000,0x0c},
    {0x26820b38,0x03},{0x18c03180,0x00},{0x20820820,0x00},{0x000f8000,0x00},{0x23f8c62e,0x11},{0x231f463e,0x1e},
    {0x2308422e,0x0e},{0x2518c65c,0x1c},{0x210f421f,0x1f},{0x210f421f,0x10},{0x231bc22e,0x0f},{0x231fc631,0x11},
    {0x0842108e,0x0e},{0x24210847,0x0c},{0x254c5251,0x11},{0x21084210,0x1f},{0x231ad771,0x11},{0x233ae631,0x11},
    {0x2318c62e,0x0e},{0x210f463e,0x10},{0x2558c62e,0x0d},{0x254f463e,0x11},{0x0217420f,0x1e},{0x0842109f,0x04},
    {0x2318c631,0x0e},{0x1518c631,0x04},{0x2b5ac631,0x0a},{0x22a22a31,0x11},{0x08454631,0x04},{0x2082083f,0x1f}};
float4 ps(V input) : SV_Target {
    float4 background = float4(0, 0, 0, Color.w);
    float2 p = input.position.xy - Rect.xy - Target.w;
    if (p.x < 0 || p.y < 0) return background;
    uint2 cell = uint2(p / Target.z);
    uint column = cell.x / 6, row = cell.y / 9, gx = cell.x % 6, gy = cell.y % 9;
    if (column >= Layout.x || row >= Layout.y || gx >= 5 || gy >= 7) return background;
    uint index = row * Layout.x + column;
    // Static indices only: dynamically indexed root constants misread on AMD.
    uint q = index / 16;
    uint4 packed = q == 0 ? Text[0] : q == 1 ? Text[1] : q == 2 ? Text[2] : q == 3 ? Text[3] : q == 4 ? Text[4] : Text[5];
    uint part = (index / 4) % 4;
    uint word = part == 0 ? packed.x : part == 1 ? packed.y : part == 2 ? packed.z : packed.w;
    uint glyph = min((word >> ((index % 4) * 8)) & 255, 41);
    uint2 bits = Font[glyph];
    uint on = gy < 6 ? (bits.x >> (gy * 5 + 4 - gx)) & 1 : (bits.y >> (4 - gx)) & 1;
    return on ? float4(Color.rgb, 1) : background;
}
