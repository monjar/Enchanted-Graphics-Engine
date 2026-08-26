#!/usr/bin/env python3
"""Generates assets/prefabs/pickup.egeprefab.

A prefab is a scene fragment as JSON, and this one is written by hand rather
than saved out of an editor because there is no editor command for it yet -
`prefab::save` exists and the editor will grow a menu item for it. What the
script is really for is the *ids*: a prefab names its mesh and material by
asset id, and the demo's assets are built in code, so their ids come from
`Guid::fromName("mesh:box")` and friends rather than from a sidecar file.

Those ids are an FNV-1a pair over the name, which is reproduced below. The
alternative is copying hex out of a log and hoping nobody renames anything,
and the point of deriving them here is that a rename breaks the script rather
than the asset.
"""

import json
import os

MASK = (1 << 64) - 1
PRIME = 1099511628211
OFFSET_BASIS = 14695981039346656037


def fnv1a(text, basis):
    """The engine's own hash, from core/Guid.cpp."""
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


def main():
    # The demo catalogues its procedural assets under these names, so these
    # are the ids a reference to them has to hold.
    mesh = guid_from_name("mesh:box")
    material = guid_from_name("material:Pickup")

    # One entity: a small crate that falls. Written in the same shape a scene
    # is - a version, and an array of entities whose parents are positions
    # inside that array - because a prefab is a fragment of one.
    document = {
        "version": 1,
        "entities": [
            {
                "name": "Pickup",
                "components": {
                    "ege::Transform": {
                        "translation": [0.0, 0.0, 0.0],
                        "scale": [0.18, 0.18, 0.18],
                        "rotation": [0.0, 0.6, 0.0],
                    },
                    "ege::MeshRenderer": {
                        "mesh": mesh,
                        "material": material,
                        "visible": True,
                    },
                    "ege::BoxCollider": {
                        "halfExtents": [0.5, 0.5, 0.5],
                        "offset": [0.0, 0.0, 0.0],
                    },
                    "ege::RigidBody": {
                        "mass": 0.4,
                        "kinematic": False,
                        "friction": 0.5,
                        "restitution": 0.25,
                        "linearDamping": 0.05,
                        "angularDamping": 0.05,
                        "gravityFactor": 1.0,
                        "sensor": False,
                    },
                    "ege::PhysicsLayer": {"name": "Props"},
                },
            }
        ],
    }

    path = os.path.join("assets", "prefabs", "pickup.egeprefab")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(document, handle, indent=2)
        handle.write("\n")
    print(f"wrote {path}")
    print(f"  mesh     {mesh}  (mesh:box)")
    print(f"  material {material}  (material:Pickup)")


if __name__ == "__main__":
    main()
