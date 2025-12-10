#include <cstdint>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <fstream>

// Настройки GLM для работы с Vulkan
#define GLM_FORCE_RADIANS           // Использовать радианы вместо градусов
#define GLM_FORCE_DEPTH_ZERO_TO_ONE // Глубина от 0 до 1 (стандарт Vulkan)
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>
#include <veekay/veekay.hpp>
#include <veekay/geometry/Vertex.hpp>
#include <veekay/geometry/Cube.hpp>
#include <veekay/geometry/Cylinder.hpp>
#include <veekay/scene/Camera.hpp>
#include <veekay/scene/Lights.hpp>
#include <veekay/scene/Material.hpp>
#include <imgui.h>

namespace vkgeom = veekay::geometry;
namespace vkscene = veekay::scene;

namespace {

// === СТРУКТУРЫ ДАННЫХ ===

// Uniform Buffer Object для данных сцены (передаётся в шейдеры)
struct SceneUBO {
    glm::mat4 projection;           // Матрица проекции (перспектива)
    glm::mat4 view;                 // Матрица вида (позиция камеры)
    alignas(16) glm::vec3 viewPos;  // Позиция камеры в мировых координатах (выравнивание для GPU)
};

// Push Constants для быстрой передачи данных каждого объекта в шейдер
struct PushConstants {
    glm::mat4 model;           // Матрица модели (позиция, поворот, масштаб объекта)
    glm::mat4 normalMatrix;    // Матрица для корректной трансформации нормалей
    glm::vec4 color;           // Цвет объекта (RGBA)
    float metallic;            // Металличность материала (0.0 = диэлектрик, 1.0 = металл)
    float roughness;           // Шероховатость материала (0.0 = гладкий, 1.0 = матовый)
    float padding[2];          // Выравнивание для GPU
};

// === VULKAN РЕСУРСЫ ===
VkShaderModule vertexShaderModule, fragmentShaderModule;  // Скомпилированные шейдеры
VkDescriptorSetLayout descriptorSetLayout;                // Описание того, какие данные передаются в шейдеры
VkDescriptorPool descriptorPool;                          // Пул для выделения descriptor sets
VkDescriptorSet descriptorSet;                            // Набор дескрипторов (связывает буферы с шейдерами)
VkPipelineLayout pipelineLayout;                          // Макет конвейера (описывает входные данные)
VkPipeline pipeline;                                      // Графический конвейер (состояние рендеринга)

// Структура для хранения Vulkan буфера и его памяти
struct VulkanBuffer {
    VkBuffer buffer;            // Сам буфер
    VkDeviceMemory memory;      // Выделенная память на GPU
};

// === ГЕОМЕТРИЧЕСКИЕ БУФЕРЫ ===
// Буферы для кубов (здания, земля)
VulkanBuffer cubeVertexBuffer, cubeIndexBuffer;
uint32_t cubeIndexCount;

// Буферы для цилиндров (столбы фонарей)
VulkanBuffer cylinderVertexBuffer, cylinderIndexBuffer;
uint32_t cylinderIndexCount;

// Буферы для колёс (толстые цилиндры с крышками)
VulkanBuffer wheelCylinderVertexBuffer, wheelCylinderIndexBuffer;
uint32_t wheelCylinderIndexCount;

// === UNIFORM БУФЕРЫ (данные, общие для всех объектов) ===
VulkanBuffer sceneUboBuffer;          // Камера и проекция
VulkanBuffer materialUboBuffer;       // Материал
VulkanBuffer dirLightUboBuffer;       // Направленный свет (луна)
VulkanBuffer pointLightsSsboBuffer;   // Точечные источники света (фонари)
VulkanBuffer spotLightsSsboBuffer;    // Прожекторы (фары автомобиля)

// === ОБЪЕКТЫ СЦЕНЫ ===
vkscene::Camera camera(glm::vec3(0.0f, 3.0f, 15.0f));  // Камера, начинается на высоте 3, на расстоянии 15
vkscene::Material buildingMaterial;                     // Материал зданий
vkscene::DirectionalLight moonLight;                    // Направленный свет (имитация луны)
std::vector<vkscene::PointLight> pointLights;          // Массив точечных источников света
std::vector<vkscene::SpotLight> spotLights;            // Массив прожекторов

// === УПРАВЛЕНИЕ МЫШЬЮ И ВРЕМЕНЕМ ===
bool firstMouse = true;              // Флаг для первого движения мыши
float lastX = 640.0f, lastY = 360.0f; // Последняя позиция курсора
float deltaTime = 0.0f;              // Время между кадрами (для плавного движения)
float lastFrame = 0.0f;              // Время предыдущего кадра

// === ИГРОВЫЕ ОБЪЕКТЫ ===

// Структура для описания здания
struct Building {
    glm::vec3 position;  // Позиция в мире
    glm::vec3 scale;     // Размер (ширина, высота, глубина)
    glm::vec3 color;     // Цвет
    float metallic;      // Металличность
    float roughness;     // Шероховатость
};

// Структура для описания уличного фонаря
struct StreetLamp {
    glm::vec3 position;    // Позиция столба
    glm::vec3 lightColor;  // Цвет света
    float lightRadius;     // Радиус освещения
};

std::vector<Building> buildings;      // Массив всех зданий в сцене
std::vector<StreetLamp> streetLamps;  // Массив всех фонарей

// === ФУНКЦИЯ: Загрузка скомпилированного шейдера ===
// Читает .spv файл (SPIR-V байткод) и создаёт VkShaderModule
VkShaderModule loadShaderModule(const char* path) {
    // Открываем файл в бинарном режиме и сразу переходим в конец для определения размера
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error(std::string("Failed to open shader file: ") + path);
    }
    
    // Получаем размер файла и создаём буфер
    size_t size = file.tellg();
    std::vector<char> buffer(size);
    
    // Возвращаемся в начало и читаем весь файл
    file.seekg(0);
    file.read(buffer.data(), size);
    file.close();
    
    // Создаём VkShaderModule из байткода
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = buffer.size(),
        .pCode = reinterpret_cast<const uint32_t*>(buffer.data())
    };
    
    VkShaderModule result;
    if (vkCreateShaderModule(veekay::app.vk_device, &info, nullptr, &result) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shader module!");
    }
    return result;
}

// === ФУНКЦИЯ: Поиск подходящего типа памяти GPU ===
// GPU имеет разные типы памяти (видимая с CPU, быстрая, кэшируемая и т.д.)
uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(veekay::app.vk_physical_device, &memProperties);
    
    // Ищем тип памяти, который соответствует требованиям
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type!");
}

// === ФУНКЦИЯ: Создание Vulkan буфера ===
// Создаёт буфер на GPU с заданным размером и свойствами
VulkanBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                          VkMemoryPropertyFlags properties) {
    VulkanBuffer result{};
    
    // 1. Создаём сам буфер
    VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,  // Как будет использоваться (вершины, индексы, uniform и т.д.)
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE  // Эксклюзивный доступ одной очереди
    };
    
    if (vkCreateBuffer(veekay::app.vk_device, &bufferInfo, nullptr, &result.buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer!");
    }
    
    // 2. Запрашиваем требования к памяти для этого буфера
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(veekay::app.vk_device, result.buffer, &memRequirements);
    
    // 3. Выделяем память на GPU
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties)
    };
    
    if (vkAllocateMemory(veekay::app.vk_device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory!");
    }
    
    // 4. Связываем буфер с выделенной памятью
    vkBindBufferMemory(veekay::app.vk_device, result.buffer, result.memory, 0);
    return result;
}

// === ФУНКЦИЯ: Копирование данных в буфер GPU ===
// Маппит память GPU в адресное пространство CPU, копирует данные и размаппивает
void copyDataToBuffer(VulkanBuffer& buffer, const void* data, size_t size) {
    void* mappedData;
    vkMapMemory(veekay::app.vk_device, buffer.memory, 0, size, 0, &mappedData);
    memcpy(mappedData, data, size);  // Прямое копирование из CPU в GPU
    vkUnmapMemory(veekay::app.vk_device, buffer.memory);
}

// === ФУНКЦИЯ: Освобождение буфера ===
void destroyBuffer(const VulkanBuffer& buffer) {
    vkFreeMemory(veekay::app.vk_device, buffer.memory, nullptr);
    vkDestroyBuffer(veekay::app.vk_device, buffer.buffer, nullptr);
}

// === ФУНКЦИЯ: Обработка ввода с клавиатуры ===
void processInput(GLFWwindow* window, float deltaTime) {
    float velocity = 5.0f * deltaTime;  // Скорость движения (умножается на время кадра)
    
    // WASD для перемещения
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.moveForward(velocity);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.moveForward(-velocity);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.moveRight(-velocity);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.moveRight(velocity);
    
    // Пробел и Shift для перемещения вверх/вниз
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
        camera.moveUp(velocity);
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        camera.moveUp(-velocity);
    
    // ESC для выхода
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
}

// === ФУНКЦИЯ: Обработка движения мыши (камера) ===
void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    // Пропускаем первое движение мыши, чтобы избежать резкого поворота
    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
        return;
    }

    // Вычисляем смещение курсора
    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos;  // Инвертировано, т.к. Y растёт вниз
    
    lastX = xpos;
    lastY = ypos;

    // Применяем чувствительность
    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    // Поворачиваем камеру
    camera.rotate(xoffset, yoffset);
}

// === ФУНКЦИЯ: Инициализация сцены и Vulkan ресурсов ===
void initialize() {
    VkDevice& device = veekay::app.vk_device;
    
    // Захватываем курсор для управления камерой
    glfwSetInputMode(veekay::app.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(veekay::app.window, mouse_callback);
    
    // === СОЗДАНИЕ ЗДАНИЙ ===
    // Каждое здание: позиция, размер, цвет, металличность, шероховатость
    buildings = {
        {{-6.0f, 3.5f, -5.0f}, {2.2f, 7.0f, 2.2f}, {0.12f, 0.18f, 0.25f}, 0.3f, 0.7f},   // Синеватое стекло
        {{0.0f, 2.0f, -4.0f}, {2.0f, 4.0f, 2.0f}, {0.2f, 0.2f, 0.22f}, 0.05f, 0.85f},     // Тёмный бетон
        {{6.0f, 4.0f, -3.0f}, {2.5f, 8.0f, 2.5f}, {0.08f, 0.12f, 0.2f}, 0.25f, 0.75f},    // Синее стекло
        {{-4.0f, 1.2f, 3.0f}, {1.8f, 2.4f, 1.8f}, {0.25f, 0.18f, 0.13f}, 0.02f, 0.9f},    // Кирпич
        {{4.0f, 3.0f, 2.0f}, {1.6f, 6.0f, 1.6f}, {0.28f, 0.28f, 0.32f}, 0.2f, 0.75f},     // Светлый металл
        {{-8.0f, 1.5f, 0.0f}, {1.5f, 3.0f, 1.5f}, {0.18f, 0.18f, 0.2f}, 0.0f, 0.95f},     // Матовый бетон
        {{8.0f, 5.0f, 0.0f}, {2.0f, 10.0f, 2.0f}, {0.06f, 0.08f, 0.12f}, 0.4f, 0.65f},    // Тёмное стекло
        {{0.0f, 1.0f, 5.0f}, {1.4f, 2.0f, 1.4f}, {0.32f, 0.32f, 0.35f}, 0.1f, 0.8f},      // Светлый бетон
    };
    
    // === СОЗДАНИЕ УЛИЧНЫХ ФОНАРЕЙ ===
    streetLamps.clear();
    std::vector<glm::vec3> lampPositions = {
        {-10.0f, 0.0f, -8.0f}, {-2.0f, 0.0f, -8.0f}, {10.0f, 0.0f, -8.0f},
        {-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f},
        {-10.0f, 0.0f, 8.0f}, {-2.0f, 0.0f, 8.0f}, {10.0f, 0.0f, 8.0f},
    };

    for (const auto& pos : lampPositions) {
        streetLamps.push_back({
            pos,                                 // Позиция столба
            glm::vec3(1.0f, 0.9f, 0.7f),        // Тёплый жёлтый свет
            12.0f                                // Радиус освещения
        });
    }
    
    // === СОЗДАНИЕ ГЕОМЕТРИИ КУБА ===
    vkgeom::Cube cubeGeom;
    cubeIndexCount = cubeGeom.getIndexCount();
    
    // Создаём буфер вершин для куба
    cubeVertexBuffer = createBuffer(
        cubeGeom.getVerticesSizeInBytes(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,  // Будет использоваться как буфер вершин
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT  // Доступен с CPU
    );
    copyDataToBuffer(cubeVertexBuffer, cubeGeom.getVerticesData(), cubeGeom.getVerticesSizeInBytes());
    
    // Создаём буфер индексов для куба
    cubeIndexBuffer = createBuffer(
        cubeGeom.getIndicesSizeInBytes(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,  // Будет использоваться как буфер индексов
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    copyDataToBuffer(cubeIndexBuffer, cubeGeom.getIndicesData(), cubeGeom.getIndicesSizeInBytes());
    
    // === СОЗДАНИЕ ГЕОМЕТРИИ ЦИЛИНДРА (столбы фонарей) ===
    vkgeom::Cylinder cylinderGeom(0.1f, 3.0f, 16);  // Радиус 0.1, высота 3, 16 сегментов
    cylinderIndexCount = cylinderGeom.getIndexCount();
    
    cylinderVertexBuffer = createBuffer(
        cylinderGeom.getVerticesSizeInBytes(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    copyDataToBuffer(cylinderVertexBuffer, cylinderGeom.getVerticesData(), cylinderGeom.getVerticesSizeInBytes());
    
    cylinderIndexBuffer = createBuffer(
        cylinderGeom.getIndicesSizeInBytes(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    copyDataToBuffer(cylinderIndexBuffer, cylinderGeom.getIndicesData(), cylinderGeom.getIndicesSizeInBytes());
    
    // === СОЗДАНИЕ ГЕОМЕТРИИ КОЛЁС (толстый цилиндр с крышками) ===
    vkgeom::Cylinder wheelCylinderGeom(0.8f, 0.5f, 20, true);  // Радиус 0.8, высота 0.5, 20 сегментов, с крышками
    wheelCylinderIndexCount = wheelCylinderGeom.getIndexCount();

    wheelCylinderVertexBuffer = createBuffer(
        wheelCylinderGeom.getVerticesSizeInBytes(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    copyDataToBuffer(wheelCylinderVertexBuffer, wheelCylinderGeom.getVerticesData(), 
                    wheelCylinderGeom.getVerticesSizeInBytes());

    wheelCylinderIndexBuffer = createBuffer(
        wheelCylinderGeom.getIndicesSizeInBytes(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    copyDataToBuffer(wheelCylinderIndexBuffer, wheelCylinderGeom.getIndicesData(), 
                    wheelCylinderGeom.getIndicesSizeInBytes());

    // === СОЗДАНИЕ ТОЧЕЧНЫХ ИСТОЧНИКОВ СВЕТА (из фонарей) ===
    pointLights.clear();
    for (const auto& lamp : streetLamps) {
        vkscene::PointLight light;
        light.position = lamp.position + glm::vec3(0.0f, 3.0f, 0.0f);  // Лампочка на верхушке столба
        light.color = glm::vec4(lamp.lightColor, 1.5f);  // Цвет и интенсивность света
        // Параметры затухания (constant + linear*d + quadratic*d²)
        light.constant = 1.0f;
        light.linear = 0.09f;
        light.quadratic = 0.032f;
        light.padding = glm::vec2(0.0f);
        pointLights.push_back(light);
    }
    
    // === СОЗДАНИЕ ПРОЖЕКТОРОВ (фары автомобиля) ===
    spotLights.clear();
        
    glm::vec3 carPos = glm::vec3(0.0f, 0.0f, 0.0f);         // Позиция машины
    float carRotation = 0.0f;                               // Поворот машины
    glm::vec3 carDirection = glm::vec3(0.0f, 0.0f, -1.0f);  // Направление взгляда машины
        
    // ОДНА ШИРОКАЯ ФАРА во всю ширину машины
    vkscene::SpotLight wideHeadlight;
    wideHeadlight.position = carPos + glm::vec3(0.0f, 0.4f, 0.78f);  // Спереди машины
    wideHeadlight.direction = glm::normalize(carDirection + glm::vec3(0.0f, -0.1f, 0.0f));  // Немного вниз
    wideHeadlight.color = glm::vec4(1.0f, 0.95f, 0.85f, 2.2f);  // Тёплый белый свет, яркость 2.2
    wideHeadlight.constant = 1.0f;
    wideHeadlight.linear = 0.06f;
    wideHeadlight.quadratic = 0.012f;
    wideHeadlight.cutOff = glm::cos(glm::radians(45.0f));       // Внутренний угус конуса
    wideHeadlight.outerCutOff = glm::cos(glm::radians(65.0f));  // Внешний угол (плавное затухание)
    wideHeadlight.padding = 0.0f;
    spotLights.push_back(wideHeadlight);
    
    // === СОЗДАНИЕ UNIFORM БУФЕРОВ ===
    // Буфер для данных сцены (камера, проекция)
    sceneUboBuffer = createBuffer(sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    // Буфер для материала
    materialUboBuffer = createBuffer(sizeof(vkscene::Material), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    // Буфер для направленного света (луна)
    dirLightUboBuffer = createBuffer(sizeof(vkscene::DirectionalLight), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    // Storage буферы для массивов источников света
    pointLightsSsboBuffer = createBuffer(sizeof(vkscene::PointLight) * vkscene::MAX_POINT_LIGHTS,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    spotLightsSsboBuffer = createBuffer(sizeof(vkscene::SpotLight) * vkscene::MAX_SPOT_LIGHTS,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    // === СОЗДАНИЕ DESCRIPTOR SET LAYOUT ===
    // Описывает, какие буферы доступны в шейдерах и на каких binding'ах
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // SceneUBO
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // Material
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // DirectionalLight
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // PointLights[]
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}   // SpotLights[]
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout!");
    }
    
    // === СОЗДАНИЕ DESCRIPTOR POOL ===
    // Пул для выделения descriptor sets
    std::vector<VkDescriptorPoolSize> poolSizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},  // 3 uniform буфера
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}   // 2 storage буфера
    };
    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data()
    };
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool!");
    }
    
    // === ВЫДЕЛЕНИЕ DESCRIPTOR SET ===
    VkDescriptorSetAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 1,
        .pSetLayouts = &descriptorSetLayout
    };
    if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets!");
    }
    
    // === СВЯЗЫВАНИЕ БУФЕРОВ С DESCRIPTOR SET ===
    VkDescriptorBufferInfo sceneUboInfo{sceneUboBuffer.buffer, 0, sizeof(SceneUBO)};
    VkDescriptorBufferInfo materialUboInfo{materialUboBuffer.buffer, 0, sizeof(vkscene::Material)};
    VkDescriptorBufferInfo dirLightUboInfo{dirLightUboBuffer.buffer, 0, sizeof(vkscene::DirectionalLight)};
    VkDescriptorBufferInfo pointLightsInfo{pointLightsSsboBuffer.buffer, 0, 
        sizeof(vkscene::PointLight) * vkscene::MAX_POINT_LIGHTS};
    VkDescriptorBufferInfo spotLightsInfo{spotLightsSsboBuffer.buffer, 0, 
        sizeof(vkscene::SpotLight) * vkscene::MAX_SPOT_LIGHTS};
    
    std::vector<VkWriteDescriptorSet> descriptorWrites = {
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptorSet, .dstBinding = 0,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &sceneUboInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptorSet, .dstBinding = 1,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &materialUboInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptorSet, .dstBinding = 2,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dirLightUboInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptorSet, .dstBinding = 3,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &pointLightsInfo},
        {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = descriptorSet, .dstBinding = 4,
         .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &spotLightsInfo}
    };
    vkUpdateDescriptorSets(device, descriptorWrites.size(), descriptorWrites.data(), 0, nullptr);
    
    // === СОЗДАНИЕ PIPELINE LAYOUT ===
    // Описывает push constants и descriptor set layouts
    VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants)  // Передаём матрицы и параметры каждого объекта
    };
    
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &descriptorSetLayout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pushConstantRange
    };
    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create pipeline layout!");
    }
    
    // === ЗАГРУЗКА ШЕЙДЕРОВ ===
    vertexShaderModule = loadShaderModule("shaders/simple.vert.spv");
    fragmentShaderModule = loadShaderModule("shaders/simple.frag.spv");
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertexShaderModule, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragmentShaderModule, .pName = "main"}
    };
    
    // === ОПИСАНИЕ ФОРМАТА ВЕРШИН ===
    VkVertexInputBindingDescription bindingDescription{
        0, sizeof(vkgeom::Vertex), VK_VERTEX_INPUT_RATE_VERTEX
    };
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkgeom::Vertex, position)},  // location 0: position
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkgeom::Vertex, normal)},    // location 1: normal
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vkgeom::Vertex, uv)}            // location 2: uv
    };
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size()),
        .pVertexAttributeDescriptions = attributeDescriptions.data()
    };
    
    // === НАСТРОЙКА СБОРКИ ПРИМИТИВОВ ===
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,  // Треугольники
        .primitiveRestartEnable = VK_FALSE
    };
    
    // === НАСТРОЙКА VIEWPORT И SCISSOR ===
    VkViewport viewport{0.0f, 0.0f, (float)veekay::app.window_width, 
                        (float)veekay::app.window_height, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {veekay::app.window_width, veekay::app.window_height}};
    VkPipelineViewportStateCreateInfo viewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor
    };
    
    // === НАСТРОЙКА РАСТЕРИЗАЦИИ ===
    VkPipelineRasterizationStateCreateInfo rasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,  // Заполнять треугольники
        .lineWidth = 1.0f,
        .cullMode = VK_CULL_MODE_NONE,        // Не отбрасывать задние грани (для отладки)
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE
    };
    
    // === НАСТРОЙКА МУЛЬТИСЭМПЛИНГА ===
    VkPipelineMultisampleStateCreateInfo multisampling{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT  // Без MSAA
    };
    
    // === НАСТРОЙКА DEPTH TEST ===
    VkPipelineDepthStencilStateCreateInfo depthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,        // Включаем тест глубины
        .depthWriteEnable = VK_TRUE,       // Записываем в буфер глубины
        .depthCompareOp = VK_COMPARE_OP_LESS,  // Пропускаем фрагменты ближе к камере
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE
    };
    
    // === НАСТРОЙКА СМЕШИВАНИЯ ЦВЕТОВ ===
    VkPipelineColorBlendAttachmentState colorBlendAttachment{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | 
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE  // Без прозрачности
    };
    
    VkPipelineColorBlendStateCreateInfo colorBlending{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 1,
        .pAttachments = &colorBlendAttachment
    };
    
    // === СОЗДАНИЕ ГРАФИЧЕСКОГО КОНВЕЙЕРА ===
    VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = shaderStages,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending,
        .layout = pipelineLayout,
        .renderPass = veekay::app.vk_render_pass,
        .subpass = 0
    };
    
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics pipeline!");
    }
}

// === ФУНКЦИЯ: Очистка ресурсов ===
void shutdown() {
    VkDevice& device = veekay::app.vk_device;
    vkDeviceWaitIdle(device);  // Ждём завершения всех операций GPU
    
    // Удаляем конвейер и layout
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    
    // Удаляем все буферы геометрии
    destroyBuffer(cubeVertexBuffer);
    destroyBuffer(cubeIndexBuffer);
    destroyBuffer(cylinderVertexBuffer);
    destroyBuffer(cylinderIndexBuffer);
    destroyBuffer(wheelCylinderVertexBuffer);
    destroyBuffer(wheelCylinderIndexBuffer);
    
    // Удаляем uniform буферы
    destroyBuffer(sceneUboBuffer);
    destroyBuffer(materialUboBuffer);
    destroyBuffer(dirLightUboBuffer);
    destroyBuffer(pointLightsSsboBuffer);
    destroyBuffer(spotLightsSsboBuffer);
    
    // Удаляем шейдеры
    vkDestroyShaderModule(device, fragmentShaderModule, nullptr);
    vkDestroyShaderModule(device, vertexShaderModule, nullptr);
}

// === ФУНКЦИЯ: Обновление сцены каждый кадр ===
void update(double time) {
    // Вычисляем deltaTime для плавного движения
    float currentFrame = time;
    deltaTime = currentFrame - lastFrame;
    lastFrame = currentFrame;
    
    // Обрабатываем ввод с клавиатуры
    processInput(veekay::app.window, deltaTime);
    
    // === ОБНОВЛЕНИЕ UNIFORM БУФЕРА СЦЕНЫ ===
    SceneUBO sceneUbo;
    // Матрица проекции (перспектива)
    sceneUbo.projection = glm::perspective(
        glm::radians(60.0f),  // FOV
        (float)veekay::app.window_width / (float)veekay::app.window_height,  // Aspect ratio
        0.1f, 100.0f  // Near, Far planes
    );
    sceneUbo.projection[1][1] *= -1;  // Инвертируем Y для Vulkan
    sceneUbo.view = camera.getViewMatrix();  // Матрица вида из камеры
    sceneUbo.viewPos = camera.getPosition();  // Позиция камеры для расчёта освещения
    copyDataToBuffer(sceneUboBuffer, &sceneUbo, sizeof(sceneUbo));
    
    // === ОБНОВЛЕНИЕ НАПРАВЛЕННОГО СВЕТА (ЛУНА) ===
    moonLight.direction = glm::normalize(glm::vec3(0.5f, -1.0f, 0.3f));  // Направление света
    moonLight.color = glm::vec4(0.7f, 0.8f, 1.0f, 0.4f);  // Синеватый, слабый свет
    copyDataToBuffer(dirLightUboBuffer, &moonLight, sizeof(moonLight));
    
    // === ОБНОВЛЕНИЕ ТОЧЕЧНЫХ ИСТОЧНИКОВ СВЕТА ===
    copyDataToBuffer(pointLightsSsboBuffer, pointLights.data(), 
                     sizeof(vkscene::PointLight) * pointLights.size());
    
    // === ОБНОВЛЕНИЕ ПРОЖЕКТОРОВ ===
    copyDataToBuffer(spotLightsSsboBuffer, spotLights.data(), 
                     sizeof(vkscene::SpotLight) * spotLights.size());
}

// === ФУНКЦИЯ: Рендеринг кадра ===
void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);
    
    // Начинаем запись команд
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    vkBeginCommandBuffer(cmd, &beginInfo);
    
    // Настраиваем очистку экрана
    VkClearValue clearValues[2];
    clearValues[0].color = {{0.02f, 0.03f, 0.06f, 1.0f}};  // Тёмно-синий фон (ночное небо)
    clearValues[1].depthStencil = {1.0f, 0};                // Очистка буфера глубины
    
    // Начинаем render pass
    VkRenderPassBeginInfo renderPassInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = veekay::app.vk_render_pass,
        .framebuffer = framebuffer,
        .renderArea.extent = {veekay::app.window_width, veekay::app.window_height},
        .clearValueCount = 2,
        .pClearValues = clearValues
    };
    
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
        // Привязываем конвейер и descriptor sets
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 
                                0, 1, &descriptorSet, 0, nullptr);
        
        PushConstants push;
        VkDeviceSize offset = 0;
        
        // === РИСУЕМ КУБЫ ===
        vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        
        // ЗЕМЛЯ (большой плоский куб)
        push.model = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f)), 
                               glm::vec3(60.0f, 0.05f, 60.0f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(0.08f, 0.08f, 0.09f, 1.0f);  // Тёмно-серый асфальт
        push.metallic = 0.0f;
        push.roughness = 0.98f;  // Очень матовая поверхность
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        
        // НЕБОСКРЁБЫ
        for (const auto& building : buildings) {
            push.model = glm::translate(glm::mat4(1.0f), building.position);
            push.model = glm::scale(push.model, building.scale);
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(building.color, 1.0f);
            push.metallic = building.metallic;
            push.roughness = building.roughness;
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        }
        
        // === РИСУЕМ ЦИЛИНДРЫ (столбы фонарей) ===
        vkCmdBindVertexBuffers(cmd, 0, 1, &cylinderVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cylinderIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        
        for (const auto& lamp : streetLamps) {
            push.model = glm::translate(glm::mat4(1.0f), lamp.position + glm::vec3(0.0f, 1.5f, 0.0f));
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(0.15f, 0.15f, 0.15f, 1.0f);  // Тёмно-серый металл
            push.metallic = 0.5f;
            push.roughness = 0.5f;
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cylinderIndexCount, 1, 0, 0, 0);
        }
        
        // === РИСУЕМ ЛАМПОЧКИ ФОНАРЕЙ (кубы) ===
        vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        
        for (const auto& lamp : streetLamps) {
            push.model = glm::translate(glm::mat4(1.0f), lamp.position + glm::vec3(0.0f, 3.0f, 0.0f));
            push.model = glm::scale(push.model, glm::vec3(0.2f, 0.2f, 0.2f));
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(lamp.lightColor * 1.5f, 1.0f);  // Светящийся цвет
            push.metallic = 0.0f;
            push.roughness = 0.3f;
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        }
        
        // === РИСУЕМ АВТОМОБИЛЬ ===
        glm::vec3 carPos = glm::vec3(0.0f, 0.0f, 0.0f);
        float carRotation = 0.0f;

        // КУЗОВ АВТОМОБИЛЯ
        push.model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.37f, 0.0f));
        push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        push.model = glm::scale(push.model, glm::vec3(0.8f, 0.4f, 1.6f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(0.7f, 0.08f, 0.08f, 1.0f);  // Красный
        push.metallic = 0.2f;
        push.roughness = 0.6f;
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);

        // КАБИНА АВТОМОБИЛЯ
        push.model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.65f, -0.15f));
        push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        push.model = glm::scale(push.model, glm::vec3(0.7f, 0.35f, 0.9f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(0.6f, 0.06f, 0.06f, 1.0f);  // Тёмно-красный
        push.metallic = 0.15f;
        push.roughness = 0.7f;
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);

        // КОЛЁСА (используем специальную геометрию)
        vkCmdBindVertexBuffers(cmd, 0, 1, &wheelCylinderVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, wheelCylinderIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        // Позиции 4 колёс относительно машины
        std::vector<glm::vec3> wheelOffsets = {
            {-0.45f, 0.25f, 0.6f},   // Переднее левое
            {0.45f, 0.25f, 0.6f},    // Переднее правое
            {-0.45f, 0.25f, -0.6f},  // Заднее левое
            {0.45f, 0.25f, -0.6f}    // Заднее правое
        };

        for (const auto& wheelOffset : wheelOffsets) {
            push.model = glm::translate(glm::mat4(1.0f), carPos + wheelOffset);
            push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
            push.model = glm::rotate(push.model, glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));  // Поворачиваем цилиндр горизонтально
            push.model = glm::scale(push.model, glm::vec3(0.2f, 0.15f, 0.2f));
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(0.08f, 0.08f, 0.08f, 1.0f);  // Чёрная резина
            push.metallic = 0.05f;
            push.roughness = 0.95f;  // Очень матовая
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, wheelCylinderIndexCount, 1, 0, 0, 0);
        }

        // ФАРА АВТОМОБИЛЯ (широкая светящаяся полоса)
        vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        push.model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.4f, 0.81f));
        push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        push.model = glm::scale(push.model, glm::vec3(0.7f, 0.05f, 0.04f));  // Широкая и тонкая
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(2.8f, 2.8f, 2.6f, 1.0f);  // Яркий светящийся цвет
        push.metallic = 0.95f;  // Очень металлическая (отражающая)
        push.roughness = 0.05f;  // Очень гладкая
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
    
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace

// === ФУНКЦИЯ: Главная точка входа ===
int main() {
    try {
        return veekay::run({
            .init = initialize,    // Инициализация при запуске
            .shutdown = shutdown,  // Очистка при завершении
            .update = update,      // Обновление каждый кадр
            .render = render       // Рендеринг каждый кадр
        });
    } catch (const std::exception& e) {
        std::cerr << "FATAL ERROR: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
