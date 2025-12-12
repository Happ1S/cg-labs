#include <cstdint>
#include <memory>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <fstream>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
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
#include <veekay/graphics/Texture.hpp>

namespace vkgeom = veekay::geometry;
namespace vkscene = veekay::scene;

namespace {

// Структура для передачи общих данных сцены (матрицы проекции и вида)
struct SceneUBO {
    glm::mat4 projection;
    glm::mat4 view;
    alignas(16) glm::vec3 viewPos;
};

// Push Constants - данные, отправляемые в шейдер для каждого отдельного объекта
struct PushConstants {
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec4 color;
    float metallic;
    float roughness;
    glm::vec2 uvScale; // Масштабирование UV-координат (отвечает за повторение/тайлинг текстуры)
    float useTexture;  // Флаг (1.0 = использовать текстуру, 0.0 = только цвет)
};

VkShaderModule vertexShaderModule, fragmentShaderModule;
VkDescriptorSetLayout descriptorSetLayout;
VkDescriptorPool descriptorPool;

// Наборы дескрипторов (Descriptor Sets) связывают ресурсы (буферы, текстуры) с шейдером.
// Для каждого материала/текстуры создаем свой набор.
VkDescriptorSet descriptorSet; 
VkDescriptorSet buildingDescriptorSet; // Набор для стен зданий
VkDescriptorSet roofDescriptorSet;     // Набор для крыш
VkDescriptorSet roadDescriptorSet;     // Набор для дороги
VkDescriptorSet carDescriptorSet;      // Набор для машины
VkPipelineLayout pipelineLayout;
VkPipeline pipeline;

struct VulkanBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
};

// Буферы для геометрии (вершины и индексы)
VulkanBuffer cubeVertexBuffer, cubeIndexBuffer;
VulkanBuffer cylinderVertexBuffer, cylinderIndexBuffer;
uint32_t cubeIndexCount;
uint32_t cylinderIndexCount;
VulkanBuffer wheelCylinderVertexBuffer, wheelCylinderIndexBuffer;
uint32_t wheelCylinderIndexCount;

// Буферы для освещения и материалов
VulkanBuffer sceneUboBuffer, materialUboBuffer, dirLightUboBuffer;
VulkanBuffer pointLightsSsboBuffer, spotLightsSsboBuffer;

// === ТЕКСТУРНЫЕ ОБЪЕКТЫ ===
// Указатели на объекты текстур, которые загружают картинки и создают Vulkan Image/Sampler
veekay::graphics::Texture* buildingTexture = nullptr;
veekay::graphics::Texture* roofTexture = nullptr;
veekay::graphics::Texture* carTexture = nullptr;
std::unique_ptr<veekay::graphics::Texture> roadTexture;

vkscene::Camera camera(glm::vec3(0.0f, 3.0f, 15.0f)); 
vkscene::Material buildingMaterial;
vkscene::DirectionalLight moonLight;
float moonLightIntensity = 0.4f; 
std::vector<vkscene::PointLight> pointLights;
std::vector<vkscene::SpotLight> spotLights;

bool firstMouse = true;
float lastX = 640.0f, lastY = 360.0f;
float deltaTime = 0.0f;
float lastFrame = 0.0f;

struct Building {
    glm::vec3 position;
    glm::vec3 scale;
    glm::vec3 color;
    float metallic;
    float roughness;
};

struct StreetLamp {
    glm::vec3 position;
    glm::vec3 lightColor;
    float lightRadius;
};

std::vector<Building> buildings;
std::vector<StreetLamp> streetLamps;

// Загрузка спир-в шейдера с диска
VkShaderModule loadShaderModule(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error(std::string("Failed to open shader file: ") + path);
    }
    size_t size = file.tellg();
    std::vector<char> buffer(size);
    file.seekg(0);
    file.read(buffer.data(), size);
    file.close();
    
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

// Поиск подходящего типа памяти GPU
uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(veekay::app.vk_physical_device, &memProperties);
    
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && 
            (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type!");
}

// Создание буфера Vulkan
VulkanBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, 
                          VkMemoryPropertyFlags properties) {
    VulkanBuffer result{};
    VkBufferCreateInfo bufferInfo{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    
    if (vkCreateBuffer(veekay::app.vk_device, &bufferInfo, nullptr, &result.buffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create buffer!");
    }
    
    VkMemoryRequirements memRequirements;
    vkGetBufferMemoryRequirements(veekay::app.vk_device, result.buffer, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties)
    };
    
    if (vkAllocateMemory(veekay::app.vk_device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate buffer memory!");
    }
    
    vkBindBufferMemory(veekay::app.vk_device, result.buffer, result.memory, 0);
    return result;
}

// Копирование данных из CPU в GPU буфер
void copyDataToBuffer(VulkanBuffer& buffer, const void* data, size_t size) {
    void* mappedData;
    vkMapMemory(veekay::app.vk_device, buffer.memory, 0, size, 0, &mappedData);
    memcpy(mappedData, data, size);
    vkUnmapMemory(veekay::app.vk_device, buffer.memory);
}

void destroyBuffer(const VulkanBuffer& buffer) {
    vkFreeMemory(veekay::app.vk_device, buffer.memory, nullptr);
    vkDestroyBuffer(veekay::app.vk_device, buffer.buffer, nullptr);
}

// Обработка клавиатуры (перемещение камеры, управление светом)
void processInput(GLFWwindow* window, float deltaTime) {
    float velocity = 5.0f * deltaTime;
    
    if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS)
        camera.moveForward(velocity);
    if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS)
        camera.moveForward(-velocity);
    if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS)
        camera.moveRight(-velocity);
    if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS)
        camera.moveRight(velocity);
    if (glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS)
        camera.moveUp(velocity);
    if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS)
        camera.moveUp(-velocity);
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
    
    static bool upPressed = false;
    static bool downPressed = false;
    
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS && !upPressed) {
        moonLightIntensity = glm::clamp(moonLightIntensity + 0.1f, 0.0f, 2.0f);
        upPressed = true;
        std::cout << "Moon light intensity: " << moonLightIntensity << std::endl;
    } else if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_RELEASE) {
        upPressed = false;
    }
    
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS && !downPressed) {
        moonLightIntensity = glm::clamp(moonLightIntensity - 0.1f, 0.0f, 2.0f);
        downPressed = true;
        std::cout << "Moon light intensity: " << moonLightIntensity << std::endl;
    } else if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_RELEASE) {
        downPressed = false;
    }
}

// Обработка мыши (вращение камеры)
void mouse_callback(GLFWwindow* window, double xpos, double ypos) {
    if (firstMouse) {
        lastX = xpos;
        lastY = ypos;
        firstMouse = false;
        return;
    }

    float xoffset = xpos - lastX;
    float yoffset = lastY - ypos;
    
    lastX = xpos;
    lastY = ypos;

    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    camera.rotate(xoffset, yoffset);
}

void initialize() {
    VkDevice& device = veekay::app.vk_device;
    
    glfwSetInputMode(veekay::app.window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    glfwSetCursorPosCallback(veekay::app.window, mouse_callback);
    
    // Инициализация параметров зданий
    buildings = {
        {{-6.0f, 3.5f, -5.0f}, {2.2f, 7.0f, 2.2f}, {0.12f, 0.18f, 0.25f}, 0.3f, 0.7f},
        {{0.0f, 2.0f, -4.0f}, {2.0f, 4.0f, 2.0f}, {0.2f, 0.2f, 0.22f}, 0.05f, 0.85f},
        {{6.0f, 4.0f, -3.0f}, {2.5f, 8.0f, 2.5f}, {0.08f, 0.12f, 0.2f}, 0.25f, 0.75f},
        {{-4.0f, 1.2f, 3.0f}, {1.8f, 2.4f, 1.8f}, {0.25f, 0.18f, 0.13f}, 0.02f, 0.9f},
        {{4.0f, 3.0f, 2.0f}, {1.6f, 6.0f, 1.6f}, {0.28f, 0.28f, 0.32f}, 0.2f, 0.75f},
        {{-8.0f, 1.5f, 0.0f}, {1.5f, 3.0f, 1.5f}, {0.18f, 0.18f, 0.2f}, 0.0f, 0.95f},
        {{8.0f, 5.0f, 0.0f}, {2.0f, 10.0f, 2.0f}, {0.06f, 0.08f, 0.12f}, 0.4f, 0.65f},
        {{0.0f, 1.0f, 5.0f}, {1.4f, 2.0f, 1.4f}, {0.32f, 0.32f, 0.35f}, 0.1f, 0.8f},
    };
    
    // Инициализация фонарей
    streetLamps.clear();
    std::vector<glm::vec3> lampPositions = {
        {-10.0f, 0.0f, -8.0f}, {-2.0f, 0.0f, -8.0f}, {10.0f, 0.0f, -8.0f},
        {-10.0f, 0.0f, 0.0f}, {10.0f, 0.0f, 0.0f},
        {-10.0f, 0.0f, 8.0f}, {-2.0f, 0.0f, 8.0f}, {10.0f, 0.0f, 8.0f},
    };

    for (const auto& pos : lampPositions) {
        streetLamps.push_back({pos, glm::vec3(1.0f, 0.9f, 0.7f), 12.0f});
    }
    
    // Создание геометрии (кубы и цилиндры)
    vkgeom::Cube cubeGeom;
    cubeIndexCount = cubeGeom.getIndexCount();
    
    cubeVertexBuffer = createBuffer(cubeGeom.getVerticesSizeInBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(cubeVertexBuffer, cubeGeom.getVerticesData(), cubeGeom.getVerticesSizeInBytes());
    
    cubeIndexBuffer = createBuffer(cubeGeom.getIndicesSizeInBytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(cubeIndexBuffer, cubeGeom.getIndicesData(), cubeGeom.getIndicesSizeInBytes());
    
    vkgeom::Cylinder cylinderGeom(0.1f, 3.0f, 16);
    cylinderIndexCount = cylinderGeom.getIndexCount();
    
    cylinderVertexBuffer = createBuffer(cylinderGeom.getVerticesSizeInBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(cylinderVertexBuffer, cylinderGeom.getVerticesData(), cylinderGeom.getVerticesSizeInBytes());
    
    cylinderIndexBuffer = createBuffer(cylinderGeom.getIndicesSizeInBytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(cylinderIndexBuffer, cylinderGeom.getIndicesData(), cylinderGeom.getIndicesSizeInBytes());
    
    // Геометрия колес
    vkgeom::Cylinder wheelCylinderGeom(0.8f, 0.5f, 20, true);
    wheelCylinderIndexCount = wheelCylinderGeom.getIndexCount();

    wheelCylinderVertexBuffer = createBuffer(wheelCylinderGeom.getVerticesSizeInBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(wheelCylinderVertexBuffer, wheelCylinderGeom.getVerticesData(), wheelCylinderGeom.getVerticesSizeInBytes());

    wheelCylinderIndexBuffer = createBuffer(wheelCylinderGeom.getIndicesSizeInBytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(wheelCylinderIndexBuffer, wheelCylinderGeom.getIndicesData(), wheelCylinderGeom.getIndicesSizeInBytes());

    // Настройка источников света
    pointLights.clear();
    for (const auto& lamp : streetLamps) {
        vkscene::PointLight light;
        light.position = lamp.position + glm::vec3(0.0f, 3.0f, 0.0f);
        light.color = glm::vec4(lamp.lightColor, 1.5f);
        light.constant = 1.0f; light.linear = 0.09f; light.quadratic = 0.032f;
        pointLights.push_back(light);
    }
    
    // Фары машины (прожекторы)
    spotLights.clear();
    glm::vec3 carPos = glm::vec3(0.0f, 0.0f, 0.0f);
    glm::vec3 carDirection = glm::vec3(0.0f, 0.0f, 1.0f);
    
    vkscene::SpotLight wideHeadlight;
    wideHeadlight.position = carPos + glm::vec3(0.0f, 0.4f, 0.81f); 
    wideHeadlight.direction = glm::normalize(carDirection + glm::vec3(0.0f, -0.1f, 0.0f));
    wideHeadlight.color = glm::vec4(1.0f, 0.95f, 0.85f, 2.2f);
    wideHeadlight.constant = 1.0f; wideHeadlight.linear = 0.06f; wideHeadlight.quadratic = 0.012f;
    wideHeadlight.cutOff = glm::cos(glm::radians(45.0f));
    wideHeadlight.outerCutOff = glm::cos(glm::radians(65.0f));
    spotLights.push_back(wideHeadlight);
    
    // === ЗАГРУЗКА ТЕКСТУР ===
    // Здесь мы загружаем файлы изображений (JPG/PNG).
    // Класс Texture внутри себя создает VkImage, VkImageView и VkSampler.
    try {
        buildingTexture = new veekay::graphics::Texture("textures/building.jpg");
        std::cout << "Текстура здания загружена успешно" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Внимание: Не удалось загрузить текстуру здания: " << e.what() << std::endl;
        buildingTexture = nullptr;
    }
    
    try {
        roofTexture = new veekay::graphics::Texture("textures/roof.jpg");
        std::cout << "Текстура крыши загружена успешно" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Внимание: Не удалось загрузить текстуру крыши: " << e.what() << std::endl;
        roofTexture = nullptr;
    }
    
    try {
        carTexture = new veekay::graphics::Texture("textures/car.jpg");
        std::cout << "Текстура машины загружена успешно" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Внимание: Не удалось загрузить текстуру машины: " << e.what() << std::endl;
        carTexture = nullptr;
    }
    
    try {
        roadTexture = std::make_unique<veekay::graphics::Texture>("textures/road.jpg");
        std::cout << "Текстура дороги загружена успешно" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Внимание: Не удалось загрузить текстуру дороги: " << e.what() << std::endl;
        roadTexture = nullptr;
    }
    
    // Создание Uniform буферов (для матриц, света и т.д.)
    sceneUboBuffer = createBuffer(sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    materialUboBuffer = createBuffer(sizeof(vkscene::Material), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    dirLightUboBuffer = createBuffer(sizeof(vkscene::DirectionalLight), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    pointLightsSsboBuffer = createBuffer(sizeof(vkscene::PointLight) * vkscene::MAX_POINT_LIGHTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    spotLightsSsboBuffer = createBuffer(sizeof(vkscene::SpotLight) * vkscene::MAX_SPOT_LIGHTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    // === ОПИСАНИЕ ДЕСКРИПТОРОВ ===
    // Указываем, какие данные шейдер ожидает получить.
    // Binding 5 - это наша текстура (COMBINED_IMAGE_SAMPLER)
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr} // Текстура
    };
    
    VkDescriptorSetLayoutCreateInfo layoutInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<uint32_t>(bindings.size()),
        .pBindings = bindings.data()
    };
    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor set layout!");
    }
    
    // === ПУЛ ДЕСКРИПТОРОВ ===
    // Выделяем память под дескрипторы. Важно увеличить количество для COMBINED_IMAGE_SAMPLER,
    // так как у нас теперь много текстур (дорога, здания, крыши, машина).
    std::vector<VkDescriptorPoolSize> poolSizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * 4},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5} // Место для 5 текстур
    };
    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 5,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data()
    };
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool!");
    }
    
    // Выделение наборов дескрипторов (Sets) для каждого типа объекта
    std::vector<VkDescriptorSetLayout> layouts(5, descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 5,
        .pSetLayouts = layouts.data()
    };
    std::vector<VkDescriptorSet> descriptorSets(5);
    if (vkAllocateDescriptorSets(device, &allocInfo, descriptorSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets!");
    }
    descriptorSet = descriptorSets[0]; 
    buildingDescriptorSet = descriptorSets[1];
    roadDescriptorSet = descriptorSets[2];
    carDescriptorSet = descriptorSets[3];
    roofDescriptorSet = descriptorSets[4];
    
    // Информация о буферах для обновления дескрипторов
    VkDescriptorBufferInfo sceneUboInfo{sceneUboBuffer.buffer, 0, sizeof(SceneUBO)};
    VkDescriptorBufferInfo materialUboInfo{materialUboBuffer.buffer, 0, sizeof(vkscene::Material)};
    VkDescriptorBufferInfo dirLightUboInfo{dirLightUboBuffer.buffer, 0, sizeof(vkscene::DirectionalLight)};
    VkDescriptorBufferInfo pointLightsInfo{pointLightsSsboBuffer.buffer, 0, sizeof(vkscene::PointLight) * vkscene::MAX_POINT_LIGHTS};
    VkDescriptorBufferInfo spotLightsInfo{spotLightsSsboBuffer.buffer, 0, sizeof(vkscene::SpotLight) * vkscene::MAX_SPOT_LIGHTS};
    
    // === ЗАПОЛНЕНИЕ ДЕСКРИПТОРОВ ===
    // Эта лямбда-функция привязывает конкретную текстуру (VkImageView + VkSampler)
    // к конкретному набору дескрипторов (Descriptor Set).
    auto fillDescriptorSet = [&](VkDescriptorSet set, veekay::graphics::Texture* texture) {
        VkDescriptorImageInfo imageInfo{};
        // Если текстура передана, берем её параметры.
        if (texture) {
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = texture->getImageView();
            imageInfo.sampler = texture->getSampler();
        } else if (buildingTexture) {
            // Фолбэк на текстуру здания, если ничего не передано
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = buildingTexture->getImageView();
            imageInfo.sampler = buildingTexture->getSampler();
        }

        std::vector<VkWriteDescriptorSet> writes = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &sceneUboInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &materialUboInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dirLightUboInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &pointLightsInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &spotLightsInfo},
            // Привязка текстуры к биндингу 5
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 5, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imageInfo}
        };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    // Заполняем каждый набор своей текстурой
    fillDescriptorSet(descriptorSet, buildingTexture); 
    fillDescriptorSet(buildingDescriptorSet, buildingTexture);
    fillDescriptorSet(roadDescriptorSet, roadTexture ? roadTexture.get() : nullptr);
    fillDescriptorSet(carDescriptorSet, carTexture);
    fillDescriptorSet(roofDescriptorSet, roofTexture);
    
    // Настройка Pipeline Layout и Push Constants
    VkPushConstantRange pushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(PushConstants)
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
    
    // Создание графического пайплайна (загрузка шейдеров, настройка состояний)
    vertexShaderModule = loadShaderModule("shaders/simple.vert.spv");
    fragmentShaderModule = loadShaderModule("shaders/simple.frag.spv");
    
    VkPipelineShaderStageCreateInfo shaderStages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vertexShaderModule, .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fragmentShaderModule, .pName = "main"}
    };
    
    VkVertexInputBindingDescription bindingDescription{0, sizeof(vkgeom::Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributeDescriptions = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkgeom::Vertex, position)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(vkgeom::Vertex, normal)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(vkgeom::Vertex, uv)} // UV-координаты для текстур
    };
    
    VkPipelineVertexInputStateCreateInfo vertexInputInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bindingDescription,
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size()),
        .pVertexAttributeDescriptions = attributeDescriptions.data()
    };
    
    VkPipelineInputAssemblyStateCreateInfo inputAssembly{.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, .primitiveRestartEnable = VK_FALSE};
    VkViewport viewport{0.0f, 0.0f, (float)veekay::app.window_width, (float)veekay::app.window_height, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {veekay::app.window_width, veekay::app.window_height}};
    VkPipelineViewportStateCreateInfo viewportState{.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .pViewports = &viewport, .scissorCount = 1, .pScissors = &scissor};
    VkPipelineRasterizationStateCreateInfo rasterizer{.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .depthClampEnable = VK_FALSE, .rasterizerDiscardEnable = VK_FALSE, .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_CLOCKWISE, .depthBiasEnable = VK_FALSE};
    VkPipelineMultisampleStateCreateInfo multisampling{.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .sampleShadingEnable = VK_FALSE, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depthStencil{.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_LESS, .depthBoundsTestEnable = VK_FALSE, .stencilTestEnable = VK_FALSE};
    VkPipelineColorBlendAttachmentState colorBlendAttachment{.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT, .blendEnable = VK_FALSE};
    VkPipelineColorBlendStateCreateInfo colorBlending{.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .logicOpEnable = VK_FALSE, .attachmentCount = 1, .pAttachments = &colorBlendAttachment};
    
    VkGraphicsPipelineCreateInfo pipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = shaderStages,
        .pVertexInputState = &vertexInputInfo, .pInputAssemblyState = &inputAssembly,
        .pViewportState = &viewportState, .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depthStencil,
        .pColorBlendState = &colorBlending, .layout = pipelineLayout,
        .renderPass = veekay::app.vk_render_pass, .subpass = 0
    };
    
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create graphics pipeline!");
    }
}

void shutdown() {
    // Очистка ресурсов Vulkan при выходе
    VkDevice& device = veekay::app.vk_device;
    vkDeviceWaitIdle(device);
    
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
    vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
    vkDestroyDescriptorPool(device, descriptorPool, nullptr);
    
    destroyBuffer(cubeVertexBuffer); destroyBuffer(cubeIndexBuffer);
    destroyBuffer(cylinderVertexBuffer); destroyBuffer(cylinderIndexBuffer);
    destroyBuffer(wheelCylinderVertexBuffer); destroyBuffer(wheelCylinderIndexBuffer);
    destroyBuffer(sceneUboBuffer); destroyBuffer(materialUboBuffer); destroyBuffer(dirLightUboBuffer);
    destroyBuffer(pointLightsSsboBuffer); destroyBuffer(spotLightsSsboBuffer);
    
    delete buildingTexture;
    delete carTexture;
    delete roofTexture;
    // roadTexture удалится автоматически (unique_ptr)
    
    vkDestroyShaderModule(device, fragmentShaderModule, nullptr);
    vkDestroyShaderModule(device, vertexShaderModule, nullptr);
}

void update(double time) {
    float currentFrame = time;
    deltaTime = currentFrame - lastFrame;
    lastFrame = currentFrame;
    
    processInput(veekay::app.window, deltaTime);
    
    // Обновление UBO (камера)
    SceneUBO sceneUbo;
    sceneUbo.projection = glm::perspective(glm::radians(60.0f), (float)veekay::app.window_width / (float)veekay::app.window_height, 0.1f, 100.0f);
    sceneUbo.projection[1][1] *= -1;
    sceneUbo.view = camera.getViewMatrix();
    sceneUbo.viewPos = camera.getPosition();
    copyDataToBuffer(sceneUboBuffer, &sceneUbo, sizeof(sceneUbo));
    
    // Обновление света
    moonLight.direction = glm::normalize(glm::vec3(0.5f, -1.0f, 0.3f));
    moonLight.color = glm::vec4(0.7f, 0.8f, 1.0f, moonLightIntensity); 
    copyDataToBuffer(dirLightUboBuffer, &moonLight, sizeof(moonLight));
    
    copyDataToBuffer(pointLightsSsboBuffer, pointLights.data(), sizeof(vkscene::PointLight) * pointLights.size());
    copyDataToBuffer(spotLightsSsboBuffer, spotLights.data(), sizeof(vkscene::SpotLight) * spotLights.size());
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &beginInfo);
    
    VkClearValue clearValues[2];
    clearValues[0].color = {{0.02f, 0.03f, 0.06f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    
    VkRenderPassBeginInfo renderPassInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = veekay::app.vk_render_pass,
        .framebuffer = framebuffer,
        .renderArea.extent = {veekay::app.window_width, veekay::app.window_height},
        .clearValueCount = 2, .pClearValues = clearValues
    };
    
    vkCmdBeginRenderPass(cmd, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        
        PushConstants push;
        VkDeviceSize offset = 0;
        
        // === ОТРИСОВКА КУБОВ ===
        vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        
        // --- ДОРОГА (Ground) ---
        // 1. Биндим DescriptorSet с текстурой дороги
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 
                                0, 1, &roadDescriptorSet, 0, nullptr);
                                
        push.model = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 0.0f)), 
                               glm::vec3(60.0f, 0.05f, 60.0f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
        push.metallic = 0.0f;
        push.roughness = 0.98f;
        // ТЕКСТУРИРОВАНИЕ ДОРОГИ:
        // Используем uvScale 10.0, чтобы текстура повторилась 10 раз (тайлинг).
        // Если оставить 1.0, пиксели асфальта будут огромными.
        push.uvScale = glm::vec2(10.0f, 10.0f); 
        push.useTexture = 1.0f; // Включаем текстуру
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                          0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        
        // --- ЗДАНИЯ (Skyscrapers) ---
        // 1. Биндим текстуру стен
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 
                                0, 1, &buildingDescriptorSet, 0, nullptr);
        
        for (const auto& building : buildings) {
            push.model = glm::translate(glm::mat4(1.0f), building.position);
            push.model = glm::scale(push.model, building.scale);
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f); 
            push.metallic = building.metallic;
            push.roughness = building.roughness;
            
            // ТЕКСТУРИРОВАНИЕ СТЕН:
            // Масштабируем текстуру пропорционально размеру здания (building.scale).
            // Множитель 1.2 делает кирпичи/окна чуть мельче.
            push.uvScale = glm::vec2(building.scale.x, building.scale.y) * 1.2f; 
            push.useTexture = 1.0f; 
            
            // Перебиндиваем, чтобы гарантировать правильную текстуру на каждой итерации
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 
                                0, 1, &buildingDescriptorSet, 0, nullptr);
            
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
            
            // --- КРЫША (отдельный проход) ---
            // Биндим текстуру крыши
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 
                                0, 1, &roofDescriptorSet, 0, nullptr);
            
            float roofY = building.position.y + building.scale.y * 0.5f + 0.002f;
            push.model = glm::translate(glm::mat4(1.0f), glm::vec3(building.position.x, roofY, building.position.z));
            push.model = glm::scale(push.model, glm::vec3(building.scale.x, 0.01f, building.scale.z));
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.uvScale = glm::vec2(1.0f, 1.0f); // Крыша растянута ровно один раз
            
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        }
        
        // === ЦИЛИНДРЫ (Фонарные столбы) ===
        vkCmdBindVertexBuffers(cmd, 0, 1, &cylinderVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cylinderIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        
        for (const auto& lamp : streetLamps) {
            push.model = glm::translate(glm::mat4(1.0f), lamp.position + glm::vec3(0.0f, 1.5f, 0.0f));
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(0.15f, 0.15f, 0.15f, 1.0f);
            push.metallic = 0.5f;
            push.roughness = 0.5f;
            push.uvScale = glm::vec2(1.0f, 1.0f); 
            push.useTexture = 0.0f; // Текстуры нет, используем чистый цвет
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cylinderIndexCount, 1, 0, 0, 0);
        }
        
        // === ЛАМПЫ (Светящиеся шары) ===
        vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        
        for (const auto& lamp : streetLamps) {
            push.model = glm::translate(glm::mat4(1.0f), lamp.position + glm::vec3(0.0f, 3.0f, 0.0f));
            push.model = glm::scale(push.model, glm::vec3(0.2f, 0.2f, 0.2f));
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(lamp.lightColor * 1.5f, 1.0f);
            push.metallic = 0.0f;
            push.roughness = 0.3f;
            push.uvScale = glm::vec2(1.0f, 1.0f); 
            push.useTexture = 0.0f; 
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                              0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        }
        
        // === МАШИНА (CAR) ===
        glm::vec3 carPos = glm::vec3(0.0f, 0.0f, 0.0f);
        float carRotation = 0.0f;

        // --- Корпус (Body) ---
        // Биндим DescriptorSet машины с загруженной текстурой car.jpg
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout, 
                                0, 1, &carDescriptorSet, 0, nullptr);
        push.model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.37f, 0.0f));
        push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        push.model = glm::scale(push.model, glm::vec3(0.8f, 0.4f, 1.6f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        
        // Высокая яркость цвета для PBR-блеска в темноте
        push.color = glm::vec4(50.0f, 50.0f, 50.0f, 1.0f); 
        push.metallic = 1.0f;   // Металл
        push.roughness = 0.05f; // Гладкая поверхность
        
        // ТЕКСТУРИРОВАНИЕ МАШИНЫ:
        // Масштаб 1.0, чтобы текстура легла один-в-один на развертку модели
        push.uvScale = glm::vec2(1.0f, 1.0f); 
        push.useTexture = 1.0f; 
        
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);

        // --- Кабина (Cabin) ---
        push.model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.65f, -0.15f));
        push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        push.model = glm::scale(push.model, glm::vec3(0.7f, 0.35f, 0.9f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(50.0f, 50.0f, 50.0f, 1.0f); 
        push.metallic = 1.0f; 
        push.roughness = 0.05f; 
        push.uvScale = glm::vec2(1.0f, 1.0f); 
        push.useTexture = 1.0f; // Используем ту же текстуру машины
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);

        // --- Колеса (Wheels) ---
        vkCmdBindVertexBuffers(cmd, 0, 1, &wheelCylinderVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, wheelCylinderIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        std::vector<glm::vec3> wheelOffsets = {
            {-0.45f, 0.25f, 0.6f}, {0.45f, 0.25f, 0.6f},
            {-0.45f, 0.25f, -0.6f}, {0.45f, 0.25f, -0.6f}
        };

        for (const auto& wheelOffset : wheelOffsets) {
            push.model = glm::translate(glm::mat4(1.0f), carPos + wheelOffset);
            push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
            push.model = glm::rotate(push.model, glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
            push.model = glm::scale(push.model, glm::vec3(0.2f, 0.15f, 0.2f));
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(0.08f, 0.08f, 0.08f, 1.0f);
            push.metallic = 0.05f; // Резина не металлическая
            push.roughness = 0.95f; // Резина матовая
            push.uvScale = glm::vec2(1.0f, 1.0f);
            push.useTexture = 0.0f; // Без текстуры
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, wheelCylinderIndexCount, 1, 0, 0, 0);
        }

        // --- Фары (Headlights Mesh) ---
        vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        push.model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.4f, 0.81f)); 
        push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        push.model = glm::scale(push.model, glm::vec3(0.7f, 0.05f, 0.04f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(70.0f, 70.0f, 68.0f, 1.0f);
        push.metallic = 0.95f;
        push.roughness = 0.05f;
        push.uvScale = glm::vec2(1.0f, 1.0f); 
        push.useTexture = 0.0f; 
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
    
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace

int main() {
    try {
        return veekay::run({
            .init = initialize,
            .shutdown = shutdown,
            .update = update,
            .render = render
        });
    } catch (const std::exception& e) {
        std::cerr << "КРИТИЧЕСКАЯ ОШИБКА: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }
}
