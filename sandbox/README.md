# sandbox

A project's behaviours, built as a module the engine loads at runtime.

Nothing in the engine names anything in here. The scene refers to a behaviour
by the name it registered under, the module registers that name when it is
loaded, and whether the name resolves is a question of what has been loaded -
which is the whole point. This is where a game's gameplay code would live.

## Hot reload

Build the module while the engine is running and it will pick it up:

```sh
cmake --build --preset default --target EnchantedSandbox
```

The engine notices the file changed, loads the new copy, and rebuilds every
live behaviour from it. Reflected fields carry across; anything a behaviour
keeps privately does not, and gets `onSpawn` again instead - which is the call
a behaviour already uses to work its private state out from where things are.
Reflect the state you want to survive.

## Seeing it happen

`scripts/record_engine_demo.sh` runs the demo with the editor up, edits this
source and rebuilds the module partway through, and assembles the result into
`docs/images/engine-demo.gif`. It restores the source on the way out.
