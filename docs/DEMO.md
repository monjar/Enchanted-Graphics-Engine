# The demo

```sh
./build/default/bin/EnchantedEngine --demo
```

A nineteen-second camera tour of the demo scene, with the editor hidden and the
scene playing. It exists because a renderer is hard to demonstrate in a still
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
| Wide, on the whole set | The scene: five metal spheres, two dielectrics, a floor, an imported torus and a script-driven sheet, under a procedurally generated evening sky |
| Down the row of spheres | The roughness sweep. The highlight broadens from near-mirror to fully rough — which only reads as a sweep when it travels |
| Onto the rippling sheet | Geometry a **script** writes. 2 401 vertices are moved along their normals by a travelling wave every tick, their normals recomputed, and the result uploaded once per frame — there is no mesh file behind it |
| Close on the smoothest sphere | Image-based lighting. It reflects a sky that exists only because the engine convolved one into an irradiance map and a prefiltered specular chain at start-up |
| Low along the floor | The sun's shadows, filtered through a 3×3 PCF comparison sampler, and the red box turning — that motion is play mode running a component, and Stop puts it back |
| Up and over | The copper torus, which is a **glTF import**: a self-contained text glTF in `assets/models/`, parsed and instantiated at start-up |

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
| `--record DIR` | Write every frame there as a PNG |
| `--record-fps N` | Seconds per frame while recording; also fixes the simulation step |
| `--exit-after SECONDS` | Close after this long regardless |
