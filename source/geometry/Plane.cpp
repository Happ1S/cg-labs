#include <veekay/geometry/Plane.hpp>

namespace veekay {
namespace geometry {

Plane::Plane(float width, float length) {
    float half_w = width / 2.0f;
    float half_l = length / 2.0f;

    m_vertices = {
        // Позиция,                Нормаль,          UV-координаты
        {{-half_w, 0.0f, -half_l}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ half_w, 0.0f, -half_l}, {0.0f, 1.0f, 0.0f}, {width, 0.0f}},
        {{ half_w, 0.0f,  half_l}, {0.0f, 1.0f, 0.0f}, {width, length}},
        {{-half_w, 0.0f,  half_l}, {0.0f, 1.0f, 0.0f}, {0.0f, length}},
    };

    m_indices = {0, 1, 2, 2, 3, 0};
}

const void* Plane::getVerticesData() const { return m_vertices.data(); }
size_t Plane::getVerticesSizeInBytes() const { return m_vertices.size() * sizeof(Vertex); }
const void* Plane::getIndicesData() const { return m_indices.data(); }
size_t Plane::getIndicesSizeInBytes() const { return m_indices.size() * sizeof(uint32_t); }
uint32_t Plane::getIndexCount() const { return m_indices.size(); }

} // namespace geometry
} // namespace veekay