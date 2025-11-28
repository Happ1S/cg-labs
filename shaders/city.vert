#version 450

// --- Атрибуты вершин из буфера ---
layout (location = 0) in vec3 inPosition;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec2 inUV;

// --- UBO (Uniform Buffer Objects) ---
// binding = 0: Данные, общие для всей сцены (Камера)
layout(binding = 0, std140) uniform SceneUBO {
    mat4 projection;
    mat4 view;
    vec3 viewPos; // Позиция камеры в мировых координатах
} sceneData;

// --- Push-константы ---
// Для данных, которые меняются для каждого объекта
layout(push_constant) uniform Constants {
    mat4 model;
    mat3 normalMatrix; // Инверсная транспонированная матрица модели для нормалей
} constants;

// --- Выходные данные для фрагментного шейдера ---
layout (location = 0) out vec3 outFragPosWorld; // Позиция фрагмента в мире
layout (location = 1) out vec3 outNormalWorld;  // Нормаль фрагмента в мире
layout (location = 2) out vec2 outUV;

void main() {
    // Преобразуем позицию вершины в мировые координаты
    outFragPosWorld = vec3(constants.model * vec4(inPosition, 1.0));
    
    // Преобразуем нормаль в мировые координаты с помощью специальной матрицы
    // Это нужно для корректной работы с non-uniform масштабированием
    outNormalWorld = normalize(constants.normalMatrix * inNormal);

    // Передаем UV-координаты дальше
    outUV = inUV;

    // Рассчитываем финальную позицию вершины на экране
    gl_Position = sceneData.projection * sceneData.view * vec4(outFragPosWorld, 1.0);
}