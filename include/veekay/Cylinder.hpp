#pragma once
#include <vector>
#include <cstdint>

namespace geometry {

struct Vector {
    float x, y, z;
};

struct Vertex {
    Vector position;
};

class Cylinder {
public:
    std::vector<Vertex> vertices_;
    std::vector<uint32_t> indices_;
    Cylinder(float radius, float height, uint32_t segments);
    void generate(float radius, float height, uint32_t segments);

    // Вспомогательные геттеры
    size_t getVerticesSizeInBytes() const { return vertices_.size() * sizeof(Vertex); }
    const void* getVerticesData()   const { return vertices_.data(); }
    size_t getIndicesSizeInBytes()  const { return indices_.size() * sizeof(uint32_t); }
    const void* getIndicesData()    const { return indices_.data(); }
    uint32_t getIndexCount()        const { return static_cast<uint32_t>(indices_.size()); }
};

}