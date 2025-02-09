#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "Calculate.hpp"
#include "Data.hpp"
#include "Image.hpp"
#include "ModuleConfig.hpp"
#include "NativeWindowHints.hpp"
#include "Render.hpp"
#include "Version.hpp"

#ifdef NDEBUG
	#define LOCATION
#else
	#define STR_HELPER(x) #x
	#define STR(x) STR_HELPER(x)
	#define LOCATION __FILE__ ":" STR(__LINE__) ": "
#endif

// Miscellaneous variables

namespace {
	constexpr int MAX_FRAMES_IN_FLIGHT = 2;

	const std::vector<const char*> deviceExtensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};

#ifdef NDEBUG
	constexpr bool enableValidationLayers = false;
#else
	// needs updating, recent SDKs don't seem to provide VK_LAYER_LUNARG_standard_validation
	constexpr bool enableValidationLayers = false;
#endif

	VkResult createDebugUtilsMessengerEXT(VkInstance instance,
	                                      const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
	                                      const VkAllocationCallbacks* pAllocator,
	                                      VkDebugUtilsMessengerEXT* pDebugMessenger) {
		auto func = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
		    vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
		if (func == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	}

	void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
	                                   const VkAllocationCallbacks* pAllocator) {
		auto func = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
		    vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
		if (func == nullptr) return;
		func(instance, debugMessenger, pAllocator);
	}

	struct QueueFamilyIndices {
		std::optional<uint32_t> graphicsFamily;
		std::optional<uint32_t> presentFamily;

		bool isComplete() { return graphicsFamily.has_value() && presentFamily.has_value(); }
	};

	struct SwapChainSupportDetails {
		VkSurfaceCapabilitiesKHR capabilities;
		std::vector<VkSurfaceFormatKHR> formats;
		std::vector<VkPresentModeKHR> presentModes;
	};

	struct Device {
		VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
		VkDevice device;

		uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
			VkPhysicalDeviceMemoryProperties memProperties;
			vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

			for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
				if (typeFilter & (1 << i) &&
				    (memProperties.memoryTypes[i].propertyFlags & properties) == properties)
					return i;
			}

			throw std::runtime_error(LOCATION "failed to find suitable memory type!");
		}
	};

	struct Image {
		Device device;

		VkImage image;
		VkDeviceMemory memory;
		VkImageView view;
		VkSampler sampler;

		Image() = default;

		Image(Device device, uint32_t width, uint32_t height, VkImageType imageType,
		      VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage,
		      VkMemoryPropertyFlags properties) {
			this->device = device;
			VkImageCreateInfo imageInfo = {};
			imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
			imageInfo.imageType = imageType;
			imageInfo.extent.width = width;
			imageInfo.extent.height = height;
			imageInfo.extent.depth = 1;
			imageInfo.mipLevels = 1;
			imageInfo.arrayLayers = 1;
			imageInfo.format = format;
			imageInfo.tiling = tiling;
			imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			imageInfo.usage = usage;
			imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
			imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (vkCreateImage(device.device, &imageInfo, nullptr, &image) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create image!");

			VkMemoryRequirements memRequirements;
			vkGetImageMemoryRequirements(device.device, image, &memRequirements);

			VkMemoryAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocInfo.allocationSize = memRequirements.size;
			allocInfo.memoryTypeIndex =
			    device.findMemoryType(memRequirements.memoryTypeBits, properties);

			if (vkAllocateMemory(device.device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to allocate image memory!");

			vkBindImageMemory(device.device, image, memory, 0);
		}

		static void destroy(Image image) {
			vkDestroySampler(image.device.device, image.sampler, nullptr);
			vkDestroyImageView(image.device.device, image.view, nullptr);
			vkDestroyImage(image.device.device, image.image, nullptr);
			vkFreeMemory(image.device.device, image.memory, nullptr);
		}
	};

	typedef std::variant<uint32_t, int32_t, float> SpecializationConstant;

	struct SpecializationConstants {
		std::vector<SpecializationConstant> data;
		std::vector<VkSpecializationMapEntry> specializationInfo;
	};

	struct GraphicsPipeline {
		VkPipeline graphicsPipeline;
		VkShaderModule fragShaderModule;
		VkShaderModule vertShaderModule;
	};

	template <class resourceType>
	struct Resource {
		uint32_t id;
		std::filesystem::path path;

		resourceType rsrc;
	};

	struct Module {
		std::filesystem::path location;

		std::vector<GraphicsPipeline> layers;
		SpecializationConstants specializationConstants;

		std::vector<Resource<Image>> images;

		// Name of the fragment shader function to call
		std::string moduleName = "main";
		uint32_t vertexCount = 6;

		static void destroy(VkDevice device, Module& module) {
			for (auto& layer : module.layers) {
				vkDestroyShaderModule(device, layer.fragShaderModule, nullptr);
				vkDestroyShaderModule(device, layer.vertShaderModule, nullptr);
			}

			for (auto& image : module.images) Image::destroy(image.rsrc);
		}
	};

	struct Buffer {
		Device device;

		VkBuffer buffer;
		VkDeviceMemory memory;
		VkBufferView view = VK_NULL_HANDLE;

		VkDeviceSize size;

		Buffer() = default;

		Buffer(Device device, VkDeviceSize size, VkBufferUsageFlags usage,
		       VkMemoryPropertyFlags properties) {
			this->device = device;
			this->size = size;

			VkBufferCreateInfo bufferInfo = {};
			bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			bufferInfo.size = size;
			bufferInfo.usage = usage;
			bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			if (vkCreateBuffer(device.device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create buffer!");

			VkMemoryRequirements memRequirements;
			vkGetBufferMemoryRequirements(device.device, buffer, &memRequirements);

			VkMemoryAllocateInfo allocInfo = {};
			allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
			allocInfo.allocationSize = memRequirements.size;
			allocInfo.memoryTypeIndex =
			    device.findMemoryType(memRequirements.memoryTypeBits, properties);

			if (vkAllocateMemory(device.device, &allocInfo, nullptr, &memory) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to allocate buffer memory!");

			vkBindBufferMemory(device.device, buffer, memory, 0);
		}

		void createBufferView(VkFormat format) {
			VkBufferViewCreateInfo viewInfo = {};
			viewInfo.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO;
			viewInfo.buffer = buffer;
			viewInfo.format = format;
			viewInfo.offset = 0;
			viewInfo.range = size;

			if (vkCreateBufferView(device.device, &viewInfo, nullptr, &view) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create buffer view!");
		}

		void* mapMemory() {
			void* data;
			vkMapMemory(device.device, memory, 0, size, 0, &data);
			return data;
		}

		void unmapMemory() { vkUnmapMemory(device.device, memory); }

		static void destroy(Buffer& buffer) {
			vkDestroyBufferView(buffer.device.device, buffer.view, nullptr);
			vkDestroyBuffer(buffer.device.device, buffer.buffer, nullptr);
			vkFreeMemory(buffer.device.device, buffer.memory, nullptr);
		}
	};

	struct UniformBufferObject {
		float lVolume;
		float rVolume;
		uint32_t time;
	};
}  // namespace

class Renderer::RendererImpl {
public:
	RendererImpl(const Settings& renderSettings) {
		settings = renderSettings;

		initWindow();
		initVulkan();
	}

	bool drawFrame(const AudioData& audioData) {
		glfwPollEvents();
		if (glfwWindowShouldClose(window)) return false;

		vkWaitForFences(device.device, 1, &inFlightFences[currentFrame], VK_TRUE,
		                std::numeric_limits<uint64_t>::max());

		uint32_t imageIndex;
		VkResult result = vkAcquireNextImageKHR(
		    device.device, swapChain, std::numeric_limits<uint64_t>::max(),
		    imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);

		switch (result) {
			case VK_SUCCESS:
			case VK_SUBOPTIMAL_KHR:
				break;
			case VK_ERROR_OUT_OF_DATE_KHR:
				recreateSwapChain();
				return true;
			default:
				throw std::runtime_error(LOCATION "failed to acquire swap chain image!");
		}

		updateAudioBuffers(audioData, imageIndex);

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

		VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
		VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pWaitSemaphores = waitSemaphores;
		submitInfo.pWaitDstStageMask = waitStages;

		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffers[imageIndex];

		VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]};
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.pSignalSemaphores = signalSemaphores;

		vkResetFences(device.device, 1, &inFlightFences[currentFrame]);

		if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) !=
		    VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to submit draw command buffer!");

		VkPresentInfoKHR presentInfo = {};
		presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
		presentInfo.waitSemaphoreCount = 1;
		presentInfo.pWaitSemaphores = signalSemaphores;

		presentInfo.swapchainCount = 1;
		presentInfo.pSwapchains = &swapChain;
		presentInfo.pImageIndices = &imageIndex;

		result = vkQueuePresentKHR(presentQueue, &presentInfo);

		switch (result) {
			case VK_SUCCESS:
				break;
			case VK_ERROR_OUT_OF_DATE_KHR:
			case VK_SUBOPTIMAL_KHR:
				recreateSwapChain();
				return true;
			default:
				throw std::runtime_error(LOCATION "failed to present swap chain image!");
		}

		currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;

		return true;
	}

	~RendererImpl() {
		vkDeviceWaitIdle(device.device);

		cleanupSwapChain();

		for (size_t i = 0; i < modules.size(); ++i)
			vkDestroyPipelineLayout(device.device, pipelineLayouts[i], nullptr);

		vkDestroyRenderPass(device.device, renderPass, nullptr);

		vkDestroyDescriptorPool(device.device, descriptorPool, nullptr);

		vkDestroyDescriptorSetLayout(device.device, commonDescriptorSetLayout, nullptr);
		for (auto& layout : descriptorSetLayouts)
			vkDestroyDescriptorSetLayout(device.device, layout, nullptr);

		for (size_t i = 0; i < dataBuffers.size(); ++i) {
			Buffer::destroy(dataBuffers[i]);
			Buffer::destroy(lAudioBuffers[i]);
			Buffer::destroy(rAudioBuffers[i]);
		}

		for (auto& module : modules) Module::destroy(device.device, module);

		Image::destroy(backgroundImage);

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			vkDestroySemaphore(device.device, imageAvailableSemaphores[i], nullptr);
			vkDestroySemaphore(device.device, renderFinishedSemaphores[i], nullptr);
			vkDestroyFence(device.device, inFlightFences[i], nullptr);
		}

		vkDestroyCommandPool(device.device, commandPool, nullptr);

		vkDestroyDevice(device.device, nullptr);

		if constexpr (enableValidationLayers)
			DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);

		vkDestroySurfaceKHR(instance, surface, nullptr);
		vkDestroyInstance(instance, nullptr);

		glfwDestroyWindow(window);

		glfwTerminate();
	}

private:
	// Variables

	Settings settings;

	GLFWwindow* window;

	VkInstance instance;
	VkDebugUtilsMessengerEXT debugMessenger;

	VkSurfaceKHR surface;

	Device device;

	VkQueue graphicsQueue;
	VkQueue presentQueue;

	VkSwapchainKHR swapChain;
	std::vector<VkImage> swapChainImages;
	VkFormat swapChainImageFormat;
	VkExtent2D swapChainExtent;

	std::vector<VkImageView> swapChainImageViews;
	std::vector<VkFramebuffer> swapChainFramebuffers;

	VkRenderPass renderPass;
	VkDescriptorSetLayout commonDescriptorSetLayout;
	std::vector<VkDescriptorSetLayout> descriptorSetLayouts;
	std::vector<VkPipelineLayout> pipelineLayouts;

	std::vector<Module> modules;

	VkCommandPool commandPool;
	std::vector<VkCommandBuffer> commandBuffers;

	std::vector<Buffer> dataBuffers;
	std::vector<Buffer> lAudioBuffers;
	std::vector<Buffer> rAudioBuffers;

	Image backgroundImage;

	VkDescriptorPool descriptorPool;
	std::vector<VkDescriptorSet> commonDescriptorSets;
	std::vector<std::vector<VkDescriptorSet>> descriptorSets;

	std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> imageAvailableSemaphores;
	std::array<VkSemaphore, MAX_FRAMES_IN_FLIGHT> renderFinishedSemaphores;
	std::array<VkFence, MAX_FRAMES_IN_FLIGHT> inFlightFences;
	size_t currentFrame = 0;

	// Member functions

	void initWindow() {
		glfwInit();

		if (!glfwVulkanSupported())
			throw std::runtime_error(LOCATION "vulkan not supported by the current environment!");

		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

		glfwWindowHint(GLFW_DECORATED, settings.window.hints.decorated ? GLFW_TRUE : GLFW_FALSE);
		glfwWindowHint(GLFW_RESIZABLE, settings.window.hints.resizable ? GLFW_TRUE : GLFW_FALSE);

#ifdef GLFW_TRANSPARENT_FRAMEBUFFER
		glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER,
		               settings.window.transparency == Settings::Window::Transparency::native
		                   ? GLFW_TRUE
		                   : GLFW_FALSE);
#else
		if (settings.window.transparency == Settings::Window::Transparency::native) {
			std::cerr << LOCATION "Native transaprency unsupported by current configuration!"
			          << std::endl;
			settings.window.transparency = Settings::Window::Transparency::opaque;
		}
#endif

		window = glfwCreateWindow(settings.window.width, settings.window.height,
		                          settings.window.title.c_str(), nullptr, nullptr);
#ifdef NATIVE_WINDOW_HINTS_SUPPORTED
		if (settings.window.type == "desktop") setWindowType(window, WindowType::DESKTOP);

		if (settings.window.hints.sticky) setSticky(window);
#endif

		if (settings.window.position)
			glfwSetWindowPos(window, settings.window.position->first,
			                 settings.window.position->second);
	}

	void initVulkan() {
		createInstance();
		setupDebugCallback();
		createSurface();
		pickPhysicalDevice();
		createLogicalDevice();
		createSwapchain();
		createImageViews();
		createRenderPass();
		discoverModules();
		createDescriptorSetLayouts();
		createGraphicsPipelineLayouts();
		createGraphicsPipelines();
		createFramebuffers();
		createCommandPool();
		createAudioBuffers();
		createModuleImages();
		createBackgroundImage();
		createDescriptorPool();
		createDescriptorSets();
		createCommandBuffers();
		createSyncObjects();
	}

	void createInstance() {
		const auto extensions = getRequiredExtensions();
		if (!checkRequiredExtensionsPresent(extensions))
			throw std::runtime_error(LOCATION "missing required vulkan extension!");

		const auto layers = getRequiredLayers();
		if (!checkRequiredLayersPresent(layers))
			throw std::runtime_error(LOCATION "missing required vulkan layers!");

		VkApplicationInfo appInfo = {};
		appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName = "Vkav";
		appInfo.applicationVersion = VK_MAKE_VERSION(VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH);
		appInfo.pEngineName = "No Engine";
		appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
		appInfo.apiVersion = VK_API_VERSION_1_0;

		VkInstanceCreateInfo instanceInfo = {};
		instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceInfo.pApplicationInfo = &appInfo;
		instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
		instanceInfo.ppEnabledExtensionNames = extensions.data();
		instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
		instanceInfo.ppEnabledLayerNames = layers.data();

		if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create a vulkan instance!");
	}

	std::vector<const char*> getRequiredExtensions() const {
		uint32_t glfwExtensionCount = 0;
		const char** glfwExtensions;
		glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

		std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

		if constexpr (enableValidationLayers) {
			extensions.reserve(extensions.size() + 1);
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		}

		return extensions;
	}

	bool checkRequiredExtensionsPresent(const std::vector<const char*>& extensions) const {
		uint32_t availableExtensionCount = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr);
		std::vector<VkExtensionProperties> availableExtensions(availableExtensionCount);
		vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount,
		                                       availableExtensions.data());

		std::set<std::string> requiredExtensions(extensions.begin(), extensions.end());

		for (const auto& extension : availableExtensions)
			requiredExtensions.erase(extension.extensionName);

		return requiredExtensions.empty();
	}

	std::vector<const char*> getRequiredLayers() const {
		if constexpr (enableValidationLayers)
			return {"VK_LAYER_LUNARG_standard_validation"};
		else
			return {};
	}

	bool checkRequiredLayersPresent(const std::vector<const char*>& layers) const {
		uint32_t availableLayerCount = 0;
		vkEnumerateInstanceLayerProperties(&availableLayerCount, nullptr);
		std::vector<VkLayerProperties> availableLayers(availableLayerCount);
		vkEnumerateInstanceLayerProperties(&availableLayerCount, availableLayers.data());
		std::set<std::string> requiredLayers(layers.begin(), layers.end());

		for (const auto& layer : availableLayers) requiredLayers.erase(layer.layerName);

		return requiredLayers.empty();
	}

	void setupDebugCallback() {
		if constexpr (!enableValidationLayers) return;

		VkDebugUtilsMessengerCreateInfoEXT messengerInfo = {};
		messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
		                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		                                VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messengerInfo.pfnUserCallback = debugCallback;

		if (createDebugUtilsMessengerEXT(instance, &messengerInfo, nullptr, &debugMessenger) !=
		    VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create debug messenger!");
	}

	void createSurface() {
		if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create window surface!");
	}

	void pickPhysicalDevice() {
		uint32_t deviceCount = 0;
		vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
		if (deviceCount == 0)
			throw std::runtime_error(LOCATION "failed to find GPUs with Vulkan support!");

		std::vector<VkPhysicalDevice> devices(deviceCount);
		vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

		if (settings.physicalDevice) {
			if (settings.physicalDevice.value() >= deviceCount ||
			    !isDeviceSuitable(devices[settings.physicalDevice.value()]))
				throw std::runtime_error(LOCATION "invalid GPU selected!");

			device.physicalDevice = devices[settings.physicalDevice.value()];
			return;
		}

		for (const auto& device : devices) {
			if (isDeviceSuitable(device)) {
				this->device.physicalDevice = device;
				break;
			}
		}

		if (device.physicalDevice == VK_NULL_HANDLE)
			throw std::runtime_error(LOCATION "failed to find a suitable GPU!");
	}

	bool isDeviceSuitable(VkPhysicalDevice device) const {
		QueueFamilyIndices indices = findQueueFamilies(device);

		bool extensionsSupported = checkDeviceExtensionSupport(device);

		bool swapChainAdequate = false;
		if (extensionsSupported) {
			SwapChainSupportDetails swapChainDetails = querySwapChainSupport(device);
			swapChainAdequate =
			    !swapChainDetails.formats.empty() && !swapChainDetails.presentModes.empty();
		}

		VkPhysicalDeviceProperties deviceProperties;
		vkGetPhysicalDeviceProperties(device, &deviceProperties);

		// Size of audio buffers + volume <= maximum capacity
		bool uniformBufferSizeAdequate =
		    2 * settings.audioSize * sizeof(float) <= deviceProperties.limits.maxUniformBufferRange;

		return indices.isComplete() && extensionsSupported && swapChainAdequate &&
		       uniformBufferSizeAdequate;
	}

	bool checkDeviceExtensionSupport(VkPhysicalDevice device) const {
		uint32_t availableExtensionCount = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &availableExtensionCount, nullptr);
		std::vector<VkExtensionProperties> availableExtensions(availableExtensionCount);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &availableExtensionCount,
		                                     availableExtensions.data());

		std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
		for (const auto& availableExtension : availableExtensions)
			requiredExtensions.erase(availableExtension.extensionName);

		return requiredExtensions.empty();
	}

	QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) const {
		QueueFamilyIndices indices;

		uint32_t queueFamilyCount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

		int i = 0;
		for (const auto& queueFamily : queueFamilies) {
			if (queueFamily.queueCount <= 0) continue;

			VkBool32 presentSupport = false;
			vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

			if (presentSupport) indices.presentFamily = i;

			if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) indices.graphicsFamily = i;

			if (indices.isComplete()) break;

			++i;
		}

		return indices;
	}

	void createLogicalDevice() {
		QueueFamilyIndices indices = findQueueFamilies(device.physicalDevice);

		std::vector<VkDeviceQueueCreateInfo> queueInfos;
		std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
		                                          indices.presentFamily.value()};

		float queuePriority = 1.f;
		for (const auto& queueFamily : uniqueQueueFamilies) {
			VkDeviceQueueCreateInfo queueInfo = {};
			queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
			queueInfo.queueFamilyIndex = queueFamily;
			queueInfo.queueCount = 1;
			queueInfo.pQueuePriorities = &queuePriority;
			queueInfos.push_back(queueInfo);
		}

		VkPhysicalDeviceFeatures deviceFeatures = {};

		const auto layers = getRequiredLayers();

		VkDeviceCreateInfo deviceInfo = {};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.pQueueCreateInfos = queueInfos.data();
		deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
		deviceInfo.pEnabledFeatures = &deviceFeatures;
		deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
		deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
		deviceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
		deviceInfo.ppEnabledLayerNames = layers.data();

		if (vkCreateDevice(device.physicalDevice, &deviceInfo, nullptr, &device.device) !=
		    VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create logical device!");

		vkGetDeviceQueue(device.device, indices.graphicsFamily.value(), 0, &graphicsQueue);
		vkGetDeviceQueue(device.device, indices.presentFamily.value(), 0, &presentQueue);
	}

	void createSwapchain() {
		SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device.physicalDevice);

		VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
		VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
		VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);

		uint32_t imageCount = swapChainSupport.capabilities.minImageCount;

		if (presentMode == VK_PRESENT_MODE_MAILBOX_KHR) ++imageCount;

		if (swapChainSupport.capabilities.maxImageCount > 0 &&
		    imageCount > swapChainSupport.capabilities.maxImageCount)
			imageCount = swapChainSupport.capabilities.maxImageCount;

		VkSwapchainCreateInfoKHR swapChainInfo = {};
		swapChainInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
		swapChainInfo.surface = surface;
		swapChainInfo.minImageCount = imageCount;
		swapChainInfo.imageFormat = surfaceFormat.format;
		swapChainInfo.imageColorSpace = surfaceFormat.colorSpace;
		swapChainInfo.imageExtent = extent;
		swapChainInfo.imageArrayLayers = 1;
		swapChainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

		QueueFamilyIndices indices = findQueueFamilies(device.physicalDevice);
		uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(),
		                                 indices.presentFamily.value()};

		if (indices.graphicsFamily.value() != indices.presentFamily) {
			swapChainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			swapChainInfo.queueFamilyIndexCount = 2;
			swapChainInfo.pQueueFamilyIndices = queueFamilyIndices;
		} else {
			swapChainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
			swapChainInfo.queueFamilyIndexCount = 0;
			swapChainInfo.pQueueFamilyIndices = nullptr;
		}

		swapChainInfo.preTransform = swapChainSupport.capabilities.currentTransform;

		switch (settings.window.transparency) {
			case Settings::Window::Transparency::native:
				if (swapChainSupport.capabilities.supportedCompositeAlpha &
				    VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR) {
					swapChainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
				} else {
					std::cerr << LOCATION "native transparency not supported!\n";
					swapChainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
					settings.window.transparency = Settings::Window::Transparency::opaque;
				}
				break;
			case Settings::Window::Transparency::vulkan:
				if (swapChainSupport.capabilities.supportedCompositeAlpha &
				    VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR) {
					swapChainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
				} else {
					std::cerr << LOCATION "vulkan transparency not supported!\n";
					swapChainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
					settings.window.transparency = Settings::Window::Transparency::opaque;
				}
				break;
			case Settings::Window::Transparency::opaque:
				swapChainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
				break;
		}

		swapChainInfo.presentMode = presentMode;
		swapChainInfo.clipped = VK_TRUE;
		swapChainInfo.oldSwapchain = VK_NULL_HANDLE;

		if (vkCreateSwapchainKHR(device.device, &swapChainInfo, nullptr, &swapChain) != VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create swapchain!");

		vkGetSwapchainImagesKHR(device.device, swapChain, &imageCount, nullptr);
		swapChainImages.resize(imageCount);
		vkGetSwapchainImagesKHR(device.device, swapChain, &imageCount, swapChainImages.data());

		swapChainImageFormat = surfaceFormat.format;
		swapChainExtent = extent;
	}

	SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const {
		SwapChainSupportDetails details;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

		uint32_t formatCount = 0;
		vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

		if (formatCount != 0) {
			details.formats.resize(formatCount);
			vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount,
			                                     details.formats.data());
		}

		uint32_t presentModeCount = 0;
		vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

		if (formatCount != 0) {
			details.presentModes.resize(presentModeCount);
			vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount,
			                                          details.presentModes.data());
		}

		return details;
	}

	VkSurfaceFormatKHR chooseSwapSurfaceFormat(
	    const std::vector<VkSurfaceFormatKHR>& availableFormats) const {
		if (availableFormats.size() == 1 && availableFormats[0].format == VK_FORMAT_UNDEFINED)
			return {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};

		for (const auto& availableFormat : availableFormats) {
			if (availableFormat.format == VK_FORMAT_B8G8R8A8_UNORM &&
			    availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				return availableFormat;
		}

		return availableFormats[0];
	}

	VkPresentModeKHR chooseSwapPresentMode(
	    const std::vector<VkPresentModeKHR>& availablePresentModes) const {
		if (settings.vsync) return VK_PRESENT_MODE_FIFO_KHR;

		VkPresentModeKHR bestMode = VK_PRESENT_MODE_FIFO_KHR;

		for (const auto& availablePresentMode : availablePresentModes) {
			if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
				return availablePresentMode;
			else if (availablePresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
				bestMode = availablePresentMode;
		}

		return bestMode;
	}

	VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) const {
		if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
			return capabilities.currentExtent;
		} else {
			int width, height;
			glfwGetFramebufferSize(window, &width, &height);

			VkExtent2D extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height)};

			extent.width = std::clamp(extent.width, capabilities.minImageExtent.width,
			                          capabilities.maxImageExtent.width);
			extent.height = std::clamp(extent.height, capabilities.minImageExtent.height,
			                           capabilities.maxImageExtent.height);

			return extent;
		}
	}

	void createImageViews() {
		swapChainImageViews.resize(swapChainImages.size());

		for (size_t i = 0; i < swapChainImages.size(); ++i)
			swapChainImageViews[i] = createImageView(swapChainImages[i], swapChainImageFormat);
	}

	void discoverModules() {
		modules.resize(settings.modules.size());

		for (uint32_t i = 0; i < modules.size(); ++i) {
			modules[i].location = findModule(settings.modules[i].string());

			// find number of layers
			uint32_t layerCount = 1;
			while (std::filesystem::exists(modules[i].location / std::to_string(layerCount + 1)))
				++layerCount;
			modules[i].layers.resize(layerCount);

			// find fallback vertex shader
			const auto fallbackVertShaderPath =
			    std::filesystem::exists(modules[i].location / "vert.spv")
			        ? modules[i].location
			        : settings.moduleLocations.front() / "modules";

			// find and create shaders for each layer
			for (uint32_t layer = 0; layer < layerCount; ++layer) {
				auto vertexShaderPath = modules[i].location / std::to_string(layer + 1);
				if (!std::filesystem::exists(vertexShaderPath / "vert.spv"))
					vertexShaderPath = fallbackVertShaderPath;

				auto vertShaderCode = readFile(vertexShaderPath / "vert.spv");
				modules[i].layers[layer].vertShaderModule = createShaderModule(vertShaderCode);

				auto fragmentShaderPath = modules[i].location / std::to_string(layer + 1);
				auto fragShaderCode = readFile(fragmentShaderPath / "frag.spv");
				modules[i].layers[layer].fragShaderModule = createShaderModule(fragShaderCode);
			}

			readConfig(modules[i].location / "config", modules[i]);
			modules[i].specializationConstants.data[0] = static_cast<uint32_t>(settings.audioSize);
			modules[i].specializationConstants.data[1] = settings.smoothingLevel;
			modules[i].specializationConstants.data[4] = modules[i].vertexCount;
		}
	}

	std::filesystem::path findModule(const std::string& moduleName) const {
		if (std::filesystem::path(moduleName).is_absolute()) return moduleName;

		for (auto& path : settings.moduleLocations)
			if (std::filesystem::exists(path / "modules" / moduleName))
				return path / "modules" / moduleName;

		throw std::invalid_argument(LOCATION "Unable to locate module!");
	}

	void createGraphicsPipelineLayouts() {
		pipelineLayouts.resize(modules.size());

		for (uint32_t module = 0; module < modules.size(); ++module) {
			std::array<VkDescriptorSetLayout, 2> moduleDescSetLayouts = {
			    commonDescriptorSetLayout, descriptorSetLayouts[module]};
			VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
			pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
			pipelineLayoutInfo.setLayoutCount = moduleDescSetLayouts.size();
			pipelineLayoutInfo.pSetLayouts = moduleDescSetLayouts.data();
			pipelineLayoutInfo.pushConstantRangeCount = 0;
			pipelineLayoutInfo.pPushConstantRanges = nullptr;

			if (vkCreatePipelineLayout(device.device, &pipelineLayoutInfo, nullptr,
			                           &pipelineLayouts[module]) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create pipeline layout!");
		}
	}

	void createGraphicsPipelines() {
		VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputInfo.vertexBindingDescriptionCount = 0;
		vertexInputInfo.vertexAttributeDescriptionCount = 0;

		VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		VkViewport viewport = {};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = static_cast<float>(swapChainExtent.width);
		viewport.height = static_cast<float>(swapChainExtent.height);
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor = {};
		scissor.offset = {0, 0};
		scissor.extent = swapChainExtent;

		VkPipelineViewportStateCreateInfo viewportState = {};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.pViewports = &viewport;
		viewportState.scissorCount = 1;
		viewportState.pScissors = &scissor;

		VkPipelineRasterizationStateCreateInfo rasterizer = {};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.depthClampEnable = VK_FALSE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.lineWidth = 1.0f;
		rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
		rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
		rasterizer.depthBiasEnable = VK_FALSE;

		VkPipelineMultisampleStateCreateInfo multisampling = {};
		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.sampleShadingEnable = VK_FALSE;
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

		VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = true;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

		VkPipelineColorBlendStateCreateInfo colorBlending = {};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;

		size_t pipelineCount = 0;
		for (const auto& module : modules) pipelineCount += module.layers.size();

		std::vector<VkSpecializationInfo> specializationInfos;
		specializationInfos.reserve(modules.size());
		std::vector<std::array<VkPipelineShaderStageCreateInfo, 2>> shaderStages;
		shaderStages.reserve(pipelineCount);
		std::vector<VkGraphicsPipelineCreateInfo> pipelineInfos;
		pipelineInfos.reserve(pipelineCount);

		for (uint32_t module = 0; module < modules.size(); ++module) {
			VkSpecializationInfo specializationInfo = {};
			specializationInfo.mapEntryCount =
			    modules[module].specializationConstants.specializationInfo.size();
			specializationInfo.pMapEntries =
			    modules[module].specializationConstants.specializationInfo.data();
			specializationInfo.dataSize = modules[module].specializationConstants.data.size() *
			                              sizeof(SpecializationConstant);
			specializationInfo.pData = modules[module].specializationConstants.data.data();
			specializationInfos.push_back(specializationInfo);

			for (uint32_t layer = 0; layer < modules[module].layers.size(); ++layer) {
				modules[module].specializationConstants.data[2] = swapChainExtent.width;
				modules[module].specializationConstants.data[3] = swapChainExtent.height;

				VkPipelineShaderStageCreateInfo vertShaderStageInfo = {};
				vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
				vertShaderStageInfo.module = modules[module].layers[layer].vertShaderModule;
				vertShaderStageInfo.pName = "main";
				vertShaderStageInfo.pSpecializationInfo = &specializationInfos.back();

				VkPipelineShaderStageCreateInfo fragShaderStageInfo = {};
				fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
				fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
				fragShaderStageInfo.module = modules[module].layers[layer].fragShaderModule;
				fragShaderStageInfo.pName = modules[module].moduleName.c_str();
				fragShaderStageInfo.pSpecializationInfo = &specializationInfos[module];

				shaderStages.push_back({vertShaderStageInfo, fragShaderStageInfo});

				VkGraphicsPipelineCreateInfo pipelineInfo = {};
				pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
				pipelineInfo.stageCount = 2;
				pipelineInfo.pStages = shaderStages.back().data();
				pipelineInfo.pVertexInputState = &vertexInputInfo;
				pipelineInfo.pInputAssemblyState = &inputAssembly;
				pipelineInfo.pViewportState = &viewportState;
				pipelineInfo.pRasterizationState = &rasterizer;
				pipelineInfo.pMultisampleState = &multisampling;
				pipelineInfo.pDepthStencilState = nullptr;
				pipelineInfo.pColorBlendState = &colorBlending;
				pipelineInfo.pDynamicState = nullptr;
				pipelineInfo.layout = pipelineLayouts[module];
				pipelineInfo.renderPass = renderPass;
				pipelineInfo.subpass = 0;
				pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;
				pipelineInfo.basePipelineIndex = 0;

				pipelineInfos.push_back(pipelineInfo);
			}
		}

		std::vector<VkPipeline> pipelines(pipelineCount);
		if (vkCreateGraphicsPipelines(device.device, VK_NULL_HANDLE, pipelines.size(),
		                              pipelineInfos.data(), nullptr, pipelines.data()))
			throw std::runtime_error(LOCATION "failed to create graphics pipeline!");

		uint32_t i = 0;
		for (auto& module : modules)
			for (auto& layer : module.layers) layer.graphicsPipeline = pipelines[i++];
	}

	VkShaderModule createShaderModule(const std::vector<char>& shaderCode) {
		VkShaderModuleCreateInfo shaderModuleInfo = {};
		shaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		shaderModuleInfo.codeSize = shaderCode.size();
		shaderModuleInfo.pCode = reinterpret_cast<const uint32_t*>(shaderCode.data());

		VkShaderModule shaderModule;
		if (vkCreateShaderModule(device.device, &shaderModuleInfo, nullptr, &shaderModule) !=
		    VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create shader module!");

		return shaderModule;
	}

	void createRenderPass() {
		VkAttachmentDescription colorAttachment = {};
		colorAttachment.format = swapChainImageFormat;
		colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

		VkAttachmentReference colorAttachmentRef = {};
		colorAttachmentRef.attachment = 0;
		colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkSubpassDescription subpass = {};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &colorAttachmentRef;

		VkSubpassDependency dependency = {};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.srcAccessMask = 0;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask =
		    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

		VkRenderPassCreateInfo renderPassInfo = {};
		renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassInfo.attachmentCount = 1;
		renderPassInfo.pAttachments = &colorAttachment;
		renderPassInfo.subpassCount = 1;
		renderPassInfo.pSubpasses = &subpass;
		renderPassInfo.dependencyCount = 1;
		renderPassInfo.pDependencies = &dependency;

		if (vkCreateRenderPass(device.device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create render pass!");
	}

	void createFramebuffers() {
		swapChainFramebuffers.resize(swapChainImageViews.size());

		for (size_t i = 0; i < swapChainFramebuffers.size(); ++i) {
			VkImageView attachments[] = {swapChainImageViews[i]};

			VkFramebufferCreateInfo framebufferInfo = {};
			framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
			framebufferInfo.renderPass = renderPass;
			framebufferInfo.attachmentCount = 1;
			framebufferInfo.pAttachments = attachments;
			framebufferInfo.width = swapChainExtent.width;
			framebufferInfo.height = swapChainExtent.height;
			framebufferInfo.layers = 1;

			if (vkCreateFramebuffer(device.device, &framebufferInfo, nullptr,
			                        &swapChainFramebuffers[i]) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create framebuffer!");
		}
	}

	void createCommandPool() {
		QueueFamilyIndices queueFamilyIndices = findQueueFamilies(device.physicalDevice);

		VkCommandPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

		if (vkCreateCommandPool(device.device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create command pool!");
	}

	void createCommandBuffers() {
		commandBuffers.resize(swapChainFramebuffers.size());

		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());

		if (vkAllocateCommandBuffers(device.device, &allocInfo, commandBuffers.data()) !=
		    VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to allocate command buffers!");

		for (size_t i = 0; i < commandBuffers.size(); ++i) {
			VkCommandBufferBeginInfo beginInfo = {};
			beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
			beginInfo.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
			beginInfo.pInheritanceInfo = nullptr;

			if (vkBeginCommandBuffer(commandBuffers[i], &beginInfo) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to begin recording command buffer!");

			VkRenderPassBeginInfo renderPassInfo = {};
			renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
			renderPassInfo.renderPass = renderPass;
			renderPassInfo.framebuffer = swapChainFramebuffers[i];
			renderPassInfo.renderArea.offset = {0, 0};
			renderPassInfo.renderArea.extent = swapChainExtent;
			VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 0.0f}}};
			renderPassInfo.clearValueCount = 1;
			renderPassInfo.pClearValues = &clearColor;

			vkCmdBeginRenderPass(commandBuffers[i], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

			vkCmdBindDescriptorSets(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
			                        pipelineLayouts[0], 0, 1, &commonDescriptorSets[i], 0, nullptr);
			for (size_t module = 0; module < modules.size(); ++module) {
				vkCmdBindDescriptorSets(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
				                        pipelineLayouts[module], 1, 1, &descriptorSets[i][module],
				                        0, nullptr);
				for (const auto& layer : modules[module].layers) {
					vkCmdBindPipeline(commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
					                  layer.graphicsPipeline);
					vkCmdDraw(commandBuffers[i], modules[module].vertexCount, 1, 0, 0);
				}
			}

			vkCmdEndRenderPass(commandBuffers[i]);

			if (vkEndCommandBuffer(commandBuffers[i]) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to record command buffer!");
		}
	}

	void createSyncObjects() {
		VkSemaphoreCreateInfo semaphoreInfo = {};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fenceInfo = {};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
			if (vkCreateSemaphore(device.device, &semaphoreInfo, nullptr,
			                      &imageAvailableSemaphores[i]) != VK_SUCCESS ||
			    vkCreateSemaphore(device.device, &semaphoreInfo, nullptr,
			                      &renderFinishedSemaphores[i]) != VK_SUCCESS ||
			    vkCreateFence(device.device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create sync objects!");
		}
	}

	void cleanupSwapChain() {
		for (auto framebuffer : swapChainFramebuffers)
			vkDestroyFramebuffer(device.device, framebuffer, nullptr);

		vkFreeCommandBuffers(device.device, commandPool,
		                     static_cast<uint32_t>(commandBuffers.size()), commandBuffers.data());

		for (size_t i = 0; i < modules.size(); ++i)
			for (auto& graphicsPipeline : modules[i].layers)
				vkDestroyPipeline(device.device, graphicsPipeline.graphicsPipeline, nullptr);

		for (auto imageView : swapChainImageViews)
			vkDestroyImageView(device.device, imageView, nullptr);

		vkDestroySwapchainKHR(device.device, swapChain, nullptr);
	}

	void recreateSwapChain() {
		while (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) glfwWaitEvents();

		vkDeviceWaitIdle(device.device);

		cleanupSwapChain();

		createSwapchain();
		createImageViews();
		createGraphicsPipelines();
		createFramebuffers();
		createCommandBuffers();
	}

	void createModuleImages() {
		for (auto& module : modules) {
			for (auto& image : module.images) {
				std::filesystem::path path = image.path;
				if (!path.empty() && path.is_relative()) path = module.location / path;
				createTextureImage(path, image.rsrc);
				image.rsrc.view = createImageView(image.rsrc.image, VK_FORMAT_R8G8B8A8_UNORM);
				image.rsrc.sampler = createImageSampler();
			}
		}
	}

	void createBackgroundImage() {
		createTextureImage(settings.backgroundImage, backgroundImage);
		backgroundImage.view = createImageView(backgroundImage.image, VK_FORMAT_R8G8B8A8_UNORM);
		backgroundImage.sampler = createImageSampler();
	}

	void createTextureImage(const std::filesystem::path& imagePath, Image& image) {
		ImageFile img;
		if (!imagePath.empty()) img.open(imagePath);

		Buffer stagingBuffer(
		    device, img.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

		void* data = stagingBuffer.mapMemory();
		for (size_t y = 0; y < img.height(); ++y)
			std::copy_n(img[y], img.width() * 4,
			            reinterpret_cast<unsigned char*>(data) + y * img.width() * 4);
		stagingBuffer.unmapMemory();

		image = Image(device, img.width(), img.height(), VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
		              VK_IMAGE_TILING_OPTIMAL,
		              VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		              VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

		transitionImageLayout(image.image, VK_IMAGE_LAYOUT_UNDEFINED,
		                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copyBufferToImage(stagingBuffer.buffer, image.image, static_cast<uint32_t>(img.width()),
		                  static_cast<uint32_t>(img.height()));
		transitionImageLayout(image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		Buffer::destroy(stagingBuffer);
	}

	VkCommandBuffer beginSingleTimeCommands() {
		VkCommandBufferAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandPool = commandPool;
		allocInfo.commandBufferCount = 1;

		VkCommandBuffer commandBuffer;
		vkAllocateCommandBuffers(device.device, &allocInfo, &commandBuffer);

		VkCommandBufferBeginInfo beginInfo = {};
		beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

		vkBeginCommandBuffer(commandBuffer, &beginInfo);

		return commandBuffer;
	}

	void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
		vkEndCommandBuffer(commandBuffer);

		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &commandBuffer;

		vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
		vkQueueWaitIdle(graphicsQueue);

		vkFreeCommandBuffers(device.device, commandPool, 1, &commandBuffer);
	}

	void transitionImageLayout(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkImageMemoryBarrier barrier = {};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.oldLayout = oldLayout;
		barrier.newLayout = newLayout;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = image;
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = 0;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.baseArrayLayer = 0;
		barrier.subresourceRange.layerCount = 1;

		VkPipelineStageFlags srcStage;
		VkPipelineStageFlags dstStage;

		if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
		    newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
			barrier.srcAccessMask = 0;
			barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

			srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
			dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
		} else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
		           newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
			barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
			barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

			srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
			dstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
		} else {
			throw std::invalid_argument(LOCATION "unsupported layout transition!");
		}

		vkCmdPipelineBarrier(commandBuffer, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1,
		                     &barrier);

		endSingleTimeCommands(commandBuffer);
	}

	void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
		VkCommandBuffer commandBuffer = beginSingleTimeCommands();

		VkBufferImageCopy region = {};
		region.bufferOffset = 0;
		region.bufferRowLength = 0;
		region.bufferImageHeight = 0;

		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = 0;
		region.imageSubresource.baseArrayLayer = 0;
		region.imageSubresource.layerCount = 1;

		region.imageOffset = {0, 0, 0};
		region.imageExtent = {width, height, 1};

		vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                       1, &region);

		endSingleTimeCommands(commandBuffer);
	}

	VkImageView createImageView(VkImage image, VkFormat format) {
		VkImageViewCreateInfo viewInfo = {};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.baseMipLevel = 0;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.baseArrayLayer = 0;
		viewInfo.subresourceRange.layerCount = 1;

		VkImageView imageView;
		if (vkCreateImageView(device.device, &viewInfo, nullptr, &imageView) != VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create image view!");

		return imageView;
	}

	VkSampler createImageSampler() {
		VkSamplerCreateInfo samplerInfo = {};
		samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		samplerInfo.magFilter = VK_FILTER_LINEAR;
		samplerInfo.minFilter = VK_FILTER_LINEAR;
		samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
		samplerInfo.anisotropyEnable = VK_FALSE;
		samplerInfo.maxAnisotropy = 1;
		samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
		samplerInfo.unnormalizedCoordinates = VK_FALSE;
		samplerInfo.compareEnable = VK_FALSE;
		samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
		samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		samplerInfo.mipLodBias = 0.0f;
		samplerInfo.minLod = 0.0f;
		samplerInfo.maxLod = 0.0f;

		VkSampler sampler;
		if (vkCreateSampler(device.device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create image sampler!");

		return sampler;
	}

	void createDescriptorSetLayouts() {
		{
			VkDescriptorSetLayoutBinding dataLayoutBinding = {};
			dataLayoutBinding.binding = 0;
			dataLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
			dataLayoutBinding.descriptorCount = 1;
			dataLayoutBinding.pImmutableSamplers = nullptr;
			dataLayoutBinding.stageFlags =
			    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

			VkDescriptorSetLayoutBinding lAudioBufferLayoutBinding = {};
			lAudioBufferLayoutBinding.binding = 1;
			lAudioBufferLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
			lAudioBufferLayoutBinding.descriptorCount = 1;
			lAudioBufferLayoutBinding.pImmutableSamplers = nullptr;
			lAudioBufferLayoutBinding.stageFlags =
			    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

			VkDescriptorSetLayoutBinding rAudioBufferLayoutBinding = {};
			rAudioBufferLayoutBinding.binding = 2;
			rAudioBufferLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
			rAudioBufferLayoutBinding.descriptorCount = 1;
			rAudioBufferLayoutBinding.pImmutableSamplers = nullptr;
			rAudioBufferLayoutBinding.stageFlags =
			    VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;

			VkDescriptorSetLayoutBinding backgroundSamplerLayoutBinding = {};
			backgroundSamplerLayoutBinding.binding = 3;
			backgroundSamplerLayoutBinding.descriptorCount = 1;
			backgroundSamplerLayoutBinding.descriptorType =
			    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			backgroundSamplerLayoutBinding.pImmutableSamplers = nullptr;
			backgroundSamplerLayoutBinding.stageFlags =
			    VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

			std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
			    dataLayoutBinding, lAudioBufferLayoutBinding, rAudioBufferLayoutBinding,
			    backgroundSamplerLayoutBinding};

			VkDescriptorSetLayoutCreateInfo layoutInfo = {};
			layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			layoutInfo.pBindings = bindings.data();

			if (vkCreateDescriptorSetLayout(device.device, &layoutInfo, nullptr,
			                                &commonDescriptorSetLayout) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create descriptor set layout!");
		}

		descriptorSetLayouts.resize(modules.size());
		for (size_t module = 0; module < modules.size(); ++module) {
			std::vector<VkDescriptorSetLayoutBinding> bindings(modules[module].images.size());

			for (size_t image = 0; image < modules[module].images.size(); ++image) {
				bindings[image].binding = modules[module].images[image].id;
				bindings[image].descriptorCount = 1;
				bindings[image].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				bindings[image].pImmutableSamplers = nullptr;
				bindings[image].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
			}

			VkDescriptorSetLayoutCreateInfo layoutInfo = {};
			layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
			layoutInfo.pBindings = bindings.data();

			if (vkCreateDescriptorSetLayout(device.device, &layoutInfo, nullptr,
			                                &descriptorSetLayouts[module]) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to create descriptor set layout!");
		}
	}

	void createAudioBuffers() {
		VkDeviceSize bufferSize = settings.audioSize * sizeof(float);

		dataBuffers.resize(swapChainImages.size());

		lAudioBuffers.resize(swapChainImages.size());
		rAudioBuffers.resize(swapChainImages.size());

		for (size_t i = 0; i < dataBuffers.size(); ++i) {
			dataBuffers[i] =
			    Buffer(device, sizeof(UniformBufferObject), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

			lAudioBuffers[i] =
			    Buffer(device, bufferSize, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
			           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

			lAudioBuffers[i].createBufferView(VK_FORMAT_R32_SFLOAT);

			rAudioBuffers[i] =
			    Buffer(device, bufferSize, VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT,
			           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

			rAudioBuffers[i].createBufferView(VK_FORMAT_R32_SFLOAT);
		}
	}

	void updateAudioBuffers(const AudioData& audioData, uint32_t currentFrame) {
		static const auto startTime = std::chrono::high_resolution_clock::now();
		const auto currentTime = std::chrono::high_resolution_clock::now();
		void* data;

		data = dataBuffers[currentFrame].mapMemory();
		reinterpret_cast<UniformBufferObject*>(data)->lVolume = audioData.lVolume;
		reinterpret_cast<UniformBufferObject*>(data)->rVolume = audioData.rVolume;
		reinterpret_cast<UniformBufferObject*>(data)->time =
		    std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - startTime).count();
		dataBuffers[currentFrame].unmapMemory();

		data = lAudioBuffers[currentFrame].mapMemory();
		std::copy_n(audioData.lBuffer, settings.audioSize, reinterpret_cast<float*>(data));
		lAudioBuffers[currentFrame].unmapMemory();

		data = rAudioBuffers[currentFrame].mapMemory();
		std::copy_n(audioData.rBuffer, settings.audioSize, reinterpret_cast<float*>(data));
		rAudioBuffers[currentFrame].unmapMemory();
	}

	void createDescriptorPool() {
		std::array<VkDescriptorPoolSize, 4> poolSizes = {};
		poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		poolSizes[0].descriptorCount =
		    static_cast<uint32_t>(swapChainImages.size() * modules.size());
		poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		poolSizes[1].descriptorCount =
		    static_cast<uint32_t>(swapChainImages.size() * modules.size());
		poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
		poolSizes[2].descriptorCount =
		    static_cast<uint32_t>(swapChainImages.size() * modules.size());

		size_t resourceCount = 0;
		for (auto& module : modules) resourceCount += module.images.size();

		poolSizes[3].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		poolSizes[3].descriptorCount =
		    static_cast<uint32_t>(swapChainImages.size() * (resourceCount + modules.size()));

		VkDescriptorPoolCreateInfo poolInfo = {};
		poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
		poolInfo.pPoolSizes = poolSizes.data();
		poolInfo.maxSets = static_cast<uint32_t>(swapChainImages.size() * 2 * modules.size());

		if (vkCreateDescriptorPool(device.device, &poolInfo, nullptr, &descriptorPool) !=
		    VK_SUCCESS)
			throw std::runtime_error(LOCATION "failed to create descriptor pool!");
	}

	void createDescriptorSets() {
		VkDescriptorSetAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		allocInfo.descriptorPool = descriptorPool;
		allocInfo.descriptorSetCount = static_cast<uint32_t>(descriptorSetLayouts.size());
		allocInfo.pSetLayouts = descriptorSetLayouts.data();

		VkDescriptorSetAllocateInfo commonAllocInfo = {};
		commonAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		commonAllocInfo.descriptorPool = descriptorPool;
		commonAllocInfo.descriptorSetCount = 1;
		commonAllocInfo.pSetLayouts = &commonDescriptorSetLayout;

		commonDescriptorSets.resize(swapChainImages.size());
		descriptorSets.resize(swapChainImages.size());

		for (size_t i = 0; i < swapChainImages.size(); ++i) {
			descriptorSets[i].resize(modules.size());

			if (vkAllocateDescriptorSets(device.device, &commonAllocInfo,
			                             &commonDescriptorSets[i]) != VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to allocate descriptor sets!");

			if (vkAllocateDescriptorSets(device.device, &allocInfo, descriptorSets[i].data()) !=
			    VK_SUCCESS)
				throw std::runtime_error(LOCATION "failed to allocate descriptor sets!");

			{
				VkDescriptorBufferInfo dataBufferInfo = {};
				dataBufferInfo.buffer = dataBuffers[i].buffer;
				dataBufferInfo.offset = 0;
				dataBufferInfo.range = sizeof(UniformBufferObject);

				VkDescriptorImageInfo backgroundImageInfo = {};
				backgroundImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				backgroundImageInfo.imageView = backgroundImage.view;
				backgroundImageInfo.sampler = backgroundImage.sampler;

				std::array<VkWriteDescriptorSet, 4> descriptorWrites = {};
				descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptorWrites[0].dstBinding = 0;
				descriptorWrites[0].dstArrayElement = 0;
				descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
				descriptorWrites[0].descriptorCount = 1;
				descriptorWrites[0].pBufferInfo = &dataBufferInfo;

				descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptorWrites[1].dstBinding = 1;
				descriptorWrites[1].dstArrayElement = 0;
				descriptorWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
				descriptorWrites[1].descriptorCount = 1;
				descriptorWrites[1].pTexelBufferView = &lAudioBuffers[i].view;

				descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptorWrites[2].dstBinding = 2;
				descriptorWrites[2].dstArrayElement = 0;
				descriptorWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
				descriptorWrites[2].descriptorCount = 1;
				descriptorWrites[2].pTexelBufferView = &rAudioBuffers[i].view;

				descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
				descriptorWrites[3].dstBinding = 3;
				descriptorWrites[3].dstArrayElement = 0;
				descriptorWrites[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
				descriptorWrites[3].descriptorCount = 1;
				descriptorWrites[3].pImageInfo = &backgroundImageInfo;

				descriptorWrites[0].dstSet = commonDescriptorSets[i];
				descriptorWrites[1].dstSet = commonDescriptorSets[i];
				descriptorWrites[2].dstSet = commonDescriptorSets[i];
				descriptorWrites[3].dstSet = commonDescriptorSets[i];

				vkUpdateDescriptorSets(device.device,
				                       static_cast<uint32_t>(descriptorWrites.size()),
				                       descriptorWrites.data(), 0, nullptr);
			}

			for (size_t module = 0; module < modules.size(); ++module) {
				const size_t resourceCount = modules[module].images.size();

				std::vector<VkDescriptorImageInfo> moduleImageInfos{resourceCount};
				std::vector<VkWriteDescriptorSet> descriptorWrites{resourceCount};

				for (size_t image = 0; image < modules[module].images.size(); ++image) {
					moduleImageInfos[image].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
					moduleImageInfos[image].imageView = modules[module].images[image].rsrc.view;
					moduleImageInfos[image].sampler = modules[module].images[image].rsrc.sampler;

					descriptorWrites[image].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
					descriptorWrites[image].dstBinding = modules[module].images[image].id;
					descriptorWrites[image].dstArrayElement = 0;
					descriptorWrites[image].descriptorType =
					    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
					descriptorWrites[image].descriptorCount = 1;
					descriptorWrites[image].pImageInfo = &moduleImageInfos[image];
					descriptorWrites[image].dstSet = descriptorSets[i][module];
				}

				vkUpdateDescriptorSets(device.device,
				                       static_cast<uint32_t>(descriptorWrites.size()),
				                       descriptorWrites.data(), 0, nullptr);
			}
		}
	}

	// Static member functions

	static VKAPI_ATTR VkBool32 VKAPI_CALL
	debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT, VkDebugUtilsMessageTypeFlagsEXT,
	              const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void*) {
		std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;
		return VK_FALSE;
	}

	static std::vector<char> readFile(const std::filesystem::path& filePath) {
		std::ifstream file(filePath, std::ios::binary | std::ios::ate);

		if (!file.is_open()) throw std::runtime_error(LOCATION "failed to open file!");

		size_t fileSize = static_cast<size_t>(file.tellg());
		std::vector<char> buffer(fileSize);

		file.seekg(0);
		file.read(buffer.data(), fileSize);

		file.close();

		return buffer;
	}

	static void readConfig(const std::filesystem::path& configFilePath, Module& module) {
		std::ifstream file(configFilePath);
		if (!file.is_open()) {
			std::cerr << "shader configuration file not found!" << std::endl;
			return;
		}

		ModuleConfig config;
		try {
			config = parseConfig(file);
		} catch (const ParseException& e) {
			throw std::runtime_error(std::string(LOCATION) + "Failed to parse module config '" +
			                         configFilePath.string() + "':\n\tline " +
			                         std::to_string(e.line()) + ":" + e.what());
		} catch (const std::exception& e) {
			throw std::runtime_error(std::string(LOCATION) + "Failed to parse module config '" +
			                         configFilePath.string() + "':\n\t" + e.what());
		}

		if (config.moduleName) module.moduleName = config.moduleName.value();
		if (config.vertexCount) module.vertexCount = config.vertexCount.value();

		module.specializationConstants.data.reserve(5 + config.params.size());
		module.specializationConstants.data.resize(5);
		module.specializationConstants.specializationInfo.reserve(5 + config.params.size());
		for (uint32_t offset = 0; offset < 5; ++offset) {
			VkSpecializationMapEntry mapEntry = {};
			mapEntry.constantID = offset;
			mapEntry.offset = offset * sizeof(SpecializationConstant);
			mapEntry.size = sizeof(uint32_t);

			module.specializationConstants.specializationInfo.push_back(mapEntry);
		}

		for (auto& param : config.params) {
			VkSpecializationMapEntry mapEntry = {};
			mapEntry.constantID = param.id;
			mapEntry.offset =
			    module.specializationConstants.data.size() * sizeof(SpecializationConstant);
			mapEntry.size = sizeof(uint32_t);

			module.specializationConstants.data.push_back(param.value);
			module.specializationConstants.specializationInfo.push_back(mapEntry);
		}

		module.images.reserve(config.images.size());
		for (auto& image : config.images) {
			Resource<Image> resource = {};
			resource.id = image.id;
			resource.path = image.path;
			module.images.push_back(resource);
		}
	}
};

Renderer::Renderer(const Settings& settings) { rendererImpl = new RendererImpl(settings); }

Renderer& Renderer::operator=(Renderer&& other) noexcept {
	std::swap(rendererImpl, other.rendererImpl);
	return *this;
}

bool Renderer::drawFrame(const AudioData& audioData) { return rendererImpl->drawFrame(audioData); }

Renderer::~Renderer() { delete rendererImpl; }
