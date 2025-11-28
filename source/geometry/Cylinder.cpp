#include <veekay/geometry/Cylinder.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace veekay {
namespace geometry {

Cylinder::Cylinder(float radius, float height, uint32_t segments, bool withCaps) {
    m_vertices.clear();
    m_indices.clear();

    float halfHeight = height / 2.0f;

    if (withCaps) {
        // === ВАРИАНТ С КРЫШКАМИ ===
        
        // Генерируем вершины (включая дублированную первую вершину в конце для корректного UV)
        for (uint32_t i = 0; i <= segments; ++i) {  // <= segments для дублирования
            float angle = (float)i / (float)segments * 2.0f * M_PI;
            float x = cos(angle) * radius;
            float z = sin(angle) * radius;
            float u = (float)i / (float)segments;

            // Вершина для ВЕРХНЕЙ крышки (нормаль вверх)
            m_vertices.push_back({ {x, halfHeight, z}, {0, 1, 0}, {(x / radius + 1.0f) * 0.5f, (z / radius + 1.0f) * 0.5f} });
            // Вершина для НИЖНЕЙ крышки (нормаль вниз)
            m_vertices.push_back({ {x, -halfHeight, z}, {0, -1, 0}, {(x / radius + 1.0f) * 0.5f, (z / radius + 1.0f) * 0.5f} });

            // Вершины для БОКОВОЙ поверхности (нормаль от центра)
            glm::vec3 sideNormal = glm::normalize(glm::vec3(x, 0, z));
            m_vertices.push_back({ {x, halfHeight, z}, sideNormal, {u, 1.0f} });
            m_vertices.push_back({ {x, -halfHeight, z}, sideNormal, {u, 0.0f} });
        }

        // Добавляем центральные точки для крышек
        uint32_t topCenterIndex = m_vertices.size();
        m_vertices.push_back({ {0, halfHeight, 0}, {0, 1, 0}, {0.5f, 0.5f} });

        uint32_t bottomCenterIndex = m_vertices.size();
        m_vertices.push_back({ {0, -halfHeight, 0}, {0, -1, 0}, {0.5f, 0.5f} });

        // Генерируем индексы
        for (uint32_t i = 0; i < segments; ++i) {
            uint32_t top_v0 = i * 4;
            uint32_t top_v1 = (i + 1) * 4;
            
            uint32_t bottom_v0 = i * 4 + 1;
            uint32_t bottom_v1 = (i + 1) * 4 + 1;
            
            uint32_t side_top_v0 = i * 4 + 2;
            uint32_t side_top_v1 = (i + 1) * 4 + 2;
            
            uint32_t side_bottom_v0 = i * 4 + 3;
            uint32_t side_bottom_v1 = (i + 1) * 4 + 3;

            // Верхняя крышка
            m_indices.push_back(topCenterIndex);
            m_indices.push_back(top_v0);
            m_indices.push_back(top_v1);

            // Нижняя крышка
            m_indices.push_back(bottomCenterIndex);
            m_indices.push_back(bottom_v1);
            m_indices.push_back(bottom_v0);

            // Боковина
            m_indices.push_back(side_top_v0);
            m_indices.push_back(side_bottom_v0);
            m_indices.push_back(side_top_v1);

            m_indices.push_back(side_bottom_v0);
            m_indices.push_back(side_bottom_v1);
            m_indices.push_back(side_top_v1);
        }
        
    } else {
        // === ВАРИАНТ БЕЗ КРЫШЕК ===
        
        for (uint32_t i = 0; i <= segments; ++i) {  // <= для дублирования
            float angle = (float)i / (float)segments * 2.0f * M_PI;
            float x = cos(angle) * radius;
            float z = sin(angle) * radius;
            float u = (float)i / (float)segments;

            glm::vec3 sideNormal = glm::normalize(glm::vec3(x, 0, z));
            
            m_vertices.push_back({ {x, halfHeight, z}, sideNormal, {u, 1.0f} });
            m_vertices.push_back({ {x, -halfHeight, z}, sideNormal, {u, 0.0f} });
        }

        for (uint32_t i = 0; i < segments; ++i) {
            uint32_t top_v0 = i * 2;
            uint32_t bottom_v0 = i * 2 + 1;
            uint32_t top_v1 = (i + 1) * 2;
            uint32_t bottom_v1 = (i + 1) * 2 + 1;

            m_indices.push_back(top_v0);
            m_indices.push_back(bottom_v0);
            m_indices.push_back(top_v1);

            m_indices.push_back(bottom_v0);
            m_indices.push_back(bottom_v1);
            m_indices.push_back(top_v1);
        }
    }
}

const void* Cylinder::getVerticesData() const { return m_vertices.data(); }
size_t Cylinder::getVerticesSizeInBytes() const { return m_vertices.size() * sizeof(Vertex); }
const void* Cylinder::getIndicesData() const { return m_indices.data(); }
size_t Cylinder::getIndicesSizeInBytes() const { return m_indices.size() * sizeof(uint32_t); }
uint32_t Cylinder::getIndexCount() const { return m_indices.size(); }

} // namespace geometry
} // namespace veekay