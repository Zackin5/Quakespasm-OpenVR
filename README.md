# Quakespasm-OpenVR
OpenVR support integrated into Quakespasm.

Forked from [Dominic Szablewski's (Phoboslab) Oculus modification of Quakespasm](https://github.com/phoboslab/Quakespasm-Rift) and utilizing the [OpenVR C wrapper by Ben Newhouse](https://github.com/newhouseb/openvr-c).

# Cvars

* `vr_enabled` – 0: disabled, 1: enabled
* `vr_crosshair` – 0: disabled, 1: point, 2: laser sight
* `vr_crosshair_size` - Sets the diameter of the crosshair dot/laser from 1-32 pixels wide. Default 3.
* `vr_crosshair_depth` – Projection depth for the crosshair. Use `0` to automatically project on nearest wall/entity. Default 0.
* `vr_crosshair_alpha` – Sets the opacity for the crosshair dot/laser. Default 0.25.
* `vr_aimmode` – 1: Head Aiming, 2: Head Aiming + mouse pitch, 3: Mouse aiming, 4: Mouse aiming + mouse pitch, 5: Mouse aims, with YAW decoupled for limited area, 6: Mouse aims, with YAW decoupled for limited area and pitch decoupled completely. Default 1.
* `vr_deadzone` – Deadzone in degrees for `vr_aimmode 5`. Default 30.
* `vr_viewkick`– 0: disables viewkick on player damage/gun fire, 1: enable

# Future Plans

* Comfort options (such as tunnel vision/FOV reduction)
* Motion controls
