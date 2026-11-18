#include "game/fx/ActorRagdoll.h"

#include "engine/models/ModelAnimation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace OpenYAMM::Game
{
namespace
{
constexpr float FixedStepSeconds = 1.0f / 120.0f;
constexpr float MinimumBoneLength = 0.05f;

std::array<float, 3> subtract(const std::array<float, 3> &left, const std::array<float, 3> &right)
{
    return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

float dot(const std::array<float, 3> &left, const std::array<float, 3> &right)
{
    return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

std::array<float, 3> cross(const std::array<float, 3> &left, const std::array<float, 3> &right)
{
    return {left[1] * right[2] - left[2] * right[1],
            left[2] * right[0] - left[0] * right[2],
            left[0] * right[1] - left[1] * right[0]};
}

float length(const std::array<float, 3> &value)
{
    return std::sqrt(dot(value, value));
}

std::array<float, 3> normalized(const std::array<float, 3> &value)
{
    const float valueLength = length(value);
    return valueLength > 1.0e-6f
        ? std::array<float, 3>{value[0] / valueLength, value[1] / valueLength, value[2] / valueLength}
        : std::array<float, 3>{1.0f, 0.0f, 0.0f};
}

std::array<float, 4> rotationBetween(
    const std::array<float, 3> &sourceValue,
    const std::array<float, 3> &targetValue)
{
    const std::array<float, 3> source = normalized(sourceValue);
    const std::array<float, 3> target = normalized(targetValue);
    const float cosine = std::clamp(dot(source, target), -1.0f, 1.0f);
    if (cosine > 0.9999f)
    {
        return {0.0f, 0.0f, 0.0f, 1.0f};
    }
    if (cosine < -0.9999f)
    {
        std::array<float, 3> axis = cross(source, {1.0f, 0.0f, 0.0f});
        if (length(axis) < 1.0e-4f)
        {
            axis = cross(source, {0.0f, 1.0f, 0.0f});
        }
        axis = normalized(axis);
        return {axis[0], axis[1], axis[2], 0.0f};
    }
    const std::array<float, 3> axis = cross(source, target);
    const float scale = std::sqrt((1.0f + cosine) * 2.0f);
    return {axis[0] / scale, axis[1] / scale, axis[2] / scale, scale * 0.5f};
}

float randomSigned(uint32_t &state)
{
    state ^= state << 13u;
    state ^= state >> 17u;
    state ^= state << 5u;
    return float(state & 0xffffu) / 32767.5f - 1.0f;
}

std::array<float, 3> matrixPosition(const Engine::ModelMatrix &matrix)
{
    return {matrix[12], matrix[13], matrix[14]};
}
}

bool ActorRagdoll::initialize(
    const Engine::ModelAsset &asset,
    const Engine::ModelInstanceSystem &instances,
    Engine::ModelInstanceHandle handle,
    float groundZ,
    uint32_t seed,
    const ActorRagdollLaunch &launch)
{
    m_particles.clear();
    m_constraints.clear();
    m_groundZ = groundZ;
    m_accumulator = 0.0f;
    m_stillSeconds = 0.0f;
    m_settled = false;

    std::unordered_set<uint32_t> jointNodes;
    for (const Engine::ModelSkin &skin : asset.skins)
    {
        jointNodes.insert(skin.joints.begin(), skin.joints.end());
    }
    if (jointNodes.size() < 2)
    {
        return false;
    }

    std::vector<int> particleByNode(asset.nodes.size(), -1);
    for (uint32_t nodeIndex : asset.hierarchyOrder)
    {
        if (!jointNodes.contains(nodeIndex))
        {
            continue;
        }
        const Engine::ModelMatrix *pMatrix = instances.nodeMatrix(handle, nodeIndex);
        if (pMatrix == nullptr)
        {
            continue;
        }
        int parentParticle = -1;
        int ancestor = asset.nodes[nodeIndex].parentIndex;
        while (ancestor >= 0 && parentParticle < 0)
        {
            parentParticle = particleByNode[size_t(ancestor)];
            ancestor = parentParticle < 0 ? asset.nodes[size_t(ancestor)].parentIndex : -1;
        }
        const std::array<float, 3> position = matrixPosition(*pMatrix);
        if (parentParticle >= 0
            && length(subtract(position, m_particles[size_t(parentParticle)].position)) < MinimumBoneLength)
        {
            particleByNode[nodeIndex] = parentParticle;
            continue;
        }
        Particle particle;
        particle.nodeIndex = nodeIndex;
        particle.parent = parentParticle;
        particle.position = position;
        particle.baseMatrix = *pMatrix;
        particleByNode[nodeIndex] = int(m_particles.size());
        m_particles.push_back(particle);
    }
    if (m_particles.size() < 2)
    {
        m_particles.clear();
        return false;
    }

    const Engine::ModelBounds *pBounds = instances.bounds(handle);
    const float height = pBounds != nullptr && pBounds->valid ? pBounds->max[2] - pBounds->min[2] : 64.0f;
    m_collisionRadius = std::clamp(height * 0.018f, 1.0f, 12.0f);

    for (size_t particleIndex = 0; particleIndex < m_particles.size(); ++particleIndex)
    {
        Particle &particle = m_particles[particleIndex];
        if (particle.parent >= 0)
        {
            const float boneLength = length(subtract(
                particle.position, m_particles[size_t(particle.parent)].position));
            m_constraints.push_back({size_t(particle.parent), particleIndex, boneLength, 1.0f, true});
            const int grandparent = m_particles[size_t(particle.parent)].parent;
            if (grandparent >= 0)
            {
                const float span = length(subtract(
                    particle.position, m_particles[size_t(grandparent)].position));
                if (span > MinimumBoneLength)
                {
                    m_constraints.push_back({size_t(grandparent), particleIndex, span, 0.22f, false});
                }
            }
        }
    }
    // Some exporters use several joint roots under a non-skinned armature node. Link those roots so the pieces form
    // one body instead of falling as disconnected islands.
    size_t firstRoot = m_particles.size();
    for (size_t particleIndex = 0; particleIndex < m_particles.size(); ++particleIndex)
    {
        if (m_particles[particleIndex].parent >= 0)
        {
            continue;
        }
        if (firstRoot == m_particles.size())
        {
            firstRoot = particleIndex;
            continue;
        }
        const float span = length(subtract(m_particles[firstRoot].position, m_particles[particleIndex].position));
        if (span > MinimumBoneLength)
        {
            m_constraints.push_back({firstRoot, particleIndex, span, 1.0f, true});
        }
    }
    // Low-stiffness links between branches keep shoulders, hips and multi-legged bodies from folding into a line,
    // while the primary links remain free to articulate at their joints.
    for (size_t first = 0; first < m_particles.size(); ++first)
    {
        for (size_t second = first + 1; second < m_particles.size(); ++second)
        {
            if (m_particles[first].parent < 0 || m_particles[first].parent != m_particles[second].parent)
            {
                continue;
            }
            const float span = length(subtract(m_particles[first].position, m_particles[second].position));
            if (span > MinimumBoneLength)
            {
                m_constraints.push_back({first, second, span, 0.16f, false});
            }
        }
    }

    for (size_t particleIndex = 0; particleIndex < m_particles.size(); ++particleIndex)
    {
        Particle &particle = m_particles[particleIndex];
        float longestChild = 0.0f;
        for (size_t child = 0; child < m_particles.size(); ++child)
        {
            if (m_particles[child].parent != int(particleIndex))
            {
                continue;
            }
            const float span = length(subtract(m_particles[child].position, particle.position));
            if (span > longestChild)
            {
                longestChild = span;
                particle.directionParticle = int(child);
                particle.sourceDirection = subtract(m_particles[child].position, particle.position);
            }
        }
        if (particle.directionParticle < 0 && particle.parent >= 0)
        {
            particle.directionParticle = particle.parent;
            particle.sourceDirection = subtract(
                particle.position, m_particles[size_t(particle.parent)].position);
        }
    }

    uint32_t randomState = seed != 0 ? seed : 0x6d2b79f5u;
    const float direction = float(seed % 6283u) * 0.001f;
    const std::array<float, 2> forward = {std::cos(direction), std::sin(direction)};
    for (Particle &particle : m_particles)
    {
        const float side = randomSigned(randomState) * (18.0f + launch.twistVelocity);
        const float push = 55.0f + randomSigned(randomState) * 12.0f;
        const float lift = 22.0f + std::abs(randomSigned(randomState)) * 20.0f;
        const float impactVariation = 0.82f + 0.28f * std::abs(randomSigned(randomState));
        const std::array<float, 3> velocity = {
            forward[0] * push - forward[1] * side + launch.velocity[0] * impactVariation,
            forward[1] * push + forward[0] * side + launch.velocity[1] * impactVariation,
            lift + launch.velocity[2] * impactVariation};
        particle.previousPosition = {
            particle.position[0] - velocity[0] * FixedStepSeconds,
            particle.position[1] - velocity[1] * FixedStepSeconds,
            particle.position[2] - velocity[2] * FixedStepSeconds};
        constrainToFloor(particle);
    }
    return true;
}

void ActorRagdoll::update(float deltaSeconds, float groundZ)
{
    if (!active())
    {
        return;
    }
    const float groundShift = groundZ - m_groundZ;
    if (std::isfinite(groundShift) && groundShift != 0.0f)
    {
        for (Particle &particle : m_particles)
        {
            particle.position[2] += groundShift;
            particle.previousPosition[2] += groundShift;
        }
        m_groundZ = groundZ;
    }
    if (m_settled)
    {
        return;
    }
    m_accumulator += std::clamp(deltaSeconds, 0.0f, 0.1f);
    int steps = 0;
    while (m_accumulator >= FixedStepSeconds && steps < 12)
    {
        simulateStep(FixedStepSeconds);
        m_accumulator -= FixedStepSeconds;
        ++steps;
    }
}

void ActorRagdoll::simulateStep(float stepSeconds)
{
    std::vector<std::array<float, 3>> before;
    before.reserve(m_particles.size());
    for (Particle &particle : m_particles)
    {
        before.push_back(particle.position);
        std::array<float, 3> velocity = subtract(particle.position, particle.previousPosition);
        velocity[0] *= 0.992f;
        velocity[1] *= 0.992f;
        velocity[2] *= 0.996f;
        particle.previousPosition = particle.position;
        particle.position[0] += velocity[0];
        particle.position[1] += velocity[1];
        particle.position[2] += velocity[2] - 980.0f * stepSeconds * stepSeconds;
        constrainToFloor(particle);
    }

    for (int iteration = 0; iteration < 7; ++iteration)
    {
        for (const Constraint &constraint : m_constraints)
        {
            Particle &first = m_particles[constraint.first];
            Particle &second = m_particles[constraint.second];
            const std::array<float, 3> difference = subtract(second.position, first.position);
            const float distance = length(difference);
            if (distance <= 1.0e-6f)
            {
                continue;
            }
            const float correction = (distance - constraint.length) / distance * constraint.stiffness * 0.5f;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                first.position[axis] += difference[axis] * correction;
                second.position[axis] -= difference[axis] * correction;
            }
        }
        for (Particle &particle : m_particles)
        {
            constrainToFloor(particle);
        }
    }

    float maximumMovementSquared = 0.0f;
    for (size_t index = 0; index < m_particles.size(); ++index)
    {
        const std::array<float, 3> movement = subtract(m_particles[index].position, before[index]);
        maximumMovementSquared = std::max(maximumMovementSquared, dot(movement, movement));
    }
    const float stillDistance = std::max(0.015f * m_collisionRadius, 0.015f);
    m_stillSeconds = maximumMovementSquared < stillDistance * stillDistance
        ? m_stillSeconds + stepSeconds : 0.0f;
    if (m_stillSeconds >= 0.65f)
    {
        m_settled = true;
        for (Particle &particle : m_particles)
        {
            particle.previousPosition = particle.position;
        }
    }
}

void ActorRagdoll::constrainToFloor(Particle &particle)
{
    const float floor = m_groundZ + m_collisionRadius;
    if (particle.position[2] >= floor)
    {
        return;
    }
    const std::array<float, 3> velocity = subtract(particle.position, particle.previousPosition);
    particle.position[2] = floor;
    particle.previousPosition[0] = particle.position[0] - velocity[0] * 0.72f;
    particle.previousPosition[1] = particle.position[1] - velocity[1] * 0.72f;
    particle.previousPosition[2] = particle.position[2] + velocity[2] * 0.08f;
}

std::vector<Engine::ModelNodeWorldTransform> ActorRagdoll::nodeWorldTransforms() const
{
    std::vector<Engine::ModelNodeWorldTransform> result;
    result.reserve(m_particles.size());
    for (size_t particleIndex = 0; particleIndex < m_particles.size(); ++particleIndex)
    {
        const Particle &particle = m_particles[particleIndex];
        Engine::ModelMatrix matrix = particle.baseMatrix;
        if (particle.directionParticle >= 0)
        {
            const Particle &direction = m_particles[size_t(particle.directionParticle)];
            const std::array<float, 3> targetDirection = particle.directionParticle == particle.parent
                ? subtract(particle.position, direction.position)
                : subtract(direction.position, particle.position);
            Engine::ModelTransform turn;
            turn.rotation = rotationBetween(particle.sourceDirection, targetDirection);
            matrix = Engine::multiplyModelMatrices(Engine::composeModelTransform(turn), matrix);
        }
        matrix[12] = particle.position[0];
        matrix[13] = particle.position[1];
        matrix[14] = particle.position[2];
        result.push_back({particle.nodeIndex, matrix});
    }
    return result;
}

std::vector<std::array<float, 3>> ActorRagdoll::particlePositions() const
{
    std::vector<std::array<float, 3>> result;
    result.reserve(m_particles.size());
    for (const Particle &particle : m_particles)
    {
        result.push_back(particle.position);
    }
    return result;
}

float ActorRagdoll::maximumPrimaryConstraintError() const
{
    float result = 0.0f;
    for (const Constraint &constraint : m_constraints)
    {
        if (constraint.primary)
        {
            result = std::max(result, std::abs(length(subtract(
                m_particles[constraint.second].position,
                m_particles[constraint.first].position)) - constraint.length));
        }
    }
    return result;
}
}
