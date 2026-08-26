#pragma once

#include "core/Guid.hpp"
#include "reflect/Reflect.hpp"

#include <memory>

namespace ege {

    class Material;
    class Model;

    // A reference to an asset: the id that gets stored, and whatever the
    // database last resolved it to.
    //
    // Both halves are load-bearing and only one is saved. The id is what a
    // scene file holds and what survives the file being moved or reimported;
    // the pointer is what the renderer follows, and is rebuilt on load rather
    // than written, because a pointer means nothing in the next process. That
    // asymmetry is the whole reason `MeshRenderer` used to be unserializable.
    template<typename T>
    struct AssetRef {
        Guid id{};
        std::shared_ptr<T> value{};

        T* get() const { return value.get(); }

        bool resolved() const { return value != nullptr; }

        // Identity is the id, not the pointer: two references to the same
        // asset are the same reference whether or not either has resolved.
        friend bool operator==(const AssetRef& a, const AssetRef& b) { return a.id == b.id; }

        friend bool operator!=(const AssetRef& a, const AssetRef& b) { return !(a == b); }
    };

    struct SoundData;

    using MeshRef = AssetRef<Model>;
    using MaterialRef = AssetRef<Material>;
    using SoundRef = AssetRef<SoundData>;

}  // namespace ege

EGE_TYPE_NAME(ege::MeshRef, "MeshRef")
EGE_TYPE_NAME(ege::MaterialRef, "MaterialRef")
EGE_TYPE_NAME(ege::SoundRef, "SoundRef")
