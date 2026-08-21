# The demo

There are two, and they show different things.

| | What it shows |
|---|---|
| `--demo` | The picture. A camera tour with the editor hidden, because most of what a renderer does only becomes visible when the camera moves |
| `--demo --editor` | The engine. The same tour with the panels up, and a script module rebuilt underneath it partway through |

## The engine being used

```sh
./build/default/bin/EnchantedEngine --demo --editor --size 1280 800
```

![The engine running, with a script hot reload partway through](images/engine-demo.gif)

What is on screen is the engine, not a presentation of it: the hierarchy, an
inspector generated from reflection, live draw statistics, the console, and the
scene rendering into a viewport panel the UI samples as a texture.

Three things are worth watching.

**The stats panel.** 139 candidates, most rejected by the frustum, a few more
by the depth pyramid, and the fifty-odd survivors going out in fifteen draw
calls - because the hundred and twenty gravel stones share a mesh and a
material and are one instanced draw between them.

**The inspector.** It is showing `sandbox::Pulse`, a behaviour the engine was
not built with. It came out of `libEnchantedSandbox.so`, which was loaded at
runtime; the fields and their sliders are drawn from reflection the engine
learned about at the moment the module loaded.

**The pink sphere, about halfway through.** Its breathing suddenly deepens and
the console says why: the module was rebuilt while the engine was running, and
the engine loaded the new one and rebuilt every live behaviour from it. Nothing
restarted. `scripts/record_engine_demo.sh` records the whole thing, including
making the edit.

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
| Wide, on the whole set | The scene under its procedurally generated evening sky — and **physics** opening the show: a steel boulder drops onto a plank, rolls down it, and takes out the crate tower at the base. Play is what dropped it; Stop would put the tower back |
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
recording. `scripts/record_demo.sh` does the whole thing, including the GIF:

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
| `--editor` | Keep the editor up during the tour, with a scripted entity selected |
| `--size W H` | Open a window this size. The editor wants more than the default 800×600 |
| `--record DIR` | Write every frame there as a PNG |
| `--record-fps N` | Seconds per frame while recording; also fixes the simulation step |
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
