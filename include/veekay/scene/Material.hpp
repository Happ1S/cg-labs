#pragma once
#include <glm/glm.hpp>

namespace veekay::scene {

struct Material {
    glm::vec3 albedo;      // Базовый цвет
    float padding1;        // Выравнивание для std140
    glm::vec3 specular;    // Цвет бликов
    float shininess;       // Глянцевость (от 1 до 256)
};

} // namespace veekay::scene