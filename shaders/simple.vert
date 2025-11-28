#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragWorldPos;
layout(location = 2) out vec3 fragColor;
layout(location = 3) out float fragMetallic;
layout(location = 4) out float fragRoughness;

layout(binding = 0) uniform SceneUBO {
    mat4 projection;
    mat4 view;
    vec3 viewPos;
} scene;

layout(push_constant) uniform PushConstants {
    mat4 model;
    mat4 normalMatrix;
    vec4 color;
    float metallic;
    float roughness;
} push;

void main() {
    vec4 worldPos = push.model * vec4(inPosition, 1.0);
    gl_Position = scene.projection * scene.view * worldPos;
    
    fragWorldPos = worldPos.xyz;
    fragNormal = normalize(mat3(push.normalMatrix) * inNormal);
    fragColor = push.color.rgb;
    fragMetallic = push.metallic;
    fragRoughness = push.roughness;
}