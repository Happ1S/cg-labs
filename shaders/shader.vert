#version 450

// Входная позиция вершины
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor; 
// Передача цвета во фрагментный шейдер (если понадобится)
layout(location = 0) out vec3 v_color;

// Push-константы из CPU
layout(push_constant, std430) uniform ShaderConstants {
    mat4 projection;  // матрица перспективы
    mat4 transform;   // модельно-видовая матрица
    vec3 color;       // цвет фигуры
};

void main() {
    // Создаём 4D вектор из позиции вершины
    vec4 pos = vec4(inPosition, 1.0);

    // Применяем модельно-видовую матрицу
    vec4 transformed = transform * pos;

    // Применяем матрицу проекции
    vec4 projected = projection * transformed;

    // ---- Исправление для macOS / MoltenVK ----
    // Переворачиваем ось Y, так как Metal использует другую систему координат
    projected.y = -projected.y;

    // Отправляем финальную позицию в pipeline
    gl_Position = projected;

    // Передаём цвет во фрагментный шейдер
    v_color = inColor;
}