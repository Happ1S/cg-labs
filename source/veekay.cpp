#include <cstdint>
#include <climits>
#include <iostream>
#include <cstring>

#include <vector>

#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan.h>  // Для типов функций swapchain

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <VkBootstrap.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <veekay/veekay.hpp>

namespace {

constexpr uint32_t window_default_width = 1280;
constexpr uint32_t window_default_height = 720;
constexpr char window_title[] = "Veekay";

constexpr uint32_t max_frames_in_flight = 2;

GLFWwindow* window;

VkInstance vk_instance;
VkDebugUtilsMessengerEXT vk_debug_messenger;
VkPhysicalDevice vk_physical_device;
VkDevice vk_device;
VkSurfaceKHR vk_surface;

VkSwapchainKHR vk_swapchain;
VkFormat vk_swapchain_format;
std::vector<VkImage> vk_swapchain_images;
std::vector<VkImageView> vk_swapchain_image_views;

VkQueue vk_graphics_queue;
uint32_t vk_graphics_queue_family;

// NOTE: ImGui rendering objects
VkDescriptorPool imgui_descriptor_pool;
VkRenderPass imgui_render_pass;
VkCommandPool imgui_command_pool;
std::vector<VkCommandBuffer> imgui_command_buffers;
std::vector<VkFramebuffer> imgui_framebuffers;

VkFormat vk_image_depth_format;
VkImage vk_image_depth;
VkDeviceMemory vk_image_depth_memory;
VkImageView vk_image_depth_view;

VkRenderPass vk_render_pass;
std::vector<VkFramebuffer> vk_framebuffers;

std::vector<VkSemaphore> vk_render_semaphores;
std::vector<VkSemaphore> vk_present_semaphores;
std::vector<VkFence> vk_in_flight_fences;
uint32_t vk_current_frame;

VkCommandPool vk_command_pool;
std::vector<VkCommandBuffer> vk_command_buffers;


} // namespace

// NOTE: Global application state definition
veekay::Application veekay::app;

int veekay::run(const veekay::ApplicationInfo& app_info) {
	veekay::app.running = true;
	
	if (!glfwInit()) {
		std::cerr << "Failed to initialize GLFW\n";
		return 1;
	}

	glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
	glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
#if defined(__APPLE__)
	glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_TRUE);
	glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
#endif

	window = glfwCreateWindow(window_default_width, window_default_height,
	                          window_title, nullptr, nullptr);
	if (!window) {
		std::cerr << "Failed to create GLFW window\n";
		return 1;
	}

	int framebuffer_width = 0, framebuffer_height = 0;
	glfwGetFramebufferSize(window, &framebuffer_width, &framebuffer_height);
	veekay::app.window_width = static_cast<uint32_t>(framebuffer_width);
	veekay::app.window_height = static_cast<uint32_t>(framebuffer_height);

	veekay::app.window = window;
	std::cout << "DEBUG: veekay::app.window = " << veekay::app.window << std::endl;
	{ // NOTE: Initialize Vulkan: grab device and create swapchain
		vkb::InstanceBuilder instance_builder;

		auto builder_result = instance_builder.require_api_version(1, 2, 0)
		                                      .request_validation_layers()
		                                      .use_default_debug_messenger()
		                                      .build();
		if (!builder_result) {
			std::cerr << builder_result.error().message() << '\n';
			return 1;
		}

		auto instance = builder_result.value();

		vk_instance = instance.instance;
		vk_debug_messenger = instance.debug_messenger;

		if (glfwCreateWindowSurface(vk_instance, window, nullptr, &vk_surface) != VK_SUCCESS) {
			const char* message;
			glfwGetError(&message);
			std::cerr << message << '\n';
			return 1;
		}

		vkb::PhysicalDeviceSelector physical_device_selector(instance);

		auto selector_result = physical_device_selector.set_surface(vk_surface)
		                                               .select();
		if (!selector_result) {
			std::cerr << selector_result.error().message() << '\n';
			return 1;
		}

		auto physical_device = selector_result.value();

		{
			// Всегда создаем устройство вручную с нужными расширениями
			// Это гарантирует, что все расширения будут включены правильно
			const char* dynamic_rendering_ext = "VK_KHR_dynamic_rendering";
			const char* portability_subset_ext = "VK_KHR_portability_subset";
			const char* swapchain_ext = "VK_KHR_swapchain";
			
			// Проверяем поддержку расширений
			uint32_t extension_count = 0;
			vkEnumerateDeviceExtensionProperties(physical_device.physical_device, nullptr, &extension_count, nullptr);
			std::vector<VkExtensionProperties> available_extensions(extension_count);
			vkEnumerateDeviceExtensionProperties(physical_device.physical_device, nullptr, &extension_count, available_extensions.data());
			
			bool has_dynamic_rendering = false;
			bool has_portability_subset = false;
			bool has_swapchain = false;
			for (const auto& ext : available_extensions) {
				if (strcmp(ext.extensionName, dynamic_rendering_ext) == 0) {
					has_dynamic_rendering = true;
				}
				if (strcmp(ext.extensionName, portability_subset_ext) == 0) {
					has_portability_subset = true;
				}
				if (strcmp(ext.extensionName, swapchain_ext) == 0) {
					has_swapchain = true;
				}
			}
			
			if (!has_swapchain) {
				std::cerr << "Fatal: VK_KHR_swapchain extension not supported!\n";
				return 1;
			}
			
			// Получаем информацию о queue families
			uint32_t queue_family_count = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device.physical_device, &queue_family_count, nullptr);
			std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
			vkGetPhysicalDeviceQueueFamilyProperties(physical_device.physical_device, &queue_family_count, queue_families.data());
			
			// Находим graphics queue family, которая поддерживает present
			uint32_t graphics_queue_family = UINT32_MAX;
			for (uint32_t i = 0; i < queue_family_count; i++) {
				VkBool32 present_support = false;
				vkGetPhysicalDeviceSurfaceSupportKHR(physical_device.physical_device, i, vk_surface, &present_support);
				if ((queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present_support && queue_families[i].queueCount > 0) {
					graphics_queue_family = i;
					break;
				}
			}
			
			if (graphics_queue_family == UINT32_MAX) {
				std::cerr << "Failed to find graphics queue family with present support!\n";
				return 1;
			}
			
			// Создаем устройство с расширениями
			float queue_priority = 1.0f;
			VkDeviceQueueCreateInfo queue_create_info{
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = graphics_queue_family,
				.queueCount = 1,
				.pQueuePriorities = &queue_priority
			};
			
			std::vector<const char*> extensions_to_enable;
			extensions_to_enable.push_back(swapchain_ext); // Обязательно
			if (has_portability_subset) {
				extensions_to_enable.push_back(portability_subset_ext);
			}
			if (has_dynamic_rendering) {
				extensions_to_enable.push_back(dynamic_rendering_ext);
			}
			
			VkDeviceCreateInfo device_create_info{
				.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
				.queueCreateInfoCount = 1,
				.pQueueCreateInfos = &queue_create_info,
				.enabledExtensionCount = static_cast<uint32_t>(extensions_to_enable.size()),
				.ppEnabledExtensionNames = extensions_to_enable.data()
			};
			
			if (vkCreateDevice(physical_device.physical_device, &device_create_info, nullptr, &vk_device) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan device!\n";
				return 1;
			}
			
			if (!has_dynamic_rendering) {
				std::cerr << "WARNING: VK_KHR_dynamic_rendering extension not supported by device!" << std::endl;
				std::cerr << "  Shadow mapping will not work. This is common on macOS with MoltenVK." << std::endl;
			} else {
				std::cerr << "INFO: VK_KHR_dynamic_rendering extension is available and will be enabled." << std::endl;
			}
			
			vk_physical_device = physical_device.physical_device;
			vkGetDeviceQueue(vk_device, graphics_queue_family, 0, &vk_graphics_queue);
			vk_graphics_queue_family = graphics_queue_family;
		}

		// Создаем swapchain вручную, так как устройство создано вручную
		// Загружаем функции swapchain
		PFN_vkCreateSwapchainKHR vkCreateSwapchainKHR = 
			reinterpret_cast<PFN_vkCreateSwapchainKHR>(vkGetDeviceProcAddr(vk_device, "vkCreateSwapchainKHR"));
		PFN_vkGetSwapchainImagesKHR vkGetSwapchainImagesKHR = 
			reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(vkGetDeviceProcAddr(vk_device, "vkGetSwapchainImagesKHR"));
		
		if (!vkCreateSwapchainKHR || !vkGetSwapchainImagesKHR) {
			std::cerr << "Failed to load swapchain functions!\n";
			return 1;
		}
		
		// Получаем возможности поверхности
		VkSurfaceCapabilitiesKHR capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk_physical_device, vk_surface, &capabilities);
		
		// Получаем доступные форматы
		uint32_t format_count;
		vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, nullptr);
		std::vector<VkSurfaceFormatKHR> formats(format_count);
		vkGetPhysicalDeviceSurfaceFormatsKHR(vk_physical_device, vk_surface, &format_count, formats.data());
		
		// Выбираем формат
		VkSurfaceFormatKHR surface_format = formats[0];
		for (const auto& format : formats) {
			if (format.format == VK_FORMAT_B8G8R8A8_UNORM && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
				surface_format = format;
				break;
			}
		}
		vk_swapchain_format = surface_format.format;
		
		// Получаем доступные present modes
		uint32_t present_mode_count;
		vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device, vk_surface, &present_mode_count, nullptr);
		std::vector<VkPresentModeKHR> present_modes(present_mode_count);
		vkGetPhysicalDeviceSurfacePresentModesKHR(vk_physical_device, vk_surface, &present_mode_count, present_modes.data());
		
		VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
		for (const auto& mode : present_modes) {
			if (mode == VK_PRESENT_MODE_FIFO_KHR) {
				present_mode = mode;
				break;
			}
		}
		
		// Определяем количество изображений в swapchain
		uint32_t image_count = capabilities.minImageCount + 1;
		if (capabilities.maxImageCount > 0 && image_count > capabilities.maxImageCount) {
			image_count = capabilities.maxImageCount;
		}
		
		// Создаем swapchain
		VkSwapchainCreateInfoKHR swapchain_create_info{
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = vk_surface,
			.minImageCount = image_count,
			.imageFormat = surface_format.format,
			.imageColorSpace = surface_format.colorSpace,
			.imageExtent = {veekay::app.window_width, veekay::app.window_height},
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.preTransform = capabilities.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = present_mode,
			.clipped = VK_TRUE,
			.oldSwapchain = VK_NULL_HANDLE
		};
		
		if (vkCreateSwapchainKHR(vk_device, &swapchain_create_info, nullptr, &vk_swapchain) != VK_SUCCESS) {
			std::cerr << "Failed to create swapchain!\n";
			return 1;
		}
		
		// Получаем изображения swapchain
		vkGetSwapchainImagesKHR(vk_device, vk_swapchain, &image_count, nullptr);
		vk_swapchain_images.resize(image_count);
		vkGetSwapchainImagesKHR(vk_device, vk_swapchain, &image_count, vk_swapchain_images.data());
		
		// Создаем image views
		vk_swapchain_image_views.resize(vk_swapchain_images.size());
		for (size_t i = 0; i < vk_swapchain_images.size(); i++) {
			VkImageViewCreateInfo view_info{
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = vk_swapchain_images[i],
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = vk_swapchain_format,
				.components = {
					.r = VK_COMPONENT_SWIZZLE_IDENTITY,
					.g = VK_COMPONENT_SWIZZLE_IDENTITY,
					.b = VK_COMPONENT_SWIZZLE_IDENTITY,
					.a = VK_COMPONENT_SWIZZLE_IDENTITY
				},
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1
				}
			};
			
			if (vkCreateImageView(vk_device, &view_info, nullptr, &vk_swapchain_image_views[i]) != VK_SUCCESS) {
				std::cerr << "Failed to create image view " << i << "!\n";
				return 1;
			}
		}

		veekay::app.vk_device = vk_device;
		veekay::app.vk_physical_device = vk_physical_device;
		veekay::app.vk_graphics_queue = vk_graphics_queue;
	}

	{ // NOTE: ImGui initialization
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;

		ImGui::StyleColorsDark();

		float xscale = 1.0f, yscale = 1.0f;
		glfwGetWindowContentScale(window, &xscale, &yscale);
		ImGui::GetStyle().ScaleAllSizes(xscale);
		io.FontGlobalScale = xscale;

		ImGui_ImplGlfw_InitForVulkan(window, true);

		{
			VkDescriptorPoolSize size = {
				.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.descriptorCount = 1000,
			};

			VkDescriptorPoolCreateInfo info = {
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
				.maxSets = size.descriptorCount,
				.poolSizeCount = 1,
				.pPoolSizes = &size,
			};

			if (vkCreateDescriptorPool(vk_device, &info, 0, &imgui_descriptor_pool) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan descriptor pool for ImGui\n";
				return 1;
			}
		}

		{
			VkAttachmentDescription attachment{
				.format = vk_swapchain_format,
				.samples = VK_SAMPLE_COUNT_1_BIT,
				.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
				.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
				.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
				.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
				.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
			};

			VkAttachmentReference ref{
				.attachment = 0,
				.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
			};

			VkSubpassDescription subpass{
				.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
				.colorAttachmentCount = 1,
				.pColorAttachments = &ref,
			};

			VkSubpassDependency dependency{
				.srcSubpass = VK_SUBPASS_EXTERNAL,
				.dstSubpass = 0,
				.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
				.srcAccessMask = 0,
				.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
			};

			VkRenderPassCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
				.attachmentCount = 1,
				.pAttachments = &attachment,
				.subpassCount = 1,
				.pSubpasses = &subpass,
				.dependencyCount = 1,
				.pDependencies = &dependency,
			};

			if (vkCreateRenderPass(vk_device, &info, nullptr, &imgui_render_pass) != VK_SUCCESS) {
				std::cerr << "Failed to create ImGui Vulkan render pass\n";
				return 1;
			}
		}

		{
			VkFramebufferCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
				.renderPass = imgui_render_pass,
				.attachmentCount = 1,
				.width = veekay::app.window_width,
				.height = veekay::app.window_height,
				.layers = 1,
			};

			const size_t count = vk_swapchain_images.size();

			imgui_framebuffers.resize(count);

			for (size_t i = 0; i < count; ++i) {
				info.pAttachments = &vk_swapchain_image_views[i];
				if (vkCreateFramebuffer(vk_device, &info, nullptr, &imgui_framebuffers[i]) != VK_SUCCESS) {
					std::cerr << "Failed to create Vulkan framebuffer " << i << '\n';
					return 1;
				}
			}
		}

		{
			size_t count = imgui_framebuffers.size();

			imgui_command_buffers.resize(count);

			{
				VkCommandPoolCreateInfo info{
					.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
					.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
					.queueFamilyIndex = vk_graphics_queue_family,
				};

				if (vkCreateCommandPool(vk_device, &info, nullptr, &imgui_command_pool) != VK_SUCCESS) {
					std::cerr << "Failed to create ImGui Vulkan command pool\n";
					return 1;
				}
			}

			{
				VkCommandBufferAllocateInfo info{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
					.commandPool = imgui_command_pool,
					.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
					.commandBufferCount = static_cast<uint32_t>(imgui_command_buffers.size()),
				};

				if (vkAllocateCommandBuffers(vk_device, &info, imgui_command_buffers.data()) != VK_SUCCESS) {
					std::cerr << "Failed to allocate ImGui Vulkan command buffers\n";
					return 1;
				}
			}
		}

		ImGui_ImplVulkan_InitInfo info{
			.Instance = vk_instance,
			.PhysicalDevice = vk_physical_device,
			.Device = vk_device,
			.QueueFamily = vk_graphics_queue_family,
			.Queue = vk_graphics_queue,
			.DescriptorPool = imgui_descriptor_pool,
			.MinImageCount = static_cast<uint32_t>(vk_swapchain_images.size()),
			.ImageCount = static_cast<uint32_t>(vk_swapchain_images.size()),
			.RenderPass = imgui_render_pass,
		};

		ImGui_ImplVulkan_Init(&info);
	}

	{
		VkFormat candidates[] = {
			VK_FORMAT_D32_SFLOAT,
			VK_FORMAT_D32_SFLOAT_S8_UINT,
			VK_FORMAT_D24_UNORM_S8_UINT,
		};

		vk_image_depth_format = VK_FORMAT_UNDEFINED;

		for (const auto& f : candidates) {
			VkFormatProperties properties;
			vkGetPhysicalDeviceFormatProperties(vk_physical_device, f, &properties);

			if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
				vk_image_depth_format = f;
				break;
			}
		}
	}

	{ // NOTE: Create depth buffer
		VkImageCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = vk_image_depth_format,
			.extent = {veekay::app.window_width, veekay::app.window_height, 1},
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
		};

		if (vkCreateImage(vk_device, &info, nullptr, &vk_image_depth) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan depth image\n";
			return 1;
		}
	}

	{ // NOTE: Allocate depth buffer memory
		VkMemoryRequirements requirements;
		vkGetImageMemoryRequirements(vk_device, vk_image_depth, &requirements);

		VkPhysicalDeviceMemoryProperties properties;
		vkGetPhysicalDeviceMemoryProperties(vk_physical_device, &properties);

		uint32_t index = UINT_MAX;
		for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
			const VkMemoryType& type = properties.memoryTypes[i];

			if ((requirements.memoryTypeBits & (1 << i)) &&
			    (type.propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
				index = i;
				break;
			}
		}

		if (index == UINT_MAX) {
			std::cerr << "Failed to find required memory type for Vulkan depth image\n";
			return 1;
		}

		VkMemoryAllocateInfo info = {
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = requirements.size,
			.memoryTypeIndex = index,
		};

		if (vkAllocateMemory(vk_device, &info, nullptr, &vk_image_depth_memory) != VK_SUCCESS) {
			std::cerr << "Failed to allocate memory for Vulkan depth image\n";
			return 1;
		}

		if (vkBindImageMemory(vk_device, vk_image_depth, vk_image_depth_memory, 0) != VK_SUCCESS) {
			std::cerr << "Failed to bind Vulkan depth image with device memory\n";
			return 1;
		}
	}

	{ // NOTE: Create depth buffer view object
		VkImageViewCreateInfo info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = vk_image_depth,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = vk_image_depth_format,
			.subresourceRange = {
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		};

		if (vkCreateImageView(vk_device, &info, nullptr, &vk_image_depth_view) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan depth image view\n";
			return 1;
		}
	}

	{ // NOTE: Create render pass
		VkAttachmentDescription color_attachment{
			.format = vk_swapchain_format,

			.samples = VK_SAMPLE_COUNT_1_BIT,

			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,

			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,

			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
		};

		VkAttachmentDescription depth_attachment{
			.format = vk_image_depth_format,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
			.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};

		VkAttachmentReference color_ref{
			.attachment = 0,
			.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		};

		VkAttachmentReference depth_ref{
			.attachment = 1,
			.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
		};

		VkSubpassDescription subpass{
			.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
			.colorAttachmentCount = 1,
			.pColorAttachments = &color_ref,
			.pDepthStencilAttachment = &depth_ref,
		};

		VkAttachmentDescription attachments[] = {color_attachment, depth_attachment};

		VkSubpassDependency dependency{
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			                VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
			                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
			.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
			.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
		};

		VkRenderPassCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,

			.attachmentCount = 2,
			.pAttachments = attachments,

			.subpassCount = 1,
			.pSubpasses = &subpass,

			.dependencyCount = 1,
			.pDependencies = &dependency,
		};

		if (vkCreateRenderPass(vk_device, &info, nullptr, &vk_render_pass) != VK_SUCCESS) {
			std::cerr << "Failed to create render pass\n";
			return 1;
		}

		veekay::app.vk_render_pass = vk_render_pass;
	}

	{ // NOTE: Create framebuffer objects from swapchain images
		VkImageView attachments[] = {VK_NULL_HANDLE, vk_image_depth_view};

		VkFramebufferCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,

			.renderPass = vk_render_pass,

			.attachmentCount = 2,
			.pAttachments = attachments,

			.width = veekay::app.window_width,
			.height = veekay::app.window_height,
			.layers = 1,
		};

		const size_t count = vk_swapchain_images.size();

		vk_framebuffers.resize(count);

		for (size_t i = 0; i < count; ++i) {
			attachments[0] = vk_swapchain_image_views[i];
			if (vkCreateFramebuffer(vk_device, &info, nullptr, &vk_framebuffers[i]) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan framebuffer " << i << '\n';
				return 1;
			}
		}
	}

	{ // NOTE: Create sync primitives
		VkFenceCreateInfo fence_info{
			.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
			.flags = VK_FENCE_CREATE_SIGNALED_BIT,
		};

		VkSemaphoreCreateInfo sem_info{
			.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		};

		vk_present_semaphores.resize(vk_swapchain_images.size());

		for (size_t i = 0, e = vk_swapchain_images.size(); i != e; ++i) {
			vkCreateSemaphore(vk_device, &sem_info, nullptr, &vk_present_semaphores[i]);
		}

		vk_render_semaphores.resize(max_frames_in_flight);
		vk_in_flight_fences.resize(max_frames_in_flight);

		for (uint32_t i = 0; i < max_frames_in_flight; ++i) {
			vkCreateSemaphore(vk_device, &sem_info, nullptr, &vk_render_semaphores[i]);
			vkCreateFence(vk_device, &fence_info, nullptr, &vk_in_flight_fences[i]);
		}
	}

	{ // NOTE: Create command pool from graphics queue
		VkCommandPoolCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
			.queueFamilyIndex = vk_graphics_queue_family,
		};

		if (vkCreateCommandPool(vk_device, &info, nullptr, &vk_command_pool) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan command pool\n";
			return 1;
		}
		veekay::app.vk_command_pool = vk_command_pool;
	}

	{ // NOTE: Allocate command buffers
		vk_command_buffers.resize(vk_framebuffers.size());
		
		VkCommandBufferAllocateInfo info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = vk_command_pool,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
			.commandBufferCount = static_cast<uint32_t>(vk_command_buffers.size()),
		};

		if (vkAllocateCommandBuffers(vk_device, &info, vk_command_buffers.data()) != VK_SUCCESS) {
			std::cerr << "Failed to allocate Vulkan command buffers\n";
			return 1;
		}
	}

	app_info.init();

	while (veekay::app.running && !glfwWindowShouldClose(window)) {
		glfwPollEvents();
		double time = glfwGetTime();

		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplGlfw_NewFrame();
		ImGui::NewFrame();

		app_info.update(time);

		ImGui::Render();

		// NOTE: Wait until the previous frame finishes
		vkWaitForFences(vk_device, 1, &vk_in_flight_fences[vk_current_frame], true, UINT64_MAX);
		vkResetFences(vk_device, 1, &vk_in_flight_fences[vk_current_frame]);

		// NOTE: Get current swapchain framebuffer index
		uint32_t swapchain_image_index = 0;
		vkAcquireNextImageKHR(vk_device, vk_swapchain, UINT64_MAX,
		                      vk_render_semaphores[vk_current_frame],
		                      nullptr, &swapchain_image_index);

		VkCommandBuffer cmd = vk_command_buffers[swapchain_image_index];

		app_info.render(cmd, vk_framebuffers[swapchain_image_index]);

		VkCommandBuffer imgui_cmd = imgui_command_buffers[swapchain_image_index];
		{ // NOTE: Draw ImGui
			vkResetCommandBuffer(imgui_cmd, 0);

			{
				VkCommandBufferBeginInfo info{
					.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
					.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
				};

				vkBeginCommandBuffer(imgui_cmd, &info);
			}

			{
				VkRenderPassBeginInfo info{
					.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
					.renderPass = imgui_render_pass,
					.framebuffer = imgui_framebuffers[swapchain_image_index],
					.renderArea = {
						.extent = {veekay::app.window_width, veekay::app.window_height},
					},
				};

				vkCmdBeginRenderPass(imgui_cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
			}

			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), imgui_cmd);

			vkCmdEndRenderPass(imgui_cmd);
			vkEndCommandBuffer(imgui_cmd);
		}

		{ // NOTE: Submit commands to graphics queue
			VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

			VkCommandBuffer buffers[] = { cmd, imgui_cmd };

			VkSubmitInfo info{
				.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
				.waitSemaphoreCount = 1,
				.pWaitSemaphores = &vk_render_semaphores[vk_current_frame],
				.pWaitDstStageMask = &wait_stage,
				.commandBufferCount = 2,
				.pCommandBuffers = buffers,
				.signalSemaphoreCount = 1,
				.pSignalSemaphores = &vk_present_semaphores[swapchain_image_index],
			};

			vkQueueSubmit(vk_graphics_queue, 1, &info, vk_in_flight_fences[vk_current_frame]);
		}

		{ // NOTE: Present renderer frame
			VkPresentInfoKHR info{
				.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
				.waitSemaphoreCount = 1,
				.pWaitSemaphores = &vk_present_semaphores[swapchain_image_index],
				.swapchainCount = 1,
				.pSwapchains = &vk_swapchain,
				.pImageIndices = &swapchain_image_index,
			};

			vkQueuePresentKHR(vk_graphics_queue, &info);

			vk_current_frame = (vk_current_frame + 1) % max_frames_in_flight;
		}
	}

	vkDeviceWaitIdle(vk_device);

	app_info.shutdown();

	vkDestroyCommandPool(vk_device, vk_command_pool, nullptr);

	for (size_t i = 0, e = vk_swapchain_images.size(); i != e; ++i) {
		vkDestroySemaphore(vk_device, vk_present_semaphores[i], nullptr);
	}

	for (size_t i = 0; i < max_frames_in_flight; ++i) {
		vkDestroySemaphore(vk_device, vk_render_semaphores[i], nullptr);
		vkDestroyFence(vk_device, vk_in_flight_fences[i], nullptr);
	}
	
	vkDestroyRenderPass(vk_device, vk_render_pass, nullptr);

	vkDestroyImageView(vk_device, vk_image_depth_view, nullptr);
	vkFreeMemory(vk_device, vk_image_depth_memory, nullptr);
	vkDestroyImage(vk_device, vk_image_depth, nullptr);

	vkDestroyCommandPool(vk_device, imgui_command_pool, nullptr);
	vkDestroyRenderPass(vk_device, imgui_render_pass, nullptr);

	for (size_t i = 0, e = vk_framebuffers.size(); i != e; ++i) {
		vkDestroyFramebuffer(vk_device, vk_framebuffers[i], nullptr);
		vkDestroyFramebuffer(vk_device, imgui_framebuffers[i], nullptr);
		vkDestroyImageView(vk_device, vk_swapchain_image_views[i], nullptr);
	}

	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	vkDestroyDescriptorPool(vk_device, imgui_descriptor_pool, nullptr);
	
	vkDestroySwapchainKHR(vk_device, vk_swapchain, nullptr);
	vkDestroyDevice(vk_device, nullptr);
	vkDestroySurfaceKHR(vk_instance, vk_surface, nullptr);
	vkb::destroy_debug_utils_messenger(vk_instance, vk_debug_messenger);
	vkDestroyInstance(vk_instance, nullptr);

	glfwDestroyWindow(window);
	glfwTerminate();
	
	return 0;
}
