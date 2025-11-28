#pragma once
#include <glm/glm.hpp>

namespace veekay::scene {

// Максимальное количество источников света (синхронизировано с шейдерами)
constexpr int MAX_POINT_LIGHTS = 32;
constexpr int MAX_SPOT_LIGHTS = 8;

// Направленный свет (солнце/луна)
struct DirectionalLight {
    alignas(16) glm::vec3 direction;  // Направление света
    alignas(16) glm::vec4 color;      // RGB + интенсивность в alpha
};

// Точечный источник света (лампа, фонарь)
struct PointLight {
    alignas(16) glm::vec3 position;   // Позиция в мировых координатах
    float constant;                    // Константа затухания
    alignas(16) glm::vec4 color;      // RGB + интенсивность
    float linear;                      // Линейное затухание
    float quadratic;                   // Квадратичное затухание
    glm::vec2 padding;                 // Выравнивание
};

// Прожектор (spotlight)
struct SpotLight {
    alignas(16) glm::vec3 position;   // Позиция
    float constant;
    alignas(16) glm::vec3 direction;  // Направление луча
    float linear;
    alignas(16) glm::vec4 color;      // RGB + интенсивность
    float quadratic;
    float cutOff;                      // Внутренний угол (cos)
    float outerCutOff;                 // Внешний угол (cos)
    float padding;
};

} // namespace veekay::scene