# Game structures for debugging and mods

Fields and functions of the running game that matter for gameplay: health,
the players, damage. Addresses are in `default.xbe` as loaded (the game runs
at its own addresses in the recompiled build, so `MEM(addr)` in host code is
the same address). Names like `GetPlayer` are ours; Ghidra has them where
noted.

## Characters (`CEntityCharacter`)

| Offset | Type | Meaning |
|---|---|---|
| `+0x110` | float | maximum health |
| `+0x114` | float | health |
| `+0x168` | uint32 | flags, below |
| `+0x714` | int | state; damage is halved in state `0x15` and ignored in `0x1b` |
| `+0x720` | float | Force |
| `+0x728` | float | maximum Force |

Flags at `+0x168`:

| Bit | Meaning |
|---|---|
| `0x8` | does not die at 0 health; the HUD hides the health needle |
| `0x40` | player two (Jedi Battle), together with `0x4000` |
| `0x4000` | a player; TakeDamage updates the HUD script variables only for these |

The HUD draws health as `+0x114 / +0x110` and Force as `+0x720 / +0x728`
(`CHUD::vf04`, `0x00086170`).

## Finding the players

- `0x0024c030` `GetPlayer(n)`: `n` = 1 for player one, 2 for player two.
  Returns a cached `CEntityRef` (`0x0051423c` for player one, `0x0051422c`
  for player two) when there is one, else walks the entity list.
- Entity list: the pointer at `0x004f17c0` is the sentinel node of a
  circular list. A node holds the next node at `+0`, the entity at `+8`.
  A player is the entity with flag `0x4000`, and `0x40` clear for player one.
- `recomp/host/debugkeys.c` walks this list from the host (F1 restores
  health).

## Damage

- `0x001f1df0` `CEntityCharacter::vf47` = `TakeDamage(amount, source)`;
  returns the new health. Does nothing when the `d_nodamage` cvar is set
  (its value is the byte at `0x004f22c4`). For players it also sets the
  script variables `ObiHitPoints` and `ObiHitPointRatio`.
- `0x001f0200` `ApplyDamage(amount, source)`: health minus amount, clamped at
  0 but not at the maximum (a negative amount heals past it). Then calls
  vtable `+0xd0` (health changed), kills the character through vtable `+0xc4`
  when health drops below 0.001 (unless flag `0x8`), and calls vtable `+0xc0`
  (hit reaction).
- Walls: the physics step `FUN_00117010` and collision `FUN_00116420`
  immobilize a node through its vtable `+0x34`; the character's override
  (`0x001a9d80`, then `vf37`) sends message `0xfc0`, and
  `CEntityCharacter::vf38` answers it with TakeDamage(30).
  This is how the x87 rounding bug (#51) killed Obi-Wan on walls.

## Script commands and cvars

- Script commands (name, then the number the script interpreter uses):
  `HealHitPoints` (`0xd2`), `PlayerHasFullHealth` (`0x184`). Also present:
  `AddHitPoints`, `SetHitPoints`, `DeductHitPoints`, `SubtractHitPoints`.
- Cvars: `d_nodamage` (no damage), `ent_health`.

## Camera

- `0x0002f2a0` `camera->SetFov(h, v)` (thiscall): stores the angles in
  degrees at `+0x38` / `+0x48` and their half-angle cotangents.
  `recomp/host/widescreen.c` replaces it to widen the view.
