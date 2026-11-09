
#include "editor_state.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "native_draw.h"
#include "resolve.h"
#include "sdk_v3.hpp"

#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

namespace {
namespace draw = swml4::draw;

// Offsets come from resolve.cpp; only the translation column inside the 4x4 transform is fixed.
constexpr uint32_t kPosition = 0x60;

constexpr double kPi = 3.14159265358979323846;

bool g_enabled = true;
double g_radius = 0.45; // metres
double g_orbit_rad_per_sec = 30.0 * kPi / 180.0;
LARGE_INTEGER g_freq{}, g_t0{};
uintptr_t g_vtable = 0;

struct Snapshot {
    double vp[16]; // as laid out in the camera: index = column * 4 + row
    double pos[3];
};

// Copies what we need from the editor; false if it isn't a live editor placing a part.
// SEH-guarded so a stale pointer (editor closed while the key is held) can't crash the game.
bool read_editor(const uint8_t* ed, Snapshot* out)
{
    __try {
        const GameAddrs& r = addrs();
        if (*reinterpret_cast<const uintptr_t*>(ed) != g_vtable) return false;
        if (*reinterpret_cast<const int*>(ed + r.off_mode_a) != 1 || *reinterpret_cast<const int*>(ed + r.off_mode_b) != 0)
            return false;
        const double* vp = reinterpret_cast<const double*>(ed + r.off_camera + r.off_viewproj);
        for (int i = 0; i < 16; i++) out->vp[i] = vp[i];
        const double* p = reinterpret_cast<const double*>(ed + r.off_transform + kPosition);
        out->pos[0] = p[0];
        out->pos[1] = p[1];
        out->pos[2] = p[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool project(const Snapshot& s, const double w3[3], float* nx, float* ny, double* depth)
{
    const double* m = s.vp;
    double x = w3[0] * m[0] + w3[1] * m[4] + w3[2] * m[8] + m[12];
    double y = w3[0] * m[1] + w3[1] * m[5] + w3[2] * m[9] + m[13];
    double w = w3[0] * m[3] + w3[1] * m[7] + w3[2] * m[11] + m[15];
    if (w < 1e-3) return false;
    *nx = static_cast<float>(x / w);
    *ny = static_cast<float>(y / w);
    *depth = w;
    return true;
}

// Each ring gets its own radius so they never sit on top of each other.
constexpr double kRadiusScale[3] = {1.0, 1.18, 1.36};

void ring_offset(int axis, double angle, double radius, double out[3])
{
    double c = cos(angle) * radius, n = sin(angle) * radius;
    out[0] = out[1] = out[2] = 0;
    switch (axis) {
    case 0: out[1] = c; out[2] = n; break; // around X: Y-Z plane
    case 1: out[0] = c; out[2] = n; break; // around Y: X-Z plane (flat)
    case 2: out[0] = c; out[1] = n; break; // around Z: X-Y plane
    }
}

constexpr draw::Color kColors[3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}}; // per key pair: W/S, A/D, Q/E
constexpr float kFarLine = .4f, kFarLabel = .8f; // brightness of what is on the ring's far half
constexpr float kLabelOutline = .002f;           // native UI units, about one pixel at 1080p

// --- labels: render pass -> native text ---------------------------------------------------------------------------
struct LabelState {
    bool visible = false, far_side = false;
    float x = 0, y = 0;
};
std::mutex g_label_mx;
LabelState g_label_state[3];
uint64_t g_label_at = 0;
// Native text keeps its colour from creation, so labels belong to a key pair (its colour) and move to whichever
// ring that pair turns: one bright and one dark label per pair, one of them shown.
swml_ui_handle g_root = 0, g_labels[3][2] = {};
swml3::ui::TextOutline g_outlines[3]; // the pure colour on both halves; only the text inside it darkens
char g_label_text[3][2][48] = {}, g_outline_text[3][48] = {};
uint64_t g_ui_updates = 0, g_ui_shown = 0, g_ui_other_root = 0, g_ui_log_at = 0;

// Native text colours are AABBGGRR, like every other native colour.
uint32_t native_color(draw::Color c)
{
    return 0xff000000u | (uint32_t(c.b) << 16) | (uint32_t(c.g) << 8) | c.r;
}

void interface_update(const swml_ui_event* event, void*)
{
    if (event->kind == SWML_UI_UNMOUNT) {
        if (event->root == g_root) {
            g_root = 0;
            std::memset(g_labels, 0, sizeof g_labels);
            std::memset(g_label_text, 0, sizeof g_label_text);
            std::memset(g_outline_text, 0, sizeof g_outline_text);
            for (auto& outline : g_outlines) outline.forget();
        }
        return;
    }
    if (event->kind != SWML_UI_MOUNT && event->kind != SWML_UI_UPDATE) return;
    // One root owns the labels until it goes away: a second instance must not leave orphans behind.
    if (g_root && event->root != g_root) {
        g_ui_other_root++;
        return;
    }
    if (!g_root) {
        g_root = event->root;
        std::memset(g_label_text, 0, sizeof g_label_text);
        std::memset(g_outline_text, 0, sizeof g_outline_text);
        for (int pair = 0; pair < 3; pair++) {
            auto d = swml3::ui::description(SWML_UI_LABEL, {0, 0, .3f, .05f, SWML_UI_CENTER}, "");
            d.visible = 0;
            d.foreground = native_color(kColors[pair]);
            g_outlines[pair].create(event->root, d, kLabelOutline); // first, so it draws underneath
            for (int side = 0; side < 2; side++) {
                d.foreground = native_color(side ? draw::dim(kColors[pair], kFarLabel) : kColors[pair]);
                if (swml3::ui::create(event->root, d, g_labels[pair][side]) != SWML_OK) g_labels[pair][side] = 0;
            }
        }
    }
    LabelState state[3];
    bool fresh;
    const uint64_t now = GetTickCount64();
    {
        std::lock_guard<std::mutex> lock(g_label_mx);
        for (int i = 0; i < 3; i++) state[i] = g_label_state[i];
        fresh = now - g_label_at < 200;
    }
    g_ui_updates++;
    // The key decides at once. "fresh" only covers the render pass going quiet with the key still down (placing ended).
    const bool held = editor_hold_active();
    int axis_of[3] = {0, 1, 2};
    for (int axis = 0; axis < 3; axis++) axis_of[editor_axis_pair(axis)] = axis;
    for (int pair = 0; pair < 3; pair++) {
        const int axis = axis_of[pair];
        const bool show = held && fresh && state[axis].visible;
        const int active = state[axis].far_side ? 1 : 0;
        g_outlines[pair].visible(show);
        if (show) {
            char label[48];
            editor_pair_label(axis, label, sizeof(label));
            if (std::strcmp(label, g_outline_text[pair]) != 0) {
                g_outlines[pair].text(label);
                strcpy_s(g_outline_text[pair], label);
            }
            g_outlines[pair].rect({state[axis].x, state[axis].y, .3f, .05f, SWML_UI_CENTER});
        }
        for (int side = 0; side < 2; side++) {
            const swml_ui_handle handle = g_labels[pair][side];
            if (!handle) continue;
            const bool this_one = show && side == active;
            swml3::ui::visible(handle, this_one);
            if (!this_one) continue;
            g_ui_shown++;
            char label[48];
            editor_pair_label(axis, label, sizeof(label));
            if (std::strcmp(label, g_label_text[pair][side]) != 0) {
                swml3::ui::text(handle, label);
                strcpy_s(g_label_text[pair][side], label);
            }
            swml3::ui::rect(handle, {state[axis].x, state[axis].y, .3f, .05f, SWML_UI_CENTER});
        }
    }
    if (g_ui_shown && now >= g_ui_log_at) {
        swlog("editor: rotation labels, %llu UI updates, %llu shown, %llu from another root",
              static_cast<unsigned long long>(g_ui_updates), static_cast<unsigned long long>(g_ui_shown), static_cast<unsigned long long>(g_ui_other_root));
        g_ui_updates = g_ui_shown = g_ui_other_root = 0;
        g_ui_log_at = now + 5000;
    }
}

// --- rings: inside the editor's render pass -------------------------------------------------------------------------
void draw_rings(void* renderer, const Snapshot& s)
{
    float nx, ny;
    double center_depth;
    if (!project(s, s.pos, &nx, &ny, &center_depth)) return;
    static draw::CameraBlock block;
    draw::fill_camera(block, s.vp, s.pos);
    const draw::Pass pass{renderer, &block};
    constexpr int kSegments = 120;
    // Near halves are three lines side by side and bright, far halves one line and dark: the primitive is
    // one pixel wide and opaque.
    constexpr double kHeavy[3] = {-0.006, 0.0, 0.006};
    for (int axis = 0; axis < 3; axis++) {
        const double r = g_radius * kRadiusScale[axis];
        const draw::Color ring_color = kColors[editor_axis_pair(axis)];
        double prev[3];
        ring_offset(axis, 0, 1.0, prev);
        for (int i = 1; i <= kSegments; i++) {
            double unit[3];
            ring_offset(axis, 2.0 * kPi * i / kSegments, 1.0, unit);
            double mid[3];
            for (int k = 0; k < 3; k++) mid[k] = s.pos[k] + (prev[k] + unit[k]) * .5 * r;
            float mx, my;
            double depth;
            bool front = project(s, mid, &mx, &my, &depth) && depth < center_depth;
            const int layers = front ? 3 : 1;
            const draw::Color color = front ? ring_color : draw::dim(ring_color, kFarLine);
            for (int layer = 0; layer < layers; layer++) {
                const double radius = r + (front ? kHeavy[layer] : 0.0);
                double a[3], b[3];
                for (int k = 0; k < 3; k++) {
                    a[k] = prev[k] * radius;
                    b[k] = unit[k] * radius;
                }
                draw::line(pass, a, b, color);
            }
            for (int k = 0; k < 3; k++) prev[k] = unit[k];
        }
    }

    // One label per ring, travelling along it.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double t = double(now.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart);
    LabelState state[3];
    const float aspect = draw::ui_aspect(pass);
    for (int axis = 0; axis < 3; axis++) {
        double a = t * g_orbit_rad_per_sec + axis * (2.0 * kPi / 3.0);
        double p[3];
        ring_offset(axis, a, g_radius * kRadiusScale[axis], p);
        for (int k = 0; k < 3; k++) p[k] += s.pos[k];
        double depth;
        float px, py;
        state[axis].visible = project(s, p, &px, &py, &depth) && aspect > 0;
        if (state[axis].visible) {
            draw::ndc_to_ui(aspect, px, py, &state[axis].x, &state[axis].y);
            state[axis].far_side = depth >= center_depth;
        }
    }
    if (g_label_mx.try_lock()) {
        for (int i = 0; i < 3; i++) g_label_state[i] = state[i];
        g_label_at = GetTickCount64();
        g_label_mx.unlock();
    }
}

using Draw_t = void(__fastcall*)(void*, void*, uintptr_t, uintptr_t, uintptr_t);
Draw_t g_orig_draw = nullptr;
void __fastcall draw_detour(void* self, void* renderer, uintptr_t b, uintptr_t c, uintptr_t d)
{
    g_orig_draw(self, renderer, b, c, d);
    if (!g_enabled || !editor_hold_active()) return;
    Snapshot s;
    if (read_editor(static_cast<const uint8_t*>(self), &s)) draw_rings(renderer, s);
}

// The editor only computes the hovered part's transform inside a branch gated by the
// "Show Rotation UI" setting. While the hold key is down we let that branch run for this one
// update call and restore the byte straight after, so the saved setting never changes and the
// render pass / key handler never see the override. The game's own hints and direction lines
// produced by the branch are hidden by hook_editor_wheel.cpp.
using Update_t = void(__fastcall*)(void* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
Update_t g_orig_update = nullptr;

void __fastcall update_detour(void* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    auto* ed = static_cast<uint8_t*>(self);
    bool forced = false;
    // Keep the branch running just past the release, so the native UI update that hides the labels still arrives.
    static uint64_t release_until = 0;
    const uint64_t now = GetTickCount64();
    if (editor_hold_active()) release_until = now + 500;
    if (g_enabled && now < release_until && ed[addrs().off_hints_setting] == 0) {
        ed[addrs().off_hints_setting] = 1;
        forced = true;
    }
    g_orig_update(self, a1, a2, a3, a4);
    if (forced) ed[addrs().off_hints_setting] = 0;
}

bool install()
{
    std::wstring ini = std::wstring(settings_ini());
    g_enabled = GetPrivateProfileIntW(L"editor", L"rings", 1, ini.c_str()) != 0;
    g_radius = GetPrivateProfileIntW(L"editor", L"ring_radius_cm", 45, ini.c_str()) / 100.0;
    g_orbit_rad_per_sec = GetPrivateProfileIntW(L"editor", L"orbit_deg_per_sec", 30, ini.c_str()) * kPi / 180.0;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    if (!addrs().editor_ok) {
        swlog("editor: rotation rings unavailable (game code not recognised, see resolve: lines)");
        return false;
    }
    g_vtable = addrs().editor_vtable;
    if (!g_enabled) {
        swlog("editor: rotation rings off");
        return true;
    }
    auto* api = swml3::api();
    if (!api || !(api->capabilities() & SWML_V3_CAP_UI_APPLICATION_CONTEXT) || !draw::bind_line() || !native_find_pattern(draw::kAspectPattern)) {
        swlog("editor: rotation rings unavailable (native draw or UI not recognised)");
        g_enabled = false;
        return true;
    }
    const uintptr_t draw_fn = *reinterpret_cast<const uintptr_t*>(g_vtable + 7 * 8);
    if (!hook_address(draw_fn, reinterpret_cast<void*>(&draw_detour), reinterpret_cast<void**>(&g_orig_draw)) ||
        !hook_address(addrs().update_fn, reinterpret_cast<void*>(&update_detour), reinterpret_cast<void**>(&g_orig_update))) {
        return false;
    }
    swml_handle watch = 0;
    const auto result = swml3::ui::watch("c_ui_vehicle_editor_interface", &interface_update, nullptr, watch);
    swlog("editor: rotation rings on (radius %.2f m), game line renderer, native labels %s", g_radius, result == SWML_OK ? "on" : "unavailable");
    return true;
}

HookRegistrar reg{{"editor rotation rings (native)", /*needs_known_build=*/false, &install}};

} // namespace

