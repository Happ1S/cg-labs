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

// Параметры отсечения камеры: ближняя и дальняя плоскости
// Объекты ближе near_plane и дальше far_plane не рендерятся
float camera_near_plane = 0.01f;
float camera_far_plane = 100.0f;

// Матрица 4x4 для аффинных преобразований в 3D пространстве
// Используется для проекций, поворотов, переносов
struct Matrix {
    float m[4][4];
};

// Используем готовые типы из geometry namespace
using Vector = geometry::Vector;  // Трёхмерный вектор (x, y, z)
using Vertex = geometry::Vertex;  // Вершина с позицией и нормалью

// Push-константы передаются в шейдеры напрямую без дескрипторов
// Это быстрый способ передать небольшой объём данных (до 128 байт обычно)
struct ShaderConstants {
    Matrix projection;  // Матрица проекции (perspective или orthographic)
    Matrix view;        // Матрица вида (положение и направление камеры)
    Matrix transform;   // Матрица модели (положение и ориентация объекта)
    Vector color;       // Цвет объекта (RGB)
};

// Структура для хранения Vulkan буфера и его памяти
// Буфер - это область GPU памяти для хранения данных (вершин, индексов и т.д.)
struct VulkanBuffer {
    VkBuffer buffer;           // Хэндл буфера
    VkDeviceMemory memory;     // Хэндл выделенной памяти
};

// Шейдерные модули - скомпилированные SPIR-V программы для GPU
VkShaderModule vertex_shader_module;    // Вершинный шейдер (обрабатывает каждую вершину)
VkShaderModule fragment_shader_module;  // Фрагментный шейдер (определяет цвет пикселей)

// Layout определяет какие ресурсы доступны шейдерам
VkPipelineLayout pipeline_layout;

// Pipeline - полная конфигурация графического конвейера
// Включает шейдеры, состояние растеризации, блендинга и т.д.
VkPipeline pipeline;

// Буферы для геометрии цилиндра
VulkanBuffer vertex_buffer;  // Буфер вершин (координаты и нормали)
VulkanBuffer index_buffer;   // Буфер индексов (порядок соединения вершин)

// Объект цилиндра и количество его индексов
geometry::Cylinder* cylinder = nullptr;
uint32_t cylinder_index_count = 0;

// === ПАРАМЕТРЫ АНИМАЦИИ ===
float trajectory_radius = 3.0f;   // Радиус траектории движения
float animation_speed = 1.0f;     // Скорость анимации
float animation_time = 0.0f;      // Текущее время анимации
bool animate = true;              // Флаг: включена ли анимация

// === ПАРАМЕТРЫ РЕНДЕРИНГА ===
Vector cylinder_color = {0.3f, 0.7f, 1.0f};  // Цвет цилиндра (голубой)
bool use_perspective = false;                  // false = ортогональная проекция

// Создаёт единичную матрицу (диагональ из 1, остальное 0)
// Используется как базовая матрица для преобразований
Matrix identity() {
    Matrix result{};
    result.m[0][0] = 1.0f;
    result.m[1][1] = 1.0f;
    result.m[2][2] = 1.0f;
    result.m[3][3] = 1.0f;
    return result;
}

// Перспективная проекция - далёкие объекты меньше (как видит глаз)
// fov - угол обзора в радианах
// aspect - соотношение сторон экрана (ширина/высота)
// near, far - дистанции отсечения
Matrix perspective(float fov, float aspect, float near, float far) {
    Matrix result{};
    float tan_half_fov = tanf(fov / 2.0f);
    
    // Формулы стандартной перспективной проекции
    result.m[0][0] = 1.0f / (aspect * tan_half_fov);
    result.m[1][1] = 1.0f / tan_half_fov;
    result.m[2][2] = -(far + near) / (far - near);
    result.m[2][3] = -1.0f;
    result.m[3][2] = -(2.0f * far * near) / (far - near);
    
    return result;
}

// Ортогональная проекция - параллельные линии остаются параллельными
// Размер объектов не зависит от расстояния (как в инженерных чертежах)
Matrix orthographic(float left, float right, float bottom, float top, float near, float far) {
    Matrix result{};
    
    // Масштабирование по осям
    result.m[0][0] = 2.0f / (right - left);
    result.m[1][1] = 2.0f / (top - bottom);
    result.m[2][2] = 1.0f / (far - near);
    result.m[3][3] = 1.0f;
    
    // Сдвиг в центр
    result.m[3][0] = -(right + left) / (right - left);
    result.m[3][1] = -(top + bottom) / (top - bottom);
    result.m[3][2] = -near / (far - near);
    
    return result;
}

// Матрица переноса - сдвигает объект на вектор
Matrix translation(Vector vector) {
    Matrix result = identity();
    result.m[3][0] = vector.x;
    result.m[3][1] = vector.y;
    result.m[3][2] = vector.z;
    return result;
}

// Матрица поворота вокруг произвольной оси на заданный угол
// Использует формулу Родрига
Matrix rotation(Vector axis, float angle) {
    Matrix result{};
    
    // Нормализуем ось вращения (делаем единичной длины)
    float length = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis.x /= length;
    axis.y /= length;
    axis.z /= length;
    
    float sina = sinf(angle);
    float cosa = cosf(angle);
    float cosv = 1.0f - cosa;
    
    // Формула Родрига для поворота вокруг оси
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

// Перемножение двух матриц 4x4
// Порядок важен: A * B != B * A
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

// Умножение матрицы на вектор - применяет преобразование к точке
Vector multiply(const Matrix& m, const Vector& v) {
    Vector result;
    result.x = m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z + m.m[0][3];
    result.y = m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z + m.m[1][3];
    result.z = m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z + m.m[2][3];
    return result;
}

// Загружает скомпилированный SPIR-V шейдер из файла
VkShaderModule loadShaderModule(const char* path) {
    // Читаем бинарный файл
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    size_t size = file.tellg();
    std::vector<uint32_t> buffer(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    file.close();
    
    // Создаём шейдерный модуль из байткода
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

// Создаёт буфер в GPU памяти и копирует в него данные
// usage - для чего используется буфер (вершины, индексы и т.д.)
VulkanBuffer createBuffer(size_t size, const void *data, VkBufferUsageFlags usage) {
    VkDevice& device = veekay::app.vk_device;
    VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;
    
    VulkanBuffer result{};
    
    // Шаг 1: Создаём буфер
    {
        VkBufferCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = usage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,  // Используется только одной очередью
        };
        
        if (vkCreateBuffer(device, &info, nullptr, &result.buffer) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan buffer\n";
            return {};
        }
    }
    
    // Шаг 2: Выделяем память для буфера и копируем данные
    {
        // Узнаём требования к памяти
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        
        // Получаем свойства физического устройства
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        
        // HOST_VISIBLE - CPU может писать напрямую
        // HOST_COHERENT - не нужно делать flush/invalidate
        const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        
        // Ищем подходящий тип памяти
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
        
        // Выделяем память
        VkMemoryAllocateInfo info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = index,
        };
        
        if (vkAllocateMemory(device, &info, nullptr, &result.memory) != VK_SUCCESS) {
            std::cerr << "Failed to allocate Vulkan buffer memory\n";
            return {};
        }
        
        // Привязываем память к буферу
        if (vkBindBufferMemory(device, result.buffer, result.memory, 0) != VK_SUCCESS) {
            std::cerr << "Failed to bind Vulkan buffer memory\n";
            return {};
        }
        
        // Копируем данные в GPU память
        void* device_data;
        vkMapMemory(device, result.memory, 0, requirements.size, 0, &device_data);
        memcpy(device_data, data, size);
        vkUnmapMemory(device, result.memory);
    }
    
    return result;
}

// Освобождает ресурсы буфера
void destroyBuffer(const VulkanBuffer& buffer) {
    VkDevice& device = veekay::app.vk_device;
    vkFreeMemory(device, buffer.memory, nullptr);
    vkDestroyBuffer(device, buffer.buffer, nullptr);
}

// Функция инициализации - вызывается один раз при старте
void initialize() {
    VkDevice& device = veekay::app.vk_device;
    
    // === ПОСТРОЕНИЕ ГРАФИЧЕСКОГО ПАЙПЛАЙНА ===
    {
        // Загружаем шейдеры из скомпилированных SPIR-V файлов
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
        
        // Настраиваем стадии шейдеров
        VkPipelineShaderStageCreateInfo stage_infos[2];
        
        stage_infos[0] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_shader_module,
            .pName = "main",  // Точка входа в шейдер
        };
        
        stage_infos[1] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_shader_module,
            .pName = "main",
        };
        
        // Описываем формат входных вершин
        VkVertexInputBindingDescription buffer_binding{
            .binding = 0,
            .stride = sizeof(Vertex),  // Размер одной вершины в байтах
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,  // Данные для каждой вершины
        };
        
        // Атрибуты вершины: позиция и нормаль
        VkVertexInputAttributeDescription attributes[] = {
            {
                .location = 0,  // layout(location = 0) в шейдере
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,  // vec3
                .offset = offsetof(Vertex, position),
            },
            {
                .location = 1,  // layout(location = 1) в шейдере
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,  // vec3
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
        
        // Как интерпретировать вершины: треугольники
        VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        
        // Растеризация: заполнение полигонов, без отсечения граней
        VkPipelineRasterizationStateCreateInfo raster_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,  // Заполняем треугольники
            .cullMode = VK_CULL_MODE_NONE,        // Не отсекаем грани
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        
        // Мультисэмплинг выключен (антиалиасинг)
        VkPipelineMultisampleStateCreateInfo sample_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = false,
            .minSampleShading = 1.0f,
        };
        
        // Viewport - область экрана для рендеринга
        VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(veekay::app.window_width),
            .height = static_cast<float>(veekay::app.window_height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        
        // Scissor - прямоугольник отсечения
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
        
        // Тест глубины - ближние объекты перекрывают дальние
        VkPipelineDepthStencilStateCreateInfo depth_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,          // Включаем тест глубины
            .depthWriteEnable = true,         // Записываем в буфер глубины
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,  // Проходят ближние или равные
        };
        
        // Настройки блендинга цветов (смешивание полупрозрачных объектов)
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
        
        // Push constants - быстрый способ передать данные в шейдер
        VkPushConstantRange push_constants{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .size = sizeof(ShaderConstants),
        };
        
        // Layout пайплайна - какие ресурсы доступны шейдерам
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
        
        // Создаём полный графический пайплайн
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
    
    // === СОЗДАНИЕ ГЕОМЕТРИИ ЦИЛИНДРА ===
    // Создаём цилиндр: радиус 0.5, высота 2.0, ~50 вершин (25 сегментов по окружности)
    cylinder = new geometry::Cylinder(0.5f, 2.0f, 50);
    cylinder_index_count = cylinder->getIndexCount();
    
    // Создаём GPU буферы для вершин и индексов
    vertex_buffer = createBuffer(
        cylinder->getVerticesSizeInBytes(),
        cylinder->getVerticesData(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT  // Буфер для вершин
    );
    
    index_buffer = createBuffer(
        cylinder->getIndicesSizeInBytes(),
        cylinder->getIndicesData(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT  // Буфер для индексов
    );
}

// Функция завершения - освобождаем все ресурсы
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

// Функция обновления - вызывается каждый кадр
// Здесь обрабатываем ввод и обновляем параметры анимации
void update(double time) {
    // Создаём GUI панель управления с помощью ImGui
    ImGui::Begin("Cylinder Controls");
    ImGui::Text("Trajectory Settings:");
    ImGui::SliderFloat("Trajectory Radius", &trajectory_radius, 0.5f, 8.0f);
    ImGui::SliderFloat("Animation Speed", &animation_speed, 0.1f, 5.0f);
    ImGui::Checkbox("Animate", &animate);
    ImGui::Separator();
    ImGui::Text("Rendering Settings:");
    ImGui::Checkbox("Perspective Projection", &use_perspective);
    ImGui::Separator();
    ImGui::ColorEdit3("Cylinder Color", reinterpret_cast<float*>(&cylinder_color));
    ImGui::End();
    
    // Обновляем время анимации если включена
    if (animate) {
        animation_time = float(time) * animation_speed;
    }
}

// Функция рендеринга - формируем команды отрисовки для GPU
void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    // Сбрасываем буфер команд для записи новых
    vkResetCommandBuffer(cmd, 0);
    
    // Начинаем запись команд
    {
        VkCommandBufferBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cmd, &info);
    }
    
    // Начинаем render pass - очищаем экран и буфер глубины
    {
        VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};  // Тёмно-серый фон
        VkClearValue clear_depth{.depthStencil = {1.0f, 0}};             // Максимальная глубина
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
    
    // Записываем команды отрисовки цилиндра
    {
        // Привязываем наш графический пайплайн
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        
        // Привязываем буферы вершин и индексов
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, index_buffer.buffer, offset, VK_INDEX_TYPE_UINT32);
        
        // Центрируем цилиндр вокруг начала координат по оси Y
        float height = 2.0f;
        Vector center_offset = {0.0f, -height / 2.0f, 0.0f};
        
        // === ВЫЧИСЛЕНИЕ ТРАЕКТОРИИ ДВИЖЕНИЯ ===
        // Сложная траектория в форме восьмёрки (лемнискаты)
        // phase - фаза анимации с вариацией скорости для более интересного движения
        float phase = animation_time + 0.2f * sinf(2.0f * animation_time);
        float x = trajectory_radius * sinf(phase);              // Движение по X
        float z = trajectory_radius * 0.5f * sinf(2.0f * phase); // Движение по Z (удвоенная частота = восьмёрка)
        Vector orbital_pos = {x, 0.0f, z - 1};
        
        // Наклоняем плоскость траектории на 30 градусов вокруг оси X
        Matrix tilt = rotation({1.0f, 0.0f, 0.0f}, M_PI / 6.0f);
        Vector tilted_pos = multiply(tilt, orbital_pos);
        
        // === МАТРИЦА ПРОЕКЦИИ ===
        float aspect = float(veekay::app.window_width) / float(veekay::app.window_height);
        Matrix proj;
        
        if (use_perspective) {
            // Перспективная проекция: fov = 45°, near = 0.01, far = 100
            proj = perspective(M_PI / 4.0f, aspect, camera_near_plane, camera_far_plane);
        } else {
            // Ортогональная проекция: видимая область 5x5 единиц с учётом aspect ratio
            float ortho_half_width = 5.0f * aspect;
            float ortho_half_height = 5.0f;
            proj = orthographic(-ortho_half_width, ortho_half_width, 
                              -ortho_half_height, ortho_half_height, 
                              -10.0f, camera_far_plane);
        }

        // === МАТРИЦА ВИДА ===
        // Камера находится в точке (0, 0, 5), смотрит в направлении (0, 0, -1)
        Matrix view = translation({0.0f, 0.0f, -5.0f});
        
        // === МАТРИЦА МОДЕЛИ ===
        // Перемещаем цилиндр на позицию траектории и немного наклоняем
        Matrix model = multiply(
            translation(tilted_pos), 
            rotation({1.0f, 0.0f, 0.0f}, -M_PI / 5.0f)  // Наклон на -36°
        );
        
        // Заполняем структуру констант для шейдеров
        ShaderConstants constants{
            .projection = proj,
            .view = view,
            .transform = model,
            .color = cylinder_color,
        };
        
        // Передаём константы в шейдеры через push constants
        vkCmdPushConstants(
            cmd, pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, sizeof(ShaderConstants), &constants
        );
        
        // Команда отрисовки: рисуем цилиндр используя индексы
        vkCmdDrawIndexed(cmd, cylinder_index_count, 1, 0, 0, 0);
    }
    
    // Завершаем render pass и запись команд
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace

// Точка входа в программу
int main() {
    // Запускаем приложение с нашими callback функциями
    return veekay::run({
        .init = initialize,     // Вызывается один раз при старте
        .shutdown = shutdown,   // Вызывается при завершении
        .update = update,       // Вызывается каждый кадр перед рендерингом
        .render = render,       // Вызывается каждый кадр для отрисовки
    });
}
