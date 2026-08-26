# The demo

There are three, and they show different things.

| | What it shows |
|---|---|
| `--demo` | The picture. A camera tour with the editor hidden, because most of what a renderer does only becomes visible when the camera moves |
| `--demo --editor` | The engine. The same tour with the panels up, and a script module rebuilt underneath it partway through |
| `--demo --follow` | The game. The camera behind a rigged character walking, turning, jumping and shoving a crate — four subsystems agreeing |

## Somebody in it

```sh
./build/default/bin/EnchantedEngine --demo --follow      # watch
./build/default/bin/EnchantedEngine --play               # or take the controls
```

![A rigged humanoid walking a circuit with the camera following](images/character.gif)

Nothing is driving it by hand. The `Patrol` behaviour walks it between the
corners of a rectangle and jumps when it arrives, writing the same four intent
fields a player's hands write through `PlayerCharacter` — `move`, `run`,
`jump`, `jumpHeld`. The character controller cannot tell them apart, which is
what makes this recording reproducible rather than a thing someone did once
with a controller in their hands. `--play` swaps the driver for a keyboard or
a gamepad and changes nothing else.

Four things are agreeing on screen. The **character controller** holds the
capsule upright and decides its velocity from the intent, with acceleration,
air control, coyote time and a jump authored as a height. **Physics** finds
out how far that velocity actually gets, walks it up the step it can and
shoves the crate it cannot. The **animation system** reads `grounded` and
`planarSpeed`, picks idle, walk, run or jump, and crossfades into it at the
rate the ground demands. The **renderer** skins the result through a depth
pre-pass, an `EQUAL` depth test and GPU-driven indirect draws — and none of
those four knows the others exist.

Along one edge of its circuit there is a **pressure plate** and a door. The
plate is a collider and a `Trigger` and nothing else; the behaviour on it
counts arrivals and departures, because two things standing on a plate is two
arrivals and a door that shut on the first departure would shut on whoever
was still there. It is set to notice only the `Character` layer, so the crate
the character shoves across the same plate every lap does not open it — a
plate any passing box can open is not a door with a key.

`scripts/record_character_demo.sh` records it.

## The engine being used

```sh
./build/default/bin/EnchantedEngine --demo --editor --size 1280 800
```

![The engine running, with a script hot reload partway through](images/engine-demo.gif)

What is on screen is the engine, not a presentation of it: the hierarchy, an
inspector generated from reflection, live draw statistics, the console, and the
scene rendering into a viewport panel the UI samples as a texture.

Three things are worth watching.

**The stats panel.** 139 candidates every frame, most rejected by the frustum,
a few more by the depth pyramid, and what survives going out in a fraction as
many draw calls - because the hundred and twenty gravel stones share a mesh and
a material and are one instanced draw between them.

Its frame time belongs to the machine that made the recording, and that machine
has no GPU: it is **lavapipe**, Mesa's software rasteriser, doing a 1280x800
viewport with 4x MSAA, SSAO, three kinds of shadow and a depth pyramid on the
CPU. A few hundred milliseconds a frame is what that costs and it is not what
the engine costs on hardware. The number is honest rather than flattering on
purpose - it is measured and unclamped, so it can report worse than the quarter
of a second the simulation delta stops at.

**The inspector.** It is showing `sandbox::Pulse`, a behaviour the engine was
not built with. It came out of `libEnchantedSandbox.so`, which was loaded at
runtime; the fields and their sliders are drawn from reflection the engine
learned about at the moment the module loaded.

**The pink sphere, a third of the way in.** Its breathing suddenly deepens and
the console says why: the module was rebuilt while the engine was running, and
the engine loaded the new one and rebuilt every live behaviour from it. Nothing
restarted. `scripts/record_engine_demo.sh` records the whole thing, including
making the edit.

The recording is nine seconds out of the tour's twenty-four, at twelve frames
a second. Both numbers are a file-size decision rather than a limit: a GIF of
a 1280x800 editor costs roughly 45 kB a frame after quantisation, because the
viewport is moving and frame differencing has little to hold on to, so length
and smoothness trade directly against each other. The script records the whole
tour and keeps the window where the sphere is large in shot, because that is
where the reload is visible.

The edit it makes is to a constant rather than to a reflected field, and that
is the point. A field's value is carried across a reload - written out of the
instance being replaced and read back into its replacement - so changing a
default would prove nothing. What a reload has to demonstrate is that new
*code* is running.

## The camera tour

```sh
./build/default/bin/EnchantedEngine --demo
```

A twenty-four-second camera tour of the demo scene, with the editor hidden and
the scene playing. It exists because a renderer is hard to demonstrate in a still
image and impossible to demonstrate in a paragraph: most of what this engine
does only becomes visible when the camera moves. A mirror sphere is a coloured
ball until its reflection slides across it, a shadow is a dark patch until it
swings, and bloom is brightness until a highlight crosses the frame.

![The demo tour](images/demo-tour.gif)

Nothing about the frame is different from an ordinary one — it is the engine
being shown, not a presentation mode. The tour only supplies the camera pose
and presses Play.

## What each shot is for

| Shot | What to look at |
|---|---|
| Wide, on the whole set | The scene under its procedurally generated evening sky — and **physics** opening the show: a steel boulder drops onto a plank, rolls down it, and takes out the crate tower at the base. Play is what dropped it; Stop would put the tower back. In the foreground the orange **character** walks its circuit, turning to face where it is going and shouldering a crate aside |
| Down the row of spheres | The roughness sweep. The highlight broadens from near-mirror to fully rough — which only reads as a sweep when it travels |
| Onto the rippling sheet | Geometry a **script** writes. 2 401 vertices are moved along their normals by a travelling wave every tick, their normals recomputed, and the result uploaded once per frame — there is no mesh file behind it |
| Close on the smoothest sphere | Image-based lighting. It reflects a sky that exists only because the engine convolved one into an irradiance map and a prefiltered specular chain at start-up |
| Low along the floor | The sun's shadows, filtered through a 3×3 PCF comparison sampler, and the red box turning — that motion is play mode running a component, and Stop puts it back |
| Up and over | The copper torus, which is a **glTF import**: a self-contained text glTF in `assets/models/`, parsed and instantiated at start-up |
| Down onto the wreckage | Where the crates came to rest. Every pose is the simulation's answer, not an authored one — the same fixed-step run twice lands them identically, which a test pins bitwise |

Every material, mesh and light in that scene is either generated procedurally,
imported from a text file, or written by a script. A clean checkout ships no
binary assets, which is why the sky is computed rather than loaded and why the
torus is a `.gltf` rather than a `.glb`.

The floor's material is a `.egematerial` file, and the engine watches for it
changing: edit `assets/materials/floor.egematerial` while the tour is running
and the floor changes colour mid-shot.

## Recording it

The engine writes frames itself rather than being screen-grabbed:

```sh
./build/default/bin/EnchantedEngine --demo --record frames/ --record-fps 10
```

Each frame is copied out of the swapchain inside the frame's own command
buffer — before the image is handed to the presentation engine, because an
image that has been presented belongs to the presentation engine until it is
acquired again, and reading it there is the kind of thing that works on one
driver and corrupts on another.

While recording, time advances by exactly one frame's worth per frame rather
than by the clock, so the same recording made on two machines is the same
recording. That step is what `--record-fps` sets, and it is the recording's
frame rate rather than the engine's: a machine that renders a frame in two
seconds and one that renders it in two milliseconds produce the same file.
The stats panel is not fooled by it - the frame time there is measured, so on
a software rasteriser it reads honestly slow while the recording it appears in
still plays smoothly.

`scripts/record_demo.sh` does the whole thing, including the GIF:

```sh
./scripts/record_demo.sh
```

This is also the groundwork for the golden-image regression testing in the
roadmap's verification section: what that needs is exactly this — fixed
scenes, fixed camera, the exact pixels the GPU produced.

## Other flags

| Flag | Effect |
|---|---|
| `--demo` | Run the tour with the editor hidden and the scene playing, then close |
| `--follow` | Put the camera behind the character instead of on the tour's rails |
| `--play` | Drive the character yourself rather than letting the patrol walk it (implies `--follow`) |
| `--editor` | Keep the editor up during the tour, with a scripted entity selected |
| `--size W H` | Open a window this size. The editor wants more than the default 800×600 |
| `--record DIR` | Write every frame there as a PNG |
| `--record-fps N` | Frames per recorded second; also pins the simulation step to `1/N` |
| `--exit-after SECONDS` | Close after this long regardless |
| `--script-module PATH` | Load this module instead of the sandbox; `none` loads no behaviours at all |

## Running it without the script module

```sh
./build/default/bin/EnchantedEngine --demo --script-module none
```

The pink sphere stops breathing and the log says
`no behaviour named 'sandbox::Pulse'; skipping it`. Nothing else changes and
nothing fails: a scene naming a behaviour the running build does not have is
an ordinary situation, and it costs that behaviour rather than the scene.
