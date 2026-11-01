// naruto - native renderer view
//
// Runs on its own thread: the game's Swap hook only hands over the latest
// frame's captured draws, so presenting here never stalls the game. The
// window is created on the main thread (SDL's requirement); Vulkan runs on
// the view thread.
//
// Draws use the shaders XenosRecomp converted (tools/build-shaders.py), which
// read their constants through buffer device addresses passed as push
// constants and their textures from bindless descriptor arrays. Textures are
// not captured yet: every texture is a 1x1 white placeholder.

#include "native_view.h"

#include "shader_library.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace native_view {

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkDeviceSize kInitialUploadSize = 32ull << 20;

#define NV_INSTANCE_FUNCTIONS(X)                \
  X(vkEnumeratePhysicalDevices)                 \
  X(vkGetPhysicalDeviceProperties)              \
  X(vkGetPhysicalDeviceFeatures2)               \
  X(vkGetPhysicalDeviceMemoryProperties)        \
  X(vkGetPhysicalDeviceQueueFamilyProperties)   \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)       \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)  \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)       \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)  \
  X(vkCreateDevice)                             \
  X(vkGetDeviceProcAddr)

#define NV_DEVICE_FUNCTIONS(X)       \
  X(vkGetDeviceQueue)                \
  X(vkCreateSwapchainKHR)            \
  X(vkDestroySwapchainKHR)           \
  X(vkGetSwapchainImagesKHR)         \
  X(vkAcquireNextImageKHR)           \
  X(vkQueuePresentKHR)               \
  X(vkQueueSubmit)                   \
  X(vkQueueWaitIdle)                 \
  X(vkCreateImage)                   \
  X(vkDestroyImage)                  \
  X(vkGetImageMemoryRequirements)    \
  X(vkBindImageMemory)               \
  X(vkCreateImageView)               \
  X(vkDestroyImageView)              \
  X(vkCreateBuffer)                  \
  X(vkDestroyBuffer)                 \
  X(vkGetBufferMemoryRequirements)   \
  X(vkBindBufferMemory)              \
  X(vkGetBufferDeviceAddress)        \
  X(vkAllocateMemory)                \
  X(vkFreeMemory)                    \
  X(vkMapMemory)                     \
  X(vkCreateSampler)                 \
  X(vkCreateDescriptorSetLayout)     \
  X(vkCreateDescriptorPool)          \
  X(vkAllocateDescriptorSets)        \
  X(vkUpdateDescriptorSets)          \
  X(vkCreatePipelineLayout)          \
  X(vkCreateShaderModule)            \
  X(vkCreateGraphicsPipelines)       \
  X(vkCreateRenderPass)              \
  X(vkCreateFramebuffer)             \
  X(vkDestroyFramebuffer)            \
  X(vkCreateCommandPool)             \
  X(vkAllocateCommandBuffers)        \
  X(vkBeginCommandBuffer)            \
  X(vkEndCommandBuffer)              \
  X(vkResetCommandBuffer)            \
  X(vkCmdBeginRenderPass)            \
  X(vkCmdEndRenderPass)              \
  X(vkCmdPipelineBarrier)            \
  X(vkCmdClearColorImage)            \
  X(vkCmdBindPipeline)               \
  X(vkCmdBindDescriptorSets)         \
  X(vkCmdBindVertexBuffers)          \
  X(vkCmdBindIndexBuffer)            \
  X(vkCmdPushConstants)              \
  X(vkCmdSetViewport)                \
  X(vkCmdSetScissor)                 \
  X(vkCmdDrawIndexed)                \
  X(vkCreateSemaphore)               \
  X(vkDestroySemaphore)              \
  X(vkCreateFence)                   \
  X(vkWaitForFences)                 \
  X(vkResetFences)                   \
  X(vkDeviceWaitIdle)

// XenosRecomp's push constants: addresses of the vertex, pixel and shared
// constants.
struct PushConstants {
  VkDeviceAddress vertex_constants;
  VkDeviceAddress pixel_constants;
  VkDeviceAddress shared_constants;
};

// XenosRecomp's specialization constant bit for alpha testing.
constexpr uint32_t kSpecAlphaTest = 1 << 1;

VkBlendFactor BlendFactor(uint32_t factor) {
  static constexpr VkBlendFactor kFactors[] = {
      VK_BLEND_FACTOR_ZERO,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_ONE,
      VK_BLEND_FACTOR_SRC_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
      VK_BLEND_FACTOR_SRC_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      VK_BLEND_FACTOR_DST_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
      VK_BLEND_FACTOR_DST_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
      VK_BLEND_FACTOR_CONSTANT_COLOR,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
      VK_BLEND_FACTOR_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
      VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
  };
  return factor < std::size(kFactors) ? kFactors[factor] : VK_BLEND_FACTOR_ONE;
}

// NARUTO_NATIVE_VIEW_DEBUG bits: 1 disables depth testing.
uint32_t DebugFlags() {
  static const uint32_t flags = [] {
    const char* value = std::getenv("NARUTO_NATIVE_VIEW_DEBUG");
    return value ? uint32_t(std::strtoul(value, nullptr, 0)) : 0u;
  }();
  return flags;
}

VkBlendOp BlendOp(uint32_t op) {
  static constexpr VkBlendOp kOps[] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN,
                                       VK_BLEND_OP_MAX, VK_BLEND_OP_REVERSE_SUBTRACT};
  return op < std::size(kOps) ? kOps[op] : VK_BLEND_OP_ADD;
}

class View {
 public:
  void Submit(FrameInfo&& frame) {
    std::call_once(started_, [this] { std::thread([this] { Run(); }).detach(); });
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = std::move(frame);
    pending_ = true;
    wake_.notify_one();
  }

 private:
  void Run() {
    if (!Initialize()) {
      REXLOG_ERROR("[native view] Vulkan setup failed; the native view is off");
      return;
    }
    REXLOG_INFO("[native view] presenting on {}", device_name_);
    for (;;) {
      FrameInfo frame;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return pending_; });
        pending_ = false;
        frame = std::move(latest_);
      }
      DrawFrame(frame);
    }
  }

  bool Initialize() {
    // SDL windows belong to the main thread.
    auto create_window = [](void* self) {
      static_cast<View*>(self)->window_ = SDL_CreateWindow(
          "Naruto - native renderer", 960, 540, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    };
    if (!SDL_RunOnMainThread(create_window, this, true) || !window_) {
      REXLOG_ERROR("[native view] window creation failed: {}", SDL_GetError());
      return false;
    }

    vk_get_instance_proc_addr_ =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
    if (!vk_get_instance_proc_addr_) {
      return false;
    }
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(
        vk_get_instance_proc_addr_(VK_NULL_HANDLE, "vkCreateInstance"));
    Uint32 extension_count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "naruto native view";
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instance_info.pApplicationInfo = &app;
    instance_info.enabledExtensionCount = extension_count;
    instance_info.ppEnabledExtensionNames = extensions;
    if (!vkCreateInstance || vkCreateInstance(&instance_info, nullptr, &instance_) != VK_SUCCESS) {
      return false;
    }
#define X(name) \
  name = reinterpret_cast<PFN_##name>(vk_get_instance_proc_addr_(instance_, #name));
    NV_INSTANCE_FUNCTIONS(X)
#undef X
    if (!SDL_Vulkan_CreateSurface(window_, instance_, nullptr, &surface_)) {
      REXLOG_ERROR("[native view] surface creation failed: {}", SDL_GetError());
      return false;
    }
    return PickDevice() && CreateDevice() && CreateResources() && CreateSwapchain();
  }

  // Prefers a discrete GPU with a queue that both draws and presents, and
  // the Vulkan 1.2 features the converted shaders need.
  bool PickDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    int best_score = -1;
    for (VkPhysicalDevice device : devices) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(device, &properties);
      VkPhysicalDeviceVulkan12Features features12{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features.pNext = &features12;
      if (properties.apiVersion < VK_API_VERSION_1_2) {
        continue;
      }
      vkGetPhysicalDeviceFeatures2(device, &features);
      if (!features.features.shaderInt64 || !features12.bufferDeviceAddress ||
          !features12.runtimeDescriptorArray || !features12.descriptorBindingPartiallyBound) {
        REXLOG_INFO("[native view] {} lacks features the shaders need", properties.deviceName);
        continue;
      }
      uint32_t family_count = 0;
      vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, nullptr);
      std::vector<VkQueueFamilyProperties> families(family_count);
      vkGetPhysicalDeviceQueueFamilyProperties(device, &family_count, families.data());
      for (uint32_t family = 0; family < family_count; ++family) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(device, family, surface_, &present);
        if (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) {
          continue;
        }
        int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > best_score) {
          best_score = score;
          physical_device_ = device;
          queue_family_ = family;
          device_name_ = properties.deviceName;
        }
        break;
      }
    }
    return best_score >= 0;
  }

  bool CreateDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue_info.queueFamilyIndex = queue_family_;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.bufferDeviceAddress = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features12;
    features.features.shaderInt64 = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_info.pNext = &features;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    device_info.enabledExtensionCount = 1;
    device_info.ppEnabledExtensionNames = extensions;
    if (vkCreateDevice(physical_device_, &device_info, nullptr, &device_) != VK_SUCCESS) {
      return false;
    }
#define X(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device_, #name));
    NV_DEVICE_FUNCTIONS(X)
#undef X
    vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    vkGetPhysicalDeviceMemoryProperties(physical_device_, &memory_properties_);

    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family_;
    vkCreateCommandPool(device_, &pool_info, nullptr, &command_pool_);
    VkCommandBufferAllocateInfo alloc_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = command_pool_;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = kFramesInFlight;
    vkAllocateCommandBuffers(device_, &alloc_info, command_buffers_.data());
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
      vkCreateSemaphore(device_, &semaphore_info, nullptr, &image_acquired_[i]);
      vkCreateFence(device_, &fence_info, nullptr, &frame_done_[i]);
    }
    return true;
  }

  uint32_t MemoryType(uint32_t allowed, VkMemoryPropertyFlags required) {
    for (uint32_t i = 0; i < memory_properties_.memoryTypeCount; ++i) {
      if ((allowed & (1u << i)) &&
          (memory_properties_.memoryTypes[i].propertyFlags & required) == required) {
        return i;
      }
    }
    return UINT32_MAX;
  }

  VkDeviceMemory Allocate(const VkMemoryRequirements& requirements,
                          VkMemoryPropertyFlags properties, bool device_address) {
    uint32_t type = MemoryType(requirements.memoryTypeBits, properties);
    if (type == UINT32_MAX) {
      return VK_NULL_HANDLE;
    }
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    info.pNext = device_address ? &flags : nullptr;
    info.allocationSize = requirements.size;
    info.memoryTypeIndex = type;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    vkAllocateMemory(device_, &info, nullptr, &memory);
    return memory;
  }

  struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };

  Image CreateImage(VkImageType type, VkImageViewType view_type, VkFormat format,
                    VkExtent3D extent, uint32_t layers, VkImageUsageFlags usage,
                    VkImageAspectFlags aspect) {
    Image image;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = view_type == VK_IMAGE_VIEW_TYPE_CUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = type;
    info.format = format;
    info.extent = extent;
    info.mipLevels = 1;
    info.arrayLayers = layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(device_, &info, nullptr, &image.image);
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device_, image.image, &requirements);
    image.memory = Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    vkBindImageMemory(device_, image.image, image.memory, 0);
    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = image.image;
    view_info.viewType = view_type;
    view_info.format = format;
    view_info.subresourceRange = {aspect, 0, 1, 0, layers};
    vkCreateImageView(device_, &view_info, nullptr, &image.view);
    return image;
  }

  void DestroyImage(Image& image) {
    vkDestroyImageView(device_, image.view, nullptr);
    vkDestroyImage(device_, image.image, nullptr);
    vkFreeMemory(device_, image.memory, nullptr);
    image = {};
  }

  // Descriptor heaps with one white placeholder each, the pipeline layout
  // and the render pass.
  bool CreateResources() {
    const VkFormat kTextureFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    placeholders_[0] = CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, kTextureFormat,
                                   {1, 1, 1}, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT);
    placeholders_[1] = CreateImage(VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, kTextureFormat,
                                   {1, 1, 1}, 1, usage, VK_IMAGE_ASPECT_COLOR_BIT);
    placeholders_[2] = CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE, kTextureFormat,
                                   {1, 1, 1}, 6, usage, VK_IMAGE_ASPECT_COLOR_BIT);

    VkCommandBuffer cmd = command_buffers_[0];
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    for (Image& image : placeholders_) {
      uint32_t layers = &image == &placeholders_[2] ? 6 : 1;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image.image;
      barrier.subresourceRange = range;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkClearColorValue white = {{1.0f, 1.0f, 1.0f, 1.0f}};
      vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1,
                           &range);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);

    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.addressModeU = sampler_info.addressModeV = sampler_info.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    vkCreateSampler(device_, &sampler_info, nullptr, &sampler_);

    // Sets 0-2: 2D, 3D and cube textures; set 3: samplers (shader_common.h).
    VkDescriptorBindingFlags binding_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags_info.bindingCount = 1;
    flags_info.pBindingFlags = &binding_flags;
    VkDescriptorPoolSize pool_sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_);
    for (uint32_t set = 0; set < 4; ++set) {
      VkDescriptorType type = set < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                      : VK_DESCRIPTOR_TYPE_SAMPLER;
      VkDescriptorSetLayoutBinding binding{0, type, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
      VkDescriptorSetLayoutCreateInfo layout_info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      layout_info.pNext = &flags_info;
      layout_info.bindingCount = 1;
      layout_info.pBindings = &binding;
      vkCreateDescriptorSetLayout(device_, &layout_info, nullptr, &set_layouts_[set]);
    }
    VkDescriptorSetAllocateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    set_info.descriptorPool = descriptor_pool_;
    set_info.descriptorSetCount = 4;
    set_info.pSetLayouts = set_layouts_.data();
    if (vkAllocateDescriptorSets(device_, &set_info, descriptor_sets_.data()) != VK_SUCCESS) {
      return false;
    }
    std::array<VkDescriptorImageInfo, 4> image_infos{};
    std::array<VkWriteDescriptorSet, 4> writes{};
    for (uint32_t set = 0; set < 4; ++set) {
      image_infos[set].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      if (set < 3) {
        image_infos[set].imageView = placeholders_[set].view;
      } else {
        image_infos[set].sampler = sampler_;
      }
      writes[set] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[set].dstSet = descriptor_sets_[set];
      writes[set].descriptorCount = 1;
      writes[set].descriptorType = set < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                           : VK_DESCRIPTOR_TYPE_SAMPLER;
      writes[set].pImageInfo = &image_infos[set];
    }
    vkUpdateDescriptorSets(device_, 4, writes.data(), 0, nullptr);

    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(PushConstants)};
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 4;
    layout_info.pSetLayouts = set_layouts_.data();
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &push;
    return vkCreatePipelineLayout(device_, &layout_info, nullptr, &pipeline_layout_) ==
           VK_SUCCESS;
  }

  bool CreateRenderPass() {
    uint32_t count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, formats.data());
    format_ = formats[0];
    for (const auto& f : formats) {
      // UNORM, not sRGB: the game's output is shown as given.
      if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
        format_ = f;
        break;
      }
    }
    VkAttachmentDescription attachments[2] = {};
    attachments[0].format = format_.format;
    attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
    attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachments[1] = attachments[0];
    attachments[1].format = kDepthFormat;
    attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;
    subpass.pDepthStencilAttachment = &depth_ref;
    // The depth image is shared by the frames in flight.
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    pass_info.attachmentCount = 2;
    pass_info.pAttachments = attachments;
    pass_info.subpassCount = 1;
    pass_info.pSubpasses = &subpass;
    pass_info.dependencyCount = 1;
    pass_info.pDependencies = &dependency;
    return vkCreateRenderPass(device_, &pass_info, nullptr, &render_pass_) == VK_SUCCESS;
  }

  bool CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_device_, surface_, &caps);
    extent_ = caps.currentExtent;
    if (extent_.width == UINT32_MAX) {
      int w = 0, h = 0;
      SDL_GetWindowSizeInPixels(window_, &w, &h);
      extent_ = {uint32_t(std::clamp(w, int(caps.minImageExtent.width),
                                     int(caps.maxImageExtent.width))),
                 uint32_t(std::clamp(h, int(caps.minImageExtent.height),
                                     int(caps.maxImageExtent.height)))};
    }
    if (!extent_.width || !extent_.height) {
      return false;  // Minimized; try again on the next frame.
    }
    if (!render_pass_ && !CreateRenderPass()) {
      return false;
    }

    // Mailbox or immediate keep the view from pacing itself to the display.
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_device_, surface_, &mode_count,
                                              modes.data());
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    for (VkPresentModeKHR preferred : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
      if (std::find(modes.begin(), modes.end(), preferred) != modes.end()) {
        present_mode = preferred;
        break;
      }
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
      alpha = VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha &
                                          -int32_t(caps.supportedCompositeAlpha));
    }

    VkSwapchainKHR old = swapchain_;
    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = caps.maxImageCount ? std::min(caps.minImageCount + 1, caps.maxImageCount)
                                            : caps.minImageCount + 1;
    info.imageFormat = format_.format;
    info.imageColorSpace = format_.colorSpace;
    info.imageExtent = extent_;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = alpha;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old;
    VkSwapchainKHR created = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(device_, &info, nullptr, &created) != VK_SUCCESS) {
      return false;  // Keep presenting with the old swapchain, if any.
    }
    swapchain_ = created;
    DestroySwapchainResources();
    if (old) {
      vkDestroySwapchainKHR(device_, old, nullptr);
    }

    depth_ = CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, kDepthFormat,
                         {extent_.width, extent_.height, 1}, 1,
                         VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, images.data());
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : images) {
      Target target;
      VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      view_info.image = image;
      view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
      view_info.format = format_.format;
      view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCreateImageView(device_, &view_info, nullptr, &target.view);
      VkImageView views[] = {target.view, depth_.view};
      VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb_info.renderPass = render_pass_;
      fb_info.attachmentCount = 2;
      fb_info.pAttachments = views;
      fb_info.width = extent_.width;
      fb_info.height = extent_.height;
      fb_info.layers = 1;
      vkCreateFramebuffer(device_, &fb_info, nullptr, &target.framebuffer);
      // One per image: reacquiring the image is what makes reuse safe.
      vkCreateSemaphore(device_, &semaphore_info, nullptr, &target.render_done);
      targets_.push_back(target);
    }
    return true;
  }

  void DestroySwapchainResources() {
    for (Target& target : targets_) {
      vkDestroyFramebuffer(device_, target.framebuffer, nullptr);
      vkDestroyImageView(device_, target.view, nullptr);
      vkDestroySemaphore(device_, target.render_done, nullptr);
    }
    targets_.clear();
    if (depth_.image) {
      DestroyImage(depth_);
    }
  }

  void Recreate() {
    vkDeviceWaitIdle(device_);
    CreateSwapchain();
  }

  struct Upload {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint8_t* mapped = nullptr;
    VkDeviceSize size = 0;
    VkDeviceAddress address = 0;
  };

  // Grows the frame's upload buffer; the frame's fence has been waited on.
  bool ReserveUpload(Upload& upload, VkDeviceSize size) {
    if (upload.size >= size) {
      return true;
    }
    if (upload.buffer) {
      vkDestroyBuffer(device_, upload.buffer, nullptr);
      vkFreeMemory(device_, upload.memory, nullptr);
      upload = {};
    }
    VkDeviceSize new_size = kInitialUploadSize;
    while (new_size < size) {
      new_size *= 2;
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = new_size;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    vkCreateBuffer(device_, &info, nullptr, &upload.buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device_, upload.buffer, &requirements);
    upload.memory = Allocate(
        requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
        true);
    if (!upload.memory) {
      vkDestroyBuffer(device_, upload.buffer, nullptr);
      upload = {};
      return false;
    }
    vkBindBufferMemory(device_, upload.buffer, upload.memory, 0);
    vkMapMemory(device_, upload.memory, 0, VK_WHOLE_SIZE, 0,
                reinterpret_cast<void**>(&upload.mapped));
    VkBufferDeviceAddressInfo address_info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    address_info.buffer = upload.buffer;
    upload.address = vkGetBufferDeviceAddress(device_, &address_info);
    upload.size = new_size;
    return true;
  }

  VkShaderModule ShaderModule(int index) {
    if (size_t(index) >= shader_modules_.size()) {
      shader_modules_.resize(index + 1, VK_NULL_HANDLE);
    }
    VkShaderModule& module = shader_modules_[index];
    if (!module) {
      const std::vector<uint32_t>& spirv = shader_library::Shaders()[index].spirv;
      VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      info.codeSize = spirv.size() * 4;
      info.pCode = spirv.data();
      vkCreateShaderModule(device_, &info, nullptr, &module);
    }
    return module;
  }

  // The pipeline for the draw's shaders, vertex layout and render state, or
  // VK_NULL_HANDLE if it can't be created.
  VkPipeline Pipeline(const FrameInfo& frame, const Draw& draw) {
    uint32_t depth_state = draw.depth_control & 0x76;  // test, write, function
    uint32_t color_mask = draw.color_mask & 0xF;
    std::vector<uint32_t> key = {uint32_t(draw.vertex_shader), uint32_t(draw.pixel_shader),
                                 draw.topology, draw.primitive_restart, draw.alpha_test,
                                 depth_state, draw.blend_control, color_mask};
    for (uint32_t i = 0; i < draw.binding_count; ++i) {
      key.push_back(frame.bindings[draw.first_binding + i].stride);
    }
    for (uint32_t i = 0; i < draw.attribute_count; ++i) {
      const VertexAttribute& a = frame.attributes[draw.first_attribute + i];
      key.insert(key.end(), {a.location, a.binding, a.offset, a.format});
    }
    auto [it, added] = pipelines_.try_emplace(std::move(key), VK_NULL_HANDLE);
    if (!added) {
      return it->second;
    }

    uint32_t spec_value = draw.alpha_test ? kSpecAlphaTest : 0;
    VkSpecializationMapEntry spec_entry{0, 0, sizeof(uint32_t)};
    VkSpecializationInfo spec{1, &spec_entry, sizeof(spec_value), &spec_value};
    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = ShaderModule(draw.vertex_shader);
    stages[0].pName = "main";
    stages[0].pSpecializationInfo = &spec;
    uint32_t stage_count = 1;
    if (draw.pixel_shader >= 0) {
      stages[1] = stages[0];
      stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
      stages[1].module = ShaderModule(draw.pixel_shader);
      stage_count = 2;
    }

    std::vector<VkVertexInputBindingDescription> bindings;
    for (uint32_t i = 0; i < draw.binding_count; ++i) {
      bindings.push_back({i, frame.bindings[draw.first_binding + i].stride,
                          VK_VERTEX_INPUT_RATE_VERTEX});
    }
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (uint32_t i = 0; i < draw.attribute_count; ++i) {
      const VertexAttribute& a = frame.attributes[draw.first_attribute + i];
      attributes.push_back({a.location, a.binding, VkFormat(a.format), a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex_input.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vertex_input.pVertexBindingDescriptions = bindings.data();
    vertex_input.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vertex_input.pVertexAttributeDescriptions = attributes.data();

    VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    input_assembly.topology = VkPrimitiveTopology(draw.topology);
    input_assembly.primitiveRestartEnable = draw.primitive_restart;
    VkPipelineViewportStateCreateInfo viewport{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // RB_DEPTHCONTROL: bit 1 test, bit 2 write, bits 4-6 function in
    // Vulkan's order.
    VkPipelineDepthStencilStateCreateInfo depth{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = ((depth_state >> 1) & 1) && !(DebugFlags() & 1);
    depth.depthWriteEnable = depth.depthTestEnable && ((depth_state >> 2) & 1);
    depth.depthCompareOp = VkCompareOp((depth_state >> 4) & 7);
    // RB_BLENDCONTROL: source factor, function and destination factor for
    // color in bits 0-12, for alpha in bits 16-28.
    uint32_t blend = draw.blend_control;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.srcColorBlendFactor = BlendFactor(blend & 0x1F);
    attachment.colorBlendOp = BlendOp((blend >> 5) & 7);
    attachment.dstColorBlendFactor = BlendFactor((blend >> 8) & 0x1F);
    attachment.srcAlphaBlendFactor = BlendFactor((blend >> 16) & 0x1F);
    attachment.alphaBlendOp = BlendOp((blend >> 21) & 7);
    attachment.dstAlphaBlendFactor = BlendFactor((blend >> 24) & 0x1F);
    attachment.blendEnable = (blend & 0x1FFF1FFF) != 0x00010001;
    attachment.colorWriteMask = color_mask;
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = 1;
    color_blend.pAttachments = &attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &input_assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &color_blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = render_pass_;
    VkResult result =
        vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &it->second);
    if (result != VK_SUCCESS) {
      it->second = VK_NULL_HANDLE;
      REXLOG_WARN("[native view] pipeline for shaders {} and {} failed: {}", draw.vertex_shader,
                  draw.pixel_shader, int(result));
    }
    return it->second;
  }

  // A draw's viewport in render target pixels: Xenos maps clip space with
  // x = xoffset + xscale * x_ndc (y likewise; the shaders flip y for Vulkan).
  struct XenosViewport {
    float x_scale, x_offset, y_scale, y_offset, z_scale, z_offset;
  };

  static XenosViewport ViewportOf(const Draw& draw) {
    uint32_t vte = draw.vte_control;
    const float* v = draw.viewport;
    return {(vte & 1) ? v[0] : 1.0f,  (vte & 2) ? v[1] : 0.0f,  (vte & 4) ? v[2] : 1.0f,
            (vte & 8) ? v[3] : 0.0f,  (vte & 16) ? v[4] : 1.0f, (vte & 32) ? v[5] : 0.0f};
  }

  // The render target area a pass draws to, from its largest viewport; it is
  // scaled to fill the window.
  void FitPass(const FrameInfo& frame, uint32_t pass) {
    target_width_ = target_height_ = 0.0f;
    for (const Draw& draw : frame.captured) {
      if (draw.pass == pass) {
        XenosViewport v = ViewportOf(draw);
        target_width_ = std::max(target_width_, v.x_offset + std::abs(v.x_scale));
        target_height_ = std::max(target_height_, v.y_offset + std::abs(v.y_scale));
      }
    }
    if (target_width_ < 16.0f || target_height_ < 16.0f) {
      target_width_ = 1280.0f;
      target_height_ = 720.0f;
    }
  }

  VkViewport Viewport(const Draw& draw) {
    XenosViewport v = ViewportOf(draw);
    float x_scale = v.x_scale, x_offset = v.x_offset, y_scale = v.y_scale,
          y_offset = v.y_offset, z_scale = v.z_scale, z_offset = v.z_offset;
    float sx = extent_.width / target_width_, sy = extent_.height / target_height_;
    VkViewport viewport;
    viewport.x = (x_offset - x_scale) * sx;
    viewport.width = 2.0f * x_scale * sx;
    viewport.y = (y_offset + y_scale) * sy;
    viewport.height = -2.0f * y_scale * sy;
    viewport.minDepth = std::clamp(z_offset, 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(z_offset + z_scale, 0.0f, 1.0f);
    return viewport;
  }

  // Left/Right in the view's window step through passes, Home goes back to
  // the busiest one.
  void HandleKeys() {
    if (SDL_GetKeyboardFocus() != window_) {
      return;
    }
    const bool* keys = SDL_GetKeyboardState(nullptr);
    bool left = keys[SDL_SCANCODE_LEFT], right = keys[SDL_SCANCODE_RIGHT],
         home = keys[SDL_SCANCODE_HOME];
    if (left && !key_left_) {
      pass_offset_--;
    }
    if (right && !key_right_) {
      pass_offset_++;
    }
    if (home) {
      pass_offset_ = 0;
    }
    key_left_ = left;
    key_right_ = right;
  }

  void SetTitle(std::string title) {
    if (title == title_) {
      return;
    }
    title_ = title;
    auto set = [](void* data) {
      auto* args = static_cast<std::pair<SDL_Window*, std::string>*>(data);
      SDL_SetWindowTitle(args->first, args->second.c_str());
      delete args;
    };
    SDL_RunOnMainThread(set, new std::pair<SDL_Window*, std::string>(window_, title), false);
  }

  // NARUTO_NATIVE_VIEW_DUMP=n logs the shown pass's draws of the n-th frame,
  // for checking capture against what the game set.
  void Dump(const FrameInfo& frame, uint32_t pass) {
    static const long dump_frame = [] {
      const char* value = std::getenv("NARUTO_NATIVE_VIEW_DUMP");
      return value ? std::atol(value) : -1;
    }();
    if (long(frames_total_) != dump_frame) {
      return;
    }
    auto f32 = [&](uint32_t offset) {
      float v;
      std::memcpy(&v, frame.data.data() + offset, 4);
      return v;
    };
    const PassInfo& info = frame.passes[pass];
    REXLOG_INFO("[native view] pass {}: clear color {} ({} {} {} {}), clear depth {}", pass,
                info.has_clear_color, info.clear_color[0], info.clear_color[1],
                info.clear_color[2], info.clear_color[3], info.clear_depth);
    uint32_t n = 0;
    for (const Draw& d : frame.captured) {
      if (d.pass != pass || n++ >= 6) {
        continue;
      }
      const auto& shaders = shader_library::Shaders();
      VkViewport vp = Viewport(d);
      REXLOG_INFO(
          "[native view] draw: vs {}@{:#x} ps {} topology {} indices {} surface {:#x} vte {:#x} "
          "depth {:#x} blend {:#x} mask {:#x} vport [{} {} {} {} {} {}] -> {} {} {} {} {} {}",
          shaders[d.vertex_shader].library, shaders[d.vertex_shader].offset,
          d.pixel_shader >= 0 ? shaders[d.pixel_shader].offset : 0, d.topology, d.index_count,
          d.surface_info, d.vte_control, d.depth_control, d.blend_control, d.color_mask,
          d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], d.viewport[4],
          d.viewport[5], vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
      for (uint32_t i = 0; i < d.attribute_count; ++i) {
        const VertexAttribute& a = frame.attributes[d.first_attribute + i];
        const VertexBinding& b = frame.bindings[d.first_binding + a.binding];
        const auto& element = shaders[d.vertex_shader].vertex_elements[a.location];
        std::string values;
        for (uint32_t v = 0; v < 2; ++v) {
          uint32_t at = b.data_offset + v * b.stride + a.offset;
          values += fmt::format(" ({} {} {} | {:08x})", f32(at), f32(at + 4), f32(at + 8),
                                *reinterpret_cast<const uint32_t*>(frame.data.data() + at));
        }
        REXLOG_INFO("[native view]   input {} usage {}.{} binding {} stride {} offset {} format {}:{}",
                    a.location, element.usage, element.usage_index, a.binding, b.stride, a.offset,
                    a.format, values);
      }
      std::string constants;
      for (uint32_t c = 0; c < 8 * 4; ++c) {
        constants += fmt::format(" {:.3g}", f32(d.vertex_constants + c * 4));
      }
      REXLOG_INFO("[native view]   c0-c7:{}", constants);
    }
  }

  void DrawFrame(const FrameInfo& frame) {
    if (!swapchain_ && !CreateSwapchain()) {
      return;
    }

    // The busiest pass is normally the 3D scene.
    uint32_t pass_count = uint32_t(frame.passes.size());
    std::vector<uint32_t> pass_draws(pass_count);
    for (const Draw& draw : frame.captured) {
      pass_draws[std::min(draw.pass, pass_count - 1)]++;
    }
    uint32_t busiest = uint32_t(std::max_element(pass_draws.begin(), pass_draws.end()) -
                                pass_draws.begin());
    HandleKeys();
    uint32_t pass = uint32_t(((int64_t(busiest) + pass_offset_) % pass_count + pass_count) %
                             pass_count);
    FitPass(frame, pass);
    Dump(frame, pass);
    frames_total_++;
    SetTitle(fmt::format("Naruto - native renderer | pass {}/{}{} | {} of {} draws", pass + 1,
                         pass_count, pass_offset_ ? "" : " (busiest)", pass_draws[pass],
                         frame.draws));

    VkFence fence = frame_done_[frame_index_];
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
    Upload& upload = uploads_[frame_index_];
    if (!ReserveUpload(upload, std::max<VkDeviceSize>(frame.data.size(), 1))) {
      REXLOG_ERROR("[native view] upload buffer allocation failed");
      return;
    }
    uint32_t image_index = 0;
    VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                              image_acquired_[frame_index_], VK_NULL_HANDLE,
                                              &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
      Recreate();
      return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
      return;
    }
    vkResetFences(device_, 1, &fence);
    std::memcpy(upload.mapped, frame.data.data(), frame.data.size());

    VkCommandBuffer cmd = command_buffers_[frame_index_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    const PassInfo& pass_info = frame.passes[pass];
    VkClearValue clears[2] = {};
    if (pass_info.has_clear_color) {
      std::copy(pass_info.clear_color, pass_info.clear_color + 4, clears[0].color.float32);
    } else {
      clears[0].color = {{0.08f, 0.08f, 0.1f, 1.0f}};
    }
    clears[1].depthStencil = {pass_info.clear_depth, 0};
    VkRenderPassBeginInfo pass_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass_begin.renderPass = render_pass_;
    pass_begin.framebuffer = targets_[image_index].framebuffer;
    pass_begin.renderArea = {{0, 0}, extent_};
    pass_begin.clearValueCount = 2;
    pass_begin.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &pass_begin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 4,
                            descriptor_sets_.data(), 0, nullptr);
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    VkPipeline bound = VK_NULL_HANDLE;
    uint32_t drawn = 0;
    for (const Draw& draw : frame.captured) {
      if (draw.pass != pass || !draw.index_count) {
        continue;
      }
      VkViewport viewport = Viewport(draw);
      if (viewport.width <= 0.0f || viewport.height == 0.0f) {
        continue;
      }
      VkPipeline pipeline = Pipeline(frame, draw);
      if (!pipeline) {
        continue;
      }
      if (pipeline != bound) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        bound = pipeline;
      }
      vkCmdSetViewport(cmd, 0, 1, &viewport);
      std::array<VkBuffer, 16> buffers;
      std::array<VkDeviceSize, 16> offsets;
      for (uint32_t i = 0; i < draw.binding_count; ++i) {
        buffers[i] = upload.buffer;
        offsets[i] = frame.bindings[draw.first_binding + i].data_offset;
      }
      if (draw.binding_count) {
        vkCmdBindVertexBuffers(cmd, 0, draw.binding_count, buffers.data(), offsets.data());
      }
      vkCmdBindIndexBuffer(cmd, upload.buffer, draw.index_offset, VK_INDEX_TYPE_UINT32);
      PushConstants push{upload.address + draw.vertex_constants,
                         upload.address + draw.pixel_constants,
                         upload.address + draw.shared_constants};
      vkCmdPushConstants(cmd, pipeline_layout_,
                         VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), &push);
      vkCmdDrawIndexed(cmd, draw.index_count, 1, 0, 0, 0);
      drawn++;
    }
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore render_done = targets_[image_index].render_done;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_acquired_[frame_index_];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    vkQueueSubmit(queue_, 1, &submit, fence);

    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image_index;
    VkResult presented = vkQueuePresentKHR(queue_, &present);
    frame_index_ = (frame_index_ + 1) % kFramesInFlight;
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR ||
        acquired == VK_SUBOPTIMAL_KHR) {
      Recreate();
    }

    frames_presented_++;
    draws_drawn_ += drawn;
    auto now = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(now - log_start_).count();
    if (seconds >= 10.0) {
      REXLOG_INFO(
          "[native view] {:.1f} fps, {:.1f} draws/frame in the shown pass, {} passes, "
          "{:.1f} MB captured/frame, {} pipelines",
          frames_presented_ / seconds, double(draws_drawn_) / std::max<uint64_t>(frames_presented_, 1),
          pass_count, frame.data.size() / 1048576.0, pipelines_.size());
      frames_presented_ = 0;
      draws_drawn_ = 0;
      log_start_ = now;
    }
  }

  struct Target {
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkSemaphore render_done = VK_NULL_HANDLE;
  };

  std::once_flag started_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool pending_ = false;
  FrameInfo latest_;

  SDL_Window* window_ = nullptr;
  std::string title_;
  int64_t pass_offset_ = 0;
  float target_width_ = 1280.0f;
  float target_height_ = 720.0f;
  bool key_left_ = false;
  bool key_right_ = false;
  uint64_t frames_presented_ = 0;
  uint64_t frames_total_ = 0;
  uint64_t draws_drawn_ = 0;
  std::chrono::steady_clock::time_point log_start_ = std::chrono::steady_clock::now();

  std::string device_name_;
  PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr_ = nullptr;
#define X(name) PFN_##name name = nullptr;
  NV_INSTANCE_FUNCTIONS(X)
  NV_DEVICE_FUNCTIONS(X)
#undef X
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory_properties_{};
  uint32_t queue_family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kFramesInFlight> command_buffers_{};
  std::array<VkSemaphore, kFramesInFlight> image_acquired_{};
  std::array<VkFence, kFramesInFlight> frame_done_{};
  std::array<Upload, kFramesInFlight> uploads_{};
  uint32_t frame_index_ = 0;

  std::array<Image, 3> placeholders_{};
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 4> set_layouts_{};
  std::array<VkDescriptorSet, 4> descriptor_sets_{};
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::vector<VkShaderModule> shader_modules_;
  std::map<std::vector<uint32_t>, VkPipeline> pipelines_;

  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkSurfaceFormatKHR format_{};
  VkExtent2D extent_{};
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  Image depth_;
  std::vector<Target> targets_;
};

View& GetView() {
  // Never destroyed: the view thread runs until the process exits.
  static View* view = new View();
  return *view;
}

}  // namespace

bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("NARUTO_NATIVE_VIEW");
    return value && *value && *value != '0';
  }();
  return enabled;
}

void SubmitFrame(FrameInfo&& frame) {
  if (Enabled()) {
    GetView().Submit(std::move(frame));
  }
}

}  // namespace native_view
