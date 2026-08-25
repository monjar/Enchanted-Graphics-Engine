// Frame graph compilation.
//
// Everything the graph decides - which passes survive, what order they run
// in, which barriers separate them, which attachments clear and which load -
// is decided by compile() before any Vulkan object is involved, so all of it
// can be pinned here without a GPU. Execution is covered by the CI render
// smoke test, where the validation layers check the same decisions against
// a real driver.

#include "rhi/FrameGraph.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <stdexcept>
#include <string>

using ege::FrameGraph;
using ege::FrameGraphResource;
using ege::ResourceAccess;
using ege::TransientImageDesc;

namespace {

    constexpr VkExtent2D outputExtent{800, 600};

    TransientImageDesc colorDesc() {
        TransientImageDesc desc{};
        desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        return desc;
    }

    TransientImageDesc depthDesc() {
        TransientImageDesc desc{};
        desc.format = VK_FORMAT_D32_SFLOAT;
        desc.clearValue.depthStencil = {1.0f, 0};
        return desc;
    }

    ege::ImportedImageDesc backbufferDesc() {
        ege::ImportedImageDesc desc{};
        desc.format = VK_FORMAT_B8G8R8A8_SRGB;
        desc.extent = outputExtent;
        desc.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        return desc;
    }

    FrameGraphResource importBackbuffer(FrameGraph& graph) {
        return graph.importImage("backbuffer", backbufferDesc());
    }

    void noopExecute(VkCommandBuffer, const ege::FrameGraphResources&) {}

    // The shape the real frame has: a scene pass into an HDR target with
    // depth, then a post pass sampling it into the backbuffer.
    struct SceneAndPost {
        FrameGraphResource sceneColor;
        FrameGraphResource sceneDepth;
        FrameGraphResource backbuffer;
    };

    SceneAndPost declareSceneAndPost(FrameGraph& graph) {
        SceneAndPost frame{};
        frame.sceneColor = graph.createTransient("sceneColor", colorDesc());
        frame.sceneDepth = graph.createTransient("sceneDepth", depthDesc());
        frame.backbuffer = importBackbuffer(graph);

        graph.addPass(
            "scene",
            [&](FrameGraph::PassBuilder& pass) {
                pass.write(frame.sceneColor, ResourceAccess::colorWrite);
                pass.write(frame.sceneDepth, ResourceAccess::depthWrite);
            },
            noopExecute);

        graph.addPass(
            "post",
            [&](FrameGraph::PassBuilder& pass) {
                pass.read(frame.sceneColor, ResourceAccess::sampled);
                pass.write(frame.backbuffer, ResourceAccess::colorWrite);
            },
            noopExecute);

        return frame;
    }

}  // namespace

TEST_CASE("passes run in declaration order") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareSceneAndPost(graph);
    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    CHECK(graph.passName(graph.compiledPasses()[0].passIndex) == "scene");
    CHECK(graph.passName(graph.compiledPasses()[1].passIndex) == "post");
}

TEST_CASE("a pass whose output nothing consumes is culled") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareSceneAndPost(graph);

    FrameGraphResource orphan = graph.createTransient("orphan", colorDesc());
    graph.addPass(
        "deadEnd",
        [&](FrameGraph::PassBuilder& pass) { pass.write(orphan, ResourceAccess::colorWrite); },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    for (const auto& compiledPass : graph.compiledPasses()) {
        CHECK(graph.passName(compiledPass.passIndex) != "deadEnd");
    }
}

TEST_CASE("culling follows chains, not just direct writes") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareSceneAndPost(graph);

    // A writes a, B reads a and writes b - and b goes nowhere, so both die.
    FrameGraphResource a = graph.createTransient("a", colorDesc());
    FrameGraphResource b = graph.createTransient("b", colorDesc());
    graph.addPass(
        "chainA",
        [&](FrameGraph::PassBuilder& pass) { pass.write(a, ResourceAccess::colorWrite); },
        noopExecute);
    graph.addPass(
        "chainB",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(a, ResourceAccess::sampled);
            pass.write(b, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();
    CHECK(graph.compiledPasses().size() == 2);
}

TEST_CASE("first use of a transient transitions from UNDEFINED") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    auto frame = declareSceneAndPost(graph);
    graph.compile();

    const auto& scenePass = graph.compiledPasses()[0];
    REQUIRE(scenePass.barriers.size() == 2);

    const auto& colorBarrier = scenePass.barriers[0];
    CHECK(colorBarrier.resourceIndex == frame.sceneColor.index);
    CHECK(colorBarrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(colorBarrier.newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(colorBarrier.firstUse);

    const auto& depthBarrier = scenePass.barriers[1];
    CHECK(depthBarrier.resourceIndex == frame.sceneDepth.index);
    CHECK(depthBarrier.newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    CHECK(depthBarrier.firstUse);
}

TEST_CASE("read after write gets a render-to-sample barrier") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    auto frame = declareSceneAndPost(graph);
    graph.compile();

    const auto& postPass = graph.compiledPasses()[1];

    const FrameGraph::PlannedBarrier* sampleBarrier = nullptr;
    for (const auto& barrier : postPass.barriers) {
        if (barrier.resourceIndex == frame.sceneColor.index) {
            sampleBarrier = &barrier;
        }
    }
    REQUIRE(sampleBarrier != nullptr);
    CHECK(sampleBarrier->oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(sampleBarrier->newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(sampleBarrier->srcStage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    CHECK(sampleBarrier->srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    CHECK(sampleBarrier->dstStage == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    CHECK(sampleBarrier->dstAccess == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    CHECK_FALSE(sampleBarrier->firstUse);
}

TEST_CASE("the imported backbuffer chains after its declared source stage") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    auto frame = declareSceneAndPost(graph);
    graph.compile();

    const auto& postPass = graph.compiledPasses()[1];
    const FrameGraph::PlannedBarrier* acquireBarrier = nullptr;
    for (const auto& barrier : postPass.barriers) {
        if (barrier.resourceIndex == frame.backbuffer.index) {
            acquireBarrier = &barrier;
        }
    }
    REQUIRE(acquireBarrier != nullptr);
    CHECK(acquireBarrier->oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
    CHECK(acquireBarrier->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(acquireBarrier->srcStage == VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    // Imported images are never patched with recycled-image history.
    CHECK_FALSE(acquireBarrier->firstUse);
}

TEST_CASE("the backbuffer ends the frame in its declared final layout") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    auto frame = declareSceneAndPost(graph);
    graph.compile();

    REQUIRE(graph.finalBarriers().size() == 1);
    const auto& present = graph.finalBarriers()[0];
    CHECK(present.resourceIndex == frame.backbuffer.index);
    CHECK(present.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    CHECK(present.newLayout == VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    CHECK(present.srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // And the transition is made visible to whatever the owner does next -
    // presenting it, or the copy the frame recorder and the occlusion
    // readback both record once the graph has finished.
    CHECK((present.dstAccess & VK_ACCESS_2_MEMORY_READ_BIT) != 0);
}

TEST_CASE("the first writer clears, a second writer loads") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "opaque",
        [&](FrameGraph::PassBuilder& pass) { pass.write(backbuffer, ResourceAccess::colorWrite); },
        noopExecute);
    graph.addPass(
        "overlay",
        [&](FrameGraph::PassBuilder& pass) { pass.write(backbuffer, ResourceAccess::colorWrite); },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    CHECK(graph.compiledPasses()[0].colorAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
    CHECK(graph.compiledPasses()[1].colorAttachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);

    // Same layout both times, but write-after-write still needs the barrier.
    CHECK_FALSE(graph.compiledPasses()[1].barriers.empty());
}

TEST_CASE("results nobody reads are not stored") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    auto frame = declareSceneAndPost(graph);
    graph.compile();

    const auto& scenePass = graph.compiledPasses()[0];
    // The color target is sampled by the post pass; depth is not read again.
    CHECK(scenePass.colorAttachments[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
    REQUIRE(scenePass.depthAttachment.has_value());
    CHECK(scenePass.depthAttachment->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE);

    // The backbuffer is imported: whoever owns it reads it, so it stores.
    const auto& postPass = graph.compiledPasses()[1];
    CHECK(postPass.colorAttachments[0].resourceIndex == frame.backbuffer.index);
    CHECK(postPass.colorAttachments[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
}

TEST_CASE("reading a resource nothing has written is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource backbuffer = importBackbuffer(graph);
    FrameGraphResource never = graph.createTransient("neverWritten", colorDesc());

    graph.addPass(
        "broken",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(never, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    CHECK_THROWS_AS(graph.compile(), std::logic_error);
}

TEST_CASE("a graph with no imported output is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource target = graph.createTransient("target", colorDesc());
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) { pass.write(target, ResourceAccess::colorWrite); },
        noopExecute);

    CHECK_THROWS_AS(graph.compile(), std::logic_error);
}

TEST_CASE("two depth attachments in one pass are refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource backbuffer = importBackbuffer(graph);
    FrameGraphResource depthA = graph.createTransient("depthA", depthDesc());
    FrameGraphResource depthB = graph.createTransient("depthB", depthDesc());

    graph.addPass(
        "broken",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(backbuffer, ResourceAccess::colorWrite);
            pass.write(depthA, ResourceAccess::depthWrite);
            pass.write(depthB, ResourceAccess::depthWrite);
        },
        noopExecute);

    CHECK_THROWS_AS(graph.compile(), std::logic_error);
}

TEST_CASE("beginFrame forgets the previous frame's declarations") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareSceneAndPost(graph);
    graph.compile();
    REQUIRE(graph.compiledPasses().size() == 2);

    graph.beginFrame(outputExtent);
    importBackbuffer(graph);
    graph.compile();
    CHECK(graph.compiledPasses().empty());
    // The untouched backbuffer still has to reach its final layout.
    CHECK(graph.finalBarriers().size() == 1);
    CHECK(graph.finalBarriers()[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
}

TEST_CASE("each layer of an array image clears on its own first write") {
    // The property cascades depend on: writing layer 0 must not make layer 1
    // load whatever layer 0 left, because layer 1 has never been touched this
    // frame. Keying clear-versus-load on the image rather than the layer is
    // the obvious mistake, and it shows up as later cascades inheriting the
    // first one's depth.
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    TransientImageDesc cascades = depthDesc();
    cascades.extent = {1024, 1024};
    cascades.layers = 3;
    FrameGraphResource shadowMap = graph.createTransient("shadowMap", cascades);

    for (uint32_t layer = 0; layer < 3; layer++) {
        graph.addPass(
            "cascade" + std::to_string(layer),
            [&, layer](FrameGraph::PassBuilder& pass) {
                pass.write(shadowMap, ResourceAccess::depthWrite, layer);
            },
            noopExecute);
    }
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(shadowMap, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();
    REQUIRE(graph.compiledPasses().size() == 4);

    for (uint32_t layer = 0; layer < 3; layer++) {
        const auto& depth = graph.compiledPasses()[layer].depthAttachment;
        REQUIRE(depth.has_value());
        CHECK(depth->layer == layer);
        CHECK(depth->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
        // Something samples it afterwards, so every layer is kept.
        CHECK(depth->storeOp == VK_ATTACHMENT_STORE_OP_STORE);
    }
}

TEST_CASE("writing one layer twice loads the second time") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    TransientImageDesc cascades = depthDesc();
    cascades.layers = 2;
    FrameGraphResource shadowMap = graph.createTransient("shadowMap", cascades);

    for (int pass = 0; pass < 2; pass++) {
        graph.addPass(
            "again" + std::to_string(pass),
            [&](FrameGraph::PassBuilder& builder) {
                builder.write(shadowMap, ResourceAccess::depthWrite, 1);
            },
            noopExecute);
    }
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(shadowMap, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();
    REQUIRE(graph.compiledPasses().size() == 3);
    CHECK(graph.compiledPasses()[0].depthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
    CHECK(graph.compiledPasses()[1].depthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
}

TEST_CASE("consecutive layer writes are separated by a barrier") {
    // Layout is tracked per image, so back-to-back writes to different layers
    // are still a write-after-write on the same image and must be ordered.
    // Only the first is a first use.
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    TransientImageDesc cascades = depthDesc();
    cascades.layers = 2;
    FrameGraphResource shadowMap = graph.createTransient("shadowMap", cascades);

    for (uint32_t layer = 0; layer < 2; layer++) {
        graph.addPass(
            "cascade" + std::to_string(layer),
            [&, layer](FrameGraph::PassBuilder& pass) {
                pass.write(shadowMap, ResourceAccess::depthWrite, layer);
            },
            noopExecute);
    }
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(shadowMap, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses()[0].barriers.size() == 1);
    CHECK(graph.compiledPasses()[0].barriers[0].firstUse);
    CHECK(graph.compiledPasses()[0].barriers[0].oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);

    REQUIRE(graph.compiledPasses()[1].barriers.size() == 1);
    CHECK_FALSE(graph.compiledPasses()[1].barriers[0].firstUse);
    CHECK(
        graph.compiledPasses()[1].barriers[0].oldLayout ==
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    // And the render-to-sample transition still happens before the reader.
    const auto& sceneBarriers = graph.compiledPasses()[2].barriers;
    const bool becomesReadable = std::any_of(
        sceneBarriers.begin(), sceneBarriers.end(), [](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        });
    CHECK(becomesReadable);
}

TEST_CASE("a layer the image does not have is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    importBackbuffer(graph);

    TransientImageDesc cascades = depthDesc();
    cascades.layers = 2;
    FrameGraphResource shadowMap = graph.createTransient("shadowMap", cascades);

    CHECK_THROWS_AS(
        graph.addPass(
            "past the end",
            [&](FrameGraph::PassBuilder& pass) {
                pass.write(shadowMap, ResourceAccess::depthWrite, 2);
            },
            noopExecute),
        std::logic_error);
}

// ---- storage buffers -------------------------------------------------------
//
// A compute pass filling a buffer that a raster pass then reads is the shape
// light culling has. The graph has to notice that dependency and separate the
// two, and it has to do so without a layout to key on - which is what makes
// buffers a different case from every image above rather than the same one.

namespace {

    ege::TransientBufferDesc lightListDesc() {
        ege::TransientBufferDesc desc{};
        desc.size = 4096;
        return desc;
    }

    // A cull pass writing a buffer, a scene pass reading it while writing the
    // backbuffer. The minimal graph in which a buffer dependency exists.
    struct CullAndShade {
        FrameGraphResource lightList;
        FrameGraphResource backbuffer;
    };

    CullAndShade declareCullAndShade(FrameGraph& graph) {
        CullAndShade frame{};
        frame.lightList = graph.createTransientBuffer("lightList", lightListDesc());
        frame.backbuffer = importBackbuffer(graph);

        graph.addPass(
            "cull",
            [&](FrameGraph::PassBuilder& pass) {
                pass.write(frame.lightList, ResourceAccess::storageWrite);
            },
            noopExecute);

        graph.addPass(
            "shade",
            [&](FrameGraph::PassBuilder& pass) {
                pass.read(frame.lightList, ResourceAccess::storageRead);
                pass.write(frame.backbuffer, ResourceAccess::colorWrite);
            },
            noopExecute);

        return frame;
    }

}  // namespace

TEST_CASE("a compute pass feeding a raster pass survives culling") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareCullAndShade(graph);
    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    CHECK(graph.passName(graph.compiledPasses()[0].passIndex) == "cull");
    CHECK(graph.passName(graph.compiledPasses()[1].passIndex) == "shade");
}

TEST_CASE("a buffer nobody reads takes its writer with it") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource lightList = graph.createTransientBuffer("lightList", lightListDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "cull",
        [&](FrameGraph::PassBuilder& pass) { pass.write(lightList, ResourceAccess::storageWrite); },
        noopExecute);
    graph.addPass(
        "present",
        [&](FrameGraph::PassBuilder& pass) { pass.write(backbuffer, ResourceAccess::colorWrite); },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 1);
    CHECK(graph.passName(graph.compiledPasses()[0].passIndex) == "present");
}

TEST_CASE("the write-then-read of a buffer is separated by a barrier") {
    // Without this the fragment shader reads a list the compute shader has not
    // finished writing - which on a real driver is a race that usually looks
    // like nothing at all until the scene gets busy enough to expose it.
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareCullAndShade(graph);
    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const auto& shadeBarriers = graph.compiledPasses()[1].barriers;

    const bool chained = std::any_of(
        shadeBarriers.begin(), shadeBarriers.end(), [](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.srcStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                   barrier.srcAccess == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT &&
                   barrier.dstStage == VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT &&
                   barrier.dstAccess == VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        });
    CHECK(chained);
}

TEST_CASE("a buffer barrier asks for no layout transition") {
    // Buffers have no layout. A planned barrier that named one would be
    // recorded against an image barrier struct it does not belong in.
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    const CullAndShade frame = declareCullAndShade(graph);
    graph.compile();

    for (const FrameGraph::CompiledPass& pass : graph.compiledPasses()) {
        for (const FrameGraph::PlannedBarrier& barrier : pass.barriers) {
            if (barrier.resourceIndex != frame.lightList.index) {
                continue;
            }
            CHECK(barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED);
            CHECK(barrier.newLayout == VK_IMAGE_LAYOUT_UNDEFINED);
        }
    }
}

TEST_CASE("the first touch of a recycled buffer still owes a barrier") {
    // The cross-frame hazard: this frame's compute write against last frame's
    // fragment read of the same physical buffer. An image gets this barrier
    // for free because its layout starts UNDEFINED and has to change; a buffer
    // has no layout, so nothing would fire without asking for it explicitly.
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    const CullAndShade frame = declareCullAndShade(graph);
    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const auto& cullBarriers = graph.compiledPasses()[0].barriers;

    const bool firstUse = std::any_of(
        cullBarriers.begin(), cullBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == frame.lightList.index && barrier.firstUse;
        });
    CHECK(firstUse);
}

TEST_CASE("a buffer write produces no attachment") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareCullAndShade(graph);
    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const FrameGraph::CompiledPass& cull = graph.compiledPasses()[0];
    CHECK(cull.colorAttachments.empty());
    CHECK_FALSE(cull.depthAttachment.has_value());
}

TEST_CASE("reading a buffer nothing has written is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource lightList = graph.createTransientBuffer("lightList", lightListDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "shade",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(lightList, ResourceAccess::storageRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    CHECK_THROWS_AS(graph.compile(), std::logic_error);
}

TEST_CASE("an image access on a buffer is refused, and the reverse") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource lightList = graph.createTransientBuffer("lightList", lightListDesc());
    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    importBackbuffer(graph);

    CHECK_THROWS_AS(
        graph.addPass(
            "buffer as attachment",
            [&](FrameGraph::PassBuilder& pass) {
                pass.write(lightList, ResourceAccess::colorWrite);
            },
            noopExecute),
        std::logic_error);

    CHECK_THROWS_AS(
        graph.addPass(
            "image as storage",
            [&](FrameGraph::PassBuilder& pass) {
                pass.write(sceneColor, ResourceAccess::storageWrite);
            },
            noopExecute),
        std::logic_error);
}

TEST_CASE("a transient buffer with no size is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    CHECK_THROWS_AS(
        graph.createTransientBuffer("empty", ege::TransientBufferDesc{}), std::logic_error);
}

// ---- cube images -----------------------------------------------------------
//
// Point-light shadows want the layers of an array read as directions rather
// than as indices. What changes is only how the whole-image view is made; the
// per-layer views a pass renders through are the same 2D views as always.

TEST_CASE("a cube image is written one face at a time like any other layer") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    TransientImageDesc cubeDesc = depthDesc();
    cubeDesc.layers = 6;
    cubeDesc.cube = true;
    FrameGraphResource shadowCube = graph.createTransient("shadowCube", cubeDesc);

    for (uint32_t face = 0; face < 6; face++) {
        graph.addPass(
            "face" + std::to_string(face),
            [&, face](FrameGraph::PassBuilder& pass) {
                pass.write(shadowCube, ResourceAccess::depthWrite, face);
            },
            noopExecute);
    }
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(shadowCube, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 7);
    // Each face is the first write of its own layer, so each clears.
    for (uint32_t face = 0; face < 6; face++) {
        const FrameGraph::CompiledPass& pass = graph.compiledPasses()[face];
        REQUIRE(pass.depthAttachment.has_value());
        CHECK(pass.depthAttachment->layer == face);
        CHECK(pass.depthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
    }
}

TEST_CASE("a cube array holds a whole cube per light") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    importBackbuffer(graph);

    TransientImageDesc cubeDesc = depthDesc();
    cubeDesc.layers = 6 * 4;
    cubeDesc.cube = true;
    // Four lights' worth of faces in one image is the point: one binding and
    // one barrier rather than four of each.
    CHECK_NOTHROW(graph.createTransient("shadowCubes", cubeDesc));
}

TEST_CASE("a cube image without a multiple of six layers is refused") {
    // Caught at declaration, because the alternative is VK_IMAGE_VIEW_TYPE_CUBE
    // failing at view creation with nothing to say about which resource asked
    // for it.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    TransientImageDesc cubeDesc = depthDesc();
    cubeDesc.cube = true;
    cubeDesc.layers = 4;
    CHECK_THROWS_AS(graph.createTransient("notACube", cubeDesc), std::logic_error);

    cubeDesc.layers = 1;
    CHECK_THROWS_AS(graph.createTransient("stillNotACube", cubeDesc), std::logic_error);
}

// ---- multisampling ---------------------------------------------------------
//
// A multisampled attachment is never sampled directly. The pass that renders
// into it names a single-sample image to average into, and that is what
// everything downstream reads - so the graph has to treat the resolve target
// as written here, and has to know not to make it an attachment of its own.

namespace {

    TransientImageDesc multisampledColorDesc() {
        TransientImageDesc desc = colorDesc();
        desc.samples = VK_SAMPLE_COUNT_4_BIT;
        return desc;
    }

}  // namespace

TEST_CASE("a resolve names where the multisampled attachment averages to") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource sceneMs = graph.createTransient("sceneColorMs", multisampledColorDesc());
    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(sceneMs, ResourceAccess::colorWrite);
            pass.resolve(sceneMs, sceneColor);
        },
        noopExecute);
    graph.addPass(
        "post",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(sceneColor, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const FrameGraph::CompiledPass& scene = graph.compiledPasses()[0];
    // One attachment, not two: the resolve target is named by the
    // multisampled attachment rather than being attached beside it.
    REQUIRE(scene.colorAttachments.size() == 1);
    CHECK(scene.colorAttachments[0].resourceIndex == sceneMs.index);
    CHECK(scene.colorAttachments[0].resolveResourceIndex == sceneColor.index);
}

TEST_CASE("the resolve target counts as written, so reading it is legal") {
    // Without this the graph would reject the post pass for reading something
    // nothing produced - the resolve is the only thing that writes it.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource sceneMs = graph.createTransient("sceneColorMs", multisampledColorDesc());
    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(sceneMs, ResourceAccess::colorWrite);
            pass.resolve(sceneMs, sceneColor);
        },
        noopExecute);
    graph.addPass(
        "post",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(sceneColor, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    CHECK_NOTHROW(graph.compile());

    // And the render-to-sample transition is derived for it exactly as for
    // any other attachment a later pass reads.
    const auto& postBarriers = graph.compiledPasses()[1].barriers;
    const bool becomesReadable = std::any_of(
        postBarriers.begin(), postBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == sceneColor.index &&
                   barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                   barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        });
    CHECK(becomesReadable);
}

TEST_CASE("resolving something the pass does not render to is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource sceneMs = graph.createTransient("sceneColorMs", multisampledColorDesc());
    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    // The pass writes the backbuffer as well, so that it survives culling -
    // a pass nothing consumes is dropped before anything is checked about it,
    // which is right but would make this test pass for the wrong reason.
    graph.addPass(
        "forgot to write it",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(backbuffer, ResourceAccess::colorWrite);
            pass.resolve(sceneMs, sceneColor);
        },
        noopExecute);

    CHECK_THROWS_AS(graph.compile(), std::logic_error);
}

TEST_CASE("a resolve between mismatched sample counts is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource sceneMs = graph.createTransient("sceneColorMs", multisampledColorDesc());
    FrameGraphResource alsoMs = graph.createTransient("alsoMs", multisampledColorDesc());
    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    importBackbuffer(graph);

    // A single-sample source has nothing to average.
    CHECK_THROWS_AS(
        graph.addPass(
            "resolve from single",
            [&](FrameGraph::PassBuilder& pass) { pass.resolve(sceneColor, sceneColor); },
            noopExecute),
        std::logic_error);

    // A multisampled destination is not a resolve target.
    CHECK_THROWS_AS(
        graph.addPass(
            "resolve into multisampled",
            [&](FrameGraph::PassBuilder& pass) { pass.resolve(sceneMs, alsoMs); },
            noopExecute),
        std::logic_error);
}

TEST_CASE("single-sample attachments name no resolve at all") {
    // The path taken whenever multisampling is off, which is every frame on a
    // device that cannot do it.
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    declareSceneAndPost(graph);
    graph.compile();

    for (const FrameGraph::CompiledPass& pass : graph.compiledPasses()) {
        for (const FrameGraph::PlannedAttachment& attachment : pass.colorAttachments) {
            CHECK(attachment.resolveResourceIndex == FrameGraphResource::invalidIndex);
        }
    }
}

// ---- depth pre-pass --------------------------------------------------------
//
// Depth laid down by one pass and tested against by another is the first
// producer/consumer pair in this engine that is not a read at all. Nothing
// samples the pre-pass's output; the scene pass consumes it by loading it and
// comparing against it, which the graph has to recognise or it culls the
// producer and discards its result.

namespace {

    TransientImageDesc multisampledDepthDesc() {
        TransientImageDesc desc = depthDesc();
        desc.samples = VK_SAMPLE_COUNT_4_BIT;
        return desc;
    }

}  // namespace

TEST_CASE("a pass whose depth a later pass loads is not culled") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    FrameGraphResource sceneDepth = graph.createTransient("sceneDepth", depthDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "depthPrePass",
        [&](FrameGraph::PassBuilder& pass) { pass.write(sceneDepth, ResourceAccess::depthWrite); },
        noopExecute);
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(sceneColor, ResourceAccess::colorWrite);
            pass.write(sceneDepth, ResourceAccess::depthWrite);
        },
        noopExecute);
    graph.addPass(
        "post",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(sceneColor, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    // Three passes, in order: the pre-pass writes nothing anyone samples, so
    // liveness has to come from the scene pass loading its depth.
    REQUIRE(graph.compiledPasses().size() == 3);
    CHECK(graph.passName(graph.compiledPasses()[0].passIndex) == "depthPrePass");

    const FrameGraph::CompiledPass& prePass = graph.compiledPasses()[0];
    REQUIRE(prePass.depthAttachment.has_value());
    CHECK(prePass.depthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
    // And storing it is the whole point: discarded depth means the scene pass
    // tests EQUAL against an empty buffer and draws nothing at all.
    CHECK(prePass.depthAttachment->storeOp == VK_ATTACHMENT_STORE_OP_STORE);

    const FrameGraph::CompiledPass& scene = graph.compiledPasses()[1];
    REQUIRE(scene.depthAttachment.has_value());
    CHECK(scene.depthAttachment->loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
    // Nothing loads it after the scene pass, so this one still discards.
    CHECK(scene.depthAttachment->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE);
}

TEST_CASE("a writer kept alive only by a later writer still gets its barrier") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    FrameGraphResource sceneDepth = graph.createTransient("sceneDepth", depthDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "depthPrePass",
        [&](FrameGraph::PassBuilder& pass) { pass.write(sceneDepth, ResourceAccess::depthWrite); },
        noopExecute);
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(sceneColor, ResourceAccess::colorWrite);
            pass.write(sceneDepth, ResourceAccess::depthWrite);
        },
        noopExecute);
    graph.addPass(
        "post",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(sceneColor, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    // The pre-pass discards whatever the recycled physical image held.
    const auto& preBarriers = graph.compiledPasses()[0].barriers;
    const bool discards = std::any_of(
        preBarriers.begin(), preBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == sceneDepth.index &&
                   barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && barrier.firstUse;
        });
    CHECK(discards);

    // Same layout on both sides, but write-after-write across two passes is
    // still a hazard: without the barrier the scene pass's depth test can
    // read what the pre-pass has not finished writing.
    const auto& sceneBarriers = graph.compiledPasses()[1].barriers;
    const bool ordered = std::any_of(
        sceneBarriers.begin(), sceneBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == sceneDepth.index &&
                   (barrier.srcAccess & VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT) != 0;
        });
    CHECK(ordered);
}

TEST_CASE("a multisampled depth attachment resolves like a colour one") {
    // What single-sample consumers of depth - screen-space occlusion, the
    // occlusion pyramid - read, since neither can sample a multisampled image.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource sceneColor = graph.createTransient("sceneColor", colorDesc());
    FrameGraphResource depthMs = graph.createTransient("sceneDepthMs", multisampledDepthDesc());
    FrameGraphResource depthResolved = graph.createTransient("sceneDepth", depthDesc());
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "depthPrePass",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(depthMs, ResourceAccess::depthWrite);
            pass.resolve(depthMs, depthResolved);
        },
        noopExecute);
    graph.addPass(
        "occlusion",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(depthResolved, ResourceAccess::sampled);
            pass.write(sceneColor, ResourceAccess::colorWrite);
        },
        noopExecute);
    graph.addPass(
        "post",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(sceneColor, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 3);
    const FrameGraph::CompiledPass& prePass = graph.compiledPasses()[0];
    // The resolve target is named by the depth attachment, not attached
    // beside it - a pass has one depth attachment and this is still one.
    REQUIRE(prePass.depthAttachment.has_value());
    CHECK(prePass.depthAttachment->resourceIndex == depthMs.index);
    CHECK(prePass.depthAttachment->resolveResourceIndex == depthResolved.index);
    CHECK(prePass.colorAttachments.empty());

    // And the resolved copy is readable afterwards on the same terms as a
    // resolved colour target: the resolve is what wrote it.
    const auto& occlusionBarriers = graph.compiledPasses()[1].barriers;
    const bool becomesReadable = std::any_of(
        occlusionBarriers.begin(),
        occlusionBarriers.end(),
        [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == depthResolved.index &&
                   barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        });
    CHECK(becomesReadable);
}

TEST_CASE("resolving depth into colour, or colour into depth, is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource depthMs = graph.createTransient("depthMs", multisampledDepthDesc());
    FrameGraphResource colorMs = graph.createTransient("colorMs", multisampledColorDesc());
    FrameGraphResource color = graph.createTransient("color", colorDesc());
    FrameGraphResource depth = graph.createTransient("depth", depthDesc());
    importBackbuffer(graph);

    CHECK_THROWS_AS(
        graph.addPass(
            "depth into colour",
            [&](FrameGraph::PassBuilder& pass) { pass.resolve(depthMs, color); },
            noopExecute),
        std::logic_error);

    CHECK_THROWS_AS(
        graph.addPass(
            "colour into depth",
            [&](FrameGraph::PassBuilder& pass) { pass.resolve(colorMs, depth); },
            noopExecute),
        std::logic_error);
}

// ---------------------------------------------------------------------------
// Compute reads, imported buffers, and imports whose contents survive.
//
// The four capabilities GPU-driven culling needs from the graph: a compute
// pass sampling an image, a compute pass reading a buffer, a draw reading the
// commands a compute pass wrote, and something that outlives a frame.
// ---------------------------------------------------------------------------

TEST_CASE("a compute pass sampling an image chains after the pass that drew it") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource depth = graph.createTransient("depth", depthDesc());
    FrameGraphResource verdicts =
        graph.createTransientBuffer("verdicts", ege::TransientBufferDesc{4096});
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "prepass",
        [&](FrameGraph::PassBuilder& pass) { pass.write(depth, ResourceAccess::depthWrite); },
        noopExecute);
    graph.addPass(
        "cull",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(depth, ResourceAccess::computeSampled);
            pass.write(verdicts, ResourceAccess::storageWrite);
        },
        noopExecute);
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(verdicts, ResourceAccess::storageRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 3);
    const auto& cullBarriers = graph.compiledPasses()[1].barriers;
    const bool chained = std::any_of(
        cullBarriers.begin(), cullBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == depth.index &&
                   barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   barrier.dstStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                   barrier.dstAccess == VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        });
    CHECK(chained);
}

TEST_CASE("a fragment read after a compute read of the same image needs no transition") {
    // Both land in SHADER_READ_ONLY_OPTIMAL, and read-after-read in an
    // unchanged layout is the one case a barrier may be elided. Getting this
    // wrong costs a pipeline barrier per frame for nothing.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource depth = graph.createTransient("depth", depthDesc());
    FrameGraphResource verdicts =
        graph.createTransientBuffer("verdicts", ege::TransientBufferDesc{4096});
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "prepass",
        [&](FrameGraph::PassBuilder& pass) { pass.write(depth, ResourceAccess::depthWrite); },
        noopExecute);
    graph.addPass(
        "cull",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(depth, ResourceAccess::computeSampled);
            pass.write(verdicts, ResourceAccess::storageWrite);
        },
        noopExecute);
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(depth, ResourceAccess::sampled);
            pass.read(verdicts, ResourceAccess::storageRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 3);
    const auto& sceneBarriers = graph.compiledPasses()[2].barriers;
    const bool touchesDepth = std::any_of(
        sceneBarriers.begin(), sceneBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == depth.index;
        });
    CHECK_FALSE(touchesDepth);
}

TEST_CASE("draw commands written by compute are made visible at the indirect stage") {
    // Not at the vertex stage. The command processor reads a draw's count and
    // parameters before any shader runs, so a barrier naming the vertex stage
    // would come too late for the draw that reads them - and would usually
    // work anyway, until a driver that reads ahead further does not.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource commands =
        graph.createTransientBuffer("drawCommands", ege::TransientBufferDesc{1024});
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "buildCommands",
        [&](FrameGraph::PassBuilder& pass) { pass.write(commands, ResourceAccess::storageWrite); },
        noopExecute);
    graph.addPass(
        "drawIndirect",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(commands, ResourceAccess::indirectRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const auto& drawBarriers = graph.compiledPasses()[1].barriers;
    const bool chained = std::any_of(
        drawBarriers.begin(), drawBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == commands.index &&
                   barrier.srcStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                   barrier.srcAccess == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT &&
                   barrier.dstStage == VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT &&
                   barrier.dstAccess == VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
        });
    CHECK(chained);
}

TEST_CASE("one compute pass reading what another wrote is separated") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource bounds =
        graph.createTransientBuffer("bounds", ege::TransientBufferDesc{1024});
    FrameGraphResource verdicts =
        graph.createTransientBuffer("verdicts", ege::TransientBufferDesc{1024});
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "gather",
        [&](FrameGraph::PassBuilder& pass) { pass.write(bounds, ResourceAccess::storageWrite); },
        noopExecute);
    graph.addPass(
        "test",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(bounds, ResourceAccess::computeStorageRead);
            pass.write(verdicts, ResourceAccess::storageWrite);
        },
        noopExecute);
    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(verdicts, ResourceAccess::storageRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 3);
    const auto& testBarriers = graph.compiledPasses()[1].barriers;
    const bool chained = std::any_of(
        testBarriers.begin(), testBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == bounds.index &&
                   barrier.srcStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                   barrier.dstStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                   barrier.dstAccess == VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        });
    CHECK(chained);
}

TEST_CASE("an imported buffer can be read without any pass having written it") {
    // Which is the whole point of importing one: its contents came from
    // somewhere the graph knows nothing about - the host, or the frame before
    // this one. A transient read before it is written is a bug; this is not.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    ege::ImportedBufferDesc desc{};
    desc.buffer = reinterpret_cast<VkBuffer>(0x1234);
    desc.size = 2048;
    desc.srcStage = VK_PIPELINE_STAGE_2_HOST_BIT;
    desc.srcAccess = VK_ACCESS_2_HOST_WRITE_BIT;

    FrameGraphResource visibility = graph.importBuffer("visibility", desc);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(visibility, ResourceAccess::storageRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 1);
    const auto& barriers = graph.compiledPasses()[0].barriers;
    const bool waitsForTheHost = std::any_of(
        barriers.begin(), barriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == visibility.index &&
                   barrier.srcStage == VK_PIPELINE_STAGE_2_HOST_BIT &&
                   barrier.srcAccess == VK_ACCESS_2_HOST_WRITE_BIT;
        });
    CHECK(waitsForTheHost);
}

TEST_CASE("what the graph wrote into an imported buffer is left visible to its owner") {
    // The buffer counterpart of an imported image's final layout transition.
    // Without it the copy recorded after the graph, or the host read after
    // the fence, sees whatever happens to have reached memory.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    ege::ImportedBufferDesc desc{};
    desc.buffer = reinterpret_cast<VkBuffer>(0x1234);
    desc.size = 2048;

    FrameGraphResource readback = graph.importBuffer("readback", desc);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.write(readback, ResourceAccess::storageWrite);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    const auto& finishing = graph.finalBarriers();
    const bool published = std::any_of(
        finishing.begin(), finishing.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == readback.index &&
                   barrier.srcStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                   barrier.srcAccess == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT &&
                   barrier.dstAccess == VK_ACCESS_2_MEMORY_READ_BIT &&
                   barrier.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
                   barrier.newLayout == VK_IMAGE_LAYOUT_UNDEFINED;
        });
    CHECK(published);
}

TEST_CASE("an imported buffer the graph only read is owed nothing at the end") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    ege::ImportedBufferDesc desc{};
    desc.buffer = reinterpret_cast<VkBuffer>(0x1234);
    desc.size = 2048;

    FrameGraphResource visibility = graph.importBuffer("visibility", desc);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "scene",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(visibility, ResourceAccess::storageRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    const auto& finishing = graph.finalBarriers();
    const bool mentioned = std::any_of(
        finishing.begin(), finishing.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == visibility.index;
        });
    CHECK_FALSE(mentioned);
}

TEST_CASE("an imported buffer with no handle or no size is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    ege::ImportedBufferDesc noHandle{};
    noHandle.size = 16;
    CHECK_THROWS_AS(graph.importBuffer("noHandle", noHandle), std::logic_error);

    ege::ImportedBufferDesc noSize{};
    noSize.buffer = reinterpret_cast<VkBuffer>(0x1234);
    CHECK_THROWS_AS(graph.importBuffer("noSize", noSize), std::logic_error);
}

TEST_CASE("an image access on an imported buffer is refused") {
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    ege::ImportedBufferDesc desc{};
    desc.buffer = reinterpret_cast<VkBuffer>(0x1234);
    desc.size = 16;
    FrameGraphResource imported = graph.importBuffer("imported", desc);

    graph.addPass(
        "wrong",
        [&](FrameGraph::PassBuilder& pass) {
            CHECK_THROWS_AS(pass.read(imported, ResourceAccess::sampled), std::logic_error);
        },
        noopExecute);
}

TEST_CASE("an import whose contents survive is loaded, not cleared") {
    // A discardable import - the swapchain - enters as UNDEFINED and the
    // first pass to write it clears, because last frame's picture is not this
    // frame's. An import that says which layout it is in is saying the
    // opposite: what is in there is this frame's input.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    ege::ImportedImageDesc desc{};
    desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    desc.extent = outputExtent;
    desc.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    desc.srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    FrameGraphResource history = graph.importImage("history", desc);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "accumulate",
        [&](FrameGraph::PassBuilder& pass) { pass.write(history, ResourceAccess::colorWrite); },
        noopExecute);
    graph.addPass(
        "present",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(history, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const auto& attachments = graph.compiledPasses()[0].colorAttachments;
    REQUIRE(attachments.size() == 1);
    CHECK(attachments[0].resourceIndex == history.index);
    CHECK(attachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD);
    CHECK(attachments[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE);
}

TEST_CASE("a discardable import is still cleared on its first write") {
    // The other half of the pair above, pinned so that giving imports a
    // preserved mode did not quietly change what the swapchain does.
    FrameGraph graph;
    graph.beginFrame(outputExtent);
    const SceneAndPost frame = declareSceneAndPost(graph);
    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const auto& attachments = graph.compiledPasses()[1].colorAttachments;
    REQUIRE(attachments.size() == 1);
    CHECK(attachments[0].resourceIndex == frame.backbuffer.index);
    CHECK(attachments[0].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR);
}

TEST_CASE("a preserved import transitions from the layout it says it is in") {
    // Not from UNDEFINED, which is the transition that is allowed to throw
    // the contents away - and the way a driver would be within its rights to
    // hand back an image full of nothing.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    ege::ImportedImageDesc desc{};
    desc.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    desc.extent = outputExtent;
    desc.srcStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    desc.srcAccess = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    FrameGraphResource history = graph.importImage("history", desc);
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "read",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(history, ResourceAccess::sampled);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 1);
    const auto& barriers = graph.compiledPasses()[0].barriers;
    const bool fromItsOwnLayout = std::any_of(
        barriers.begin(), barriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == history.index &&
                   barrier.oldLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL &&
                   barrier.newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
                   barrier.srcAccess == VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT && !barrier.firstUse;
        });
    CHECK(fromItsOwnLayout);
}

TEST_CASE("a compute-compacted buffer read by the vertex stage is separated there") {
    // The instance buffer a culling pass compacts is read by the vertex
    // shader, not the fragment shader - a barrier naming the fragment stage
    // would let the vertex fetch race the compaction and win on some drivers.
    FrameGraph graph;
    graph.beginFrame(outputExtent);

    FrameGraphResource instances =
        graph.createTransientBuffer("instances", ege::TransientBufferDesc{4096});
    FrameGraphResource backbuffer = importBackbuffer(graph);

    graph.addPass(
        "compact",
        [&](FrameGraph::PassBuilder& pass) { pass.write(instances, ResourceAccess::storageWrite); },
        noopExecute);
    graph.addPass(
        "draw",
        [&](FrameGraph::PassBuilder& pass) {
            pass.read(instances, ResourceAccess::vertexRead);
            pass.write(backbuffer, ResourceAccess::colorWrite);
        },
        noopExecute);

    graph.compile();

    REQUIRE(graph.compiledPasses().size() == 2);
    const auto& drawBarriers = graph.compiledPasses()[1].barriers;
    const bool chained = std::any_of(
        drawBarriers.begin(), drawBarriers.end(), [&](const FrameGraph::PlannedBarrier& barrier) {
            return barrier.resourceIndex == instances.index &&
                   barrier.srcStage == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT &&
                   barrier.dstStage == VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT &&
                   barrier.dstAccess == VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        });
    CHECK(chained);
}
