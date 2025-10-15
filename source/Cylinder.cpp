#include "veekay/Cylinder.hpp"
#include <cmath>

namespace geometry {

Cylinder::Cylinder(float radius, float height, uint32_t segments) {
    generate(radius, height, segments);
}

void Cylinder::generate(float radius, float height, uint32_t segments) {
    vertices_.clear();
    indices_.clear();

    // Sides
    for (uint32_t i = 0; i < segments; ++i) {
        float angle = 2.0f * M_PI * i / segments;
        float next_angle = 2.0f * M_PI * (i + 1) / segments;

        float x1 = radius * cosf(angle);
        float z1 = radius * sinf(angle);
        float x2 = radius * cosf(next_angle);
        float z2 = radius * sinf(next_angle);

        // Bottom
        Vertex bottom1;
        bottom1.position = {x1, 0.0f, z1};
        bottom1.normal = {x1 / radius, 0.0f, z1 / radius};
        vertices_.push_back(bottom1);

        Vertex bottom2;
        bottom2.position = {x2, 0.0f, z2};
        bottom2.normal = {x2 / radius, 0.0f, z2 / radius};
        vertices_.push_back(bottom2);

        // Top
        Vertex top1;
        top1.position = {x1, height, z1};
        top1.normal = {x1 / radius, 0.0f, z1 / radius};
        vertices_.push_back(top1);

        Vertex top2;
        top2.position = {x2, height, z2};
        top2.normal = {x2 / radius, 0.0f, z2 / radius};
        vertices_.push_back(top2);

        // Indices for side quad
        uint32_t base = vertices_.size() - 4;
        indices_.push_back(base);     // bottom1
        indices_.push_back(base + 2); // top1
        indices_.push_back(base + 1); // bottom2
        indices_.push_back(base + 1); // bottom2
        indices_.push_back(base + 2); // top1
        indices_.push_back(base + 3); // top2
    }

    // Top cap
    uint32_t center_top = vertices_.size();
    Vertex top_center;
    top_center.position = {0.0f, height, 0.0f};
    top_center.normal = {0.0f, 1.0f, 0.0f};
    vertices_.push_back(top_center);

    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t top1 = i * 4 + 2;
        uint32_t top2 = ((i + 1) % segments) * 4 + 2;
        indices_.push_back(center_top);
        indices_.push_back(top2);
        indices_.push_back(top1);
    }

    // Bottom cap
    uint32_t center_bottom = vertices_.size();
    Vertex bottom_center;
    bottom_center.position = {0.0f, 0.0f, 0.0f};
    bottom_center.normal = {0.0f, -1.0f, 0.0f};
    vertices_.push_back(bottom_center);

    for (uint32_t i = 0; i < segments; ++i) {
        uint32_t bottom1 = i * 4;
        uint32_t bottom2 = ((i + 1) % segments) * 4;
        indices_.push_back(center_bottom);
        indices_.push_back(bottom1);
        indices_.push_back(bottom2);
    }
}

}