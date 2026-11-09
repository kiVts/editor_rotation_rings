#pragma once

// Shared between the editor hooks.
bool editor_hold_active(); // true while the hold-to-rotate key is down (hook_editor_rotate.cpp)
void* editor_object();
// "W/S"-style label of the player's binds for the key pair that turns an axis (0 X, 1 Y, 2 Z) during the
// current hold (camera relative, chosen when the hold key went down), upper-case.
void editor_pair_label(int axis, char* out, unsigned long long size);
// The key pair (0 W/S, 1 A/D, 2 Q/E) that turns an axis during the current hold. Ring and label colours follow the pair.
int editor_axis_pair(int axis);
