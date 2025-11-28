#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;

layout(location = 0) out vec3 outColor;

layout(push_constant) uniform PushConstants {
    mat4 model;
} push;

void main() {
    // Жестко закодированная простая камера
    mat4 view = mat4(
        1.0, 0.0, 0.0, 0.0,
        0.0, 1.0, 0.0, 0.0,
        0.0, 0.0, 1.0, 0.0,
        0.0, 0.0, -5.0, 1.0
    );
    
    mat4 projection = mat4(
        1.5, 0.0, 0.0, 0.0,
        0.0, 2.0, 0.0, 0.0,
        0.0, 0.0, -1.01, -1.0,
        0.0, 0.0, -0.201, 0.0
    );
    
    gl_Position = projection * view * push.model * vec4(inPosition, 1.0);
    outColor = vec3(1.0, 0.0, 1.0); // Яркая магента
}