#pragma once

#include "engine/models/ModelInstance.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace OpenYAMM::Game
{
struct ActorRagdollLaunch
{
    std::array<float, 3> velocity = {};
    // Extra per-joint sideways velocity. Damage types use this to give otherwise equal hits a different motion.
    float twistVelocity = 0.0f;
};

// A lightweight articulated corpse solver. It drives the skin joints of one already-created model instance; gameplay
// position, collision, loot and saves remain owned by the actor runtime. Particles use Verlet integration and fixed
// distance/shape constraints, so the same death settles consistently across render frame rates.
class ActorRagdoll
{
public:
    bool initialize(
        const Engine::ModelAsset &asset,
        const Engine::ModelInstanceSystem &instances,
        Engine::ModelInstanceHandle handle,
        float groundZ,
        uint32_t seed,
        const ActorRagdollLaunch &launch = {});
    void update(float deltaSeconds, float groundZ);

    std::vector<Engine::ModelNodeWorldTransform> nodeWorldTransforms() const;
    std::vector<std::array<float, 3>> particlePositions() const;
    bool active() const { return m_particles.size() >= 2; }
    bool settled() const { return m_settled; }
    size_t particleCount() const { return m_particles.size(); }
    float maximumPrimaryConstraintError() const;

private:
    struct Particle
    {
        uint32_t nodeIndex = 0;
        int parent = -1;
        int directionParticle = -1;
        std::array<float, 3> position = {};
        std::array<float, 3> previousPosition = {};
        std::array<float, 3> sourceDirection = {};
        Engine::ModelMatrix baseMatrix = {};
    };
    struct Constraint
    {
        size_t first = 0;
        size_t second = 0;
        float length = 0.0f;
        float stiffness = 1.0f;
        bool primary = false;
    };

    void simulateStep(float stepSeconds);
    void constrainToFloor(Particle &particle);

    std::vector<Particle> m_particles;
    std::vector<Constraint> m_constraints;
    float m_groundZ = 0.0f;
    float m_collisionRadius = 1.0f;
    float m_accumulator = 0.0f;
    float m_stillSeconds = 0.0f;
    bool m_settled = false;
};
}
