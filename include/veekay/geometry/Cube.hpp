#pragma once

#include <vector>
#include <cstdint>
#include "Vertex.hpp" // Используем нашу новую структуру

namespace veekay {
namespace geometry {

// Класс, отвечающий за генерацию геометрии куба.
class Cube {
public:
    Cube();

    // Методы для получения доступа к данным геометрии
    const void* getVerticesData() const;
    size_t getVerticesSizeInBytes() const;
    uint32_t getVertexCount() const;

    const void* getIndicesData() const;
    size_t getIndicesSizeInBytes() const;
    uint32_t getIndexCount() const;

private:
    std::vector<Vertex> m_vertices;
    std::vector<uint32_t> m_indices;
};

} // namespace geometry
} // namespace veekay