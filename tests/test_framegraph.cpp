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

    FrameGraphResource importBackbuffer(FrameGraph& graph) {
        return graph.importImage(
            "backbuffer",
            VK_NULL_HANDLE,
            VK_NULL_HANDLE,
            VK_FORMAT_B8G8R8A8_SRGB,
            outputExtent,
            VkClearValue{},
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
