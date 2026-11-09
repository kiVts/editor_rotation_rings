
#include "editor_state.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "resolve.h"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

constexpr uintptr_t kLayoutLabels = 0x140723920;
constexpr uintptr_t kSetText = 0x1401dce80;
constexpr uintptr_t kStringReserve = 0x14017cbb0;
constexpr uintptr_t kFree = 0x1409b2d50;

// c_vehicle_editor_rotation_labels_ui fields.
constexpr uint32_t kVisible = 0x1c;                        // byte
constexpr uint32_t kCircle[3] = {0x50, 0x58, 0x60};        // per-axis composite (X, Y, Z)
constexpr uint32_t kRotateLabel[3] = {0x80, 0x88, 0x90};   // text, rotate key
constexpr uint32_t kFlipLabel[3] = {0x98, 0xa0, 0xa8};     // text, flip key

constexpr uint32_t kElemX = 0x8, kElemY = 0xc, kElemVisible = 0x1c;
constexpr uint32_t kChildren = 0x30, kChildCap = 0x38, kChildStart = 0x3c, kChildCount = 0x40;

struct GameString {
    char* data;
    uint32_t len;
    uint32_t cap;
};

using Layout_t = void(__fastcall*)(uint8_t* labels, void* ui, void* text_ctx, void* camera, void* transform);
using SetText_t = void(__fastcall*)(void* element, void* text_ctx, const GameString* text);
using Reserve_t = void(__fastcall*)(GameString* s, uint32_t capacity);
using Free_t = void(__fastcall*)(void* p);

Layout_t g_orig = nullptr;
SetText_t g_set_text = nullptr;
Reserve_t g_reserve = nullptr;
Free_t g_free = nullptr;

bool g_enabled = true;
bool g_hide_only = true; // [editor] rings=1 (default) -> hide the game overlay entirely
double g_orbit_rad_per_sec = 30.0 * 3.14159265358979 / 180.0;
float g_radius = 0.07f; // the game places its labels 0.07 from the circle centre
LARGE_INTEGER g_freq{}, g_t0{};

uint8_t* field_ptr(uint8_t* obj, uint32_t off) { return *reinterpret_cast<uint8_t**>(obj + off); }

void set_text(void* element, void* text_ctx, const char* text)
{
    GameString s{nullptr, 0, 0};
    uint32_t len = static_cast<uint32_t>(strlen(text));
    s.len = len;
    g_reserve(&s, len + 1);
    if (!s.data) return;
    memcpy(s.data, text, len + 1);
    g_set_text(element, text_ctx, &s);
    g_free(s.data);
}

void hide_icons(uint8_t* circle, const uint8_t* keep_a, const uint8_t* keep_b)
{
    auto** children = *reinterpret_cast<uint8_t***>(circle + kChildren);
    uint32_t cap = *reinterpret_cast<uint32_t*>(circle + kChildCap);
    uint32_t start = *reinterpret_cast<uint32_t*>(circle + kChildStart);
    uint32_t count = *reinterpret_cast<uint32_t*>(circle + kChildCount);
    if (!children || cap == 0 || count > cap) return;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* child = children[(start + i) % cap];
        if (child && child != keep_a && child != keep_b) child[kElemVisible] = 0;
    }
}

void __fastcall layout_detour(uint8_t* labels, void* ui, void* text_ctx, void* camera, void* transform)
{
    g_orig(labels, ui, text_ctx, camera, transform);
    if (!g_enabled) return;
    if (g_hide_only) { // stage 3 draws its own rings; the game's overlay and axis lines go away
        labels[kVisible] = 0;
        // The camera argument is editor + off_camera; clearing the hints flag stops the editor
        // render pass from drawing the red/green/blue direction lines this frame.
        auto* editor = static_cast<uint8_t*>(camera) - addrs().off_camera;
        editor[addrs().off_hints_flag] = 0;
        return;
    }

    bool show = editor_hold_active();
    labels[kVisible] = show ? 1 : 0;
    if (!show) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);

    static const char* kPairs[3][2] = {{"W", "S"}, {"A", "D"}, {"Q", "E"}};
    for (int axis = 0; axis < 3; axis++) {
        uint8_t* circle = field_ptr(labels, kCircle[axis]);
        uint8_t* first = field_ptr(labels, kRotateLabel[axis]);
        uint8_t* second = field_ptr(labels, kFlipLabel[axis]);
        if (!circle || !first || !second) continue;

        set_text(first, text_ctx, kPairs[axis][0]);
        set_text(second, text_ctx, kPairs[axis][1]);

        // Both labels on the circle, opposite each other; each axis starts at its own phase.
        double a = t * g_orbit_rad_per_sec + axis * (2.0 * 3.14159265358979 / 3.0);
        float c = static_cast<float>(cos(a)) * g_radius, s = static_cast<float>(sin(a)) * g_radius;
        *reinterpret_cast<float*>(first + kElemX) = c;
        *reinterpret_cast<float*>(first + kElemY) = s;
        *reinterpret_cast<float*>(second + kElemX) = -c;
        *reinterpret_cast<float*>(second + kElemY) = -s;
        first[kElemVisible] = 1;
        second[kElemVisible] = 1;

        hide_icons(circle, first, second);
    }
}

bool install()
{
    std::wstring ini = std::wstring(settings_ini());
    g_enabled = GetPrivateProfileIntW(L"editor", L"wheel", 1, ini.c_str()) != 0;
    g_hide_only = GetPrivateProfileIntW(L"editor", L"rings", 1, ini.c_str()) != 0;
    int deg = GetPrivateProfileIntW(L"editor", L"orbit_deg_per_sec", 30, ini.c_str());
    int radius_milli = GetPrivateProfileIntW(L"editor", L"orbit_radius_milli", 70, ini.c_str());
    g_orbit_rad_per_sec = deg * 3.14159265358979 / 180.0;
    g_radius = radius_milli / 1000.0f;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);

    g_set_text = game_ptr<SetText_t>(kSetText);
    g_reserve = game_ptr<Reserve_t>(kStringReserve);
    g_free = game_ptr<Free_t>(kFree);
    swlog("editor: rotation wheel %s (orbit %d deg/s, radius %.3f)", g_enabled ? "on" : "off", deg, g_radius);
    if (g_hide_only) {
        // Rings mode: only hides the game overlay, so the resolved layout function is enough.
        if (!addrs().editor_ok) return false;
        return hook_address(addrs().layout_fn, reinterpret_cast<void*>(&layout_detour), reinterpret_cast<void**>(&g_orig));
    }
    // Legacy stage-2 relabelling uses many fixed offsets: exact known build only.
    if (!game_build_matches()) {
        swlog("editor: stage-2 overlay relabel needs the known game build; skipped");
        return false;
    }
    return hook_game(kLayoutLabels, reinterpret_cast<void*>(&layout_detour), reinterpret_cast<void**>(&g_orig));
}

HookRegistrar reg{{"editor rotation wheel (rotation labels overlay)", /*needs_known_build=*/false, &install}};

} // namespace
