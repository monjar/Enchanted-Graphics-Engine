#pragma once

namespace ege {

    // Registers the JSON conversion for the Script component.
    //
    // Separate from the reflection-driven path because a behaviour list is not
    // something reflection can describe: the concrete type of each entry is
    // known only at runtime, from a name. Called by registerBuiltinComponents
    // alongside the asset conversions, for the same reason.
    void registerScriptSerializer();

}  // namespace ege
