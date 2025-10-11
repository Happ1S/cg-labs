#version 450

// Входной цвет из vertex shader
layout(location = 0) in vec3 v_color;  // <-- ДОБАВЬ ЭТО!

// Выходной цвет пикселя
layout(location = 0) out vec4 final_color;

layout(push_constant, std430) uniform ShaderConstants {
    mat4 projection;
    mat4 transform;
    vec3 color;
};

void main() {
    final_color = vec4(v_color, 1.0);  // <-- используй v_color из vertex shader
}