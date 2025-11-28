#include <veekay/geometry/Sphere.hpp>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace veekay {
namespace geometry {

Sphere::Sphere(float radius, int sectorCount, int stackCount) {
    float x, y, z, xy;
    float nx, ny, nz, lengthInv = 1.0f / radius;
    float s, t;

    float sectorStep = 2 * M_PI / sectorCount;
    float stackStep = M_PI / stackCount;
    float sectorAngle, stackAngle;

    for(int i = 0; i <= stackCount; ++i) {
        stackAngle = M_PI / 2 - i * stackStep;
        xy = radius * cosf(stackAngle);
        z = radius * sinf(stackAngle);

        for(int j = 0; j <= sectorCount; ++j) {
            sectorAngle = j * sectorStep;
            x = xy * cosf(sectorAngle);
            y = xy * sinf(sectorAngle);
            nx = x * lengthInv;
            ny = y * lengthInv;
            nz = z * lengthInv;
            s = (float)j / sectorCount;
            t = (float)i / stackCount;
            m_vertices.push_back({{x, y, z}, {nx, ny, nz}, {s, t}});
        }
    }

    int k1, k2;
    for(int i = 0; i < stackCount; ++i) {
        k1 = i * (sectorCount + 1);
        k2 = k1 + sectorCount + 1;
        for(int j = 0; j < sectorCount; ++j, ++k1, ++k2) {
            if (i != 0) {
                m_indices.push_back(k1);
                m_indices.push_back(k2);
                m_indices.push_back(k1 + 1);
            }
            if (i != (stackCount-1)) {
                m_indices.push_back(k1 + 1);
                m_indices.push_back(k2);
                m_indices.push_back(k2 + 1);
            }
        }
    }
}

const void* Sphere::getVerticesData() const { return m_vertices.data(); }
size_t Sphere::getVerticesSizeInBytes() const { return m_vertices.size() * sizeof(Vertex); }
const void* Sphere::getIndicesData() const { return m_indices.data(); }
size_t Sphere::getIndicesSizeInBytes() const { return m_indices.size() * sizeof(uint32_t); }
uint32_t Sphere::getIndexCount() const { return m_indices.size(); }

} // namespace geometry
} // namespace veekay