#pragma once

#include <vector>
#include <cstdint>
#include "Vertex.hpp"

namespace veekay {
namespace geometry {

class Sphere {
public:
    Sphere(float radius = 0.5f, int sectorCount = 36, int stackCount = 18);

    const void* getVerticesData() const;
    size_t getVerticesSizeInBytes() const;
    
    const void* getIndicesData() const;
    size_t getIndicesSizeInBytes() const;
    uint32_t getIndexCount() const;

private:
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
};

} // namespace geometry
} // namespace veekay