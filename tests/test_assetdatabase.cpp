// The asset database's device-free half.
//
// Cataloguing, sidecar creation and id resolution need no GPU, and they carry
// the property the whole pipeline exists for: a reference keeps resolving
// after the file it names has been moved or renamed. Loading needs a device
// and is covered by the headless render test instead.

#include "assets/AssetDatabase.hpp"
#include "core/Guid.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <string>

using ege::AssetDatabase;
using ege::AssetKind;
using ege::AssetRecord;
using ege::Guid;

namespace {

    namespace fs = std::filesystem;

    // A directory that cleans up after itself, named per test so parallel runs
    // cannot collide.
    class TemporaryTree {
    public:
        explicit TemporaryTree(const std::string& label)
            : root{fs::temp_directory_path() / ("ege-assets-" + label)} {
            std::error_code errorCode;
            fs::remove_all(root, errorCode);
            fs::create_directories(root, errorCode);
        }

        ~TemporaryTree() {
            std::error_code errorCode;
            fs::remove_all(root, errorCode);
        }

        TemporaryTree(const TemporaryTree&) = delete;
        TemporaryTree& operator=(const TemporaryTree&) = delete;

        const fs::path& path() const { return root; }

        fs::path write(const fs::path& relative, const std::string& contents) const {
            const fs::path full = root / relative;
            std::error_code errorCode;
            fs::create_directories(full.parent_path(), errorCode);
            std::ofstream file{full};
            file << contents;
            return full;
        }

    private:
        fs::path root;
    };

}  // namespace

TEST_CASE("scanning catalogues importable files and skips the rest") {
    TemporaryTree tree{"catalogue"};
    tree.write("models/ship.obj", "# an obj\n");
    tree.write("textures/hull.png", "not really a png");
    tree.write("materials/hull.egematerial", "{}");
    tree.write("models/ship.gltf", "{}");
    tree.write("notes.txt", "ignored");
    tree.write("README.md", "ignored");

    AssetDatabase& database = AssetDatabase::instance();
    database.clear();
    const std::size_t catalogued = database.scan(tree.path());

    CHECK(catalogued == 4);
    CHECK(database.all().size() == 4);

    const AssetRecord* mesh = database.findByPath(fs::path{"models"} / "ship.obj");
    REQUIRE(mesh != nullptr);
    CHECK(mesh->kind == AssetKind::mesh);
    CHECK(mesh->name == "ship");
    CHECK_FALSE(mesh->id.isNull());

    CHECK(database.findByPath(fs::path{"textures"} / "hull.png")->kind == AssetKind::texture);
    CHECK(
        database.findByPath(fs::path{"materials"} / "hull.egematerial")->kind ==
        AssetKind::material);
    // A glTF describes a hierarchy, not one mesh, so it catalogues as a scene.
    CHECK(database.findByPath(fs::path{"models"} / "ship.gltf")->kind == AssetKind::scene);
    CHECK(database.findByPath("notes.txt") == nullptr);

    database.clear();
}

TEST_CASE("a sidecar is written once and then honoured") {
    TemporaryTree tree{"sidecar"};
    tree.write("textures/hull.png", "pixels");

    AssetDatabase& database = AssetDatabase::instance();
    database.clear();
    database.scan(tree.path());
    const Guid first = database.all().front().id;

    const fs::path sidecar = tree.path() / "textures" / "hull.png.egameta";
    REQUIRE(fs::exists(sidecar));

    // A second scan must read the id back rather than mint a new one, or
    // every restart would orphan every reference written before it.
    database.clear();
    database.scan(tree.path());
    CHECK(database.all().front().id == first);

    // And the sidecar itself is not an asset.
    CHECK(database.findByPath(fs::path{"textures"} / "hull.png.egameta") == nullptr);

    database.clear();
}

TEST_CASE("an id survives its file being renamed and moved") {
    TemporaryTree tree{"rename"};
    tree.write("textures/hull.png", "pixels");

    AssetDatabase& database = AssetDatabase::instance();
    database.clear();
    database.scan(tree.path());
    const Guid original = database.all().front().id;

    // Move the file and its sidecar together, as any file manager or version
    // control would.
    std::error_code errorCode;
    fs::create_directories(tree.path() / "art", errorCode);
    fs::rename(tree.path() / "textures" / "hull.png", tree.path() / "art" / "plating.png");
    fs::rename(
        tree.path() / "textures" / "hull.png.egameta", tree.path() / "art" / "plating.png.egameta");

    database.clear();
    database.scan(tree.path());

    REQUIRE(database.all().size() == 1);
    const AssetRecord& moved = database.all().front();
    CHECK(moved.id == original);
    CHECK(moved.name == "plating");
    CHECK(moved.path == fs::path{"art"} / "plating.png");
    // Which is the whole point: a scene file holding the old id still finds it.
    CHECK(database.find(original) == &moved);

    database.clear();
}

TEST_CASE("a corrupt sidecar is replaced rather than parsed as a null id") {
    TemporaryTree tree{"corrupt"};
    tree.write("textures/hull.png", "pixels");
    tree.write("textures/hull.png.egameta", "{ this is not json");

    AssetDatabase& database = AssetDatabase::instance();
    database.clear();
    database.scan(tree.path());

    REQUIRE(database.all().size() == 1);
    CHECK_FALSE(database.all().front().id.isNull());

    database.clear();
}

TEST_CASE("assets built in code get stable ids from their names") {
    AssetDatabase& database = AssetDatabase::instance();
    database.clear();

    const Guid box = database.addMesh("box", nullptr);
    const Guid sphere = database.addMesh("sphere", nullptr);

    CHECK(box == Guid::fromName("mesh:box"));
    CHECK(box != sphere);
    REQUIRE(database.find(box) != nullptr);
    CHECK(database.find(box)->builtin);
    CHECK(database.find(box)->kind == AssetKind::mesh);
    // Registering the same name again is the same asset, not a second one.
    CHECK(database.addMesh("box", nullptr) == box);
    CHECK(database.all().size() == 2);

    // A mesh and a material sharing a name are still distinct assets.
    CHECK(database.addMaterial("box", nullptr) != box);

    database.clear();
}

TEST_CASE("sub-asset ids are derived from their container") {
    const Guid container = Guid::generate();
    const Guid other = Guid::generate();

    const Guid first = AssetDatabase::subAssetId(container, AssetKind::mesh, 0);
    const Guid second = AssetDatabase::subAssetId(container, AssetKind::mesh, 1);

    // Stable, so re-importing the container keeps every reference pointing at
    // the same piece of it.
    CHECK(first == AssetDatabase::subAssetId(container, AssetKind::mesh, 0));
    CHECK(first != second);
    // Distinct across containers, and across kinds within one container.
    CHECK(first != AssetDatabase::subAssetId(other, AssetKind::mesh, 0));
    CHECK(first != AssetDatabase::subAssetId(container, AssetKind::material, 0));
}

TEST_CASE("resolving without a device returns null instead of failing") {
    AssetDatabase& database = AssetDatabase::instance();
    database.clear();

    CHECK(database.mesh(Guid::generate()) == nullptr);
    CHECK(database.texture(Guid::generate()) == nullptr);
    CHECK(database.material(Guid::generate()) == nullptr);

    database.clear();
}
