1.2.3: the camera's pitch counts too. Q/E turn about whichever axis is nearest your view direction: looking down
(past 45 degrees) that is the vertical axis, so Q/E spin the part flat and A/D turn it about the screen's up.
W/S still follow the horizontal look.

1.2.2: A/D steer the part left / right (vertical axis) and Q/E lay it over onto its side / upside down (the axis
along your view), like a wheel seen from behind; W/S unchanged. Colours stay with the keys (green A/D, blue Q/E).

1.2.1: ring and label colours follow the key pair (red W/S, green A/D, blue Q/E), not the axis, so a ring's colour always
matches its label.

1.2.0: the keys follow the camera, chosen when the hold key goes down: W/S roll the part away from / towards you like
a wheel, A/D tip it left / right, Q/E yaw it left / right; each ring's label shows the pair that turns it for that view.
camera_relative=0 keeps the old fixed X / Y / Z layout. invert_x/y/z are now invert_ws/ad/qe (old names still read).

1.1.2: the key labels disappear as soon as the hold key is released, instead of lingering for 200 ms.

1.1.0: the rings are drawn by the game (SDK native_draw.h, the game's own line primitive, at its rotation-axis meshes)
and the key labels are native text labels. No GL or SwapBuffers callback of its own.

1.0.1: runs on the 2026-10-28 game build (0x6ae218ec) through the SDK port table (hooked functions unchanged).

# Editor Rotation Rings

Replaces the vehicle editor's rotation keys and hint overlay.

Hold your "Show Rotation UI" keybind (set in the game's Controls; it no longer toggles the old
rotation UI) while placing a part:

- Forward / Backward roll the part away from / towards you, like a wheel seen from behind
- Left / Right steer it left / right around the vertical axis
- Interact Left / Right lay it over onto its left / right side (twice: upside down) around the axis nearest your view;
  looking down that is the vertical axis, so it spins flat

Which vehicle axis each pair turns is picked from where the camera looks when you press the hold key and stays
until you release it. The rings stay on the vehicle's axes and take the colour of the pair that turns them (red W/S, green A/D, blue Q/E),
each labelled with the keys
that turn it for that view and the labels orbit slowly; rebinding a key applies immediately. The game's own
rotation hints and direction lines are hidden.

Settings (settings.ini, [editor]): hold_key (game = use the Show Rotation UI bind, or a key name such
as capslock), camera_relative, rings, ring_radius_cm, orbit_deg_per_sec, invert_ws / invert_ad / invert_qe.

Only affects your own editor; nothing changes for other players.
