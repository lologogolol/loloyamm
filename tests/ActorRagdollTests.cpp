#include "game/fx/ActorRagdoll.h"

#include <doctest/doctest.h>

#include <memory>

namespace
{
std::shared_ptr<OpenYAMM::Engine::ModelAsset> makeRagdollAsset()
{
    using namespace OpenYAMM::Engine;
    std::shared_ptr<ModelAsset> asset = std::make_shared<ModelAsset>();
    asset->nodes.resize(5);
    asset->nodes[0].name = "hips";
    asset->nodes[0].transform.translation = {0.0f, 0.0f, 48.0f};
    asset->nodes[0].childIndices = {1, 3};
    asset->nodes[1].name = "spine";
    asset->nodes[1].parentIndex = 0;
    asset->nodes[1].transform.translation = {0.0f, 0.0f, 24.0f};
    asset->nodes[1].childIndices = {2};
    asset->nodes[2].name = "head";
    asset->nodes[2].parentIndex = 1;
    asset->nodes[2].transform.translation = {0.0f, 0.0f, 18.0f};
    asset->nodes[3].name = "leg";
    asset->nodes[3].parentIndex = 0;
    asset->nodes[3].transform.translation = {10.0f, 0.0f, -22.0f};
    asset->nodes[3].childIndices = {4};
    asset->nodes[4].name = "foot";
    asset->nodes[4].parentIndex = 3;
    asset->nodes[4].transform.translation = {0.0f, 0.0f, -20.0f};
    for (ModelNode &node : asset->nodes)
    {
        node.matrix = composeModelTransform(node.transform);
    }
    asset->hierarchyOrder = {0, 1, 2, 3, 4};
    asset->nodeIndicesByName = {{"hips", 0}, {"spine", 1}, {"head", 2}, {"leg", 3}, {"foot", 4}};
    ModelSkin skin;
    skin.joints = {0, 1, 2, 3, 4};
    skin.inverseBindMatrices.resize(skin.joints.size(), identityModelMatrix());
    asset->skins.push_back(std::move(skin));
    return asset;
}
}

TEST_CASE("articulated ragdoll keeps its skeleton constrained, collides with the floor and settles")
{
    using namespace OpenYAMM::Engine;
    using OpenYAMM::Game::ActorRagdoll;
    const std::shared_ptr<ModelAsset> asset = makeRagdollAsset();
    ModelInstanceSystem instances;
    const ModelInstanceHandle handle = instances.create(asset);
    ActorRagdoll ragdoll;
    REQUIRE(ragdoll.initialize(*asset, instances, handle, 0.0f, 42));
    CHECK_EQ(ragdoll.particleCount(), 5);

    for (int step = 0; step < 600 && !ragdoll.settled(); ++step)
    {
        ragdoll.update(1.0f / 60.0f, 0.0f);
    }
    CHECK(ragdoll.settled());
    CHECK_LT(ragdoll.maximumPrimaryConstraintError(), 0.15f);
    for (const std::array<float, 3> &position : ragdoll.particlePositions())
    {
        CHECK_GE(position[2], 0.99f);
    }

    const std::vector<ModelNodeWorldTransform> transforms = ragdoll.nodeWorldTransforms();
    REQUIRE_EQ(transforms.size(), ragdoll.particleCount());
    REQUIRE(instances.setNodeWorldTransforms(handle, transforms));
    for (const ModelNodeWorldTransform &transform : transforms)
    {
        const ModelMatrix *pMatrix = instances.nodeMatrix(handle, transform.nodeIndex);
        REQUIRE(pMatrix != nullptr);
        CHECK((*pMatrix)[12] == doctest::Approx(transform.matrix[12]));
        CHECK((*pMatrix)[13] == doctest::Approx(transform.matrix[13]));
        CHECK((*pMatrix)[14] == doctest::Approx(transform.matrix[14]));
    }
}

TEST_CASE("articulated ragdoll fixed steps are deterministic across render frame rates")
{
    using namespace OpenYAMM::Engine;
    using OpenYAMM::Game::ActorRagdoll;
    const std::shared_ptr<ModelAsset> asset = makeRagdollAsset();
    ModelInstanceSystem instances;
    const ModelInstanceHandle firstHandle = instances.create(asset);
    const ModelInstanceHandle secondHandle = instances.create(asset);
    ActorRagdoll first;
    ActorRagdoll second;
    REQUIRE(first.initialize(*asset, instances, firstHandle, 0.0f, 1234));
    REQUIRE(second.initialize(*asset, instances, secondHandle, 0.0f, 1234));
    for (int frame = 0; frame < 120; ++frame)
    {
        first.update(1.0f / 60.0f, 0.0f);
    }
    for (int frame = 0; frame < 60; ++frame)
    {
        second.update(1.0f / 30.0f, 0.0f);
    }
    const std::vector<std::array<float, 3>> firstPositions = first.particlePositions();
    const std::vector<std::array<float, 3>> secondPositions = second.particlePositions();
    REQUIRE_EQ(firstPositions.size(), secondPositions.size());
    for (size_t particle = 0; particle < firstPositions.size(); ++particle)
    {
        for (size_t axis = 0; axis < 3; ++axis)
        {
            CHECK(firstPositions[particle][axis]
                == doctest::Approx(secondPositions[particle][axis]).epsilon(1.0e-5));
        }
    }
}

TEST_CASE("articulated ragdoll launch impulse flings the detached skeleton")
{
    using namespace OpenYAMM::Engine;
    using OpenYAMM::Game::ActorRagdoll;
    using OpenYAMM::Game::ActorRagdollLaunch;
    const std::shared_ptr<ModelAsset> asset = makeRagdollAsset();
    ModelInstanceSystem instances;
    ActorRagdoll baseline;
    ActorRagdoll launched;
    REQUIRE(baseline.initialize(*asset, instances, instances.create(asset), 0.0f, 17));
    ActorRagdollLaunch launch;
    launch.velocity = {600.0f, 0.0f, 180.0f};
    launch.twistVelocity = 65.0f;
    REQUIRE(launched.initialize(*asset, instances, instances.create(asset), 0.0f, 17, launch));
    for (int frame = 0; frame < 30; ++frame)
    {
        baseline.update(1.0f / 60.0f, 0.0f);
        launched.update(1.0f / 60.0f, 0.0f);
    }
    const std::vector<std::array<float, 3>> baselinePositions = baseline.particlePositions();
    const std::vector<std::array<float, 3>> launchedPositions = launched.particlePositions();
    float baselineCenterX = 0.0f;
    float launchedCenterX = 0.0f;
    for (size_t particle = 0; particle < baselinePositions.size(); ++particle)
    {
        baselineCenterX += baselinePositions[particle][0];
        launchedCenterX += launchedPositions[particle][0];
    }
    baselineCenterX /= float(baselinePositions.size());
    launchedCenterX /= float(launchedPositions.size());
    CHECK_GT(launchedCenterX - baselineCenterX, 150.0f);
    CHECK_LT(launched.maximumPrimaryConstraintError(), 0.15f);
}
