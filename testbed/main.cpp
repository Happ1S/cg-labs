#include <cstdint>
#include <memory>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <fstream>
#include <cmath>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <GLFW/glfw3.h>
#include <vulkan/vulkan.h>  // Для типов функций Dynamic Rendering
#include <veekay/veekay.hpp>
#include <veekay/geometry/Vertex.hpp>
#include <veekay/geometry/Cube.hpp>
#include <veekay/geometry/Cylinder.hpp>
#include <veekay/geometry/Sphere.hpp>
#include <veekay/scene/Camera.hpp>
#include <veekay/scene/Lights.hpp>
#include <veekay/scene/Material.hpp>
#include <veekay/graphics/Texture.hpp>

namespace vkgeom = veekay::geometry;
namespace vkscene = veekay::scene;

namespace {

// Структура push-констант для shadow pass
struct ShadowPushConstants {
    glm::mat4 model;
    glm::mat4 lightSpaceMatrix;
};

// Структура для передачи общих данных сцены (матрицы проекции и вида)
struct SceneUBO {
    // ВАЖНО: на macOS/MoltenVK строго соблюдаем std140 выравнивание,
    // иначе шейдер может читать мусор (особенно для mat4 после vec3).
    alignas(16) glm::mat4 projection;
    alignas(16) glm::mat4 view;
    alignas(16) glm::vec3 viewPos;
    float _pad0 = 0.0f; // std140 padding (vec3 занимает 16 байт)
    alignas(16) glm::mat4 lightSpaceMatrix; // Матрица для преобразования в пространство света
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
    float debugShadowMap; // Новый флаг для отладки shadow map
};

VkShaderModule vertexShaderModule, fragmentShaderModule;
VkShaderModule shadowVertexShaderModule; // Шейдер для shadow pass
VkDescriptorSetLayout descriptorSetLayout;
VkDescriptorPool descriptorPool;

// Наборы дескрипторов (Descriptor Sets) связывают ресурсы (буферы, текстуры) с шейдером.
// Для каждого материала/текстуры создаем свой набор.
VkDescriptorSet descriptorSet;
VkDescriptorSet buildingDescriptorSet; // Набор для стен зданий
VkDescriptorSet roofDescriptorSet;     // Набор для крыш
VkDescriptorSet roadDescriptorSet;     // Набор для дороги
VkDescriptorSet carDescriptorSet;      // Набор для машины
VkDescriptorSet moonDescriptorSet;     // Набор для луны
VkDescriptorSet skyDescriptorSet;      // Набор для неба
VkPipelineLayout pipelineLayout;
VkPipeline pipeline;
VkPipeline skyPipeline; // Пайплайн для неба (без записи в depth)
VkPipelineLayout shadowPipelineLayout;
VkPipeline shadowPipeline; // Пайплайн для shadow pass

// Shadow map resources
VkImage shadowMapImage;
VkDeviceMemory shadowMapMemory;
VkImageView shadowMapView;
VkSampler shadowMapSampler;
VkSampler shadowMapDebugSampler;
VkRenderPass shadowRenderPass; // RenderPass для shadow map (альтернатива Dynamic Rendering)
VkFramebuffer shadowFramebuffer; // Framebuffer для shadow map
constexpr uint32_t SHADOW_MAP_SIZE = 2048;
VkFormat shadowMapFormat = VK_FORMAT_UNDEFINED;

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
VulkanBuffer sphereVertexBuffer, sphereIndexBuffer;
uint32_t sphereIndexCount;

// Буферы для освещения и материалов
VulkanBuffer sceneUboBuffer, materialUboBuffer, dirLightUboBuffer;
VulkanBuffer pointLightsSsboBuffer, spotLightsSsboBuffer;

// === ТЕКСТУРНЫЕ ОБЪЕКТЫ ===
// Указатели на объекты текстур, которые загружают картинки и создают Vulkan Image/Sampler
veekay::graphics::Texture* buildingTexture = nullptr;
veekay::graphics::Texture* roofTexture = nullptr;
veekay::graphics::Texture* carTexture = nullptr;
veekay::graphics::Texture* moonTexture = nullptr;
veekay::graphics::Texture* skyTexture = nullptr;
std::unique_ptr<veekay::graphics::Texture> roadTexture;

vkscene::Camera camera(glm::vec3(0.0f, 3.0f, 15.0f)); 
vkscene::Material buildingMaterial;
vkscene::DirectionalLight moonLight;
float moonLightIntensity = 1.5f; // Увеличена начальная интенсивность для лучшей видимости
float moonAngle = glm::pi<float>() * 0.25f; // Начальный угол (45 градусов - луна видна) 
std::vector<vkscene::PointLight> pointLights;
std::vector<vkscene::SpotLight> spotLights;

bool firstMouse = true;
float lastX = 640.0f, lastY = 360.0f;
float deltaTime = 0.0f;
float lastFrame = 0.0f;
bool debugShadowMapEnabled = false; // true = показывать raw shadow depth (режим отладки)

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

// Вспомогательные функции для командных буферов
VkCommandBuffer beginSingleTimeCommands() {
    VkCommandBufferAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = veekay::app.vk_command_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1
    };
    
    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(veekay::app.vk_device, &allocInfo, &commandBuffer);
    
    VkCommandBufferBeginInfo beginInfo{
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    
    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
    vkEndCommandBuffer(commandBuffer);
    
    VkSubmitInfo submitInfo{
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &commandBuffer
    };
    
    vkQueueSubmit(veekay::app.vk_graphics_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(veekay::app.vk_graphics_queue);
    
    vkFreeCommandBuffers(veekay::app.vk_device, veekay::app.vk_command_pool, 1, &commandBuffer);
}

// Вычисление матрицы пространства света (используется в update и renderShadowPass)
glm::mat4 calculateLightSpaceMatrix() {
    float nearPlane = 0.1f, farPlane = 150.0f;  // Увеличиваем дальность
    float orthoSize = 100.0f;  // Увеличиваем размер для лучшего покрытия сцены
    
    // На macOS/MoltenVK особенно важно использовать Vulkan clip-space:
    // z должен быть в диапазоне [0..w], иначе геометрия может полностью клипаться и shadow map остаётся “пустой”.
    glm::mat4 lightProjection = glm::orthoRH_ZO(-orthoSize, orthoSize, -orthoSize, orthoSize, nearPlane, farPlane);
    // Используем текущее направление луны
    glm::vec3 lightPos = -moonLight.direction * 50.0f;  // Отодвигаем источник света дальше
    glm::mat4 lightView = glm::lookAtRH(lightPos, glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    
    return lightProjection * lightView;
}

VkFormat pickShadowMapDepthFormat() {
    // Нам нужен формат, который:
    // - можно использовать как depth attachment
    // - можно семплировать в шейдере (shadow map)
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D16_UNORM,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };

    VkPhysicalDevice physicalDevice = veekay::app.vk_physical_device;
    for (VkFormat fmt : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, fmt, &props);

        const VkFormatFeatureFlags needed =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;

        if ((props.optimalTilingFeatures & needed) == needed) {
            return fmt;
        }
    }

    throw std::runtime_error("No supported depth format for shadow map (attachment+sampled).");
}

// Создание shadow map (текстура глубины для теней)
void createShadowMap() {
    VkDevice& device = veekay::app.vk_device;
    
    // Выбираем формат, который и рендерится, и семплится.
    shadowMapFormat = pickShadowMapDepthFormat();
    
    // Создание изображения для shadow map
    VkImageCreateInfo imageInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = shadowMapFormat,
        .extent = {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED
    };
    
    if (vkCreateImage(device, &imageInfo, nullptr, &shadowMapImage) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map image!");
    }
    
    // Выделение памяти
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(device, shadowMapImage, &memRequirements);
    
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = memRequirements.size,
        .memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
    };
    
    if (vkAllocateMemory(device, &allocInfo, nullptr, &shadowMapMemory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate shadow map memory!");
    }
    
    vkBindImageMemory(device, shadowMapImage, shadowMapMemory, 0);
    
    // Создание ImageView
    VkImageViewCreateInfo viewInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = shadowMapImage,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = shadowMapFormat,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1
        }
    };
    
    if (vkCreateImageView(device, &viewInfo, nullptr, &shadowMapView) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map view!");
    }
    
    // Создание сэмплера с поддержкой сравнения (для PCF)
    VkSamplerCreateInfo samplerInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_TRUE,  // Включаем сравнение
        .compareOp = VK_COMPARE_OP_LESS_OR_EQUAL,  // Для shadow mapping
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE,
        .unnormalizedCoordinates = VK_FALSE,
        .minLod = 0.0f,
        .maxLod = 0.0f
    };
    
    if (vkCreateSampler(device, &samplerInfo, nullptr, &shadowMapSampler) != VK_SUCCESS) {
    throw std::runtime_error("Failed to create shadow map sampler!");
    }

    // Debug sampler (raw depth): compareEnable = VK_FALSE
    VkSamplerCreateInfo debugSamplerInfo = samplerInfo;
    debugSamplerInfo.compareEnable = VK_FALSE;
    debugSamplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    debugSamplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;

    if (vkCreateSampler(device, &debugSamplerInfo, nullptr, &shadowMapDebugSampler) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map DEBUG sampler!");
    }

    // ВАЖНО: render pass использует initialLayout = READ_ONLY (см. ниже),
    // поэтому нужно один раз перевести изображение из UNDEFINED в READ_ONLY после создания.
    {
        VkCommandBuffer cmd = beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = shadowMapImage,
            .subresourceRange = {
                .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        };

        vkCmdPipelineBarrier(
            cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier
        );

        endSingleTimeCommands(cmd);
    }
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
    
    // Переключение режима отладки shadow map по нажатию клавиши Z
    static bool zPressed = false;
    if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_PRESS && !zPressed) {
        debugShadowMapEnabled = !debugShadowMapEnabled;
        zPressed = true;
        std::cout << "Debug Shadow Map: " << (debugShadowMapEnabled ? "Enabled" : "Disabled") << std::endl;
    } else if (glfwGetKey(window, GLFW_KEY_Z) == GLFW_RELEASE) {
        zPressed = false;
    }
    
    // Управление интенсивностью луны стрелками вверх/вниз (плавно при удержании)
    const float intensitySpeed = 1.5f; // единиц интенсивности в секунду
    if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) {
        moonLightIntensity = glm::clamp(moonLightIntensity + intensitySpeed * deltaTime, 0.0f, 5.0f);
    }
    if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
        moonLightIntensity = glm::clamp(moonLightIntensity - intensitySpeed * deltaTime, 0.0f, 5.0f);
    }

    // Управление направлением луны стрелками влево/вправо (плавно при удержании)
    const float moonAngularSpeed = 0.9f; // радиан в секунду
    if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) {
        moonAngle -= moonAngularSpeed * deltaTime;
    }
    if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) {
        moonAngle += moonAngularSpeed * deltaTime;
    }

    // Нормализуем угол в [0..2π)
    const float twoPi = 2.0f * glm::pi<float>();
    moonAngle = std::fmod(moonAngle, twoPi);
    if (moonAngle < 0.0f) moonAngle += twoPi;
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
    
    // Используем RenderPass вместо Dynamic Rendering (работает на всех платформах, включая macOS)
    // Создаем RenderPass для shadow map
    
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
    
    // Геометрия сферы для луны
    vkgeom::Sphere sphereGeom(1.0f, 32, 16);
    sphereIndexCount = sphereGeom.getIndexCount();
    
    sphereVertexBuffer = createBuffer(sphereGeom.getVerticesSizeInBytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(sphereVertexBuffer, sphereGeom.getVerticesData(), sphereGeom.getVerticesSizeInBytes());
    
    sphereIndexBuffer = createBuffer(sphereGeom.getIndicesSizeInBytes(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    copyDataToBuffer(sphereIndexBuffer, sphereGeom.getIndicesData(), sphereGeom.getIndicesSizeInBytes());

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
    
    // Загрузка текстуры луны
    try {
        moonTexture = new veekay::graphics::Texture("textures/moon.jpg");
        std::cout << "Текстура луны загружена успешно" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Внимание: Не удалось загрузить текстуру луны: " << e.what() << std::endl;
        std::cerr << "Используется текстура здания как fallback" << std::endl;
        moonTexture = buildingTexture; // Fallback на текстуру здания
    }

    // Загрузка текстуры неба (панорама 5000x3000)
    try {
        skyTexture = new veekay::graphics::Texture("textures/sky.jpg");
        std::cout << "Sky texture loaded successfully" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Warning: failed to load sky texture: " << e.what() << std::endl;
        skyTexture = nullptr;
    }
    
    // Создание Uniform буферов (для матриц, света и т.д.)
    sceneUboBuffer = createBuffer(sizeof(SceneUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    materialUboBuffer = createBuffer(sizeof(vkscene::Material), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    dirLightUboBuffer = createBuffer(sizeof(vkscene::DirectionalLight), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    pointLightsSsboBuffer = createBuffer(sizeof(vkscene::PointLight) * vkscene::MAX_POINT_LIGHTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    spotLightsSsboBuffer = createBuffer(sizeof(vkscene::SpotLight) * vkscene::MAX_SPOT_LIGHTS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    
    // === СОЗДАНИЕ SHADOW MAP ===
    createShadowMap();
    
    // === СОЗДАНИЕ RENDERPASS ДЛЯ SHADOW MAP ===
    // Используем RenderPass вместо Dynamic Rendering для совместимости с macOS
    VkAttachmentDescription shadowDepthAttachment{
        .format = shadowMapFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    };
    
    VkAttachmentReference shadowDepthRef{
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    };
    
    VkSubpassDescription shadowSubpass{
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 0,
        .pColorAttachments = nullptr,
        .pDepthStencilAttachment = &shadowDepthRef
    };
    
    VkSubpassDependency dependencies[2];

    // Первая зависимость: READ_ONLY (семплинг прошлым кадром) -> DEPTH_ATTACHMENT (запись этим кадром)
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = 0; // VK_DEPENDENCY_BY_REGION_BIT;

    // Вторая зависимость: для перехода из DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    // в DEPTH_STENCIL_READ_ONLY_OPTIMAL (после рендера в shadow map, перед основным проходом)
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = 0; // VK_DEPENDENCY_BY_REGION_BIT;
    
    VkRenderPassCreateInfo shadowRenderPassInfo{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &shadowDepthAttachment,
        .subpassCount = 1,
        .pSubpasses = &shadowSubpass,
        .dependencyCount = 2, // Теперь у нас 2 зависимости
        .pDependencies = dependencies // Указываем на массив зависимостей
    };
    
    if (vkCreateRenderPass(device, &shadowRenderPassInfo, nullptr, &shadowRenderPass) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow render pass!");
    }
    
    // Создаем Framebuffer для shadow map
    VkFramebufferCreateInfo shadowFramebufferInfo{
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = shadowRenderPass,
        .attachmentCount = 1,
        .pAttachments = &shadowMapView,
        .width = SHADOW_MAP_SIZE,
        .height = SHADOW_MAP_SIZE,
        .layers = 1
    };
    
    if (vkCreateFramebuffer(device, &shadowFramebufferInfo, nullptr, &shadowFramebuffer) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow framebuffer!");
    }
    
    // === ОПИСАНИЕ ДЕСКРИПТОРОВ ===
    // Указываем, какие данные шейдер ожидает получить.
    // Binding 5 - это наша текстура (COMBINED_IMAGE_SAMPLER)
    // Binding 6 - это shadow map (COMBINED_IMAGE_SAMPLER с comparison)
    std::vector<VkDescriptorSetLayoutBinding> bindings = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}, // Текстура
        {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},  // Shadow map
        {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}  // Debug Shadow map Raw
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
    // так как у нас теперь много текстур (дорога, здания, крыши, машина, луна) + shadow map + debug raw depth.
    std::vector<VkDescriptorPoolSize> poolSizes = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3 * 7},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * 7},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8 * 7} // + debugShadowMapRaw
    };
    VkDescriptorPoolCreateInfo poolInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 7,
        .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
        .pPoolSizes = poolSizes.data()
    };
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create descriptor pool!");
    }
    
    // Выделение наборов дескрипторов (Sets) для каждого типа объекта
    std::vector<VkDescriptorSetLayout> layouts(7, descriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = descriptorPool,
        .descriptorSetCount = 7,
        .pSetLayouts = layouts.data()
    };
    std::vector<VkDescriptorSet> descriptorSets(7);
    if (vkAllocateDescriptorSets(device, &allocInfo, descriptorSets.data()) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate descriptor sets!");
    }
    descriptorSet = descriptorSets[0]; 
    buildingDescriptorSet = descriptorSets[1];
    roadDescriptorSet = descriptorSets[2];
    carDescriptorSet = descriptorSets[3];
    roofDescriptorSet = descriptorSets[4];
    moonDescriptorSet = descriptorSets[5];
    skyDescriptorSet = descriptorSets[6];
    
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

        // Shadow map descriptor
        VkDescriptorImageInfo shadowMapInfo{
            .sampler = shadowMapSampler,
            .imageView = shadowMapView,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        };

        VkDescriptorImageInfo shadowMapDebugInfo{
            .sampler = shadowMapDebugSampler,
            .imageView = shadowMapView,
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
        };
        
        std::vector<VkWriteDescriptorSet> writes = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &sceneUboInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &materialUboInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &dirLightUboInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 3, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &pointLightsInfo},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 4, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &spotLightsInfo},
            // Привязка текстуры к биндингу 5
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 5, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &imageInfo},
            // Привязка shadow map к биндингу 6
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 6, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &shadowMapInfo},
            // Привязка raw shadow map для отладки к биндингу 7
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 7, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &shadowMapDebugInfo}

        };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    };

    // Заполняем каждый набор своей текстурой
    fillDescriptorSet(descriptorSet, buildingTexture); 
    fillDescriptorSet(buildingDescriptorSet, buildingTexture);
    fillDescriptorSet(roadDescriptorSet, roadTexture ? roadTexture.get() : nullptr);
    fillDescriptorSet(carDescriptorSet, carTexture);
    fillDescriptorSet(roofDescriptorSet, roofTexture);
    fillDescriptorSet(moonDescriptorSet, moonTexture);
    fillDescriptorSet(skyDescriptorSet, skyTexture);
    
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
    
    // Rasterizer для shadow pass с depth bias
    VkPipelineRasterizationStateCreateInfo shadowRasterizer{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f,
        // ВАЖНО: для диагностики/совместимости отключаем culling.
        // Если winding у мешей/матриц отличается от ожиданий, FRONT cull может “съедать” всю геометрию -> shadow map остаётся белой.
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_TRUE,  // Включаем depth bias для уменьшения shadow acne
        .depthBiasConstantFactor = 1.25f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 1.75f
    };
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

    // === СОЗДАНИЕ SKY PIPELINE ===
    // Отдельный пайплайн для неба: depth test включен, но depth write выключен,
    // чтобы небо не "перекрывало" объекты сцены.
    VkPipelineDepthStencilStateCreateInfo skyDepthStencil = depthStencil;
    skyDepthStencil.depthWriteEnable = VK_FALSE;
    skyDepthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineRasterizationStateCreateInfo skyRasterizer = rasterizer;
    skyRasterizer.cullMode = VK_CULL_MODE_NONE; // рисуем купол изнутри без риска "не той" ориентации

    VkGraphicsPipelineCreateInfo skyPipelineInfo = pipelineInfo;
    skyPipelineInfo.pDepthStencilState = &skyDepthStencil;
    skyPipelineInfo.pRasterizationState = &skyRasterizer;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &skyPipelineInfo, nullptr, &skyPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create sky pipeline!");
    }
    
    // === СОЗДАНИЕ SHADOW PIPELINE ===
    // Загружаем shadow vertex shader (простой шейдер, который только записывает глубину)
    shadowVertexShaderModule = loadShaderModule("shaders/shadow.vert.spv");
    
    VkPipelineShaderStageCreateInfo shadowShaderStages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = shadowVertexShaderModule, .pName = "main"}
    };
    
    // Pipeline layout для shadow pass (только push constants)
    VkPushConstantRange shadowPushConstantRange{
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = sizeof(ShadowPushConstants)  // Явно указываем размер структуры
    };
    
    VkPipelineLayoutCreateInfo shadowPipelineLayoutInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 0,
        .pSetLayouts = nullptr,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &shadowPushConstantRange
    };
    
    if (vkCreatePipelineLayout(device, &shadowPipelineLayoutInfo, nullptr, &shadowPipelineLayout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline layout!");
    }
    
    // Настройка pipeline для shadow pass (только глубина, без цвета)
    // Добавляем динамические состояния для viewport и scissor
    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicStateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
        .pDynamicStates = dynamicStates.data()
    };

    // viewport/scissor по умолчанию — под shadow map (даже если динамика где-то не сработает)
    VkViewport shadowViewport{0.0f, 0.0f, (float)SHADOW_MAP_SIZE, (float)SHADOW_MAP_SIZE, 0.0f, 1.0f};
    VkRect2D shadowScissor{{0, 0}, {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}};
    VkPipelineViewportStateCreateInfo shadowViewportState{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &shadowViewport,
        .scissorCount = 1,
        .pScissors = &shadowScissor
    };

    // Настройка pipeline для shadow pass (только глубина, без цвета)
    VkPipelineDepthStencilStateCreateInfo shadowDepthStencil{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depthBoundsTestEnable = VK_FALSE,
        .stencilTestEnable = VK_FALSE
    };

    VkPipelineColorBlendStateCreateInfo shadowColorBlend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .attachmentCount = 0,
        .pAttachments = nullptr
    };
    
    VkGraphicsPipelineCreateInfo shadowPipelineInfo{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 1,
        .pStages = shadowShaderStages,
        .pVertexInputState = &vertexInputInfo,
        .pInputAssemblyState = &inputAssembly,
        .pViewportState = &shadowViewportState,
        .pRasterizationState = &shadowRasterizer,  // Используем shadow rasterizer с depth bias
        .pMultisampleState = &multisampling,
        .pDepthStencilState = &shadowDepthStencil,
        .pColorBlendState = &shadowColorBlend,  // 0 color attachments
        .pDynamicState = &dynamicStateInfo,
        .layout = shadowPipelineLayout,
        .renderPass = shadowRenderPass,
        .subpass = 0
    };
    
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &shadowPipelineInfo, nullptr, &shadowPipeline) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline!");
    }
}

void shutdown() {
    // Очистка ресурсов Vulkan при выходе
    VkDevice& device = veekay::app.vk_device;
    vkDeviceWaitIdle(device);
    
    // Shadow map cleanup
    vkDestroyFramebuffer(device, shadowFramebuffer, nullptr);
    vkDestroyRenderPass(device, shadowRenderPass, nullptr);
    vkDestroySampler(device, shadowMapSampler, nullptr);
    vkDestroySampler(device, shadowMapDebugSampler, nullptr); // Добавляем очистку дебаг-сэмплера
    vkDestroyImageView(device, shadowMapView, nullptr);
    vkDestroyImage(device, shadowMapImage, nullptr);
    vkFreeMemory(device, shadowMapMemory, nullptr);
    
    vkDestroyPipeline(device, shadowPipeline, nullptr);
    vkDestroyPipelineLayout(device, shadowPipelineLayout, nullptr);
    vkDestroyShaderModule(device, shadowVertexShaderModule, nullptr);
    
    vkDestroyPipeline(device, skyPipeline, nullptr);
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
    
    // moonTexture может быть fallback-ом на buildingTexture, чтобы не сделать double-free.
    if (moonTexture && moonTexture != buildingTexture) {
        delete moonTexture;
    }

    if (skyTexture && skyTexture != buildingTexture) {
        delete skyTexture;
    }
    
    vkDestroyShaderModule(device, fragmentShaderModule, nullptr);
    vkDestroyShaderModule(device, vertexShaderModule, nullptr);
}

void update(double time) {
    float currentFrame = time;
    deltaTime = currentFrame - lastFrame;
    lastFrame = currentFrame;
    
    processInput(veekay::app.window, deltaTime);
    
    // Обновление направления луны по кругу (как настоящая луна)
    // moonAngle: 0 = восток (рассвет), PI/2 = север (полдень), PI = запад (закат), 3*PI/2 = юг (полночь)
    // Высота луны меняется от -1 (под горизонтом) до 1 (над головой)
    float moonHeight = glm::sin(moonAngle); // Высота над горизонтом
    float moonHorizontal = glm::cos(moonAngle); // Горизонтальное направление
    
    // Направление луны: движется по кругу
    // X - восток/запад, Y - высота, Z - север/юг
    // Увеличиваем влияние для более заметного движения
    moonLight.direction = glm::normalize(glm::vec3(
        moonHorizontal * 1.0f,  // Увеличена горизонтальная компонента (было 0.7f)
        moonHeight,             // Высота (синус угла)
        moonHorizontal * 0.5f   // Увеличена компонента для глубины (было 0.3f)
    ));
    
    // Выводим информацию о направлении луны для отладки
    static float lastAngleLog = -1.0f;
    if (glm::abs(moonAngle - lastAngleLog) > 0.1f) {
        float angleDegrees = moonAngle * 180.0f / glm::pi<float>();
        std::cout << "Moon angle: " << angleDegrees << " degrees, height: " << moonHeight 
                  << ", direction: (" << moonLight.direction.x << ", " 
                  << moonLight.direction.y << ", " << moonLight.direction.z << ")" << std::endl;
        lastAngleLog = moonAngle;
    }
    
    // Цвет луны меняется в зависимости от высоты (закат/рассвет более теплые)
    glm::vec3 moonColor;
    if (moonHeight < 0.0f) {
        // Луна под горизонтом - темнее
        moonColor = glm::vec3(0.3f, 0.4f, 0.6f) * (1.0f + moonHeight * 0.5f);
    } else if (moonHeight < 0.3f) {
        // Закат/рассвет - теплые оттенки
        float t = moonHeight / 0.3f;
        moonColor = glm::mix(glm::vec3(1.0f, 0.7f, 0.5f), glm::vec3(0.7f, 0.8f, 1.0f), t);
    } else {
        // Ночное небо - холодные оттенки
        moonColor = glm::vec3(0.7f, 0.8f, 1.0f);
    }
    
    moonLight.color = glm::vec4(moonColor, moonLightIntensity);
    
    // Вычисление матрицы пространства света для shadow mapping
    glm::mat4 lightSpaceMatrix = calculateLightSpaceMatrix();
    
    // Обновление UBO (камера + light space matrix)
    SceneUBO sceneUbo;
    // Увеличиваем far plane, чтобы луна/небо на больших дистанциях не клипались.
    sceneUbo.projection = glm::perspective(glm::radians(60.0f), (float)veekay::app.window_width / (float)veekay::app.window_height, 0.1f, 1600.0f);
    sceneUbo.projection[1][1] *= -1;
    sceneUbo.view = camera.getViewMatrix();
    sceneUbo.viewPos = camera.getPosition();
    sceneUbo.lightSpaceMatrix = lightSpaceMatrix;
    copyDataToBuffer(sceneUboBuffer, &sceneUbo, sizeof(sceneUbo));
    
    copyDataToBuffer(dirLightUboBuffer, &moonLight, sizeof(moonLight));
    
    copyDataToBuffer(pointLightsSsboBuffer, pointLights.data(), sizeof(vkscene::PointLight) * pointLights.size());
    copyDataToBuffer(spotLightsSsboBuffer, spotLights.data(), sizeof(vkscene::SpotLight) * spotLights.size());
}

// Функция для рендеринга shadow pass (использует RenderPass вместо Dynamic Rendering)
void renderShadowPass(VkCommandBuffer cmd) {
    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};

    VkRenderPassBeginInfo rp{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = shadowRenderPass,
        .framebuffer = shadowFramebuffer,
        .renderArea = {{0, 0}, {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}},
        .clearValueCount = 1,
        .pClearValues = &clearValue
    };

    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{0.0f, 0.0f, (float)SHADOW_MAP_SIZE, (float)SHADOW_MAP_SIZE, 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {SHADOW_MAP_SIZE, SHADOW_MAP_SIZE}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline);

    glm::mat4 lightSpace = calculateLightSpaceMatrix();

    VkDeviceSize offset = 0;

    // Дорога
    vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    {
        glm::mat4 model =
            glm::translate(glm::mat4(1.0f), glm::vec3(0.0f)) *
            glm::scale(glm::mat4(1.0f), glm::vec3(60.0f, 0.05f, 60.0f));

        ShadowPushConstants spc{ model, lightSpace };
        vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(ShadowPushConstants), &spc);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
    }

    // Здания + крыши
    for (const Building& b : buildings) {
        // здание
        {
            glm::mat4 model =
                glm::translate(glm::mat4(1.0f), b.position) *
                glm::scale(glm::mat4(1.0f), b.scale);

            ShadowPushConstants spc{ model, lightSpace };
            vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(ShadowPushConstants), &spc);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        }

        // крыша
        {
            float roofY = b.position.y + b.scale.y * 0.5f + 0.002f;
            glm::mat4 model =
                glm::translate(glm::mat4(1.0f), glm::vec3(b.position.x, roofY, b.position.z)) *
                glm::scale(glm::mat4(1.0f), glm::vec3(b.scale.x, 0.01f, b.scale.z));

            ShadowPushConstants spc{ model, lightSpace };
            vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(ShadowPushConstants), &spc);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        }
    }

    // Цилиндры (фонарные столбы)
    vkCmdBindVertexBuffers(cmd, 0, 1, &cylinderVertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, cylinderIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const auto& lamp : streetLamps) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), lamp.position + glm::vec3(0.0f, 1.5f, 0.0f));
        ShadowPushConstants spc{ model, lightSpace };
        vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &spc);
        vkCmdDrawIndexed(cmd, cylinderIndexCount, 1, 0, 0, 0);
    }

    // Лампы (кубы)
    vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (const auto& lamp : streetLamps) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), lamp.position + glm::vec3(0.0f, 3.0f, 0.0f));
        model = glm::scale(model, glm::vec3(0.2f, 0.2f, 0.2f));
        ShadowPushConstants spc{ model, lightSpace };
        vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &spc);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
    }

    // Машина
    glm::vec3 carPos = glm::vec3(0.0f, 0.0f, 0.0f);
    float carRotation = 0.0f;

    // Корпус
    vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.37f, 0.0f));
        model = glm::rotate(model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(0.8f, 0.4f, 1.6f));
        ShadowPushConstants spc{ model, lightSpace };
        vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &spc);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
    }

    // Кабина
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.65f, -0.15f));
        model = glm::rotate(model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(0.7f, 0.35f, 0.9f));
        ShadowPushConstants spc{ model, lightSpace };
        vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &spc);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
    }

    // Колеса
    vkCmdBindVertexBuffers(cmd, 0, 1, &wheelCylinderVertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, wheelCylinderIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    std::vector<glm::vec3> wheelOffsets = {
        {-0.45f, 0.25f, 0.6f}, {0.45f, 0.25f, 0.6f},
        {-0.45f, 0.25f, -0.6f}, {0.45f, 0.25f, -0.6f}
    };
    for (const auto& wheelOffset : wheelOffsets) {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), carPos + wheelOffset);
        model = glm::rotate(model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::rotate(model, glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
        model = glm::scale(model, glm::vec3(0.2f, 0.15f, 0.2f));
        ShadowPushConstants spc{ model, lightSpace };
        vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &spc);
        vkCmdDrawIndexed(cmd, wheelCylinderIndexCount, 1, 0, 0, 0);
    }

    // Фары
    vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
    {
        glm::mat4 model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.4f, 0.81f));
        model = glm::rotate(model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        model = glm::scale(model, glm::vec3(0.7f, 0.05f, 0.04f));
        ShadowPushConstants spc{ model, lightSpace };
        vkCmdPushConstants(cmd, shadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(ShadowPushConstants), &spc);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);
    
    VkCommandBufferBeginInfo beginInfo{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cmd, &beginInfo);
    
    // === SHADOW PASS ===
    renderShadowPass(cmd);
    
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
    
        PushConstants push;
        VkDeviceSize offset = 0;

        // === SKY (небо) ===
        // Рисуем первым: depth write выключен, поэтому объекты потом нормально рисуются поверх.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &skyDescriptorSet, 0, nullptr);

        vkCmdBindVertexBuffers(cmd, 0, 1, &sphereVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, sphereIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        {
            glm::vec3 camPos = camera.getPosition();
            glm::mat4 model = glm::translate(glm::mat4(1.0f), camPos);
            model = glm::scale(model, glm::vec3(1400.0f)); // < far plane (1600)

            push.model = model;
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
            push.metallic = -1.0f;   // unlit branch в simple.frag
            push.roughness = 1.0f;
            // Тайлинг панорамы по X -> "мельче" звёзды. Чем меньше тайлинг, тем меньше швов.
            push.uvScale = glm::vec2(2.0f, 1.0f);
            push.useTexture = 3.0f;  // флаг "sky equirect mapping"
            push.debugShadowMap = 0.0f; // не отлаживаем shadow map на небе

            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, sphereIndexCount, 1, 0, 0, 0);
        }

        // === Основной пайплайн ===
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        
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
        push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
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
            push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
            
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
            push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
            
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
            push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
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
            push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
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
        
        // Практически белая, блестящая "лакированная" машина.
        // Текстура car.jpg часто тёмная -> отключаем её, чтобы цвет был реально белым.
        // Делаем цвет "сверхбелым" (HDR), чтобы в ночной сцене не выглядел серым.
        push.color = glm::vec4(1.5f, 1.5f, 1.5f, 1.0f);
        // Глянцевая белая краска — почти диэлектрик, но очень гладкая.
        // База: белая краска (нужен diffuse, иначе машина выглядит чёрной кроме блика).
        push.metallic = 0.5f;
        push.roughness = 0.10f;
        
        // ТЕКСТУРИРОВАНИЕ МАШИНЫ:
        // Масштаб 1.0, чтобы текстура легла один-в-один на развертку модели
        push.uvScale = glm::vec2(1.0f, 1.0f); 
        push.useTexture = 2.0f; // текстура + clearcoat (лак)
        push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
        
        vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                        0, sizeof(PushConstants), &push);
        vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);

        // --- Кабина (Cabin) ---
        push.model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.65f, -0.15f));
        push.model = glm::rotate(push.model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
        push.model = glm::scale(push.model, glm::vec3(0.7f, 0.35f, 0.9f));
        push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
        push.color = glm::vec4(1.5f, 1.5f, 1.5f, 1.0f);
        push.metallic = 0.5f;
        push.roughness = 0.10f;
        push.uvScale = glm::vec2(1.0f, 1.0f); 
        push.useTexture = 2.0f; // текстура + clearcoat (лак)
        push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
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
            push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, wheelCylinderIndexCount, 1, 0, 0, 0);
        }

        // --- Фары (Headlights Mesh) ---
        vkCmdBindVertexBuffers(cmd, 0, 1, &cubeVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, cubeIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
        {
            glm::mat4 model = glm::translate(glm::mat4(1.0f), carPos + glm::vec3(0.0f, 0.4f, 0.81f));
            model = glm::rotate(model, carRotation, glm::vec3(0.0f, 1.0f, 0.0f));
            model = glm::scale(model, glm::vec3(0.7f, 0.05f, 0.04f));
            push.model = model;
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            push.color = glm::vec4(70.0f, 70.0f, 68.0f, 1.0f);
            push.metallic = 0.95f;
            push.roughness = 0.05f;
            push.uvScale = glm::vec2(1.0f, 1.0f); 
            push.useTexture = 0.0f; 
            push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f; // Передаем флаг отладки
            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                            0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, cubeIndexCount, 1, 0, 0, 0);
        }

        // === ЛУНА (сфера с текстурой moon.png) ===
        // Рисуем как отдельный объект: она должна быть видимой в небе.
        vkCmdBindVertexBuffers(cmd, 0, 1, &sphereVertexBuffer.buffer, &offset);
        vkCmdBindIndexBuffer(cmd, sphereIndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
                                0, 1, &moonDescriptorSet, 0, nullptr);

        {
            // Позиция луны вдоль направления света (на "стороне" источника).
            // Чуть дальше, чтобы визуально она была "за" направленным светом.
            glm::vec3 moonPos = -glm::normalize(moonLight.direction) * 140.0f;
            glm::mat4 model = glm::translate(glm::mat4(1.0f), moonPos);
            model = glm::scale(model, glm::vec3(12.0f)); // Размер луны (ещё чуть больше)

            push.model = model;
            push.normalMatrix = glm::mat4(glm::transpose(glm::inverse(glm::mat3(push.model))));
            // Делаем луну эмиссивной (не освещается directional light'ом).
            // Снижаем эмиссию, чтобы луна не пересвечивалась
            push.color = glm::vec4(2.0f, 2.0f, 2.0f, 1.0f);
            push.metallic = -1.0f; // флаг "unlit" в simple.frag
            push.roughness = 1.0f;
            push.uvScale = glm::vec2(1.0f, 1.0f);
            push.useTexture = 1.0f;
            push.debugShadowMap = debugShadowMapEnabled ? 1.0f : 0.0f;

            vkCmdPushConstants(cmd, pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                               0, sizeof(PushConstants), &push);
            vkCmdDrawIndexed(cmd, sphereIndexCount, 1, 0, 0, 0);
        }
    
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
