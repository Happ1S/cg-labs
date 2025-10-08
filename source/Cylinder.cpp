#include "veekay/Cylinder.hpp"
#include <cmath>

namespace geometry {

Cylinder::Cylinder(float radius, float height, uint32_t segments) {
    if (segments < 3) segments = 3; // минимум 3
    generate(radius, height, segments);
}

void Cylinder::generate(float radius, float height, uint32_t segments) {
    if (segments < 3) segments = 3;
    vertices_.clear();
    indices_.clear();

    float halfHeight = height / 2.0f;

    // Боковая поверхность
    for (uint32_t i = 0; i < segments; ++i) {
        float angle = 2.0f * M_PI * i / segments;
        float x = radius * cosf(angle);
        float z = radius * sinf(angle);
        vertices_.push_back({{x, -halfHeight, z}}); // низ
        vertices_.push_back({{x, +halfHeight, z}}); // верх
    }

    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t next = (i + 1) % segments;
        uint32_t v0 = i * 2;
        uint32_t v1 = v0 + 1;
        uint32_t v2 = next * 2;
        uint32_t v3 = v2 + 1;
        indices_.push_back(v0); indices_.push_back(v1); indices_.push_back(v2);
        indices_.push_back(v1); indices_.push_back(v3); indices_.push_back(v2);
    }

    // Нижняя крышка (центр)
    uint32_t bottom_center_idx = vertices_.size();
    vertices_.push_back({{0.0f, -halfHeight, 0.0f}});
    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t v0 = i * 2;
        uint32_t v1 = ((i + 1) % segments) * 2;
        indices_.push_back(bottom_center_idx);
        indices_.push_back(v0);
        indices_.push_back(v1);
    }

    // Верхняя крышка (центр)
    uint32_t top_center_idx = vertices_.size();
    vertices_.push_back({{0.0f, +halfHeight, 0.0f}});
    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t v0 = i * 2 + 1;
        uint32_t v1 = ((i + 1) % segments) * 2 + 1;
        indices_.push_back(top_center_idx);
        indices_.push_back(v1);
        indices_.push_back(v0);
    }
}

} // namespace geometry