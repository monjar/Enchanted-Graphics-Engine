#!/usr/bin/env python3
"""Generates assets/scenes/level.egescene - the sandbox game.

A level is a scene file, and a scene file is a list of entities with
components. There is no level editor yet, so this script is the level editor:
it writes the same JSON the engine's own serializer writes, which is what
makes the result openable, editable in the inspector, and saveable again.

Two kinds of id appear in it and they come from different places.

  - **Meshes** are the primitives the engine ships. Their ids are derived
    from their names - the same FNV-1a pair `Guid::fromName` computes - so
    this script can work them out without asking anything.
  - **Materials** are files, and a file's id lives in the `.egameta` sidecar
    beside it. Those are read rather than derived, because the database
    generated them and they are the only copy.

The scene's convention is the engine's: **-Y is up**, so "higher" is a
smaller y and gravity pulls towards +y. The floor's top surface is y = 0.
"""

import json
import os

MASK = (1 << 64) - 1
PRIME = 1099511628211
OFFSET_BASIS = 14695981039346656037

FLOOR_TOP = 0.0
# The character's capsule is 0.3 + 0.55 + 0.3 tall, so its centre stands this
# far above whatever it is on.
STANDING = 0.85


def fnv1a(text, basis):
    value = basis
    for byte in text.encode("utf-8"):
        value = (value ^ byte) * PRIME & MASK
    return value


def guid_from_name(name):
    """Guid::fromName: two FNV-1a passes over the same string, different bases."""
    high = fnv1a(name, OFFSET_BASIS)
    low = fnv1a(name, PRIME)
    if high == 0 and low == 0:
        high, low = 1, 1
    return f"{high:016x}{low:016x}"


def material_id(name):
    """The id the asset database gave a material file, from its sidecar."""
    path = os.path.join("assets", "materials", f"{name}.egematerial.egameta")
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)["guid"]


MESH = {name: guid_from_name(f"mesh:{name}") for name in ("box", "plane", "sphere")}


def transform(position, scale=(1.0, 1.0, 1.0), rotation=(0.0, 0.0, 0.0)):
    return {
        "translation": list(position),
        "scale": list(scale),
        "rotation": list(rotation),
    }


def renderer(mesh, material):
    return {"mesh": MESH[mesh], "material": material_id(material), "visible": True}


def collider(half=(0.5, 0.5, 0.5), offset=(0.0, 0.0, 0.0)):
    return {"halfExtents": list(half), "offset": list(offset)}


def rigid(mass=1.0, kinematic=False, friction=0.5, restitution=0.0, gravity=1.0, sensor=False):
    return {
        "mass": mass,
        "kinematic": kinematic,
        "friction": friction,
        "restitution": restitution,
        "linearDamping": 0.05,
        "angularDamping": 0.05,
        "gravityFactor": gravity,
        "sensor": sensor,
    }


def script(*behaviors):
    return {"behaviors": [{"type": name, "fields": fields} for name, fields in behaviors]}


def entity(name, components):
    return {"name": name, "components": components}


def slab(name, material, centre, size):
    """A box of `size`, drawn and solid, with its centre where you put it."""
    return entity(
        name,
        {
            "ege::Transform": transform(centre, size),
            "ege::MeshRenderer": renderer("box", material),
            "ege::BoxCollider": collider(),
        },
    )


def floor_slab(name, material, centre_xz, size_xz, thickness=0.5):
    """A slab whose *top* is the floor plane, which is what you stand on."""
    x, z = centre_xz
    width, depth = size_xz
    return slab(
        name,
        material,
        (x, FLOOR_TOP + thickness * 0.5, z),
        (width, thickness, depth),
    )


def build():
    entities = []

    # ---- Lights ---------------------------------------------------------
    # One directional light for shape and two points for warmth. Remember
    # that -Y is up: a sun shining "down" travels towards +y.
    entities.append(
        entity(
            "Sun",
            {
                "ege::Transform": transform((0.0, -6.0, 6.0)),
                "ege::DirectionalLight": {
                    "direction": [0.35, 1.0, 0.25],
                    "color": [1.0, 0.96, 0.88],
                    "intensity": 2.6,
                },
            },
        )
    )
    entities.append(
        entity(
            "Lamp start",
            {
                "ege::Transform": transform((0.0, -3.5, 1.0)),
                "ege::PointLight": {
                    "color": [1.0, 0.85, 0.65],
                    "intensity": 12.0,
                    "range": 18.0,
                    "castsShadows": True,
                },
            },
        )
    )
    entities.append(
        entity(
            "Lamp hall",
            {
                "ege::Transform": transform((0.0, -3.5, 12.0)),
                "ege::PointLight": {
                    "color": [0.8, 0.9, 1.0],
                    "intensity": 14.0,
                    "range": 20.0,
                    "castsShadows": False,
                },
            },
        )
    )

    # ---- The ground the level is made of --------------------------------
    #
    # Three platforms with a gap between the first two. The gap is the
    # obstacle: a bridge crosses it, and missing the bridge is how you lose
    # a life.
    entities.append(floor_slab("Start floor", "level_floor", (0.0, 0.0), (7.0, 7.0)))
    entities.append(floor_slab("Bridge", "level_bridge", (0.0, 5.0), (1.4, 4.2)))
    entities.append(floor_slab("Hall floor", "level_floor", (0.0, 11.0), (11.0, 8.0)))
    entities.append(floor_slab("Exit floor", "level_floor", (0.0, 18.0), (7.0, 6.0)))

    # Walls either side of the doorway, so the gate is the only way through.
    entities.append(slab("Wall left", "level_wall", (-3.25, -0.9, 15.0), (4.5, 1.8, 0.5)))
    entities.append(slab("Wall right", "level_wall", (3.25, -0.9, 15.0), (4.5, 1.8, 0.5)))

    # ---- The pit --------------------------------------------------------
    #
    # A sensor spread under everything. Falling is not a state the engine
    # has; it is a volume you eventually reach, which is all a fail state
    # needs to be.
    entities.append(
        entity(
            "Pit",
            {
                "ege::Transform": transform((0.0, 4.0, 8.0)),
                "ege::BoxCollider": collider(half=(40.0, 1.0, 40.0)),
                "ege::Trigger": {"only": "Character"},
                "ege::Script": script(("sandbox::Pit", {"catches": "Character"})),
            },
        )
    )

    # And something at the bottom of it. Not to stand on - the trigger above
    # has already sent you back by the time you arrive - but to see: a fall
    # past an empty backdrop reads as the picture breaking rather than as a
    # mistake you made, and a floor three storeys down is what makes it a
    # drop.
    entities.append(
        slab("Pit floor", "level_wall", (0.0, 6.5, 8.0), (26.0, 0.5, 30.0))
    )

    # ---- What you collect -----------------------------------------------
    #
    # Three of them, one per area, so crossing the bridge is not optional.
    for index, (x, z) in enumerate(((-2.0, 2.0), (-3.5, 12.0), (3.5, 10.0))):
        entities.append(
            entity(
                f"Coin {index + 1}",
                {
                    "ege::Transform": transform(
                        (x, FLOOR_TOP - 0.45, z), (0.35, 0.35, 0.35), (0.0, 0.7, 0.6)
                    ),
                    "ege::MeshRenderer": renderer("box", "level_gold"),
                    # Local units, scaled by the entity like every collider -
                    # so a coin drawn 0.35 across notices you from half a
                    # metre away and from head to foot. A coin whose volume
                    # was the size of the coin would be one you had to tread
                    # on exactly.
                    "ege::BoxCollider": collider(half=(1.7, 3.0, 1.7)),
                    "ege::Trigger": {"only": "Character"},
                    "ege::Script": script(
                        ("sandbox::Collectible", {"collectedBy": "Character", "spinRate": 2.2})
                    ),
                },
            )
        )

    # ---- Something to push out of the way -------------------------------
    for index, (x, z) in enumerate(((1.5, 12.5), (-1.2, 13.4))):
        entities.append(
            entity(
                f"Crate {index + 1}",
                {
                    "ege::Transform": transform(
                        (x, FLOOR_TOP - 0.4, z), (0.8, 0.8, 0.8)
                    ),
                    "ege::MeshRenderer": renderer("box", "level_crate"),
                    "ege::BoxCollider": collider(),
                    "ege::RigidBody": rigid(mass=1.6, friction=0.6),
                    "ege::PhysicsLayer": {"name": "Props"},
                },
            )
        )

    # ---- The gate -------------------------------------------------------
    #
    # Kinematic, so it pushes rather than passes through, and it sinks into
    # the floor when the last coin is taken. It has never heard of a coin.
    entities.append(
        entity(
            "Gate",
            {
                "ege::Transform": transform((0.0, -0.9, 15.0), (2.0, 1.8, 0.4)),
                "ege::MeshRenderer": renderer("box", "level_gate"),
                "ege::BoxCollider": collider(),
                "ege::RigidBody": rigid(kinematic=True, gravity=0.0),
                "ege::Script": script(
                    ("sandbox::GateSlides", {"opening": [0.0, 1.9, 0.0], "speed": 1.1})
                ),
            },
        )
    )

    # ---- The way out ----------------------------------------------------
    entities.append(
        entity(
            "Exit pad",
            {
                "ege::Transform": transform((0.0, FLOOR_TOP - 0.03, 18.5), (2.0, 0.06, 2.0)),
                "ege::MeshRenderer": renderer("box", "level_exit"),
                # Knee-high, because what a trigger draws and what it notices
                # are different shapes.
                "ege::BoxCollider": collider(half=(0.5, 12.0, 0.5), offset=(0.0, -6.0, 0.0)),
                "ege::Trigger": {"only": "Character"},
                "ege::Script": script(("sandbox::ExitPad", {"opensFor": "Character"})),
            },
        )
    )

    # ---- The rules ------------------------------------------------------
    #
    # No body and nothing to draw: a rule is a behaviour on an entity that is
    # only there to hold it.
    entities.append(
        entity(
            "Rules",
            {
                "ege::Transform": transform((0.0, 0.0, 0.0)),
                "ege::Script": script(("sandbox::LevelRules", {"needed": 3, "lives": 3})),
            },
        )
    )

    # ---- The player -----------------------------------------------------
    #
    # Named "Walker" because that is the name the follow camera looks for.
    # A box rather than the rigged model in assets/models: an imported rig
    # does not survive a scene file yet - rigs become database assets when
    # the animation import grows an asset type - so a level saved to disk
    # gets a shape that does survive one.
    #
    # The route falls in on purpose. A recording that only ever shows the
    # winning line is a recording of a corridor; what makes this a game is
    # that there is a way to lose, and the second leg is it. The run gives up
    # on a leg that ended in the pit and tries the next, which is what lets
    # the same route both fail and finish.
    route = "; ".join(
        (
            "-2 2",     # the first coin
            "3 6",      # straight off the edge, into the pit
            "0 3.5",    # back on the start platform, this time via the bridge
            "0 7",      # across it
            "-3.5 12",  # the second coin
            "3.5 10",   # the third, which opens the gate
            "0 13.6",   # up to the doorway
            "0 18.5",   # and out
        )
    )
    entities.append(
        entity(
            "Walker",
            {
                # The box is the capsule's size: 0.6 across and 1.7 tall,
                # which is what the controller below actually collides with.
                # A player drawn smaller than it collides is a player that
                # bumps into things it is not touching.
                "ege::Transform": transform(
                    (0.0, FLOOR_TOP - STANDING, -2.0), (0.6, 1.7, 0.6)
                ),
                "ege::MeshRenderer": renderer("box", "level_player"),
                "ege::CharacterController": {
                    "radius": 0.3,
                    "halfHeight": 0.55,
                    "walkSpeed": 2.2,
                    "runSpeed": 4.0,
                    "acceleration": 14.0,
                    "braking": 20.0,
                    "airControl": 0.35,
                    "jumpHeight": 0.9,
                    "jumpCutGravity": 2.6,
                    "coyoteTime": 0.12,
                    "jumpBuffer": 0.15,
                    "terminalSpeed": 40.0,
                    "turnRate": 12.0,
                    "maxSlopeAngle": 0.785398,
                    "stepHeight": 0.3,
                    "stickToFloor": 0.5,
                    "mass": 70.0,
                    "pushForce": 100.0,
                    "faceMotion": True,
                },
                "ege::PhysicsLayer": {"name": "Character"},
                # Both, and in this order. The player behaviour reads the
                # keyboard and the pad; the scripted run overwrites what it
                # wrote, until somebody actually touches something - at which
                # point it stands down for good and the level is yours.
                "ege::Script": script(
                    ("ege::PlayerCharacter", {
                        "mouseSensitivity": 0.0025,
                        "lookSpeed": 2.5,
                        "lookYaw": 0.0,
                    }),
                    ("sandbox::RespawnOnFall", {"pause": 0.6}),
                    ("sandbox::ScriptedRun", {
                        "route": route,
                        "arriveRadius": 0.45,
                        "run": False,
                        "loop": False,
                    }),
                ),
            },
        )
    )

    return {"version": 1, "entities": entities}


def main():
    document = build()
    path = os.path.join("assets", "scenes", "level.egescene")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2)
        handle.write("\n")
    print(f"wrote {path}: {len(document['entities'])} entities")


if __name__ == "__main__":
    main()
