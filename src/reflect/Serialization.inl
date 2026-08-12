// Template definitions for reflect/Serialization.hpp.

namespace ege {

    template <typename T>
    void Serializer::registerLeaf(Writer writer, Reader reader) {
        converters[&TypeRegistry::of<T>()] = Converter{std::move(writer), std::move(reader)};
    }

}  // namespace ege
