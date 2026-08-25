#include "assets/GltfLoader.hpp"

#include "anim/SkeletalAnimator.hpp"
#include "assets/AssetDatabase.hpp"
#include "core/Log.hpp"
#include "scene/Hierarchy.hpp"

#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

// Implementation lives in rhi/Texture.cpp; only the declarations are needed.
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/euler_angles.hpp>

#include <stb_image.h>

#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <unordered_map>

namespace ege::gltf {

    namespace {

        const char* resultName(cgltf_result result) {
            switch (result) {
                case cgltf_result_success:
                    return "success";
                case cgltf_result_data_too_short:
                    return "data too short";
                case cgltf_result_unknown_format:
                    return "unknown format";
                case cgltf_result_invalid_json:
                    return "invalid JSON";
                case cgltf_result_invalid_gltf:
                    return "invalid glTF";
                case cgltf_result_out_of_memory:
                    return "out of memory";
                case cgltf_result_legacy_gltf:
                    return "legacy glTF 1.0, not supported";
                case cgltf_result_file_not_found:
                    return "file not found";
                case cgltf_result_io_error:
                    return "I/O error";
                default:
                    return "unknown error";
            }
        }

        [[noreturn]] void fail(const std::string& what, cgltf_result result) {
            throw std::runtime_error{
                "glTF import failed: " + what + " (" + resultName(result) + ")"};
        }

        // RAII for the cgltf document.
        struct ParsedFile {
            cgltf_data* data = nullptr;

            ~ParsedFile() { cgltf_free(data); }
        };

        const cgltf_accessor* findAttribute(
            const cgltf_primitive& primitive, cgltf_attribute_type type) {
            for (cgltf_size i = 0; i < primitive.attributes_count; i++) {
                // Index 0 of each attribute set (TEXCOORD_0, COLOR_0): the
                // engine has one UV channel today.
                if (primitive.attributes[i].type == type && primitive.attributes[i].index == 0) {
                    return primitive.attributes[i].data;
                }
            }
            return nullptr;
        }

        GltfPrimitiveData readPrimitive(const cgltf_primitive& primitive) {
            GltfPrimitiveData out{};

            const cgltf_accessor* positions =
                findAttribute(primitive, cgltf_attribute_type_position);
            if (positions == nullptr) {
                throw std::runtime_error{"glTF import failed: a primitive has no POSITION"};
            }
            const cgltf_accessor* normals = findAttribute(primitive, cgltf_attribute_type_normal);
            const cgltf_accessor* uvs = findAttribute(primitive, cgltf_attribute_type_texcoord);
            const cgltf_accessor* colors = findAttribute(primitive, cgltf_attribute_type_color);

            const cgltf_size vertexCount = positions->count;
            out.vertices.resize(vertexCount);

            for (cgltf_size i = 0; i < vertexCount; i++) {
                Model::Vertex& vertex = out.vertices[i];

                float value[4] = {0.f, 0.f, 0.f, 1.f};
                cgltf_accessor_read_float(positions, i, value, 3);
                vertex.position = {value[0], value[1], value[2]};

                if (normals != nullptr) {
                    cgltf_accessor_read_float(normals, i, value, 3);
                    vertex.normal = {value[0], value[1], value[2]};
                }

                if (uvs != nullptr) {
                    cgltf_accessor_read_float(uvs, i, value, 2);
                    vertex.uv = {value[0], value[1]};
                }

                if (colors != nullptr) {
                    // COLOR_0 may be vec3 or vec4; alpha is dropped either way.
                    cgltf_accessor_read_float(colors, i, value, 4);
                    vertex.color = {value[0], value[1], value[2]};
                } else {
                    vertex.color = {1.f, 1.f, 1.f};
                }
            }

            // Skinning attributes, when the file carries them. Joint indices
            // are left in the file's own numbering here; buildSceneData
            // remaps them into skeleton order once the skins exist, so this
            // function stays ignorant of rigs.
            const cgltf_accessor* joints = findAttribute(primitive, cgltf_attribute_type_joints);
            const cgltf_accessor* weights = findAttribute(primitive, cgltf_attribute_type_weights);
            if (joints != nullptr && weights != nullptr) {
                out.joints.resize(vertexCount);
                out.weights.resize(vertexCount);
                for (cgltf_size i = 0; i < vertexCount; i++) {
                    cgltf_uint jointIndices[4] = {0, 0, 0, 0};
                    cgltf_accessor_read_uint(joints, i, jointIndices, 4);
                    out.joints[i] = {
                        jointIndices[0], jointIndices[1], jointIndices[2], jointIndices[3]};

                    float weightValues[4] = {0.f, 0.f, 0.f, 0.f};
                    cgltf_accessor_read_float(weights, i, weightValues, 4);
                    glm::vec4 weight{
                        weightValues[0], weightValues[1], weightValues[2], weightValues[3]};
                    const float sum = weight.x + weight.y + weight.z + weight.w;
                    // Renormalised, because quantised weights rarely sum to
                    // exactly one and a skinned vertex scaled by 0.996 is a
                    // model that breathes. A vertex bound to nothing binds
                    // wholly to its first joint instead of to the void.
                    out.weights[i] = sum > 0.f ? weight / sum : glm::vec4{1.f, 0.f, 0.f, 0.f};
                }
            }

            if (primitive.indices != nullptr) {
                out.indices.resize(primitive.indices->count);
                for (cgltf_size i = 0; i < primitive.indices->count; i++) {
                    out.indices[i] =
                        static_cast<uint32_t>(cgltf_accessor_read_index(primitive.indices, i));
                }
            } else {
                out.indices.resize(vertexCount);
                for (cgltf_size i = 0; i < vertexCount; i++) {
                    out.indices[i] = static_cast<uint32_t>(i);
                }
            }

            // Meshes without normals get area-weighted smooth ones - the
            // accumulated cross products weight themselves by triangle area.
            if (normals == nullptr) {
                for (size_t i = 0; i + 2 < out.indices.size(); i += 3) {
                    Model::Vertex& a = out.vertices[out.indices[i]];
                    Model::Vertex& b = out.vertices[out.indices[i + 1]];
                    Model::Vertex& c = out.vertices[out.indices[i + 2]];
                    const glm::vec3 faceNormal =
                        glm::cross(b.position - a.position, c.position - a.position);
                    a.normal += faceNormal;
                    b.normal += faceNormal;
                    c.normal += faceNormal;
                }
                for (Model::Vertex& vertex : out.vertices) {
                    if (glm::dot(vertex.normal, vertex.normal) > 1e-12f) {
                        vertex.normal = glm::normalize(vertex.normal);
                    }
                }
            }

            return out;
        }

        // Decodes one referenced image to RGBA8, from whichever of glTF's
        // three sources it uses: a buffer view, a base64 data URI, or an
        // external file next to the .gltf.
        GltfImageData decodeImage(
            const cgltf_options& options, const cgltf_image& image, const std::string& baseDir) {
            const unsigned char* encoded = nullptr;
            cgltf_size encodedSize = 0;
            void* base64Owned = nullptr;
            std::string filePath;

            if (image.buffer_view != nullptr) {
                encoded = cgltf_buffer_view_data(image.buffer_view);
                encodedSize = image.buffer_view->size;
            } else if (image.uri != nullptr) {
                if (std::strncmp(image.uri, "data:", 5) == 0) {
                    const char* comma = std::strchr(image.uri, ',');
                    if (comma == nullptr) {
                        throw std::runtime_error{"glTF import failed: malformed image data URI"};
                    }
                    // The payload length: three bytes per four base64 chars.
                    const cgltf_size base64Length = std::strlen(comma + 1);
                    encodedSize = base64Length - base64Length / 4;
                    const cgltf_result decoded =
                        cgltf_load_buffer_base64(&options, encodedSize, comma + 1, &base64Owned);
                    if (decoded != cgltf_result_success) {
                        fail("decoding an image data URI", decoded);
                    }
                    encoded = static_cast<const unsigned char*>(base64Owned);
                } else {
                    filePath = baseDir.empty() ? std::string{image.uri} : baseDir + "/" + image.uri;
                }
            }

            int width = 0;
            int height = 0;
            int channels = 0;
            stbi_uc* pixels = nullptr;
            if (encoded != nullptr) {
                pixels = stbi_load_from_memory(
                    encoded, static_cast<int>(encodedSize), &width, &height, &channels, 4);
            } else if (!filePath.empty()) {
                pixels = stbi_load(filePath.c_str(), &width, &height, &channels, 4);
            }
            std::free(base64Owned);

            if (pixels == nullptr) {
                throw std::runtime_error{
                    "glTF import failed: could not decode image '" +
                    std::string{image.uri != nullptr ? image.uri : "<embedded>"} + "'"};
            }

            GltfImageData out{};
            out.width = static_cast<uint32_t>(width);
            out.height = static_cast<uint32_t>(height);
            out.pixels.assign(
                pixels, pixels + static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
            stbi_image_free(pixels);
            return out;
        }

        // The engine's Transform is translation + YXZ Euler + scale; a glTF
        // node is an arbitrary TRS (or matrix). Decompose and convert.
        Transform toTransform(const cgltf_node& node) {
            float local[16];
            cgltf_node_transform_local(&node, local);
            return Transform::fromMatrix(glm::make_mat4(local));
        }

        // A joint's local rest pose, straight from its node. cgltf fills the
        // TRS fields with their defaults when the file omits them, so the
        // only case needing work is a node that gave a whole matrix instead.
        JointPose poseOf(const cgltf_node& node) {
            JointPose pose{};
            if (node.has_matrix) {
                const glm::mat4 matrix = glm::make_mat4(node.matrix);
                pose.translation = glm::vec3{matrix[3]};
                pose.scale = {
                    glm::length(glm::vec3{matrix[0]}),
                    glm::length(glm::vec3{matrix[1]}),
                    glm::length(glm::vec3{matrix[2]})};
                glm::mat3 rotation{matrix};
                rotation[0] /= pose.scale.x;
                rotation[1] /= pose.scale.y;
                rotation[2] /= pose.scale.z;
                pose.rotation = glm::normalize(glm::quat_cast(rotation));
                return pose;
            }
            pose.translation = glm::make_vec3(node.translation);
            // glTF stores quaternions xyzw; glm constructs wxyz.
            pose.rotation =
                glm::quat{node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]};
            pose.scale = glm::make_vec3(node.scale);
            return pose;
        }

        // One rig out of one glTF skin, with the joints reordered so parents
        // precede children - the invariant every pass in AnimationSampling
        // sweeps forward on, and one glTF never promises. `placedByNode`
        // comes back mapping each joint's node to its skeleton index, which
        // is what animation channels and vertex indices are remapped by.
        GltfSkinData readSkin(
            const cgltf_data& data,
            const cgltf_skin& skin,
            std::unordered_map<const cgltf_node*, uint32_t>& placedByNode) {
            GltfSkinData out{};
            out.name = skin.name != nullptr ? skin.name : "skin";

            // Which nodes are joints of this skin, by file order.
            std::unordered_map<const cgltf_node*, cgltf_size> fileOrder;
            for (cgltf_size j = 0; j < skin.joints_count; j++) {
                fileOrder.emplace(skin.joints[j], j);
            }

            // Each joint's parent *joint*: the nearest ancestor node that is
            // also a joint. Plain node parents in between - a rig grouped
            // under helper nodes - fold away here, which is the honest
            // reading: the pose hierarchy is the joints', not the file's.
            auto parentJointOf = [&](const cgltf_node* node) -> const cgltf_node* {
                for (const cgltf_node* up = node->parent; up != nullptr; up = up->parent) {
                    if (fileOrder.count(up) != 0) {
                        return up;
                    }
                }
                return nullptr;
            };

            // Place parents before children: sweep until everything whose
            // parent is placed has been, which for any true hierarchy takes
            // at most one pass per depth level. A file whose joints cycle
            // never finishes, and is refused rather than looped on.
            std::vector<const cgltf_node*> placed;
            placed.reserve(skin.joints_count);
            while (placed.size() < skin.joints_count) {
                const std::size_t before = placed.size();
                for (cgltf_size j = 0; j < skin.joints_count; j++) {
                    const cgltf_node* node = skin.joints[j];
                    if (placedByNode.count(node) != 0) {
                        continue;
                    }
                    const cgltf_node* parent = parentJointOf(node);
                    if (parent == nullptr || placedByNode.count(parent) != 0) {
                        placedByNode.emplace(node, static_cast<uint32_t>(placed.size()));
                        placed.push_back(node);
                    }
                }
                if (placed.size() == before) {
                    throw std::runtime_error{"glTF import failed: a skin's joints form a cycle"};
                }
            }

            out.skeleton.joints.resize(skin.joints_count);
            out.jointNodes.resize(skin.joints_count);
            for (std::size_t j = 0; j < placed.size(); j++) {
                const cgltf_node* node = placed[j];
                Joint& joint = out.skeleton.joints[j];
                joint.name = node->name != nullptr ? node->name : "joint";
                joint.rest = poseOf(*node);
                const cgltf_node* parent = parentJointOf(node);
                joint.parent = parent == nullptr ? -1 : static_cast<int>(placedByNode.at(parent));
                // The accessor holds the matrices in the file's joint order;
                // this joint moved, its matrix comes along.
                if (skin.inverse_bind_matrices != nullptr) {
                    float matrix[16];
                    cgltf_accessor_read_float(
                        skin.inverse_bind_matrices, fileOrder.at(node), matrix, 16);
                    joint.inverseBind = glm::make_mat4(matrix);
                }
                out.jointNodes[j] = static_cast<int>(node - data.nodes);
            }
            return out;
        }

        // Every animation in the file, resolved against one rig. Channels
        // that target nodes outside it - camera moves, prop animations - are
        // counted and skipped rather than guessed at.
        std::vector<AnimationClip> readClips(
            const cgltf_data& data,
            const std::unordered_map<const cgltf_node*, uint32_t>& placedByNode) {
            std::vector<AnimationClip> clips;
            clips.reserve(data.animations_count);

            for (cgltf_size a = 0; a < data.animations_count; a++) {
                const cgltf_animation& animation = data.animations[a];
                AnimationClip clip{};
                clip.name = animation.name != nullptr ? animation.name : "clip" + std::to_string(a);

                std::size_t skipped = 0;
                for (cgltf_size c = 0; c < animation.channels_count; c++) {
                    const cgltf_animation_channel& source = animation.channels[c];
                    const auto joint = source.target_node != nullptr
                                           ? placedByNode.find(source.target_node)
                                           : placedByNode.end();
                    if (joint == placedByNode.end() ||
                        source.target_path == cgltf_animation_path_type_weights) {
                        skipped++;
                        continue;
                    }
                    if (source.sampler->interpolation == cgltf_interpolation_type_cubic_spline) {
                        // Supportable, but not silently half-supported:
                        // reading only the values of a cubic sampler plays
                        // the clip with the wrong easing everywhere.
                        EGE_WARN(
                            "glTF: clip '{}' uses CUBICSPLINE, which is not supported; "
                            "skipping the channel",
                            clip.name);
                        skipped++;
                        continue;
                    }

                    AnimationChannel channel{};
                    channel.joint = joint->second;
                    channel.stepped =
                        source.sampler->interpolation == cgltf_interpolation_type_step;
                    switch (source.target_path) {
                        case cgltf_animation_path_type_translation:
                            channel.path = AnimationPath::translation;
                            break;
                        case cgltf_animation_path_type_rotation:
                            channel.path = AnimationPath::rotation;
                            break;
                        default:
                            channel.path = AnimationPath::scale;
                            break;
                    }

                    const cgltf_accessor* input = source.sampler->input;
                    const cgltf_accessor* output = source.sampler->output;
                    channel.times.resize(input->count);
                    channel.values.resize(output->count);
                    for (cgltf_size k = 0; k < input->count; k++) {
                        cgltf_accessor_read_float(input, k, &channel.times[k], 1);
                    }
                    const cgltf_size components = channel.path == AnimationPath::rotation ? 4 : 3;
                    for (cgltf_size k = 0; k < output->count; k++) {
                        float value[4] = {0.f, 0.f, 0.f, 0.f};
                        cgltf_accessor_read_float(output, k, value, components);
                        channel.values[k] = glm::make_vec4(value);
                    }
                    if (!channel.times.empty()) {
                        clip.duration = std::max(clip.duration, channel.times.back());
                    }
                    clip.channels.push_back(std::move(channel));
                }

                if (skipped != 0) {
                    EGE_WARN(
                        "glTF: clip '{}' skipped {} channel(s) that do not target the rig",
                        clip.name,
                        skipped);
                }
                clips.push_back(std::move(clip));
            }
            return clips;
        }

        GltfSceneData buildSceneData(
            const cgltf_options& options, const cgltf_data& data, const std::string& baseDir) {
            GltfSceneData scene{};

            // Images load lazily, keyed by cgltf image, so unreferenced
            // entries in the file cost nothing and shared ones load once.
            std::unordered_map<const cgltf_image*, int> imageIndex;
            auto imageFor = [&](const cgltf_texture_view& view, bool srgb) -> int {
                if (view.texture == nullptr || view.texture->image == nullptr) {
                    return -1;
                }
                const cgltf_image* image = view.texture->image;
                auto found = imageIndex.find(image);
                if (found == imageIndex.end()) {
                    scene.images.push_back(decodeImage(options, *image, baseDir));
                    found =
                        imageIndex.emplace(image, static_cast<int>(scene.images.size() - 1)).first;
                }
                if (srgb) {
                    scene.images[static_cast<size_t>(found->second)].srgb = true;
                }
                return found->second;
            };

            scene.materials.reserve(data.materials_count);
            for (cgltf_size i = 0; i < data.materials_count; i++) {
                const cgltf_material& source = data.materials[i];
                GltfMaterialData material{};
                material.name = source.name != nullptr ? source.name : "material";

                if (source.has_pbr_metallic_roughness) {
                    const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
                    material.properties.baseColorFactor = glm::make_vec4(pbr.base_color_factor);
                    material.properties.metallicFactor = pbr.metallic_factor;
                    material.properties.roughnessFactor = pbr.roughness_factor;
                    material.baseColorImage = imageFor(pbr.base_color_texture, true);
                    material.metallicRoughnessImage =
                        imageFor(pbr.metallic_roughness_texture, false);
                }

                material.properties.emissiveFactor = glm::make_vec3(source.emissive_factor);
                material.emissiveImage = imageFor(source.emissive_texture, true);

                material.normalImage = imageFor(source.normal_texture, false);
                if (material.normalImage >= 0) {
                    material.properties.normalScale = source.normal_texture.scale;
                }

                // The shader reads occlusion from the metallic-roughness
                // texture's red channel (the ORM packing). That is only
                // real occlusion when the file packs it that way; otherwise
                // strength 0 leaves ambient un-occluded rather than reading
                // noise.
                const bool ormPacked =
                    source.occlusion_texture.texture != nullptr &&
                    source.occlusion_texture.texture ==
                        source.pbr_metallic_roughness.metallic_roughness_texture.texture;
                if (ormPacked) {
                    material.properties.occlusionStrength = source.occlusion_texture.scale;
                } else if (material.metallicRoughnessImage >= 0) {
                    material.properties.occlusionStrength = 0.f;
                }

                if (source.alpha_mode == cgltf_alpha_mode_mask) {
                    material.properties.alphaCutoff = source.alpha_cutoff;
                }

                scene.materials.push_back(std::move(material));
            }

            scene.meshes.reserve(data.meshes_count);
            for (cgltf_size i = 0; i < data.meshes_count; i++) {
                const cgltf_mesh& source = data.meshes[i];
                GltfMeshData mesh{};
                mesh.name = source.name != nullptr ? source.name : "mesh";

                for (cgltf_size p = 0; p < source.primitives_count; p++) {
                    const cgltf_primitive& primitive = source.primitives[p];
                    if (primitive.type != cgltf_primitive_type_triangles) {
                        EGE_WARN("glTF: skipping non-triangle primitive in mesh '{}'", mesh.name);
                        continue;
                    }
                    GltfPrimitiveData data2 = readPrimitive(primitive);
                    data2.material = primitive.material != nullptr
                                         ? static_cast<int>(primitive.material - data.materials)
                                         : -1;
                    mesh.primitives.push_back(std::move(data2));
                }

                scene.meshes.push_back(std::move(mesh));
            }

            scene.nodes.reserve(data.nodes_count);
            for (cgltf_size i = 0; i < data.nodes_count; i++) {
                const cgltf_node& source = data.nodes[i];
                GltfNodeData node{};
                node.name = source.name != nullptr ? source.name : "node";
                node.transform = toTransform(source);
                node.mesh =
                    source.mesh != nullptr ? static_cast<int>(source.mesh - data.meshes) : -1;
                node.skin =
                    source.skin != nullptr ? static_cast<int>(source.skin - data.skins) : -1;
                for (cgltf_size c = 0; c < source.children_count; c++) {
                    node.children.push_back(static_cast<uint32_t>(source.children[c] - data.nodes));
                }
                scene.nodes.push_back(std::move(node));
            }

            // Rigs and their clips. The first skin is the one clips resolve
            // against and vertex joints remap to; a second skin in one file
            // is rare enough to wait for a caller, and is said aloud rather
            // than half-imported.
            if (data.skins_count > 0) {
                std::unordered_map<const cgltf_node*, uint32_t> placedByNode;
                scene.skins.push_back(readSkin(data, data.skins[0], placedByNode));
                if (data.skins_count > 1) {
                    EGE_WARN(
                        "glTF: {} skins in one file; only the first is imported", data.skins_count);
                }
                scene.clips = readClips(data, placedByNode);

                // Vertex joint indices point into the skin's own ordering;
                // remap them into skeleton order so nothing downstream ever
                // meets both. Only meshes a skinned node draws - a shared
                // mesh drawn rigid elsewhere keeps its numbers, and a rigid
                // draw never reads them.
                std::vector<uint32_t> fileToSkeleton(data.skins[0].joints_count, 0);
                for (cgltf_size j = 0; j < data.skins[0].joints_count; j++) {
                    fileToSkeleton[j] = placedByNode.at(data.skins[0].joints[j]);
                }
                for (GltfNodeData& node : scene.nodes) {
                    if (node.skin != 0 || node.mesh < 0) {
                        continue;
                    }
                    for (GltfPrimitiveData& primitive :
                         scene.meshes[static_cast<size_t>(node.mesh)].primitives) {
                        for (glm::uvec4& joints : primitive.joints) {
                            for (int lane = 0; lane < 4; lane++) {
                                if (joints[lane] < fileToSkeleton.size()) {
                                    joints[lane] = fileToSkeleton[joints[lane]];
                                }
                            }
                        }
                    }
                }
            }

            // Roots come from the default scene when one is declared, and
            // from parentless nodes otherwise.
            if (data.scene != nullptr) {
                for (cgltf_size i = 0; i < data.scene->nodes_count; i++) {
                    scene.rootNodes.push_back(
                        static_cast<uint32_t>(data.scene->nodes[i] - data.nodes));
                }
            } else {
                for (cgltf_size i = 0; i < data.nodes_count; i++) {
                    if (data.nodes[i].parent == nullptr) {
                        scene.rootNodes.push_back(static_cast<uint32_t>(i));
                    }
                }
            }

            return scene;
        }

    }  // namespace

    GltfSceneData parseFile(const std::string& path) {
        cgltf_options options{};
        ParsedFile parsed{};

        cgltf_result result = cgltf_parse_file(&options, path.c_str(), &parsed.data);
        if (result != cgltf_result_success) {
            fail("parsing " + path, result);
        }
        result = cgltf_load_buffers(&options, parsed.data, path.c_str());
        if (result != cgltf_result_success) {
            fail("loading buffers for " + path, result);
        }

        const auto slash = path.find_last_of("/\\");
        const std::string baseDir = slash == std::string::npos ? "" : path.substr(0, slash);
        return buildSceneData(options, *parsed.data, baseDir);
    }

    GltfSceneData parseMemory(const void* data, size_t size) {
        cgltf_options options{};
        ParsedFile parsed{};

        cgltf_result result = cgltf_parse(&options, data, size, &parsed.data);
        if (result != cgltf_result_success) {
            fail("parsing memory buffer", result);
        }
        result = cgltf_load_buffers(&options, parsed.data, nullptr);
        if (result != cgltf_result_success) {
            fail("loading embedded buffers", result);
        }

        return buildSceneData(options, *parsed.data, "");
    }

    ImportStats instantiate(
        Device& device,
        World& world,
        const GltfSceneData& scene,
        DescriptorPool& materialPool,
        DescriptorSetLayout& materialLayout,
        const std::string& rootName,
        Guid sourceId) {
        ImportStats stats{};

        // An import with no file behind it - the tests, or anything parsed
        // from memory - still needs stable sub-asset ids, and the root name is
        // the only stable thing it has.
        const Guid source = sourceId.isNull() ? Guid::fromName("gltf:" + rootName) : sourceId;

        std::vector<std::shared_ptr<Texture>> textures;
        textures.reserve(scene.images.size());
        for (const GltfImageData& image : scene.images) {
            TextureConfig config{};
            config.srgb = image.srgb;
            textures.push_back(Texture::fromPixels(
                device, image.pixels.data(), image.width, image.height, config));
        }
        stats.textures = textures.size();

        // Everything this import creates is catalogued under an id derived
        // from the file's own, so a scene saved afterwards can name it and a
        // later run of the same import produces the same ids. That is what
        // makes an imported model referenceable rather than a thing that only
        // exists until the process ends.
        AssetDatabase& database = AssetDatabase::instance();
        for (size_t index = 0; index < textures.size(); index++) {
            database.addTexture(
                AssetDatabase::subAssetId(source, AssetKind::texture, index),
                rootName + ":image" + std::to_string(index),
                textures[index]);
        }

        auto textureAt = [&](int index) -> std::shared_ptr<Texture> {
            return index >= 0 ? textures[static_cast<size_t>(index)] : nullptr;
        };

        std::vector<MaterialRef> materials;
        materials.reserve(scene.materials.size());
        for (size_t index = 0; index < scene.materials.size(); index++) {
            const GltfMaterialData& data = scene.materials[index];
            auto material = std::make_shared<Material>(materialPool, materialLayout);
            material->properties = data.properties;
            material->setBaseColor(textureAt(data.baseColorImage));
            material->setNormalMap(textureAt(data.normalImage));
            material->setMetallicRoughness(textureAt(data.metallicRoughnessImage));
            material->setEmissive(textureAt(data.emissiveImage));
            material->updateDescriptorSet();

            const Guid id = database.addMaterial(
                AssetDatabase::subAssetId(source, AssetKind::material, index),
                data.name.empty() ? rootName + ":material" + std::to_string(index) : data.name,
                material);
            materials.push_back(MaterialRef{id, std::move(material)});
        }
        // Primitives without a material share one default, catalogued after
        // the file's own so its id cannot collide with them.
        MaterialRef defaultMaterial{};

        // Models per (mesh, primitive), indexed flat so that adding a
        // primitive to one mesh does not renumber the ones after it.
        std::vector<std::vector<MeshRef>> models(scene.meshes.size());
        size_t modelIndex = 0;
        for (size_t m = 0; m < scene.meshes.size(); m++) {
            for (const GltfPrimitiveData& primitive : scene.meshes[m].primitives) {
                Model::Builder builder{};
                builder.vertices = primitive.vertices;
                builder.indices = primitive.indices;
                builder.joints = primitive.joints;
                builder.weights = primitive.weights;
                auto model = std::make_shared<Model>(device, builder);

                const Guid id = database.addMesh(
                    AssetDatabase::subAssetId(source, AssetKind::mesh, modelIndex),
                    scene.meshes[m].name.empty()
                        ? rootName + ":mesh" + std::to_string(modelIndex)
                        : scene.meshes[m].name + "." + std::to_string(models[m].size()),
                    model);
                models[m].push_back(MeshRef{id, std::move(model)});
                modelIndex++;
                stats.meshes++;
            }
        }

        // The root carries the +Y-up to -Y-up correction as a half-turn
        // roll, which preserves winding where a mirror would flip it.
        Entity root = world.spawn(rootName);
        Transform rootTransform{};
        rootTransform.rotation.z = glm::pi<float>();
        root.attach<Transform>(rootTransform);
        stats.root = root.id();
        stats.entities++;

        auto attachPrimitive = [&](Entity entity, size_t meshIndex, size_t primitiveIndex) {
            const GltfPrimitiveData& primitive = scene.meshes[meshIndex].primitives[primitiveIndex];
            MaterialRef material{};
            if (primitive.material >= 0) {
                material = materials[static_cast<size_t>(primitive.material)];
            } else {
                if (!defaultMaterial.resolved()) {
                    auto created = std::make_shared<Material>(materialPool, materialLayout);
                    created->updateDescriptorSet();
                    const Guid id = database.addMaterial(
                        AssetDatabase::subAssetId(
                            source, AssetKind::material, scene.materials.size()),
                        rootName + ":default",
                        created);
                    defaultMaterial = MaterialRef{id, std::move(created)};
                }
                material = defaultMaterial;
            }
            MeshRenderer renderer{};
            renderer.mesh = models[meshIndex][primitiveIndex];
            renderer.material = std::move(material);
            renderer.visible = true;
            entity.attach<MeshRenderer>(renderer);
        };

        // Nodes spawn depth-first; a node with several primitives fans them
        // out as children so each keeps its own material.
        // One rig for the whole import, shared by every entity that skins
        // against it - which is what lets two instances of one character
        // hold different poses over the same clips.
        std::shared_ptr<AnimationRig> rig;
        if (!scene.skins.empty()) {
            rig = std::make_shared<AnimationRig>();
            rig->skeleton = scene.skins[0].skeleton;
            rig->clips = scene.clips;
        }

        std::function<void(uint32_t, Entity)> spawnNode = [&](uint32_t nodeIndex, Entity parent) {
            const GltfNodeData& node = scene.nodes[nodeIndex];
            Entity entity = world.spawn(node.name);
            entity.attach<Transform>(node.transform);
            hierarchy::setParent(world, entity.id(), parent.id());
            stats.entities++;

            // A skinned node arrives already animating: an import that has
            // to be wired up before it moves is an import that ships in
            // bind pose to everyone who does not know the wiring.
            if (node.skin == 0 && rig != nullptr && !rig->clips.empty()) {
                SkeletalAnimator animator{};
                animator.rig = rig;
                entity.attach<SkeletalAnimator>(std::move(animator));
            }

            if (node.mesh >= 0) {
                const auto meshIndex = static_cast<size_t>(node.mesh);
                const size_t primitiveCount = scene.meshes[meshIndex].primitives.size();
                if (primitiveCount == 1) {
                    attachPrimitive(entity, meshIndex, 0);
                } else {
                    for (size_t p = 0; p < primitiveCount; p++) {
                        Entity primitiveEntity =
                            world.spawn(scene.meshes[meshIndex].name + ":" + std::to_string(p));
                        primitiveEntity.attach<Transform>(Transform{});
                        hierarchy::setParent(world, primitiveEntity.id(), entity.id());
                        attachPrimitive(primitiveEntity, meshIndex, p);
                        stats.entities++;
                    }
                }
            }

            for (uint32_t child : node.children) {
                spawnNode(child, entity);
            }
        };

        for (uint32_t rootNode : scene.rootNodes) {
            spawnNode(rootNode, root);
        }

        stats.materials = materials.size() + (defaultMaterial.resolved() ? 1 : 0);
        return stats;
    }

}  // namespace ege::gltf
