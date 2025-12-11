#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragWorldPos;
layout(location = 2) in vec2 fragUV;
layout(location = 3) in vec3 fragColor;
layout(location = 4) in float fragMetallic;
layout(location = 5) in float fragRoughness;
layout(location = 6) in float fragUseTexture;

layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform SceneUBO {
    mat4 projection;
    mat4 view;
    vec3 viewPos;
} scene;

layout(binding = 2) uniform DirectionalLightUBO {
    vec4 direction;
    vec4 color;
} dirLight;

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

layout(std140, binding = 3) readonly buffer PointLightSSBO {
    PointLight pointLights[];
};

layout(std140, binding = 4) readonly buffer SpotLightSSBO {
    SpotLight spotLights[];
};

layout(binding = 5) uniform sampler2D texSampler;

void main() {
    // === ТЕКСТУРИРОВАНИЕ ===
    // Просто берем пришедшие UV координаты без всяких изменений
    vec3 texColor = texture(texSampler, fragUV).rgb;
    
    // Смешивание: если текстура включена, умножаем цвет на текстуру
    vec3 baseColor;
    if (fragUseTexture > 0.5) {
        baseColor = texColor * fragColor;
    } else {
        baseColor = fragColor;
    }
    
    vec3 viewDir = normalize(scene.viewPos - fragWorldPos);
    vec3 normal = normalize(fragNormal);
    
    // Ambient
    vec3 ambient = baseColor * 0.15;
    
    // === 1. Directional Light (Луна) ===
    vec3 lightDir = normalize(-dirLight.direction.xyz); // Вектор К свету
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = baseColor * dirLight.color.rgb * diff * dirLight.color.a * 1.2 * (1.0 - fragMetallic * 0.3);
    
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32.0 / (fragRoughness + 0.1));
    vec3 specular = dirLight.color.rgb * spec * fragMetallic * 1.2;
    
    // === 2. Point Lights (Фонари) ===
    vec3 pointLightContrib = vec3(0.0);
    
    for (int i = 0; i < 8 && i < pointLights.length(); i++) {
        vec3 lightPos = pointLights[i].position;
        vec3 lightVec = lightPos - fragWorldPos;
        float distance = length(lightVec);
        vec3 lightDirPoint = normalize(lightVec);
        
        float attenuation = 1.0 / (pointLights[i].constant + 
                                   pointLights[i].linear * distance + 
                                   pointLights[i].quadratic * distance * distance);
        
        float diffPoint = max(dot(normal, lightDirPoint), 0.0);
        vec3 diffusePoint = baseColor * pointLights[i].color.rgb * diffPoint * attenuation * pointLights[i].color.a * 2.0;
        
        vec3 reflectDirPoint = reflect(-lightDirPoint, normal);
        float specPoint = pow(max(dot(viewDir, reflectDirPoint), 0.0), 16.0 / (fragRoughness + 0.1));
        vec3 specularPoint = pointLights[i].color.rgb * specPoint * fragMetallic * attenuation * pointLights[i].color.a;
        
        pointLightContrib += diffusePoint + specularPoint;
    }
    
    // === 3. Spot Lights (Фары автомобиля) ===
    vec3 spotLightContrib = vec3(0.0);
    
    for (int i = 0; i < 2 && i < spotLights.length(); i++) {
        vec3 lightPos = spotLights[i].position;
        vec3 lightDirSpot = normalize(lightPos - fragWorldPos); // Вектор от фрагмента к источнику
        float distance = length(lightPos - fragWorldPos);
        
        vec3 spotDir = normalize(spotLights[i].direction);
        // Проверяем угол между вектором "свет->фрагмент" и направлением прожектора
        float theta = dot(-lightDirSpot, spotDir); 
        
        float epsilon = spotLights[i].cutOff - spotLights[i].outerCutOff;
        float spotIntensity = clamp((theta - spotLights[i].outerCutOff) / epsilon, 0.0, 1.0);
        
        if (spotIntensity > 0.0) {
            float attenuation = 1.0 / (spotLights[i].constant + 
                                       spotLights[i].linear * distance + 
                                       spotLights[i].quadratic * distance * distance);
            
            float diffSpot = max(dot(normal, lightDirSpot), 0.0);
            vec3 diffuseSpot = baseColor * spotLights[i].color.rgb * diffSpot * attenuation * spotLights[i].color.a * spotIntensity * 3.5;
            
            vec3 reflectDirSpot = reflect(-lightDirSpot, normal);
            float specSpot = pow(max(dot(viewDir, reflectDirSpot), 0.0), 32.0 / (fragRoughness + 0.1));
            vec3 specularSpot = spotLights[i].color.rgb * specSpot * fragMetallic * attenuation * spotLights[i].color.a * spotIntensity * 2.5;
            
            spotLightContrib += diffuseSpot + specularSpot;
        }
    }
    
    vec3 finalColor = ambient + diffuse + specular + pointLightContrib + spotLightContrib;
    outColor = vec4(finalColor, 1.0);
}
