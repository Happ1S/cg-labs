#include <veekay/geometry/Cube.hpp>

namespace veekay {
namespace geometry {

Cube::Cube() {
    // Для правильного освещения у куба должно быть 24 вершины (по 4 на каждую грань),
    // так как у каждой грани своя уникальная нормаль.
    m_vertices = {
        // Позиция,                Нормаль
        // Передняя грань (+Z)
        { {-0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f} },
        { { 0.5f, -0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f} },
        { { 0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f} },
        { {-0.5f,  0.5f,  0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f} },

        // Задняя грань (-Z)
        { {-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f} },
        { { 0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f} },
        { { 0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f} },
        { {-0.5f,  0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f} },

        // Левая грань (-X)
        { {-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f} },
        { {-0.5f,  0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f} },
        { {-0.5f,  0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f} },
        { {-0.5f, -0.5f,  0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f} },

        // Правая грань (+X)
        { {0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f} },
        { {0.5f,  0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f} },
        { {0.5f,  0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f} },
        { {0.5f, -0.5f,  0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f} },

        // Нижняя грань (-Y)
        { {-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f} },
        { { 0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f} },
        { { 0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f} },
        { {-0.5f, -0.5f,  0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f} },

        // Верхняя грань (+Y)
        { {-0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f} },
        { { 0.5f,  0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f} },
        { { 0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f} },
        { {-0.5f,  0.5f,  0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f} }
    };

    // Индексы для 12 треугольников (6 граней * 2 треугольника * 3 вершины)
    m_indices.resize(36);
    for (uint32_t i = 0; i < 6; ++i) {
        m_indices[i * 6 + 0] = i * 4 + 0;
        m_indices[i * 6 + 1] = i * 4 + 1;
        m_indices[i * 6 + 2] = i * 4 + 2;
        m_indices[i * 6 + 3] = i * 4 + 2;
        m_indices[i * 6 + 4] = i * 4 + 3;
        m_indices[i * 6 + 5] = i * 4 + 0;
    }
}

const void* Cube::getVerticesData() const { return m_vertices.data(); }
size_t Cube::getVerticesSizeInBytes() const { return m_vertices.size() * sizeof(Vertex); }
uint32_t Cube::getVertexCount() const { return m_vertices.size(); }
const void* Cube::getIndicesData() const { return m_indices.data(); }
size_t Cube::getIndicesSizeInBytes() const { return m_indices.size() * sizeof(uint32_t); }
uint32_t Cube::getIndexCount() const { return m_indices.size(); }

} // namespace geometry
} // namespace veekay