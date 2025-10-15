#version 450

layout (location = 0) in vec3 frag_normal;
layout (location = 1) in vec3 frag_color;

layout (location = 0) out vec4 final_color;

layout (push_constant, std430) uniform ShaderConstants {
	mat4 projection;
	mat4 view;
	mat4 transform;
	vec3 color;
};

void main() {
	// Simple diffuse lighting
	vec3 light_dir = normalize(vec3(1.0, 1.0, 1.0));
	float diff = max(dot(normalize(frag_normal), light_dir), 0.0);
	vec3 shaded_color = frag_color * (0.3 + 0.7 * diff);

	final_color = vec4(shaded_color, 1.0f);
}