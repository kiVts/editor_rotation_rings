// Resolve the editor's addresses before the parts install (resolve.cpp, SDK).
// Ported to build 0x6ae218ec (2026-10-28): the hooked / called functions are byte-identical there and the UI element
// fields used (+0x8..0x40) did not move (mm_ui_element shrank at its end).
#include "game.h"
#include "port_table.h"
#include "resolve.h"
#include "sdk.h"

namespace {
bool setup()
{
    mod_ported_to(kPortToBuild);
    resolve(kResolveEditor);
    if (!addrs().editor_ok) mod_status("off: vehicle editor code not recognised (game update?), see editor_rotation.log");
    return addrs().editor_ok;
}
ModSetup reg{&setup};
} // namespace
