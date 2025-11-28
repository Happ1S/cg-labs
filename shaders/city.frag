#version 450

#define MAX_POINT_LIGHTS 16
#define MAX_SPOT_LIGHTS 8

layout (location = 0) in vec3 inFragPosWorld;
layout (location = 1) in vec3 inNormalWorld;
layout (location = 2) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

// Структуры, определенные на верхнем уровне для GLSL
struct PointLight {
    vec3 position;
    vec4 color;
    float constant;
    float linear;
    float quadratic;
    float padding;
};

struct SpotLight {
    vec3 position;
    vec3 direction;
    vec4 color;
    float constant;
    float linear;
    float quadratic;
    float cutOff;
    float outerCutOff;
    vec2 padding;
};

layout(binding = 0, std140) uniform SceneUBO {
    mat4 projection;
    mat4 view;
    vec3 viewPos;
} sceneData;

layout(binding = 1, std140) uniform MaterialUBO {
    vec3 albedo;
    vec3 specular;
    float shininess;
} material;

layout(binding = 2, std140) uniform DirectionalLightUBO {
    vec3 direction;
    vec4 color;
} dirLight;

layout(binding = 3, std430) readonly buffer PointLightsSSBO {
    PointLight pointLights[];
} pointLightData;

layout(binding = 4, std430) readonly buffer SpotLightsSSBO {
    SpotLight spotLights[];
} spotLightData;

vec3 calculateBlinnPhong(vec3 lightDir, vec3 lightColor, float intensity) {
    vec3 N = normalize(inNormalWorld);
    vec3 V = normalize(sceneData.viewPos - inFragPosWorld);
    vec3 H = normalize(lightDir + V);
    float diff = max(dot(N, lightDir), 0.0);
    vec3 diffuse = material.albedo * lightColor * diff * intensity;
    float spec = pow(max(dot(N, H), 0.0), material.shininess);
    vec3 specular = material.specular * lightColor * spec * intensity;
    return diffuse + specular;
}

void main() {
    vec3 ambient = vec3(0.01, 0.01, 0.05) * material.albedo;
    vec3 result = ambient;

    vec3 lightDir = normalize(-dirLight.direction);
    result += calculateBlinnPhong(lightDir, dirLight.color.rgb, dirLight.color.a);
    
    if(pointLightData.pointLights.length() > 0) {
        for (int i = 0; i < pointLightData.pointLights.length(); ++i) {
            PointLight light = pointLightData.pointLights[i];
            vec3 toLight = light.position - inFragPosWorld;
            float distance = length(toLight);
            lightDir = normalize(toLight);
            float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
            result += calculateBlinnPhong(lightDir, light.color.rgb, light.color.a) * attenuation;
        }
    }
    
    if(spotLightData.spotLights.length() > 0) {
        for (int i = 0; i < spotLightData.spotLights.length(); ++i) {
            SpotLight light = spotLightData.spotLights[i];
            vec3 toLight = light.position - inFragPosWorld;
            float distance = length(toLight);
            lightDir = normalize(toLight);
            float theta = dot(lightDir, normalize(-light.direction));
            if (theta > light.outerCutOff) {
                float epsilon = light.cutOff - light.outerCutOff;
                float intensitySpot = clamp((theta - light.outerCutOff) / epsilon, 0.0, 1.0);
                float attenuation = 1.0 / (light.constant + light.linear * distance + light.quadratic * (distance * distance));
                result += calculateBlinnPhong(lightDir, light.color.rgb, light.color.a) * attenuation * intensitySpot;
            }
        }
    }

    outFragColor = vec4(clamp(result, 0.0, 1.0), 1.0);
}