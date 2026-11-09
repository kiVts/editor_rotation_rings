
#include "editor_state.h"
#include "game.h"
#include "hooks.h"
#include "keynames.h"
#include "resolve.h"
#include "log.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Addresses and keybind offsets come from resolve.cpp (found at runtime, survive updates).

// The keys that rotate are the player's own binds, read live from the keybind table:
// Forward/Backward -> X, Left/Right -> Y, Interact Left/Right -> Z (resolve.h key_pairs).

constexpr uint32_t kStateRelease = 0;
constexpr uint32_t kStatePress = 1;

// The 5th..7th arguments are on the stack; arguments 3-4 are unused by the game.
using KeyHandler_t = uint64_t(__fastcall*)(void* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                                           uint32_t key, uint32_t state);
using GetKeybindTable_t = uint8_t*(__fastcall*)();

KeyHandler_t g_orig = nullptr;
GetKeybindTable_t g_get_table = nullptr;

bool g_enabled = true;
bool g_camera_relative = true;
uint32_t g_fixed_hold_key = 0; // from settings.ini; 0 = use the game's "Show Rotation UI" bind
bool g_invert[3] = {false, false, false}; // per key pair: W/S, A/D, Q/E
bool g_held = false;
void* g_editor = nullptr;

// Which vehicle axis each key pair (0 W/S, 1 A/D, 2 Q/E) turns, and which way: the pair's first key
// (W, A, Q) is +90 degrees (matrix sense, v' = n x v) about sign * the axis. Packed so the UI thread
// reads one consistent value: per pair 2 bits axis + 1 bit "negative".
std::atomic<uint32_t> g_mapping{0};

uint32_t pack_mapping(const int axis[3], const int sign[3])
{
    uint32_t m = 0;
    for (int p = 0; p < 3; p++) m |= (uint32_t(axis[p]) | (sign[p] < 0 ? 4u : 0u)) << (p * 3);
    return m;
}
int mapped_axis(uint32_t m, int pair) { return (m >> (pair * 3)) & 3; }
int mapped_sign(uint32_t m, int pair) { return (m >> (pair * 3)) & 4 ? -1 : 1; }

// The old fixed layout: W/S X, A/D Y, Q/E Z, the first key one game press (-90).
uint32_t fixed_mapping()
{
    const int axis[3] = {0, 1, 2}, sign[3] = {-1, -1, -1};
    return pack_mapping(axis, sign);
}

void cross(const double a[3], const double b[3], double out[3])
{
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

bool normalize(double v[3])
{
    const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len < 1e-9) return false;
    for (int k = 0; k < 3; k++) v[k] /= len;
    return true;
}

// Camera right / up / forward in vehicle coordinates: rows 0, 1 and 3 of the editor's view-projection
// (clip x right, clip y up, w = depth into the screen). Pure matrix algebra, so the game's handedness
// never enters. SEH-guarded: the editor may be going away.
bool read_camera(const uint8_t* ed, double right[3], double up[3], double fwd[3])
{
    __try {
        const GameAddrs& r = addrs();
        if (*reinterpret_cast<const uintptr_t*>(ed) != r.editor_vtable) return false;
        const double* m = reinterpret_cast<const double*>(ed + r.off_camera + r.off_viewproj); // [col * 4 + row]
        for (int k = 0; k < 3; k++) {
            right[k] = m[k * 4 + 0];
            up[k] = m[k * 4 + 1];
            fwd[k] = m[k * 4 + 3];
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return normalize(right) && normalize(up) && normalize(fwd);
}

// Picks the pairs' axes from the camera at the moment the hold key goes down.
uint32_t camera_mapping(const uint8_t* ed)
{
    double R[3], U[3], F[3];
    if (!read_camera(ed, R, U, F)) return fixed_mapping();
    int axis[3], sign[3];
    auto sgn = [](double v) { return v < 0 ? -1 : 1; };

    // W: a wheel rolling away, the screen's top going into the screen: n x U = F, so n = U x F (= +-right).
    double w[3];
    cross(U, F, w);
    axis[0] = std::fabs(w[0]) >= std::fabs(w[2]) ? 0 : 2;
    sign[0] = sgn(w[axis[0]]);

    // The other two axes: the one nearest the view direction is Q/E's, the last one A/D's. Looking level
    // that is the horizontal axis along the view (Q/E) and Y (A/D); looking down past 45 degrees Y is the
    // view axis, so Q/E spins the part flat and A/D turns it about the screen's up.
    const int rest[2] = {axis[0] == 0 ? 2 : 0, 1};
    const int view = std::fabs(F[rest[0]]) >= std::fabs(F[rest[1]]) ? 0 : 1;
    axis[2] = rest[view];
    axis[1] = rest[1 - view];

    // A: steer left, the far side turning to screen left: n x F = -R, so n = R x F (= +-up).
    double a[3];
    cross(R, F, a);
    sign[1] = sgn(a[axis[1]]);

    // Q: lay it over to the left, the screen's top going to screen left: n x U = -R, so n = R x U (= +-view).
    double q[3];
    cross(R, U, q);
    sign[2] = sgn(q[axis[2]]);
    return pack_mapping(axis, sign);
}

const char* kAxisName[3] = {"X", "Y", "Z"};

int vk_for_game_key(uint32_t code)
{
    if (code >= 0x16 && code <= 0x2f) return 'A' + static_cast<int>(code - 0x16);
    if (code >= 0x0c && code <= 0x15) return '0' + static_cast<int>(code - 0x0c);
    if (code >= 0x30 && code <= 0x3b) return VK_F1 + static_cast<int>(code - 0x30);
    struct { const char* name; int vk; } map[] = {
        {"space", VK_SPACE},   {"capslock", VK_CAPITAL}, {"tab", VK_TAB},       {"lshift", VK_LSHIFT},
        {"rshift", VK_RSHIFT}, {"lctrl", VK_LCONTROL},   {"rctrl", VK_RCONTROL}, {"lalt", VK_LMENU},
        {"ralt", VK_RMENU},    {"menu", VK_APPS},        {"insert", VK_INSERT}, {"delete", VK_DELETE},
        {"home", VK_HOME},     {"end", VK_END},          {"page up", VK_PRIOR}, {"page down", VK_NEXT},
    };
    for (const GameKeyName& k : kGameKeyNames) {
        if (k.code != code) continue;
        for (auto& m : map) {
            if (strcmp(m.name, k.name) == 0) return m.vk;
        }
    }
    return 0; // unknown: rely on press/release events only
}

uint32_t game_key_by_name(const char* name, uint32_t fallback)
{
    for (const GameKeyName& k : kGameKeyNames) {
        if (_stricmp(k.name, name) == 0) return k.code;
    }
    return fallback;
}

const char* game_key_name(uint32_t code)
{
    for (const GameKeyName& k : kGameKeyNames) {
        if (k.code == code) return k.name;
    }
    return "?";
}

uint32_t hold_key()
{
    if (g_fixed_hold_key) return g_fixed_hold_key;
    if (g_get_table && addrs().key_show_rotation)
        return *reinterpret_cast<const uint32_t*>(g_get_table() + addrs().key_show_rotation);
    return 0x4a; // capslock
}

uint64_t __fastcall key_detour(void* self, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint32_t key,
                               uint32_t state)
{
    g_editor = self;
    if (!g_enabled) return g_orig(self, a1, a2, a3, a4, key, state);

    const uint32_t hold = hold_key();
    const int hold_vk = vk_for_game_key(hold);
    if (key == hold) {
        if (state == kStatePress) { // repeats come with another state, so this is once per hold
            g_held = true;
            const uint32_t m = g_camera_relative ? camera_mapping(static_cast<const uint8_t*>(self)) : fixed_mapping();
            g_mapping.store(m);
            if (g_camera_relative) {
                swlog("editor: hold-to-rotate keys for this view: W/S %c%s, A/D %c%s, Q/E %c%s", mapped_sign(m, 0) > 0 ? '+' : '-',
                      kAxisName[mapped_axis(m, 0)], mapped_sign(m, 1) > 0 ? '+' : '-', kAxisName[mapped_axis(m, 1)],
                      mapped_sign(m, 2) > 0 ? '+' : '-', kAxisName[mapped_axis(m, 2)]);
            }
        } else if (state == kStateRelease) {
            g_held = false;
        }
        return 1; // consumed: the hold key does nothing else in the editor
    }
    // A release can be missed (alt-tab while holding); trust the OS if we know the key.
    if (g_held && hold_vk != 0 && (GetAsyncKeyState(hold_vk) & 0x8000) == 0) g_held = false;
    if (!g_held) return g_orig(self, a1, a2, a3, a4, key, state);

    const uint8_t* binds = g_get_table();
    int pair = -1, dir = 0;
    for (int i = 0; i < 6; i++) {
        if (*reinterpret_cast<const uint32_t*>(binds + addrs().key_pairs[i]) == key) {
            pair = i / 2;
            dir = (i % 2 == 0) ? +1 : -1;
            break;
        }
    }
    if (pair < 0) return g_orig(self, a1, a2, a3, a4, key, state);
    if (state == kStateRelease) return g_orig(self, a1, a2, a3, a4, key, state); // never block releases
    if (state != kStatePress) return 1;                                         // swallow key repeat

    const uint32_t m = g_mapping.load();
    const int axis = mapped_axis(m, pair);
    uint32_t rotate_key = *reinterpret_cast<const uint32_t*>(binds + addrs().key_rotate[axis]);
    if (g_invert[pair]) dir = -dir;
    // Wanted: +90 * dir * sign about the axis. One game press is -90 there, three are +90.
    int presses = dir * mapped_sign(m, pair) > 0 ? 3 : 1;
    for (int i = 0; i < presses; i++) {
        g_orig(self, a1, a2, a3, a4, rotate_key, kStatePress);
        g_orig(self, a1, a2, a3, a4, rotate_key, kStateRelease);
    }
    return 1;
}

bool install()
{
    std::wstring ini = std::wstring(settings_ini());
    g_enabled = GetPrivateProfileIntW(L"editor", L"hold_rotate", 1, ini.c_str()) != 0;
    wchar_t wname[64];
    GetPrivateProfileStringW(L"editor", L"hold_key", L"game", wname, 64, ini.c_str());
    char name[64];
    WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof(name), nullptr, nullptr);
    g_fixed_hold_key = _stricmp(name, "game") == 0 ? 0 : game_key_by_name(name, 0);
    g_camera_relative = GetPrivateProfileIntW(L"editor", L"camera_relative", 1, ini.c_str()) != 0;
    // Per key pair. The old names (invert_x/y/z) meant the same pairs in the fixed layout.
    const wchar_t* invert_keys[3][2] = {{L"invert_ws", L"invert_x"}, {L"invert_ad", L"invert_y"}, {L"invert_qe", L"invert_z"}};
    for (int p = 0; p < 3; p++) {
        const int legacy = GetPrivateProfileIntW(L"editor", invert_keys[p][1], 0, ini.c_str());
        g_invert[p] = GetPrivateProfileIntW(L"editor", invert_keys[p][0], legacy, ini.c_str()) != 0;
    }
    g_mapping.store(fixed_mapping());

    if (!addrs().editor_ok) {
        swlog("editor: hold-to-rotate unavailable (game code not recognised, see resolve: lines)");
        return false;
    }
    g_get_table = reinterpret_cast<GetKeybindTable_t>(addrs().keybind_getter);
    swlog("editor: hold-to-rotate %s (%s), hold key %s'%s' (code 0x%x), invert ws/ad/qe = %d/%d/%d", g_enabled ? "on" : "off",
          g_camera_relative ? "camera relative" : "fixed X/Y/Z",
          g_fixed_hold_key ? "from settings.ini " : "= Show Rotation UI bind, currently ", game_key_name(hold_key()), hold_key(),
          g_invert[0], g_invert[1], g_invert[2]);
    return hook_address(addrs().key_handler, reinterpret_cast<void*>(&key_detour), reinterpret_cast<void**>(&g_orig));
}

HookRegistrar reg{{"editor hold-to-rotate (vehicle editor key handler)", /*needs_known_build=*/false, &install}};

} // namespace

bool editor_hold_active()
{
    const int hold_vk = vk_for_game_key(hold_key());
    if (g_held && hold_vk != 0 && (GetAsyncKeyState(hold_vk) & 0x8000) == 0) g_held = false;
    return g_enabled && g_held;
}

void* editor_object() { return g_editor; }

int editor_axis_pair(int axis)
{
    // Each axis has exactly one pair for the current hold.
    const uint32_t m = g_mapping.load();
    for (int p = 0; p < 3; p++) {
        if (mapped_axis(m, p) == axis) return p;
    }
    return axis;
}

void editor_pair_label(int axis, char* out, unsigned long long size)
{
    if (!g_get_table || axis < 0 || axis > 2 || size == 0) {
        if (size) out[0] = 0;
        return;
    }
    const uint8_t* binds = g_get_table();
    auto key_name = [&](int i) {
        return game_key_name(*reinterpret_cast<const uint32_t*>(binds + addrs().key_pairs[i]));
    };
    const int pair = editor_axis_pair(axis);
    snprintf(out, size, "%s/%s", key_name(pair * 2), key_name(pair * 2 + 1));
    for (char* c = out; *c; c++) *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
}
