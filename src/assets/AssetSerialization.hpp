#pragma once

namespace ege {

    // Registers the JSON conversions for asset references.
    //
    // Separate from registerBuiltinSerializers because a reference is not a
    // primitive the way a float is: reading one resolves it through the asset
    // database, and `reflect/` has no business knowing that exists. Called by
    // registerBuiltinComponents, since the built-in components are what need
    // these conversions and forgetting the call would show up as a scene that
    // saves and loads with nothing in it.
    void registerAssetSerializers();

}  // namespace ege
