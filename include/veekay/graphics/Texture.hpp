#ifndef VEEKAY_GRAPHICS_TEXTURE_HPP
#define VEEKAY_GRAPHICS_TEXTURE_HPP

#include <string>
#include <vulkan/vulkan.h>
#include <veekay/veekay.hpp> // Добавлен для доступа к veekay::app
#include <veekay/stb_image/stb_image.h> // Добавлен для stb_image

namespace veekay {
namespace graphics {

class Texture {
public:
    Texture(const std::string& filepath);
    ~Texture();

    VkImage getImage() const { return image; }
    VkImageView getImageView() const { return imageView; }
    VkSampler getSampler() const { return sampler; }

private:
    VkImage image;
    VkDeviceMemory imageMemory;
    VkImageView imageView;
    VkSampler sampler;

    uint32_t width, height; // Добавлено для хранения размеров текстуры

    void loadImage(const std::string& filepath);
    void createImageView();
    void createSampler();
    void cleanup();

    // Вспомогательные функции для создания изображений и буферов
    void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling,
                     VkImageUsageFlags usage, VkMemoryPropertyFlags properties,
                     VkImage& image, VkDeviceMemory& imageMemory);
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory);
    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout);
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height);
    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);
};

} // namespace graphics
} // namespace veekay

#endif // VEEKAY_GRAPHICS_TEXTURE_HPP
