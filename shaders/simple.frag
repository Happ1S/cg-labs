#version 450

// === Входные данные (из Vertex Shader) ===
layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec2 fragUV;    // Приходят уже масштабированные UV
layout(location = 3) in vec3 fragColor; 

layout(location = 0) out vec4 outColor; // Итоговый цвет пикселя

// === Глобальные данные сцены ===
layout(binding = 0) uniform SceneUBO {
    mat4 projection;
    mat4 view;
    vec3 viewPos;
} scene;

// Направленный свет (Луна/Солнце)
layout(binding = 2) uniform DirectionalLightUBO {
    vec4 direction;
    vec4 color;
} dirLight;

// Структуры точечных источников и прожекторов
struct PointLight {
    vec3 position;
    float constant;
    vec4 color;
    float linear;
    float quadratic;
    vec2 padding;
};

struct SpotLight {
    vec3 position;
    float constant;
    vec3 direction;
    float linear;
    vec4 color;
    float quadratic;
    float cutOff;
    float outerCutOff;
    float padding;
};

// Буферы для массивов света
layout(std140, binding = 3) readonly buffer PointLightSSBO {
    PointLight pointLights[];
};

layout(std140, binding = 4) readonly buffer SpotLightSSBO {
    SpotLight spotLights[];
};

// === ТЕКСТУРА ===
// Сэмплер для чтения пикселей из загруженного изображения
layout(binding = 5) uniform sampler2D texSampler;

// === ПАРАМЕТРЫ МАТЕРИАЛА (Push Constants) ===
layout(push_constant) uniform Push {
    mat4 model;
    mat4 normalMatrix;
    vec4 color;
    float metallic;   // 1.0 = металл
    float roughness;  // Шероховатость (0.0 = зеркало, 1.0 = матовый)
    vec2 uvScale;     // Масштаб тайлинга
    float useTexture; // Флаг: использовать ли текстуру
} push;

void main() {
    // 1. Текстурирование
    // Применяем масштаб, если он не был применен в вершинном шейдере 
    vec2 uv = fragUV; 
    
    // Выборка цвета из текстуры по UV координатам
    vec3 texColor = texture(texSampler, uv).rgb;
    
    // Определение базового цвета (Albedo)
    vec3 albedo;
    if (push.useTexture > 0.5) {
        albedo = texColor * push.color.rgb; // Смешиваем текстуру с цветом
    } else {
        albedo = push.color.rgb; // Только цвет
    }
    
    vec3 viewDir = normalize(scene.viewPos - fragWorldPos);
    vec3 normal = normalize(fragNormal);
    
    // === PBR ===
    vec3 F0 = vec3(0.04); 
    F0 = mix(F0, albedo, push.metallic);
    
    // Амбиент
    vec3 ambient = albedo * 0.03;
    
    // === 1. Направленный свет (Луна) ===
    vec3 lightDir = normalize(-dirLight.direction.xyz);
    float diff = max(dot(normal, lightDir), 0.0);
    
    // Диффуз (Lambert), исчезает для металлов
    vec3 diffuse = albedo * dirLight.color.rgb * diff * dirLight.color.a * (1.0 - push.metallic);
    
    // Блик (Specular)
    vec3 reflectDir = reflect(-lightDir, normal);
    
    // Перевод roughness в степень блеска (shininess)
    float shininess = (1.0 - push.roughness) * 256.0;
    if (shininess < 1.0) shininess = 1.0;
    
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), shininess);
    vec3 specular = F0 * spec * dirLight.color.rgb * dirLight.color.a * 2.5;
    
    vec3 totalLight = ambient + diffuse + specular;
    
    // === 2. Точечные источники (Фонари) ===
    for (int i = 0; i < 8 && i < pointLights.length(); i++) {
        vec3 lightPos = pointLights[i].position;
        vec3 lightVec = lightPos - fragWorldPos;
        float distance = length(lightVec);
        vec3 L = normalize(lightVec);
        
        // Затухание света
        float attenuation = 1.0 / (pointLights[i].constant + 
                                   pointLights[i].linear * distance + 
                                   pointLights[i].quadratic * distance * distance);
        
        float diffP = max(dot(normal, L), 0.0);
        vec3 diffuseP = albedo * pointLights[i].color.rgb * diffP * attenuation * pointLights[i].color.a * (1.0 - push.metallic);
        
        vec3 reflectP = reflect(-L, normal);
        float specP = pow(max(dot(viewDir, reflectP), 0.0), shininess);
        vec3 specularP = F0 * specP * pointLights[i].color.rgb * attenuation * pointLights[i].color.a;
        
        totalLight += diffuseP + specularP;
    }
    
    // === 3. Прожекторы (Фары) ===
    for (int i = 0; i < 2 && i < spotLights.length(); i++) {
        vec3 L = normalize(spotLights[i].position - fragWorldPos);
        float distance = length(spotLights[i].position - fragWorldPos);
        vec3 S = normalize(spotLights[i].direction);
        
        // Расчет конуса света
        float theta = dot(-L, S);
        float epsilon = spotLights[i].cutOff - spotLights[i].outerCutOff;
        float intensity = clamp((theta - spotLights[i].outerCutOff) / epsilon, 0.0, 1.0);
        
        if (intensity > 0.0) {
            float attenuation = 1.0 / (spotLights[i].constant + 
                                       spotLights[i].linear * distance + 
                                       spotLights[i].quadratic * distance * distance);
                                       
            float diffS = max(dot(normal, L), 0.0);
            vec3 diffuseS = albedo * spotLights[i].color.rgb * diffS * attenuation * spotLights[i].color.a * intensity * (1.0 - push.metallic);
            
            vec3 reflectS = reflect(-L, normal);
            float specS = pow(max(dot(viewDir, reflectS), 0.0), shininess);
            vec3 specularS = F0 * specS * spotLights[i].color.rgb * attenuation * spotLights[i].color.a * intensity;
            
            totalLight += diffuseS + specularS;
        }
    }
    
    outColor = vec4(totalLight, 1.0);
}
