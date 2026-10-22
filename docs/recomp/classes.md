# Classes and vtables (from RTTI)

The image keeps MSVC RTTI for every polymorphic type, so classes, their bases
and their vtables can be read straight out of `.rdata`/`.data` without any
guesswork. `tools/rtti_vtables.py` does that and writes:

- [classes.csv](classes.csv): 885 type descriptors (name, mangled name,
  class/struct, inheritance kind, base classes with their offsets in the
  object). 799 have a CompleteObjectLocator; the other 86 are only used as
  `typeid`/`catch` types.
- [vtables.csv](vtables.csv): 1,187 vtables (741 primary, 446 secondary for
  multiple or virtual inheritance) with 12,676 slots pointing at 3,574 distinct
  functions.

`tools/ghidra_scripts/apply_rtti.py` loads both into the shared Ghidra project.
That has been done: every class is a namespace, each vtable is labelled
`<Class>::vftable` (secondary ones `vftable_<offset>`) and typed as an array of
pointers, and every slot target is a function.

## What it changed in Ghidra

- **2,633 functions created.** Ghidra's analysis had missed 2,637 of the 3,574
  virtual-function entry points (anything reached only through a vtable), and
  had mis-decoded some of them from the wrong byte offset. The function count
  went from 12,860 to 15,501.
- **19 over-long functions split** where a missed virtual function had been
  swallowed into the function before it.
- **3,419 functions named `<Class>::vfNN`**, `NN` being the slot index in the
  most basic class that has the function. Targets shared by more than 8
  classes (for example `_purecall`-style stubs and empty bodies) keep their
  default names.
- **40 library functions named** from [library-names.csv](library-names.csv)
  (CRT RTTI/EH helpers, `_ftol`, XAPI file and time wrappers, D3DX matrix
  helpers), each with a comment giving the evidence.

Run order if the project is ever rebuilt from scratch: analyze, then
`apply_rtti.py`, then `export_function_map.py` and `export_api_surface.py`.

## Shape of the code

Class names come from the binary, so they are the developers' own. The
biggest hierarchies, by number of classes deriving from them:

| Base | Derived classes | Likely role |
|---|---|---|
| `_CNode` | 133 | scene-graph node |
| `LSaveRestore` | 131 | save-game serialization interface |
| `LPoolBase` | 108 | pooled allocation |
| `CPrimitive` | 104 | collision / render primitives |
| `RNode` | 79 | render node |
| `CCallbackManager` | 66 | event callbacks |
| `RPrimitive` | 50 | render primitives |
| `CTanisNode` | 49 | engine node |
| `CMessageHandler` | 39 | message passing (`CTanisMsg<...>`, `CMessage_Typed<...>`) |
| `CWorldObject` | 33 | placed world objects |
| `TAIGoalStack`, `TAIGoal` | 33, 21 | AI goals |
| `CCharacterState` | 30 | character state machine |
| `CPhysNode` | 30 | physics |

Largest vtables: `CD3DContext` and `CDrawContext` (87 slots each: the engine's
render interface), then the `CEntity*` family at 72 to 74 slots
(`CEntityHuman`, `CEntityCharacter`, `CEntitySpline`, `CEntitySithGunShip`,
...).

Example: `CEntityHuman` derives from `CEntityCharacter`, which derives from
`CEntity`, `CCallbackManager`, `CWorldObject` (at +16), `CMessageHandler`
(at +16) and `ChoreStateMachine` (at +616).

"Likely role" is read from the names only.

## Use in the recompiler

- Every slot target is a guaranteed function entry, so the recompiler can seed
  its function list from vtables.csv in addition to direct calls.
- An indirect call `call [reg+N]` on an object of a known class can only reach
  the slot `N/4` of that class or of a class derived from it. The hierarchy in
  classes.csv bounds that set, which allows direct dispatch in the generated
  code where the set is small, with a guest-address lookup as the fallback.
- `typeid`, `dynamic_cast` and `catch` read these same RTTI structures at run
  time, so they must stay at their original addresses in the guest image.
