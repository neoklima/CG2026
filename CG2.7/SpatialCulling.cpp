#include "SpatialCulling.h"

#include <limits>
#include <numeric>

using namespace DirectX;

namespace SpatialCulling
{
    void CullLinear(
        const std::vector<BoundingBox>& objectBounds,
        const BoundingFrustum& frustum,
        std::vector<uint32_t>& outVisibleObjects,
        Stats& outStats)
    {
        outVisibleObjects.clear();
        outVisibleObjects.reserve(objectBounds.size());
        outStats = {};

        for(uint32_t objectIndex = 0;
            objectIndex < static_cast<uint32_t>(objectBounds.size());
            ++objectIndex)
        {
            ++outStats.testedObjects;
            if(frustum.Contains(objectBounds[objectIndex]) != DISJOINT)
                outVisibleObjects.push_back(objectIndex);
        }
    }

    void Octree::Build(
        const std::vector<BoundingBox>& objectBounds,
        uint32_t maxDepth,
        uint32_t leafCapacity)
    {
        mRoot.reset();
        if(objectBounds.empty())
            return;

        mMaxDepth = std::max(1u, maxDepth);
        mLeafCapacity = std::max(1u, leafCapacity);

        XMFLOAT3 minimum{
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max()};
        XMFLOAT3 maximum{
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max(),
            -std::numeric_limits<float>::max()};

        for(const BoundingBox& box : objectBounds)
        {
            minimum.x = std::min(minimum.x, box.Center.x - box.Extents.x);
            minimum.y = std::min(minimum.y, box.Center.y - box.Extents.y);
            minimum.z = std::min(minimum.z, box.Center.z - box.Extents.z);
            maximum.x = std::max(maximum.x, box.Center.x + box.Extents.x);
            maximum.y = std::max(maximum.y, box.Center.y + box.Extents.y);
            maximum.z = std::max(maximum.z, box.Center.z + box.Extents.z);
        }

        const XMFLOAT3 center{
            (minimum.x + maximum.x) * 0.5f,
            (minimum.y + maximum.y) * 0.5f,
            (minimum.z + maximum.z) * 0.5f};
        const float halfSize = std::max({
            maximum.x - minimum.x,
            maximum.y - minimum.y,
            maximum.z - minimum.z}) * 0.5f + 0.001f;

        mRoot = std::make_unique<Node>();
        mRoot->bounds = BoundingBox(center, XMFLOAT3(halfSize, halfSize, halfSize));

        std::vector<uint32_t> candidates(objectBounds.size());
        std::iota(candidates.begin(), candidates.end(), 0u);
        BuildNode(*mRoot, objectBounds, std::move(candidates), 0);
    }

    void Octree::BuildNode(
        Node& node,
        const std::vector<BoundingBox>& objectBounds,
        std::vector<uint32_t> candidates,
        uint32_t depth)
    {
        if(depth >= mMaxDepth || candidates.size() <= mLeafCapacity)
        {
            node.objectIndices = std::move(candidates);
            return;
        }

        std::array<std::vector<uint32_t>, 8> childCandidates;
        for(uint32_t objectIndex : candidates)
        {
            const BoundingBox& object = objectBounds[objectIndex];
            uint32_t childIndex = 0;
            if(object.Center.x >= node.bounds.Center.x) childIndex |= 1;
            if(object.Center.y >= node.bounds.Center.y) childIndex |= 2;
            if(object.Center.z >= node.bounds.Center.z) childIndex |= 4;

            const BoundingBox childBounds = ChildBounds(node.bounds, childIndex);
            if(ContainsBox(childBounds, object))
                childCandidates[childIndex].push_back(objectIndex);
            else
                node.objectIndices.push_back(objectIndex);
        }

        for(uint32_t childIndex = 0; childIndex < 8; ++childIndex)
        {
            if(childCandidates[childIndex].empty())
                continue;

            node.children[childIndex] = std::make_unique<Node>();
            node.children[childIndex]->bounds = ChildBounds(node.bounds, childIndex);
            BuildNode(
                *node.children[childIndex], objectBounds,
                std::move(childCandidates[childIndex]), depth + 1);
        }
    }

    void Octree::Query(
        const std::vector<BoundingBox>& objectBounds,
        const BoundingFrustum& frustum,
        std::vector<uint32_t>& outVisibleObjects,
        Stats& outStats) const
    {
        outVisibleObjects.clear();
        outVisibleObjects.reserve(objectBounds.size());
        outStats = {};
        if(mRoot)
            QueryNode(
                *mRoot, objectBounds, frustum, outVisibleObjects,
                outStats);
    }

    void Octree::QueryNode(
        const Node& node,
        const std::vector<BoundingBox>& objectBounds,
        const BoundingFrustum& frustum,
        std::vector<uint32_t>& outVisibleObjects,
        Stats& outStats) const
    {
        ++outStats.testedNodes;
        const ContainmentType nodeVisibility = frustum.Contains(node.bounds);
        if(nodeVisibility == DISJOINT)
            return;

        if(nodeVisibility == CONTAINS)
        {
            ++outStats.acceptedSubtrees;
            CollectAll(node, outVisibleObjects);
            return;
        }

        for(uint32_t objectIndex : node.objectIndices)
        {
            ++outStats.testedObjects;
            if(frustum.Contains(objectBounds[objectIndex]) != DISJOINT)
                outVisibleObjects.push_back(objectIndex);
        }

        for(const std::unique_ptr<Node>& child : node.children)
        {
            if(child)
                QueryNode(
                    *child, objectBounds, frustum, outVisibleObjects,
                    outStats);
        }
    }

    BoundingBox Octree::ChildBounds(const BoundingBox& parent, uint32_t childIndex)
    {
        const XMFLOAT3 childExtents{
            parent.Extents.x * 0.5f,
            parent.Extents.y * 0.5f,
            parent.Extents.z * 0.5f};
        const XMFLOAT3 childCenter{
            parent.Center.x + ((childIndex & 1) ? childExtents.x : -childExtents.x),
            parent.Center.y + ((childIndex & 2) ? childExtents.y : -childExtents.y),
            parent.Center.z + ((childIndex & 4) ? childExtents.z : -childExtents.z)};
        return BoundingBox(childCenter, childExtents);
    }

    bool Octree::ContainsBox(const BoundingBox& container, const BoundingBox& object)
    {
        return
            object.Center.x - object.Extents.x >= container.Center.x - container.Extents.x &&
            object.Center.y - object.Extents.y >= container.Center.y - container.Extents.y &&
            object.Center.z - object.Extents.z >= container.Center.z - container.Extents.z &&
            object.Center.x + object.Extents.x <= container.Center.x + container.Extents.x &&
            object.Center.y + object.Extents.y <= container.Center.y + container.Extents.y &&
            object.Center.z + object.Extents.z <= container.Center.z + container.Extents.z;
    }

    void Octree::CollectAll(const Node& node, std::vector<uint32_t>& outVisibleObjects)
    {
        outVisibleObjects.insert(
            outVisibleObjects.end(), node.objectIndices.begin(), node.objectIndices.end());
        for(const std::unique_ptr<Node>& child : node.children)
        {
            if(child)
                CollectAll(*child, outVisibleObjects);
        }
    }
}
