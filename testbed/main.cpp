#include <cstdint>
#include <climits>
#include <vector>
#include <iostream>
#include <fstream>
#include <cmath>

#include <veekay/veekay.hpp>
#include <veekay/Cylinder.hpp>

#include <imgui.h>
#include <vulkan/vulkan_core.h>

namespace {

float camera_near_plane = 0.01f;
float camera_far_plane = 100.0f;

struct Matrix {
    float m[4][4];
};

// Используем типы из geometry namespace
using Vector = geometry::Vector;
using Vertex = geometry::Vertex;

// NOTE: These variable will be available to shaders through push constant uniform
struct ShaderConstants {
    Matrix projection;
    Matrix view;
    Matrix transform;
    Vector color;
};

struct VulkanBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
};

VkShaderModule vertex_shader_module;
VkShaderModule fragment_shader_module;
VkPipelineLayout pipeline_layout;
VkPipeline pipeline;

VulkanBuffer vertex_buffer;
VulkanBuffer index_buffer;

// Параметры цилиндра
geometry::Cylinder* cylinder = nullptr;
uint32_t cylinder_index_count = 0;

// Параметры анимации
float trajectory_radius = 3.0f;
float animation_speed = 1.0f;
float animation_time = 0.0f;
bool animate = true;

// Параметры модели
Vector cylinder_color = {0.3f, 0.7f, 1.0f};
float cylinder_rotation = 0.0f;

Matrix identity() {
    Matrix result{};
    result.m[0][0] = 1.0f;
    result.m[1][1] = 1.0f;
    result.m[2][2] = 1.0f;
    result.m[3][3] = 1.0f;
    return result;
}

Matrix perspective(float fov, float aspect, float near, float far) {
    Matrix result{};
    float tan_half_fov = tanf(fov / 2.0f);
    
    result.m[0][0] = 1.0f / (aspect * tan_half_fov);
    result.m[1][1] = 1.0f / tan_half_fov;
    result.m[2][2] = -(far + near) / (far - near);
    result.m[2][3] = -1.0f;
    result.m[3][2] = -(2.0f * far * near) / (far - near);
    
    return result;
}

Matrix orthographic(float left, float right, float bottom, float top, float near, float far) {
    Matrix result{};
    
    result.m[0][0] = 2.0f / (right - left);
    result.m[1][1] = 2.0f / (top - bottom);
    result.m[2][2] = 1.0f / (far - near);
    result.m[3][3] = 1.0f;
    
    result.m[3][0] = -(right + left) / (right - left);
    result.m[3][1] = -(top + bottom) / (top - bottom);
    result.m[3][2] = -near / (far - near);
    
    return result;
}

Matrix translation(Vector vector) {
    Matrix result = identity();
    result.m[3][0] = vector.x;
    result.m[3][1] = vector.y;
    result.m[3][2] = vector.z;
    return result;
}

Matrix rotation(Vector axis, float angle) {
    Matrix result{};
    
    float length = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis.x /= length;
    axis.y /= length;
    axis.z /= length;
    
    float sina = sinf(angle);
    float cosa = cosf(angle);
    float cosv = 1.0f - cosa;
    
    result.m[0][0] = (axis.x * axis.x * cosv) + cosa;
    result.m[0][1] = (axis.x * axis.y * cosv) + (axis.z * sina);
    result.m[0][2] = (axis.x * axis.z * cosv) - (axis.y * sina);
    
    result.m[1][0] = (axis.y * axis.x * cosv) - (axis.z * sina);
    result.m[1][1] = (axis.y * axis.y * cosv) + cosa;
    result.m[1][2] = (axis.y * axis.z * cosv) + (axis.x * sina);
    
    result.m[2][0] = (axis.z * axis.x * cosv) + (axis.y * sina);
    result.m[2][1] = (axis.z * axis.y * cosv) - (axis.x * sina);
    result.m[2][2] = (axis.z * axis.z * cosv) + cosa;
    
    result.m[3][3] = 1.0f;
    
    return result;
}

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 4; k++) {
                result.m[j][i] += a.m[j][k] * b.m[k][i];
            }
        }
    }
    return result;
}

Vector multiply(const Matrix& m, const Vector& v) {
    Vector result;
    result.x = m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3];
    result.y = m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3];
    result.z = m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3];
    return result;
}

VkShaderModule loadShaderModule(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    size_t size = file.tellg();
    std::vector<uint32_t> buffer(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    file.close();
    
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = buffer.data(),
    };
    
    VkShaderModule result;
    if (vkCreateShaderModule(veekay::app.vk_device, &info, nullptr, &result) != VK_SUCCESS) {
        return nullptr;
    }
    
    return result;
}

VulkanBuffer createBuffer(size_t size, const void *data, VkBufferUsageFlags usage) {
    VkDevice& device = veekay::app.vk_device;
    VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;
    
    VulkanBuffer result{};
    
    {
        VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        
        if (vkCreateBuffer(device, &info, nullptr, &result.buffer) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan buffer\n";
            return {};
        }
    }
    
    {
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        
        const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        
        uint32_t index = UINT_MAX;
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
            const VkMemoryType& type = properties.memoryTypes[i];
            if ((requirements.memoryTypeBits & (1 << i)) &&
                (type.propertyFlags & flags) == flags) {
                index = i;
                break;
            }
        }
        
        if (index == UINT_MAX) {
            std::cerr << "Failed to find required memory type to allocate Vulkan buffer\n";
            return {};
        }
        
        VkMemoryAllocateInfo info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = index,
        };
        
        if (vkAllocateMemory(device, &info, nullptr, &result.memory) != VK_SUCCESS) {
            std::cerr << "Failed to allocate Vulkan buffer memory\n";
            return {};
        }
        
        if (vkBindBufferMemory(device, result.buffer, result.memory, 0) != VK_SUCCESS) {
            std::cerr << "Failed to bind Vulkan buffer memory\n";
            return {};
        }
        
        void* device_data;
        vkMapMemory(device, result.memory, 0, requirements.size, 0, &device_data);
        memcpy(device_data, data, size);
        vkUnmapMemory(device, result.memory);
    }
    
    return result;
}

void destroyBuffer(const VulkanBuffer& buffer) {
    VkDevice& device = veekay::app.vk_device;
    vkFreeMemory(device, buffer.memory, nullptr);
    vkDestroyBuffer(device, buffer.buffer, nullptr);
}

void initialize() {
    VkDevice& device = veekay::app.vk_device;
    
    { // NOTE: Build graphics pipeline
        vertex_shader_module = loadShaderModule("./shaders/shader.vert.spv");
        if (!vertex_shader_module) {
            std::cerr << "Failed to load Vulkan vertex shader from file\n";
            veekay::app.running = false;
            return;
        }
        
        fragment_shader_module = loadShaderModule("./shaders/shader.frag.spv");
        if (!fragment_shader_module) {
            std::cerr << "Failed to load Vulkan fragment shader from file\n";
            veekay::app.running = false;
            return;
        }
        
        VkPipelineShaderStageCreateInfo stage_infos[2];
        
        stage_infos[0] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_shader_module,
            .pName = "main",
        };
        
        stage_infos[1] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_shader_module,
            .pName = "main",
        };
        
        VkVertexInputBindingDescription buffer_binding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };
        
        VkVertexInputAttributeDescription attributes[] = {
            {
                .location = 0,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, position),
            },
            {
                .location = 1,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, normal),
            },
        };
        
        VkPipelineVertexInputStateCreateInfo input_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &buffer_binding,
            .vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
            .pVertexAttributeDescriptions = attributes,
        };
        
        VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        
        VkPipelineRasterizationStateCreateInfo raster_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        
        VkPipelineMultisampleStateCreateInfo sample_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = false,
            .minSampleShading = 1.0f,
        };
        
        VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(veekay::app.window_width),
            .height = static_cast<float>(veekay::app.window_height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        
        VkRect2D scissor{
            .offset = {0, 0},
            .extent = {veekay::app.window_width, veekay::app.window_height},
        };
        
        VkPipelineViewportStateCreateInfo viewport_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };
        
        VkPipelineDepthStencilStateCreateInfo depth_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,
            .depthWriteEnable = true,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        };
        
        VkPipelineColorBlendAttachmentState attachment_info{
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                              VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
        };
        
        VkPipelineColorBlendStateCreateInfo blend_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = false,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &attachment_info
        };
        
        VkPushConstantRange push_constants{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .size = sizeof(ShaderConstants),
        };
        
        VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        };
        
        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan pipeline layout\n";
            veekay::app.running = false;
            return;
        }
        
        VkGraphicsPipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages = stage_infos,
            .pVertexInputState = &input_state_info,
            .pInputAssemblyState = &assembly_state_info,
            .pViewportState = &viewport_info,
            .pRasterizationState = &raster_info,
            .pMultisampleState = &sample_info,
            .pDepthStencilState = &depth_info,
            .pColorBlendState = &blend_info,
            .layout = pipeline_layout,
            .renderPass = veekay::app.vk_render_pass,
        };
        
        if (vkCreateGraphicsPipelines(device, nullptr, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan pipeline\n";
            veekay::app.running = false;
            return;
        }
    }
    
    // Создаем цилиндр (~50 вершин = 25 сегментов)
    cylinder = new geometry::Cylinder(0.5f, 2.0f, 50);
    cylinder_index_count = cylinder->getIndexCount();
    
    // Создаем буферы используя новые методы
    vertex_buffer = createBuffer(
        cylinder->getVerticesSizeInBytes(),
        cylinder->getVerticesData(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
    );
    
    index_buffer = createBuffer(
        cylinder->getIndicesSizeInBytes(),
        cylinder->getIndicesData(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT
    );
}

void shutdown() {
    VkDevice& device = veekay::app.vk_device;
    
    delete cylinder;
    
    destroyBuffer(index_buffer);
    destroyBuffer(vertex_buffer);
    
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyShaderModule(device, fragment_shader_module, nullptr);
    vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {
    ImGui::Begin("Cylinder Controls");
    ImGui::Text("Trajectory Settings:");
    ImGui::SliderFloat("Trajectory Radius", &trajectory_radius, 0.5f, 8.0f);
    ImGui::SliderFloat("Animation Speed", &animation_speed, 0.1f, 5.0f);
    ImGui::Checkbox("Animate", &animate);
    ImGui::Separator();
    ImGui::ColorEdit3("Cylinder Color", reinterpret_cast<float*>(&cylinder_color));
    ImGui::SliderFloat("Cylinder Rotation", &cylinder_rotation, 0.0f, 2.0f * M_PI);
    ImGui::Separator();
    ImGui::End();
    
    if (animate) {
        animation_time = float(time) * animation_speed;
    }
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);
    
    {
        VkCommandBufferBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cmd, &info);
    }
    
    {
        VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
        VkClearValue clear_depth{.depthStencil = {1.0f, 0}};
        VkClearValue clear_values[] = {clear_color, clear_depth};
        
        VkRenderPassBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = veekay::app.vk_render_pass,
            .framebuffer = framebuffer,
            .renderArea = {
                .extent = {veekay::app.window_width, veekay::app.window_height},
            },
            .clearValueCount = 2,
            .pClearValues = clear_values,
        };
        
        vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    }
    
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, index_buffer.buffer, offset, VK_INDEX_TYPE_UINT32);
        
        // Центрируем цилиндр вокруг оси Y
        float height = 2.0f;
        Vector center_offset = {0.0f, -height / 2.0f, 0.0f};
        
        // Сложная траектория в форме восьмерки с замедлениями и ускорениями
        float phase = animation_time + 0.2f * sinf(2.0f * animation_time); // Добавляем вариацию скорости
        float x = trajectory_radius * sinf(phase);
        float z = trajectory_radius * 0.5f * sinf(2.0f * phase); // Форма восьмерки
        Vector orbital_pos = {x, 0.0f, z - 1};
        
        // Наклон траектории (поворот вокруг X на 30 градусов)
        Matrix tilt = rotation({1.0f, 0.0f, 0.0f}, M_PI / 6.0f); // 30 градусов
        Vector tilted_pos = multiply(tilt, orbital_pos);
        
        // Проекционная матрица
        float aspect = float(veekay::app.window_width) / float(veekay::app.window_height);
        Matrix proj;
        float ortho_half_width = 5.0f * aspect;
        float ortho_half_height = 5.0f;
        proj = orthographic(-ortho_half_width, ortho_half_width, -ortho_half_height, ortho_half_height, -10.0f, camera_far_plane);

        // Матрица вида: камера в (0,0,5), смотрит на (0,0,0)
        Matrix view = translation({0.0f, 0.0f, -5.0f});
        
        // Модель: перевод на позицию траектории + центрирование
        Matrix model = multiply(translation(tilted_pos), rotation({1.0f, 0.0f, 0.0f}, -M_PI / 5.0f));
        
        ShaderConstants constants{
            .projection = proj,
            .view = view,
            .transform = model,
            .color = cylinder_color,
        };
        
        vkCmdPushConstants(
            cmd, pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(ShaderConstants), &constants
        );
        
        vkCmdDrawIndexed(cmd, cylinder_index_count, 1, 0, 0, 0);
    }
    
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace

int main() {
    return veekay::run({
        .init = initialize,
        .shutdown = shutdown,
        .update = update,
        .render = render,
    });
}