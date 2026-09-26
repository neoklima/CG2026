#pragma once

#include "Common.h"

namespace SpatialCulling
{
    struct Stats
    {
        uint32_t testedObjects = 0;
        uint32_t testedNodes = 0;
        uint32_t acceptedSubtrees = 0;
    };

    void CullLinear(
        const std::vector<DirectX::BoundingBox>& objectBounds,
        const DirectX::BoundingFrustum& frustum,
        std::vector<uint32_t>& outVisibleObjects,
        Stats& outStats);

    class Octree
    {
    public:
        void Build(
            const std::vector<DirectX::BoundingBox>& objectBounds,
            uint32_t maxDepth = 6,
            uint32_t leafCapacity = 16);

        void Query(
            const std::vector<DirectX::BoundingBox>& objectBounds,
            const DirectX::BoundingFrustum& frustum,
            std::vector<uint32_t>& outVisibleObjects,
            Stats& outStats) const;

        bool Empty() const { return !mRoot; }

    private:
        struct Node
        {
            DirectX::BoundingBox bounds;
            std::vector<uint32_t> objectIndices;
            std::array<std::unique_ptr<Node>, 8> children;
        };

        void BuildNode(
            Node& node,
            const std::vector<DirectX::BoundingBox>& objectBounds,
            std::vector<uint32_t> candidates,
            uint32_t depth);

        void QueryNode(
            const Node& node,
            const std::vector<DirectX::BoundingBox>& objectBounds,
            const DirectX::BoundingFrustum& frustum,
            std::vector<uint32_t>& outVisibleObjects,
            Stats& outStats) const;

        static DirectX::BoundingBox ChildBounds(
            const DirectX::BoundingBox& parent,
            uint32_t childIndex);
        static bool ContainsBox(
            const DirectX::BoundingBox& container,
            const DirectX::BoundingBox& object);
        static void CollectAll(const Node& node, std::vector<uint32_t>& outVisibleObjects);

        std::unique_ptr<Node> mRoot;
        uint32_t mMaxDepth = 6;
        uint32_t mLeafCapacity = 16;
    };
}
