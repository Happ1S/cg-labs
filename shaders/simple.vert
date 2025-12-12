#version 450

// === Входные данные вершины) ===
layout(location = 0) in vec3 inPosition; // Позиция (локальная)
layout(location = 1) in vec3 inNormal;   // Нормаль
layout(location = 2) in vec2 inUV;       // Текстурные координаты

// === Выходные данные (во фрагментный шейдер) ===
layout(location = 0) out vec3 fragNormal;      // Нормаль (мировая)
layout(location = 1) out vec3 fragWorldPos;    // Позиция (мировая)
layout(location = 2) out vec2 fragUV;          // Итоговые UV координаты
layout(location = 3) out vec3 fragColor;       // Цвет
layout(location = 4) out float fragMetallic;   // Металличность
layout(location = 5) out float fragRoughness;  // Шероховатость
layout(location = 6) out float fragUseTexture; // Флаг текстуры

layout(binding = 0) uniform SceneUBO {
    mat4 projection;
    mat4 view;
    vec3 viewPos;
} scene;

layout(push_constant) uniform PushConstants {
    mat4 model;        // Матрица трансформации
    mat4 normalMatrix; // Матрица для нормалей
    vec4 color;        // Базовый цвет
    float metallic;
    float roughness;
    vec2 uvScale;      // Масштаб текстуры (тайлинг)
    float useTexture;  // 1.0 - есть текстура, 0.0 - нет
} push;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);
    
    gl_Position = scene.projection * scene.view * worldPos;
    
    fragWorldPos = worldPos.xyz;
    fragNormal = normalize(mat3(push.normalMatrix) * inNormal);
    
    fragUV = inUV * push.uvScale;     
    
    fragColor = push.color.rgb;       
    fragMetallic = push.metallic;
    fragRoughness = push.roughness;
    fragUseTexture = push.useTexture; 
}
