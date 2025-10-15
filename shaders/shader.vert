#version 450

layout (location = 0) in vec3 v_position;
layout (location = 1) in vec3 v_normal;

layout (push_constant, std430) uniform ShaderConstants {
	mat4 projection;
	mat4 view;
	mat4 transform;
	vec3 color;
};

layout (location = 0) out vec3 frag_normal;
layout (location = 1) out vec3 frag_color;

void main() {
	vec4 point = vec4(v_position, 1.0f);
	vec4 transformed = transform * point;
	vec4 viewed = view * transformed;
	vec4 projected = projection * viewed;

	gl_Position = projected;

	frag_normal = mat3(transform) * v_normal; // Transform normal
	frag_color = color;
}