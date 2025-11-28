#pragma once

// Подключаем GLM для математических типов
#include <glm/glm.hpp>

namespace veekay {
namespace geometry {

// Структура, описывающая одну вершину.
// Эти данные будут отправляться в вершинный шейдер.
struct Vertex {
    glm::vec3 position; // Координаты вершины в 3D-пространстве
    glm::vec3 normal;   // Вектор нормали (для расчета освещения)
    glm::vec2 uv;       // Текстурные координаты (для наложения асфальта, и т.д.)
};

} // namespace geometry
} // namespace veekay