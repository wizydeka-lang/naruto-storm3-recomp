// naruto - native renderer view
//
// Runs on its own thread: the game's Swap hook only hands over the latest
// frame's captured draws, so presenting here never stalls the game. The
// window is created on the main thread (SDL's requirement); Vulkan runs on
// the view thread.
//
// The whole frame is drawn as the game issued it: each pass renders into
// Vulkan images standing in for the Xenos EDRAM render targets (one per
// EDRAM base, format and pitch, so they persist across passes like EDRAM),
// each Resolve copies a render target into an image registered under the
// destination texture's address, and the window shows the texture
// D3DDevice_Swap presented. All of these images stay in the GENERAL layout,
// with a full barrier between passes and resolves.
//
// Draws use the shaders XenosRecomp converted (tools/build-shaders.py), which
// read their constants through buffer device addresses passed as push
// constants and their textures from bindless descriptor arrays. A texture is
// a resolved image if one was resolved to its address; otherwise it is
// decoded from guest memory on first use (texture_decode.h) and kept while
// its memory is unchanged. Textures the view can't decode are a 1x1 white
// placeholder.

#include "native_view.h"

#include "shader_library.h"
#include "texture_decode.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace native_view {

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
constexpr VkImageUsageFlags kColorTargetUsage =
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageUsageFlags kDepthTargetUsage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr VkDeviceSize kInitialUploadSize = 32ull << 20;
// Descriptor array sizes for 2D, 3D and cube textures; entry 0 is the
// placeholder.
constexpr uint32_t kHeapSizes[3] = {8192, 256, 256};
// One sampler per address mode X and Y (3 bits each) and linear or point
// magnification and minification.
constexpr uint32_t kSamplerCount = 256;
// Texture data uploaded per frame at most; the rest waits for later frames.
constexpr size_t kTextureUploadBudget = 96u << 20;
// Size of XenosRecomp's shared constants.
constexpr uint32_t kSharedConstantsSize = 288;
// Device-local memory for cached vertex data is allocated in chunks of at
// least this size.
constexpr VkDeviceSize kVertexChunkSize = 32ull << 20;
// Frames cached vertex data may go unused before it is freed.
constexpr uint64_t kVertexIdleFrames = 600;
// Frames a texture may go unused before it is freed.
constexpr uint64_t kTextureIdleFrames = 1800;

#define NV_INSTANCE_FUNCTIONS(X)                \
  X(vkEnumeratePhysicalDevices)                 \
  X(vkGetPhysicalDeviceProperties)              \
  X(vkGetPhysicalDeviceFeatures2)               \
  X(vkGetPhysicalDeviceMemoryProperties)        \
  X(vkGetPhysicalDeviceFormatProperties)        \
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
  X(vkCreateCommandPool)             \
  X(vkAllocateCommandBuffers)        \
  X(vkBeginCommandBuffer)            \
  X(vkEndCommandBuffer)              \
  X(vkResetCommandBuffer)            \
  X(vkCmdBeginRendering)             \
  X(vkCmdEndRendering)               \
  X(vkCmdBlitImage)                  \
  X(vkCmdCopyImageToBuffer)          \
  X(vkCmdClearDepthStencilImage)     \
  X(vkCmdPipelineBarrier)            \
  X(vkCmdClearColorImage)            \
  X(vkCmdClearAttachments)           \
  X(vkCmdCopyBufferToImage)          \
  X(vkCmdCopyBuffer)                 \
  X(vkCmdBindPipeline)               \
  X(vkCmdBindDescriptorSets)         \
  X(vkCmdBindVertexBuffers)          \
  X(vkCmdBindIndexBuffer)            \
  X(vkCmdPushConstants)              \
  X(vkCmdSetViewport)                \
  X(vkCmdSetScissor)                 \
  X(vkCmdDrawIndexed)                \
  X(vkCreateQueryPool)               \
  X(vkCmdResetQueryPool)             \
  X(vkCmdBeginQuery)                 \
  X(vkCmdEndQuery)                   \
  X(vkCmdWriteTimestamp)             \
  X(vkGetQueryPoolResults)           \
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

// NARUTO_NATIVE_VIEW_DEBUG bits: 1 disables depth testing, 2 face culling;
// 4 flips which winding is front-facing; 8 skips the game's clears and
// clears depth when a frame first uses it instead.
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
    frames_submitted_++;
    if (pending_) {
      Recycle(std::move(latest_.data));  // never drawn
    }
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
      std::lock_guard<std::mutex> lock(mutex_);
      Recycle(std::move(frame.data));
    }
  }

 public:
  std::vector<uint8_t> RecycledData() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (spare_data_.empty()) {
      return {};
    }
    std::vector<uint8_t> data = std::move(spare_data_.back());
    spare_data_.pop_back();
    data.clear();
    return data;
  }

 private:
  // Keeps a few frames' data buffers for reuse; mutex_ is held.
  void Recycle(std::vector<uint8_t>&& data) {
    if (spare_data_.size() < 3) {
      spare_data_.push_back(std::move(data));
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
    app.apiVersion = VK_API_VERSION_1_3;
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
  // the Vulkan 1.3 features the view needs.
  bool PickDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    int best_score = -1;
    for (VkPhysicalDevice device : devices) {
      VkPhysicalDeviceProperties properties;
      vkGetPhysicalDeviceProperties(device, &properties);
      VkPhysicalDeviceVulkan13Features features13{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
      VkPhysicalDeviceVulkan12Features features12{
          VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      features12.pNext = &features13;
      VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      features.pNext = &features12;
      if (properties.apiVersion < VK_API_VERSION_1_3) {
        continue;
      }
      vkGetPhysicalDeviceFeatures2(device, &features);
      if (!features.features.shaderInt64 || !features12.bufferDeviceAddress ||
          !features12.runtimeDescriptorArray || !features12.descriptorBindingPartiallyBound ||
          !features12.descriptorBindingSampledImageUpdateAfterBind ||
          !features12.descriptorBindingUpdateUnusedWhilePending ||
          !features13.dynamicRendering) {
        REXLOG_INFO("[native view] {} lacks features the shaders need", properties.deviceName);
        continue;
      }
      statistics_ = features.features.pipelineStatisticsQuery;
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
          timestamp_period_ =
              families[family].timestampValidBits ? properties.limits.timestampPeriod : 0.0f;
          mirror_clamp_ = features12.samplerMirrorClampToEdge;
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
    VkPhysicalDeviceVulkan13Features features13{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features features12{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.pNext = &features13;
    features12.bufferDeviceAddress = VK_TRUE;
    features12.runtimeDescriptorArray = VK_TRUE;
    features12.descriptorBindingPartiallyBound = VK_TRUE;
    features12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    features12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    features12.samplerMirrorClampToEdge = mirror_clamp_;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &features12;
    features.features.shaderInt64 = VK_TRUE;
    features.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    features.features.pipelineStatisticsQuery = statistics_;
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
                    VkImageAspectFlags aspect, VkComponentMapping components = {}) {
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
    view_info.components = components;
    view_info.subresourceRange = {aspect, 0, 1, 0, layers};
    vkCreateImageView(device_, &view_info, nullptr, &image.view);
    return image;
  }

  // A decoded texture; index 0 means the view couldn't decode it and draws
  // with the placeholder.
  struct CachedTexture {
    Image image;
    uint32_t heap = 0;  // 2D, 3D or cube descriptor array
    uint32_t index = 0;
    uint64_t fingerprint = 0;
    uint64_t checked_frame = UINT64_MAX;
    uint64_t last_used = 0;
  };

  // Freed once the frames in flight that might use it are done.
  struct Retired {
    Image image;
    uint32_t heap;
    uint32_t index;
    uint64_t frame;
  };

  struct PendingUpload {
    CachedTexture* texture;
    texture_decode::Texture decoded;
    VkDeviceSize offset;
  };

  void WriteTextureDescriptor(uint32_t heap, uint32_t index, VkImageView view,
                              VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    VkDescriptorImageInfo image_info{VK_NULL_HANDLE, view, layout};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_sets_[heap];
    write.dstArrayElement = index;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
  }

  // Sampler table index for a texture fetch constant: address modes X and Y,
  // and whether magnification and minification filter linearly.
  static uint32_t SamplerIndex(const uint32_t fetch[6]) {
    uint32_t mag = (fetch[3] >> 19) & 3, min = (fetch[3] >> 21) & 3;
    return ((fetch[0] >> 10) & 7) | (((fetch[0] >> 13) & 7) << 3) | (uint32_t(mag != 0) << 6) |
           (uint32_t(min != 0) << 7);
  }

  void Retire(CachedTexture& texture) {
    if (texture.index) {
      retired_.push_back({texture.image, texture.heap, texture.index, frames_total_});
    }
    texture = {};
  }

  void FreeRetired() {
    auto done = [&](const Retired& r) { return r.frame + kFramesInFlight <= frames_total_; };
    for (Retired& r : retired_) {
      if (done(r)) {
        DestroyImage(r.image);
        if (r.index) {
          free_indices_[r.heap].push_back(r.index);
        }
      }
    }
    std::erase_if(retired_, done);
    if (frames_total_ % 300 == 0) {
      std::erase_if(vertex_data_, [&](const auto& entry) {
        if (entry.second.last_used + kVertexIdleFrames >= frames_total_) {
          return false;
        }
        FreeVertexMemory(entry.second);
        return true;
      });
      for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.last_used + kTextureIdleFrames < frames_total_) {
          Retire(it->second);
          it = textures_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }

  // The cache entry for a bound texture, decoded into uploads if it is new or
  // its memory changed; nullptr if it has to wait for a later frame.
  const CachedTexture* Texture(const FrameInfo& frame, const uint32_t fetch[6],
                               std::vector<PendingUpload>& uploads, size_t& budget) {
    // The fields that decide the image: pitch and tiling, format, endian and
    // address, size, swizzle, dimension.
    auto resolved = resolved_.find(fetch[1] & 0x1FFFF000);
    if (resolved != resolved_.end()) {
      return &resolved->second.entry;
    }
    std::array<uint32_t, 5> key = {fetch[0] & 0xFFC00000, fetch[1], fetch[2],
                                   fetch[3] & 0x1FFE, fetch[5] & 0x600};
    auto it = textures_.find(key);
    if (it != textures_.end()) {
      CachedTexture& texture = it->second;
      texture.last_used = frames_total_;
      if (texture.checked_frame == frames_total_) {
        return &texture;
      }
      texture.checked_frame = frames_total_;
      uint64_t fingerprint = texture_decode::Fingerprint(frame.guest_base, fetch);
      if (fingerprint == texture.fingerprint) {
        return &texture;
      }
      Retire(texture);
      textures_.erase(it);
    }

    CachedTexture entry;
    entry.last_used = entry.checked_frame = frames_total_;
    entry.fingerprint = texture_decode::Fingerprint(frame.guest_base, fetch);
    texture_decode::Texture decoded;
    const char* reason = nullptr;
    if (!texture_decode::Describe(fetch, decoded, &reason)) {
      WarnOnce(reason, fetch[1] & 0x3F);
      textures_failed_++;
      return &textures_.emplace(key, entry).first->second;
    }
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical_device_, VkFormat(decoded.format), &properties);
    if (!(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
      WarnOnce("texture format not supported by the GPU", decoded.format);
      textures_failed_++;
      return &textures_.emplace(key, entry).first->second;
    }
    entry.heap = uint32_t(decoded.dimension);
    if (!budget || free_indices_[entry.heap].empty()) {
      return nullptr;
    }
    texture_decode::Decode(frame.guest_base, fetch, decoded);
    if (const char* dir = std::getenv("NARUTO_NATIVE_VIEW_TEXTURES")) {
      // A PPM where the format allows; otherwise the decoded data as is,
      // named with its size, layers and VkFormat.
      std::string name = fmt::format("{}/{:08x}_{:08x}", dir, fetch[1], fetch[2]);
      if (!texture_decode::WritePreview(decoded, (name + ".ppm").c_str())) {
        std::string path = fmt::format("{}_{}x{}x{}_f{}.raw", name, decoded.width,
                                       decoded.height, decoded.layers * decoded.depth,
                                       decoded.format);
        if (FILE* file = std::fopen(path.c_str(), "wb")) {
          std::fwrite(decoded.data.data(), 1, decoded.data.size(), file);
          std::fclose(file);
        }
      }
    }
    budget = decoded.data.size() >= budget ? 0 : budget - decoded.data.size();

    static constexpr VkImageViewType kViewTypes[3] = {
        VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE};
    VkComponentMapping components{
        VkComponentSwizzle(decoded.swizzle[0]), VkComponentSwizzle(decoded.swizzle[1]),
        VkComponentSwizzle(decoded.swizzle[2]), VkComponentSwizzle(decoded.swizzle[3])};
    entry.image = CreateImage(
        decoded.dimension == texture_decode::Dimension::k3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        kViewTypes[entry.heap], VkFormat(decoded.format),
        {decoded.width, decoded.height, decoded.depth}, decoded.layers,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
        components);
    entry.index = free_indices_[entry.heap].back();
    free_indices_[entry.heap].pop_back();
    WriteTextureDescriptor(entry.heap, entry.index, entry.image.view);
    CachedTexture* cached = &textures_.emplace(key, entry).first->second;
    uploads.push_back({cached, std::move(decoded), 0});
    return cached;
  }

  void WarnOnce(const char* reason, uint32_t value) {
    if (warned_.insert({reason, value}).second) {
      REXLOG_WARN("[native view] texture not drawn: {} ({})", reason, value);
    }
  }

  void DestroyImage(Image& image) {
    if (!image.image) {
      return;
    }
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

    // Samplers for every address mode pair and filter (SamplerIndex).
    static constexpr VkSamplerAddressMode kModes[8] = {
        VK_SAMPLER_ADDRESS_MODE_REPEAT,          VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,   VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
      auto mode = [&](uint32_t xenos) {
        VkSamplerAddressMode m = kModes[xenos];
        return m == VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE && !mirror_clamp_
                   ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE
                   : m;
      };
      VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      sampler_info.addressModeU = mode(i & 7);
      sampler_info.addressModeV = mode((i >> 3) & 7);
      sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      sampler_info.magFilter = (i >> 6) & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      sampler_info.minFilter = (i >> 7) & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
      sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
      sampler_info.maxLod = VK_LOD_CLAMP_NONE;
      vkCreateSampler(device_, &sampler_info, nullptr, &samplers_[i]);
    }

    // Sets 0-2: 2D, 3D and cube textures; set 3: samplers (shader_common.h).
    // Texture entries are written while other frames are in flight.
    VkDescriptorBindingFlags texture_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                             VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                             VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorBindingFlags sampler_flags = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorPoolSize pool_sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kHeapSizes[0] + kHeapSizes[1] + kHeapSizes[2]},
        {VK_DESCRIPTOR_TYPE_SAMPLER, kSamplerCount}};
    VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pool_info.maxSets = 4;
    pool_info.poolSizeCount = 2;
    pool_info.pPoolSizes = pool_sizes;
    vkCreateDescriptorPool(device_, &pool_info, nullptr, &descriptor_pool_);
    for (uint32_t set = 0; set < 4; ++set) {
      bool textures = set < 3;
      VkDescriptorSetLayoutBindingFlagsCreateInfo flags_info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
      flags_info.bindingCount = 1;
      flags_info.pBindingFlags = textures ? &texture_flags : &sampler_flags;
      VkDescriptorSetLayoutBinding binding{
          0, textures ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
          textures ? kHeapSizes[set] : kSamplerCount, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
      VkDescriptorSetLayoutCreateInfo layout_info{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      layout_info.pNext = &flags_info;
      layout_info.flags =
          textures ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT : 0;
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
    for (uint32_t heap = 0; heap < 3; ++heap) {
      WriteTextureDescriptor(heap, 0, placeholders_[heap].view);
      for (uint32_t index = kHeapSizes[heap] - 1; index > 0; --index) {
        free_indices_[heap].push_back(index);
      }
    }
    std::vector<VkDescriptorImageInfo> sampler_infos(kSamplerCount);
    for (uint32_t i = 0; i < kSamplerCount; ++i) {
      sampler_infos[i].sampler = samplers_[i];
    }
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = descriptor_sets_[3];
    write.descriptorCount = kSamplerCount;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = sampler_infos.data();
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

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

  void PickSurfaceFormat() {
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
  }

  // The swapchain is only blitted to.
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
    if (!format_.format) {
      PickSurfaceFormat();
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
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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

    uint32_t image_count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, nullptr);
    std::vector<VkImage> images(image_count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &image_count, images.data());
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (VkImage image : images) {
      Target target;
      target.image = image;
      // One per image: reacquiring the image is what makes reuse safe.
      vkCreateSemaphore(device_, &semaphore_info, nullptr, &target.render_done);
      targets_.push_back(target);
    }
    return true;
  }

  void DestroySwapchainResources() {
    for (Target& target : targets_) {
      vkDestroySemaphore(device_, target.render_done, nullptr);
    }
    targets_.clear();
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

  // Cached vertex data on the GPU, in a chunk of device-local memory.
  struct ResidentVertices {
    uint32_t chunk;
    VkDeviceSize offset, size;
    uint64_t last_used;  // frame
  };
  struct VertexChunk {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
  };

  // Finds room for size bytes of vertex data, first fit, adding a chunk if
  // none has room. Returns false if memory runs out.
  bool AllocateVertexMemory(VkDeviceSize size, ResidentVertices& resident) {
    size = (size + 255) & ~VkDeviceSize(255);
    for (uint32_t c = 0; c < vertex_chunks_.size(); ++c) {
      auto& free = vertex_chunks_[c].free;
      for (auto it = free.begin(); it != free.end(); ++it) {
        if (it->second >= size) {
          resident = {c, it->first, size, 0};
          if (it->second > size) {
            free[it->first + size] = it->second - size;
          }
          free.erase(it);
          return true;
        }
      }
    }
    VertexChunk chunk;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max(size, kVertexChunkSize);
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(device_, &info, nullptr, &chunk.buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device_, chunk.buffer, &requirements);
    chunk.memory = Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    if (!chunk.memory) {
      vkDestroyBuffer(device_, chunk.buffer, nullptr);
      return false;
    }
    vkBindBufferMemory(device_, chunk.buffer, chunk.memory, 0);
    if (info.size > size) {
      chunk.free[size] = info.size - size;
    }
    resident = {uint32_t(vertex_chunks_.size()), 0, size, 0};
    vertex_chunks_.push_back(std::move(chunk));
    return true;
  }

  // Returns vertex data's memory to its chunk, merging it with free
  // neighbors. No frame in flight may still use it.
  void FreeVertexMemory(const ResidentVertices& resident) {
    auto& free = vertex_chunks_[resident.chunk].free;
    auto [it, added] = free.emplace(resident.offset, resident.size);
    auto next = std::next(it);
    if (next != free.end() && it->first + it->second == next->first) {
      it->second += next->second;
      free.erase(next);
    }
    if (it != free.begin()) {
      auto previous = std::prev(it);
      if (previous->first + previous->second == it->first) {
        previous->second += it->second;
        free.erase(it);
      }
    }
  }

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
                 VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
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

  // A Vulkan image standing in for an EDRAM render target or depth buffer.
  struct RenderTarget {
    Image image;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool depth = false;
  };

  // The attachments of a pass: render targets 0-3 and the depth buffer.
  struct PassTargets {
    std::array<RenderTarget*, 4> color{};
    RenderTarget* depth = nullptr;
    uint32_t color_count = 0;  // highest used render target + 1
    VkExtent2D extent{};
  };

  // A texture filled by resolves rather than from guest memory.
  struct ResolvedTexture {
    CachedTexture entry;  // its image and 2D descriptor
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    bool depth = false;
  };

  // Render targets are identified by EDRAM base, format and pitch.
  static uint64_t TargetKey(uint32_t surface_info, uint32_t info, bool depth) {
    uint64_t pitch = surface_info & 0x3FFF;
    uint64_t format = depth ? (info >> 16) & 1 : (info >> 16) & 0xF;
    return (uint64_t(depth) << 63) | (pitch << 32) | (format << 16) | (info & 0xFFF);
  }

  // RB_COLOR_INFO color_format -> a Vulkan format holding the same values.
  static VkFormat ColorFormat(uint32_t info) {
    switch ((info >> 16) & 0xF) {
      case 2:   // 2_10_10_10
      case 10:  // 2_10_10_10_AS_10_10_10_10
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      case 3:   // 2_10_10_10_FLOAT
      case 5:   // 16_16_16_16
      case 7:   // 16_16_16_16_FLOAT
      case 12:  // 2_10_10_10_FLOAT_AS_16_16_16_16
        return VK_FORMAT_R16G16B16A16_SFLOAT;
      case 4:  // 16_16
      case 6:  // 16_16_FLOAT
        return VK_FORMAT_R16G16_SFLOAT;
      case 14:
        return VK_FORMAT_R32_SFLOAT;
      case 15:
        return VK_FORMAT_R32G32_SFLOAT;
      default:  // 8_8_8_8, 8_8_8_8_GAMMA
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
  }

  void InitializeImage(VkImage image, bool depth, uint32_t layers = 1) {
    new_images_.push_back({image, depth, layers});
  }

  // The render target for key, at least `height` rows; recreated (losing
  // its contents) if it has to grow.
  RenderTarget* RenderTargetFor(uint64_t key, uint32_t info, uint32_t height) {
    RenderTarget& target = render_targets_[key];
    uint32_t width = std::max<uint32_t>(uint32_t(key >> 32) & 0x3FFF, 1);
    if (target.image.image && target.height >= height && target.width == width) {
      return &target;
    }
    if (target.image.image) {
      retired_.push_back({target.image, 0, 0, frames_total_});
    }
    target.depth = key >> 63;
    target.format = target.depth ? kDepthFormat : ColorFormat(info);
    target.width = width;
    target.height = std::max(height, target.height);
    target.image = CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, target.format,
                               {target.width, target.height, 1}, 1,
                               target.depth ? kDepthTargetUsage : kColorTargetUsage,
                               target.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                            : VK_IMAGE_ASPECT_COLOR_BIT);
    InitializeImage(target.image.image, target.depth);
    return &target;
  }

  // The image a resolve writes: the destination texture's base level,
  // registered under its address. Returns nullptr for destinations the view
  // can't hold.
  ResolvedTexture* ResolveDestination(const Resolve& resolve) {
    if (resolve.level || ((resolve.fetch[5] >> 9) & 3) == 2 || ((resolve.fetch[5] >> 9) & 3) == 3) {
      WarnOnce("resolve to a mip level, 3D or cube texture", resolve.fetch[1] & 0x3F);
      return nullptr;
    }
    bool depth = (resolve.flags & 7) == 4;
    texture_decode::Texture described;
    const char* reason = nullptr;
    VkFormat format;
    VkComponentMapping components{};
    uint32_t width = (resolve.fetch[2] & 0x1FFF) + 1, height = ((resolve.fetch[2] >> 13) & 0x1FFF) + 1;
    if (depth) {
      format = VK_FORMAT_R32_SFLOAT;
      components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                    VK_COMPONENT_SWIZZLE_ONE};
    } else if (texture_decode::Describe(resolve.fetch, described, &reason)) {
      format = VkFormat(described.format);
      // A copy into an 8-bit-per-channel texture stores the render target's
      // red in Z and blue in X (the ARGB layout the game reads with a ZYXW
      // swizzle); the image keeps red in R, so components are remapped.
      uint32_t xenos_format = resolve.fetch[1] & 0x3F;
      bool swap_red_blue = xenos_format == 6 || xenos_format == 50;
      static constexpr uint32_t kChannel[2][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}};
      VkComponentSwizzle swizzles[4];
      for (int c = 0; c < 4; ++c) {
        uint32_t select = (resolve.fetch[3] >> (1 + 3 * c)) & 7;
        swizzles[c] = select < 4 ? VkComponentSwizzle(VK_COMPONENT_SWIZZLE_R +
                                                      kChannel[swap_red_blue][select])
                      : select == 5 ? VK_COMPONENT_SWIZZLE_ONE
                                    : VK_COMPONENT_SWIZZLE_ZERO;
      }
      components = {swizzles[0], swizzles[1], swizzles[2], swizzles[3]};
    } else {
      WarnOnce("resolve destination format", resolve.fetch[1] & 0x3F);
      return nullptr;
    }
    VkFormatProperties properties;
    vkGetPhysicalDeviceFormatProperties(physical_device_, format, &properties);
    VkFormatFeatureFlags needed = depth ? VK_FORMAT_FEATURE_TRANSFER_DST_BIT
                                        : VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((properties.optimalTilingFeatures & needed) != needed) {
      WarnOnce("resolve destination format not writable", format);
      return nullptr;
    }
    uint32_t address = resolve.fetch[1] & 0x1FFFF000;
    ResolvedTexture& texture = resolved_[address];
    if (texture.entry.image.image && texture.format == format && texture.width == width &&
        texture.height == height) {
      return &texture;
    }
    if (texture.entry.image.image) {
      retired_.push_back({texture.entry.image, 0, texture.entry.index, frames_total_});
    } else {
      // Any texture decoded from this address is replaced.
      for (auto it = textures_.begin(); it != textures_.end();) {
        if (((it->first[1] & 0x1FFFF000) == address)) {
          Retire(it->second);
          it = textures_.erase(it);
        } else {
          ++it;
        }
      }
    }
    if (free_indices_[0].empty()) {
      resolved_.erase(address);
      return nullptr;
    }
    texture.format = format;
    texture.width = width;
    texture.height = height;
    texture.depth = depth;
    texture.entry = {};
    texture.entry.image =
        CreateImage(VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, format, {width, height, 1}, 1,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_ASPECT_COLOR_BIT, components);
    texture.entry.index = free_indices_[0].back();
    free_indices_[0].pop_back();
    texture.entry.last_used = frames_total_;
    WriteTextureDescriptor(0, texture.entry.index, texture.entry.image.view,
                           VK_IMAGE_LAYOUT_GENERAL);
    InitializeImage(texture.entry.image.image, false);
    return &texture;
  }

  // The area a clear covers: the viewport set when the game cleared (the
  // GPU offsets the clear like the draws, so small blur and bloom passes
  // clear only their part of a shared render target).
  static VkRect2D ClearArea(const Clear& clear) {
    float x = clear.viewport[1] - clear.viewport[0];
    float y = clear.viewport[3] + clear.viewport[2];
    float width = 2.0f * clear.viewport[0], height = -2.0f * clear.viewport[2];
    if (height < 0.0f) {
      y += height;
      height = -height;
    }
    return {{int32_t(std::max(0.0f, std::round(x))), int32_t(std::max(0.0f, std::round(y)))},
            {uint32_t(std::max(0.0f, std::round(width))),
             uint32_t(std::max(0.0f, std::round(height)))}};
  }

  static VkRect2D Clamp(VkRect2D area, VkExtent2D extent) {
    uint32_t x = std::min<uint32_t>(area.offset.x, extent.width);
    uint32_t y = std::min<uint32_t>(area.offset.y, extent.height);
    return {{int32_t(x), int32_t(y)},
            {std::min(area.extent.width, extent.width - x),
             std::min(area.extent.height, extent.height - y)}};
  }

  // Clears the clear's targets with vkCmdClearAttachments if all of them are
  // attachments of the current pass; false if some aren't.
  bool ClearAttachments(VkCommandBuffer cmd, const Clear& clear, VkRect2D area,
                        const PassTargets& pt) {
    std::array<VkClearAttachment, 5> attachments{};
    uint32_t count = 0;
    for (int i = 0; i < 4; ++i) {
      if (!(clear.flags & (1u << i))) {
        continue;
      }
      auto target = render_targets_.find(TargetKey(clear.surface_info, clear.color_info[i], false));
      bool found = false;
      for (uint32_t a = 0; a < pt.color_count && !found; ++a) {
        if (target != render_targets_.end() && pt.color[a] == &target->second) {
          VkClearAttachment& attachment = attachments[count++];
          attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
          attachment.colorAttachment = a;
          std::memcpy(attachment.clearValue.color.float32, clear.color, sizeof(clear.color));
          found = true;
        }
      }
      if (!found) {
        return false;
      }
    }
    if (clear.flags & 0x10) {
      auto target = render_targets_.find(TargetKey(clear.surface_info, clear.depth_info, true));
      if (target == render_targets_.end() || pt.depth != &target->second) {
        return false;
      }
      VkClearAttachment& attachment = attachments[count++];
      attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      attachment.clearValue.depthStencil = {clear.depth, clear.stencil};
    }
    VkClearRect rect{Clamp(area, pt.extent), 0, 1};
    if (count && rect.rect.extent.width && rect.rect.extent.height) {
      vkCmdClearAttachments(cmd, count, attachments.data(), 1, &rect);
    }
    return true;
  }

  // Clears the clear's targets outside the pass's rendering: the ones for the
  // bound surface (EDRAM base, format and pitch), each in a rendering of its
  // own. Other targets sharing the EDRAM base are different surfaces whose
  // contents the game still uses.
  void RecordClear(VkCommandBuffer cmd, const Clear& clear, VkRect2D area) {
    auto clear_target = [&](const RenderTarget& target, const VkClearAttachment& attachment) {
      VkRect2D rect = Clamp(area, {target.width, target.height});
      if (!target.image.image || !rect.extent.width || !rect.extent.height) {
        return;
      }
      VkRenderingAttachmentInfo info{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      info.imageView = target.image.view;
      info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      info.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
      info.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      rendering.renderArea = rect;
      rendering.layerCount = 1;
      if (target.depth) {
        rendering.pDepthAttachment = &info;
      } else {
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &info;
      }
      vkCmdBeginRendering(cmd, &rendering);
      VkClearRect clear_rect{rect, 0, 1};
      vkCmdClearAttachments(cmd, 1, &attachment, 1, &clear_rect);
      vkCmdEndRendering(cmd);
    };
    for (int i = 0; i < 4; ++i) {
      if (!(clear.flags & (1u << i))) {
        continue;
      }
      auto target = render_targets_.find(TargetKey(clear.surface_info, clear.color_info[i], false));
      if (target != render_targets_.end()) {
        VkClearAttachment attachment{VK_IMAGE_ASPECT_COLOR_BIT, 0};
        std::memcpy(attachment.clearValue.color.float32, clear.color, sizeof(clear.color));
        clear_target(target->second, attachment);
      }
    }
    if (clear.flags & 0x10) {
      auto target = render_targets_.find(TargetKey(clear.surface_info, clear.depth_info, true));
      if (target != render_targets_.end()) {
        VkClearAttachment attachment{VK_IMAGE_ASPECT_DEPTH_BIT, 0};
        attachment.clearValue.depthStencil = {clear.depth, clear.stencil};
        clear_target(target->second, attachment);
      }
    }
  }

  // The pipeline for the draw's shaders, vertex layout, render state and the
  // pass's attachment formats, or VK_NULL_HANDLE if it can't be created.
  VkPipeline Pipeline(const FrameInfo& frame, const Draw& draw, const PassTargets& targets) {
    uint32_t depth_state = targets.depth ? draw.depth_control & 0x76 : 0;  // test, write, function
    uint32_t color_mask = draw.color_mask & 0xFFFF;
    uint32_t cull_state = draw.mode_control & 7;  // cull front, cull back, clockwise front
    std::array<VkFormat, 4> color_formats{};
    for (uint32_t i = 0; i < targets.color_count; ++i) {
      color_formats[i] = targets.color[i] ? targets.color[i]->format : VK_FORMAT_UNDEFINED;
    }
    VkFormat depth_format = targets.depth ? targets.depth->format : VK_FORMAT_UNDEFINED;
    std::vector<uint32_t> key = {uint32_t(draw.vertex_shader), uint32_t(draw.pixel_shader),
                                 draw.topology, draw.primitive_restart, draw.alpha_test,
                                 depth_state, color_mask, cull_state, targets.color_count,
                                 uint32_t(depth_format)};
    for (uint32_t i = 0; i < targets.color_count; ++i) {
      key.insert(key.end(), {uint32_t(color_formats[i]), draw.blend_control[i]});
    }
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
    // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 back faces; bit 2
    // makes clockwise triangles front-facing.
    raster.cullMode = ((cull_state & 1) ? VK_CULL_MODE_FRONT_BIT : 0) |
                      ((cull_state & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    if (DebugFlags() & 2) {
      raster.cullMode = VK_CULL_MODE_NONE;
    }
    bool clockwise = (cull_state >> 2) & 1;
    if (DebugFlags() & 4) {
      clockwise = !clockwise;
    }
    raster.frontFace = clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
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
    // color in bits 0-12, for alpha in bits 16-28. RB_COLOR_MASK has four
    // bits per render target.
    std::array<VkPipelineColorBlendAttachmentState, 4> blend_attachments{};
    for (uint32_t i = 0; i < targets.color_count; ++i) {
      uint32_t blend = draw.blend_control[i];
      VkPipelineColorBlendAttachmentState& attachment = blend_attachments[i];
      attachment.srcColorBlendFactor = BlendFactor(blend & 0x1F);
      attachment.colorBlendOp = BlendOp((blend >> 5) & 7);
      attachment.dstColorBlendFactor = BlendFactor((blend >> 8) & 0x1F);
      attachment.srcAlphaBlendFactor = BlendFactor((blend >> 16) & 0x1F);
      attachment.alphaBlendOp = BlendOp((blend >> 21) & 7);
      attachment.dstAlphaBlendFactor = BlendFactor((blend >> 24) & 0x1F);
      attachment.blendEnable = (blend & 0x1FFF1FFF) != 0x00010001;
      attachment.colorWriteMask = targets.color[i] ? (color_mask >> (4 * i)) & 0xF : 0;
    }
    VkPipelineColorBlendStateCreateInfo color_blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    color_blend.attachmentCount = targets.color_count;
    color_blend.pAttachments = blend_attachments.data();
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = targets.color_count;
    rendering.pColorAttachmentFormats = color_formats.data();
    rendering.depthAttachmentFormat = depth_format;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
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

  static VkViewport Viewport(const Draw& draw) {
    XenosViewport v = ViewportOf(draw);
    VkViewport viewport;
    viewport.x = v.x_offset - v.x_scale;
    viewport.width = 2.0f * v.x_scale;
    viewport.y = v.y_offset + v.y_scale;
    viewport.height = -2.0f * v.y_scale;
    viewport.minDepth = std::clamp(v.z_offset, 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(v.z_offset + v.z_scale, 0.0f, 1.0f);
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
    bool f12 = keys[SDL_SCANCODE_F12];
    if (f12 && !key_f12_) {
      snapshot_requested_ = true;
    }
    key_left_ = left;
    key_right_ = right;
    key_f12_ = f12;
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
  // for checking capture against what the game set; pass UINT32_MAX logs
  // every pass's draws.
  // A binding's vertices on the CPU.
  static const uint8_t* BindingData(const FrameInfo& frame, const VertexBinding& binding) {
    const uint8_t* data = binding.cached >= 0 ? frame.vertex_data[binding.cached]->bytes.data()
                                              : frame.data.data();
    return data + binding.data_offset;
  }

  void Dump(const FrameInfo& frame, uint32_t pass, bool forced) {
    static const long dump_frame = [] {
      const char* value = std::getenv("NARUTO_NATIVE_VIEW_DUMP");
      return value ? std::atol(value) : -1;
    }();
    if (!forced && long(frames_total_) != dump_frame) {
      return;
    }
    REXLOG_INFO("[native view] dump of frame {}", frames_total_);
    auto f32 = [&](uint32_t offset) {
      float v;
      std::memcpy(&v, frame.data.data() + offset, 4);
      return v;
    };
    std::vector<std::map<std::string, uint32_t>> summaries(frame.passes.size());
    for (const Draw& d : frame.captured) {
      std::string targets;
      for (int i = 0; i < 4; ++i) {
        if ((d.color_mask >> (4 * i)) & 0xF) {
          targets += fmt::format(" rt{}={:x}", i, d.color_info[i]);
        }
      }
      if (d.depth_control & 6) {
        targets += fmt::format(" depth={:x}/{:x}", d.depth_info, d.depth_control & 0x76);
      }
      targets += fmt::format(" surface={:x} vp=[{} {} {} {}]", d.surface_info, d.viewport[0],
                             d.viewport[1], d.viewport[2], d.viewport[3]);
      summaries[std::min<size_t>(d.pass, summaries.size() - 1)][targets]++;
    }
    for (size_t p = 0; p < summaries.size(); ++p) {
      for (const auto& [targets, count] : summaries[p]) {
        REXLOG_INFO("[native view] pass {}: {} draws{}", p, count, targets);
      }
    }
    for (const Resolve& r : frame.resolves) {
      REXLOG_INFO(
          "[native view] resolve after pass {}: flags {:#x} surface {:#x} source {:#x} rect {} "
          "[{} {} {} {}] point [{} {}] level {} slice {} to fetch {:08x} {:08x} {:08x} {:08x}",
          r.pass, r.flags, r.surface_info, r.source_info, r.has_rect, r.rect[0], r.rect[1],
          r.rect[2], r.rect[3], r.point[0], r.point[1], r.level, r.slice, r.fetch[0], r.fetch[1],
          r.fetch[2], r.fetch[3]);
    }
    for (const Clear& c : frame.clears) {
      REXLOG_INFO("[native view] clear in pass {} before draw {}: flags {:#x} color {:x} {:x} "
                  "depth {:x} values ({} {} {} {}) z {} viewport [{} {} {} {}]",
                  c.pass, c.before_draw, c.flags, c.color_info[0], c.color_info[1], c.depth_info,
                  c.color[0], c.color[1], c.color[2], c.color[3], c.depth, c.viewport[0],
                  c.viewport[1], c.viewport[2], c.viewport[3]);
    }
    REXLOG_INFO("[native view] front buffer: {} {:08x} {:08x} {:08x}", frame.has_front_buffer,
                frame.front_buffer[0], frame.front_buffer[1], frame.front_buffer[2]);
    if (pass != UINT32_MAX) {
      const PassInfo& info = frame.passes[pass];
      REXLOG_INFO("[native view] pass {}: clear color {} ({} {} {} {}), clear depth {}", pass,
                  info.has_clear_color, info.clear_color[0], info.clear_color[1],
                  info.clear_color[2], info.clear_color[3], info.clear_depth);
    }
    uint32_t n = 0;
    for (const Draw& d : frame.captured) {
      if ((pass != UINT32_MAX && d.pass != pass) || n++ >= 1000) {
        continue;
      }
      const auto& shaders = shader_library::Shaders();
      VkViewport vp = Viewport(d);
      REXLOG_INFO(
          "[native view] draw in pass {}: vs {}@{:#x} ps {} topology {} indices {} surface {:#x} vte {:#x} "
          "depth {:#x} blend {:#x} mask {:#x} mode {:#x} alpha test {} vertices {}+{} (missing {} "
          "bytes) vport [{} {} {} {} {} {}] -> {} {} {} {} {} {}",
          d.pass, shaders[d.vertex_shader].library, shaders[d.vertex_shader].offset,
          d.pixel_shader >= 0 ? shaders[d.pixel_shader].offset : 0, d.topology, d.index_count,
          d.surface_info, d.vte_control, d.depth_control, d.blend_control[0], d.color_mask,
          d.mode_control, d.alpha_test, d.first_vertex, d.vertex_count, d.missing_bytes,
          d.viewport[0], d.viewport[1], d.viewport[2], d.viewport[3], d.viewport[4],
          d.viewport[5], vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
      for (uint32_t i = 0; i < d.attribute_count; ++i) {
        const VertexAttribute& a = frame.attributes[d.first_attribute + i];
        const VertexBinding& b = frame.bindings[d.first_binding + a.binding];
        const auto& element = shaders[d.vertex_shader].vertex_elements[a.location];
        std::string values;
        const uint8_t* vertices = BindingData(frame, b);
        auto vertex_f32 = [&](uint32_t at) {
          float value;
          std::memcpy(&value, vertices + at, 4);
          return value;
        };
        for (uint32_t v = 0; v < 2; ++v) {
          uint32_t at = v * b.stride + a.offset;
          values += fmt::format(" ({} {} {} | {:08x})", vertex_f32(at), vertex_f32(at + 4),
                                vertex_f32(at + 8),
                                *reinterpret_cast<const uint32_t*>(vertices + at));
        }
        auto swizzle = [](uint32_t s) {
          std::string text;
          for (int c = 0; c < 4; ++c) {
            text += "xyzw01?_"[(s >> (3 * c)) & 7];
          }
          return text;
        };
        REXLOG_INFO("[native view]   input {} usage {}.{} binding {} stride {} offset {} format {} "
                    "(xenos {}, swizzle {} library {}):{}",
                    a.location, element.usage, element.usage_index, a.binding, b.stride, a.offset,
                    a.format, a.xenos_format, swizzle(a.swizzle), swizzle(a.library_swizzle),
                    values);
      }
      for (uint32_t b = 0; b < d.binding_count; ++b) {
        const VertexBinding& binding = frame.bindings[d.first_binding + b];
        for (uint32_t v = 0; v < 2; ++v) {
          std::string words;
          for (uint32_t w = 0; w < binding.stride / 4 && w < 16; ++w) {
            words += fmt::format(" {:08x}", *reinterpret_cast<const uint32_t*>(
                                                 BindingData(frame, binding) +
                                                 v * binding.stride + w * 4));
          }
          REXLOG_INFO("[native view]   binding {} vertex {}:{}", b, v, words);
        }
      }
      for (uint32_t t = 0; t < d.texture_count; ++t) {
        const TextureBinding& b = frame.textures[d.first_texture + t];
        REXLOG_INFO("[native view]   sampler {}: fetch {:08x} {:08x} {:08x} {:08x} {:08x} {:08x}",
                    b.sampler, b.fetch[0], b.fetch[1], b.fetch[2], b.fetch[3], b.fetch[4],
                    b.fetch[5]);
      }
      std::string constants;
      for (uint32_t c = 0; c < 8 * 4; ++c) {
        constants += fmt::format(" {:.3g}", f32(d.vertex_constants + c * 4));
      }
      REXLOG_INFO("[native view]   c0-c7:{}", constants);
      // NARUTO_NATIVE_VIEW_DUMP_VS=offset logs all vertex shader constants of
      // the draws using that library vertex shader.
      if (DumpedVertexShader(d)) {
        for (uint32_t c = 0; c < 256; c += 4) {
          std::string row;
          for (uint32_t i = c * 4; i < (c + 4) * 4; ++i) {
            row += fmt::format(" {:.6g}", f32(d.vertex_constants + i * 4));
          }
          REXLOG_INFO("[native view]   c{}-c{}:{}", c, c + 3, row);
        }
      }
    }
  }

  // NARUTO_NATIVE_VIEW_DUMP_VS=offset: the library vertex shader whose draws
  // the dump details.
  static bool DumpedVertexShader(const Draw& draw) {
    static const long dump_vs = [] {
      const char* value = std::getenv("NARUTO_NATIVE_VIEW_DUMP_VS");
      return value ? std::strtol(value, nullptr, 0) : -1;
    }();
    return long(shader_library::Shaders()[draw.vertex_shader].offset) == dump_vs;
  }

  static void Barrier(PFN_vkCmdPipelineBarrier barrier, VkCommandBuffer cmd) {
    VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    memory.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    memory.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
            &memory, 0, nullptr, 0, nullptr);
  }

  // Where each pass draws: the render targets its draws write and the depth
  // buffer they test, sized to what the frame's viewports and resolves reach.
  std::vector<PassTargets> PlanTargets(const FrameInfo& frame) {
    uint32_t pass_count = uint32_t(frame.passes.size());
    std::map<uint64_t, uint32_t> heights;
    std::map<uint64_t, uint32_t> infos;
    auto need = [&](uint64_t key, uint32_t info, float bottom) {
      uint32_t height = uint32_t(std::clamp(std::ceil(bottom), 1.0f, 8192.0f));
      heights[key] = std::max(heights[key], height);
      infos[key] = info;
    };
    for (const Draw& draw : frame.captured) {
      XenosViewport v = ViewportOf(draw);
      float bottom = v.y_offset + std::abs(v.y_scale);
      for (uint32_t i = 0; i < 4; ++i) {
        if ((draw.color_mask >> (4 * i)) & 0xF) {
          need(TargetKey(draw.surface_info, draw.color_info[i], false), draw.color_info[i], bottom);
        }
      }
      if (draw.depth_control & 6) {
        need(TargetKey(draw.surface_info, draw.depth_info, true), draw.depth_info, bottom);
      }
    }
    for (const Resolve& r : frame.resolves) {
      bool depth = (r.flags & 7) == 4;
      float bottom = r.has_rect ? float(r.rect[3])
                                : float(((r.fetch[2] >> 13) & 0x1FFF) + 1 + r.point[1]);
      need(TargetKey(r.surface_info, r.source_info, depth), r.source_info, bottom);
    }
    for (auto& [key, height] : heights) {
      RenderTargetFor(key, infos[key], height);
    }

    std::vector<PassTargets> passes(pass_count);
    for (const Draw& draw : frame.captured) {
      PassTargets& targets = passes[std::min(draw.pass, pass_count - 1)];
      for (uint32_t i = 0; i < 4; ++i) {
        if ((draw.color_mask >> (4 * i)) & 0xF) {
          targets.color[i] = &render_targets_[TargetKey(draw.surface_info, draw.color_info[i], false)];
          targets.color_count = std::max(targets.color_count, i + 1);
        }
      }
      if (draw.depth_control & 6) {
        targets.depth = &render_targets_[TargetKey(draw.surface_info, draw.depth_info, true)];
      }
    }
    for (PassTargets& targets : passes) {
      targets.extent = {UINT32_MAX, UINT32_MAX};
      for (RenderTarget* target : targets.color) {
        if (target) {
          targets.extent.width = std::min(targets.extent.width, target->width);
          targets.extent.height = std::min(targets.extent.height, target->height);
        }
      }
      if (targets.depth) {
        targets.extent.width = std::min(targets.extent.width, targets.depth->width);
        targets.extent.height = std::min(targets.extent.height, targets.depth->height);
      }
    }
    return passes;
  }

  // Copies a resolve's source rectangle into its destination texture.
  void RecordResolve(VkCommandBuffer cmd, const Resolve& r, const RenderTarget& source,
                     const ResolvedTexture& dest) {
    int32_t x1 = 0, y1 = 0, x2 = int32_t(dest.width), y2 = int32_t(dest.height);
    if (r.has_rect) {
      x1 = r.rect[0], y1 = r.rect[1], x2 = r.rect[2], y2 = r.rect[3];
    }
    int32_t dx = r.point[0], dy = r.point[1];
    x2 = std::min({x2, int32_t(source.width), x1 + int32_t(dest.width) - dx});
    y2 = std::min({y2, int32_t(source.height), y1 + int32_t(dest.height) - dy});
    if (x1 < 0 || y1 < 0 || dx < 0 || dy < 0 || x2 <= x1 || y2 <= y1) {
      return;
    }
    uint32_t w = uint32_t(x2 - x1), h = uint32_t(y2 - y1);
    if (!source.depth) {
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[0] = {x1, y1, 0};
      blit.srcOffsets[1] = {x2, y2, 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[0] = {dx, dy, 0};
      blit.dstOffsets[1] = {dx + int32_t(w), dy + int32_t(h), 1};
      vkCmdBlitImage(cmd, source.image.image, VK_IMAGE_LAYOUT_GENERAL, dest.entry.image.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_NEAREST);
      return;
    }
    // Depth goes through a buffer: depth and color images can't be copied
    // directly.
    if (!ReserveScratch(VkDeviceSize(w) * h * 4)) {
      return;
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copy.imageOffset = {x1, y1, 0};
    copy.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, source.image.image, VK_IMAGE_LAYOUT_GENERAL, scratch_.buffer, 1,
                           &copy);
    Barrier(vkCmdPipelineBarrier, cmd);
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageOffset = {dx, dy, 0};
    vkCmdCopyBufferToImage(cmd, scratch_.buffer, dest.entry.image.image, VK_IMAGE_LAYOUT_GENERAL,
                           1, &copy);
  }

  // GPU timestamps at the start of each pass, after the last pass and after
  // presenting, for the timing log. Returns how many the frame records (0
  // when off).
  uint32_t StartTimestamps(VkCommandBuffer cmd, uint32_t count) {
    timestamp_counts_[frame_index_] = 0;
    if (!timestamp_period_ || count > kTimestamps) {
      return 0;
    }
    if (!timestamp_pool_) {
      VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      info.queryType = VK_QUERY_TYPE_TIMESTAMP;
      info.queryCount = kTimestamps * kFramesInFlight;
      if (vkCreateQueryPool(device_, &info, nullptr, &timestamp_pool_) != VK_SUCCESS) {
        timestamp_period_ = 0;
        return 0;
      }
    }
    vkCmdResetQueryPool(cmd, timestamp_pool_, frame_index_ * kTimestamps, count);
    timestamp_counts_[frame_index_] = count;
    return count;
  }

  void WriteTimestamp(VkCommandBuffer cmd, uint32_t count, uint32_t index) {
    if (index < count) {
      vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamp_pool_,
                          frame_index_ * kTimestamps + index);
    }
  }

  // Adds the timestamps of the frame last recorded in this slot, whose fence
  // has been waited on, to the per-pass GPU times.
  void ReadTimestamps() {
    uint32_t count = timestamp_counts_[frame_index_];
    if (count < 2) {
      return;
    }
    uint64_t values[kTimestamps];
    if (vkGetQueryPoolResults(device_, timestamp_pool_, frame_index_ * kTimestamps, count,
                              sizeof(values), values, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
      return;
    }
    if (pass_gpu_ms_.size() < count - 1) {
      pass_gpu_ms_.resize(count - 1);
    }
    for (uint32_t i = 0; i + 1 < count; ++i) {
      pass_gpu_ms_[i] += (values[i + 1] - values[i]) * timestamp_period_ / 1e6;
    }
    gpu_ms_ += (values[count - 1] - values[0]) * timestamp_period_ / 1e6;
    gpu_frames_++;
  }

  bool ReserveScratch(VkDeviceSize size) {
    if (scratch_.size >= size) {
      return true;
    }
    if (scratch_.buffer) {
      vkDeviceWaitIdle(device_);
      vkDestroyBuffer(device_, scratch_.buffer, nullptr);
      vkFreeMemory(device_, scratch_.memory, nullptr);
      scratch_ = {};
    }
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = std::max<VkDeviceSize>(size, 16ull << 20);
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(device_, &info, nullptr, &scratch_.buffer);
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device_, scratch_.buffer, &requirements);
    scratch_.memory = Allocate(requirements, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false);
    if (!scratch_.memory) {
      vkDestroyBuffer(device_, scratch_.buffer, nullptr);
      scratch_ = {};
      return false;
    }
    vkBindBufferMemory(device_, scratch_.buffer, scratch_.memory, 0);
    scratch_.size = info.size;
    return true;
  }

  // NARUTO_NATIVE_VIEW_SNAPSHOTS=dir with NARUTO_NATIVE_VIEW_DUMP=n saves
  // each pass's first render target (after the pass and its resolves) and
  // the presented frame, downscaled, as PPM files in dir/<frame>/, for frame
  // n and every 300th frame after it (NARUTO_NATIVE_VIEW_SNAPSHOT_EVERY
  // changes the interval). F12 in the view (or creating out/snapshot-request)
  // saves the current frame the same way, to dir or out/snapshots. Each
  // snapshot also logs all of the frame's draws.
  static constexpr uint32_t kSnapshotWidth = 320, kSnapshotHeight = 180;

  bool PrepareSnapshots(uint32_t count) {
    VkDeviceSize size = VkDeviceSize(count) * kSnapshotWidth * kSnapshotHeight * 4;
    if (!snapshot_image_.image) {
      snapshot_image_ = CreateImage(
          VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
          {kSnapshotWidth, kSnapshotHeight, 1}, 1,
          VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
          VK_IMAGE_ASPECT_COLOR_BIT);
      InitializeImage(snapshot_image_.image, false);
    }
    if (snapshot_buffer_.size < size) {
      if (snapshot_buffer_.buffer) {
        vkDestroyBuffer(device_, snapshot_buffer_.buffer, nullptr);
        vkFreeMemory(device_, snapshot_buffer_.memory, nullptr);
        snapshot_buffer_ = {};
      }
      VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      info.size = size;
      info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      vkCreateBuffer(device_, &info, nullptr, &snapshot_buffer_.buffer);
      VkMemoryRequirements requirements;
      vkGetBufferMemoryRequirements(device_, snapshot_buffer_.buffer, &requirements);
      snapshot_buffer_.memory = Allocate(
          requirements,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, false);
      if (!snapshot_buffer_.memory) {
        REXLOG_ERROR("[native view] no memory for {} snapshots ({} MB)", count, size >> 20);
        vkDestroyBuffer(device_, snapshot_buffer_.buffer, nullptr);
        snapshot_buffer_ = {};
        return false;
      }
      vkBindBufferMemory(device_, snapshot_buffer_.buffer, snapshot_buffer_.memory, 0);
      vkMapMemory(device_, snapshot_buffer_.memory, 0, VK_WHOLE_SIZE, 0,
                  reinterpret_cast<void**>(&snapshot_buffer_.mapped));
      snapshot_buffer_.size = size;
    }
    return true;
  }

  void RecordSnapshot(VkCommandBuffer cmd, VkImage source, VkExtent2D extent, uint32_t slot) {
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(extent.width), int32_t(extent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {int32_t(kSnapshotWidth), int32_t(kSnapshotHeight), 1};
    vkCmdBlitImage(cmd, source, VK_IMAGE_LAYOUT_GENERAL, snapshot_image_.image,
                   VK_IMAGE_LAYOUT_GENERAL, 1, &blit, VK_FILTER_LINEAR);
    Barrier(vkCmdPipelineBarrier, cmd);
    VkBufferImageCopy copy{};
    copy.bufferOffset = VkDeviceSize(slot) * kSnapshotWidth * kSnapshotHeight * 4;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kSnapshotWidth, kSnapshotHeight, 1};
    vkCmdCopyImageToBuffer(cmd, snapshot_image_.image, VK_IMAGE_LAYOUT_GENERAL,
                           snapshot_buffer_.buffer, 1, &copy);
    Barrier(vkCmdPipelineBarrier, cmd);
  }

  // Slots: one per pass, the presented frame, then two per draw of the pass
  // NARUTO_NATIVE_VIEW_DRAW_SNAPSHOTS names (first_draw_slot onwards): its
  // first and second color targets.
  void WriteSnapshots(const char* root, uint32_t count, uint32_t first_draw_slot,
                      uint32_t draw_pass) {
    std::string dir = fmt::format("{}/{:06}", root, frames_total_);
    std::error_code error;
    std::filesystem::create_directories(dir, error);
    for (uint32_t slot = 0; slot < count; ++slot) {
      std::string path =
          slot >= first_draw_slot ? fmt::format("{}/pass{:02}_draw{:03}_rt{}.ppm", dir, draw_pass,
                                                (slot - first_draw_slot) / 2,
                                                (slot - first_draw_slot) % 2)
          : slot + 1 == first_draw_slot ? fmt::format("{}/final.ppm", dir)
                                        : fmt::format("{}/pass{:02}.ppm", dir, slot);
      FILE* file = std::fopen(path.c_str(), "wb");
      if (!file) {
        continue;
      }
      std::fprintf(file, "P6 %u %u 255\n", kSnapshotWidth, kSnapshotHeight);
      const uint8_t* pixels = snapshot_buffer_.mapped +
                              size_t(slot) * kSnapshotWidth * kSnapshotHeight * 4;
      for (uint32_t i = 0; i < kSnapshotWidth * kSnapshotHeight; ++i) {
        std::fwrite(pixels + i * 4, 1, 3, file);
      }
      std::fclose(file);
    }
    REXLOG_INFO("[native view] wrote {} snapshots to {}", count, dir);
  }

  void DrawFrame(const FrameInfo& frame) {
    if (!swapchain_ && !CreateSwapchain()) {
      return;
    }
    VkFence fence = frame_done_[frame_index_];
    auto wait_start = std::chrono::steady_clock::now();
    VkResult waited = vkWaitForFences(device_, 1, &fence, VK_TRUE, 5'000'000'000ull);
    if (waited != VK_SUCCESS) {
      REXLOG_ERROR("[native view] waiting for frame {} failed: {}", frames_total_, int(waited));
      return;
    }
    auto cpu_start = std::chrono::steady_clock::now();
    wait_seconds_ += std::chrono::duration<double>(cpu_start - wait_start).count();
    ReadTimestamps();
    frames_total_++;
    FreeRetired();
    HandleKeys();
    uint32_t pass_count = uint32_t(frame.passes.size());
    // 0 shows the presented frame; n shows pass n's first render target.
    uint32_t shown_pass = pass_offset_ ? uint32_t(((pass_offset_ - 1) % int64_t(pass_count) +
                                                   pass_count) % pass_count)
                                       : UINT32_MAX;
    // F12, or out/snapshot-request appearing (for scripts), asks for a
    // snapshot.
    std::error_code error;
    bool requested = snapshot_requested_ || std::filesystem::remove("out/snapshot-request", error);
    if (requested) {
      REXLOG_INFO("[native view] snapshot requested at frame {}", frames_total_);
    }
    snapshot_requested_ = false;
    static const long dump_frame = [] {
      const char* value = std::getenv("NARUTO_NATIVE_VIEW_DUMP");
      return value ? std::atol(value) : -1;
    }();
    static const long snapshot_every = [] {
      const char* value = std::getenv("NARUTO_NATIVE_VIEW_SNAPSHOT_EVERY");
      long every = value ? std::atol(value) : 300;
      return every > 0 ? every : 300;
    }();
    const char* snapshot_dir = std::getenv("NARUTO_NATIVE_VIEW_SNAPSHOTS");
    bool snapshots = (snapshot_dir && dump_frame >= 0 && long(frames_total_) >= dump_frame &&
                      (long(frames_total_) - dump_frame) % snapshot_every == 0) ||
                     requested;
    if (!snapshot_dir) {
      snapshot_dir = "out/snapshots";
    }
    Dump(frame, snapshots ? UINT32_MAX : shown_pass == UINT32_MAX ? 0 : shown_pass, snapshots);

    // Plan the frame on the CPU, in the game's order: resolves create their
    // destination images before later draws look textures up.
    new_images_.clear();
    static const uint32_t draw_pass = [] {
      const char* value = std::getenv("NARUTO_NATIVE_VIEW_DRAW_SNAPSHOTS");
      return value ? uint32_t(std::strtoul(value, nullptr, 10)) : UINT32_MAX;
    }();
    uint32_t draw_snapshots = 0;
    if (snapshots) {
      for (const Draw& draw : frame.captured) {
        draw_snapshots += draw.pass == draw_pass ? 2 : 0;
      }
    }
    if (snapshots && !PrepareSnapshots(pass_count + 1 + draw_snapshots)) {
      snapshots = false;
    }
    std::vector<PassTargets> targets = PlanTargets(frame);
    std::vector<ResolvedTexture*> resolve_destinations(frame.resolves.size());
    std::vector<PendingUpload> texture_uploads;
    size_t budget = kTextureUploadBudget;
    shared_.resize(frame.captured.size() * kSharedConstantsSize);
    size_t next_resolve = 0;
    auto plan_resolves = [&](uint32_t up_to_pass) {
      for (; next_resolve < frame.resolves.size() &&
             frame.resolves[next_resolve].pass <= up_to_pass;
           ++next_resolve) {
        resolve_destinations[next_resolve] = ResolveDestination(frame.resolves[next_resolve]);
      }
    };
    for (size_t d = 0; d < frame.captured.size(); ++d) {
      const Draw& draw = frame.captured[d];
      if (draw.pass) {
        plan_resolves(draw.pass - 1);
      }
      uint8_t* shared = shared_.data() + d * kSharedConstantsSize;
      std::memcpy(shared, frame.data.data() + draw.shared_constants, kSharedConstantsSize);
      // XenosRecomp's layout: 2D, 3D and cube texture indices at 0, 64 and
      // 128, sampler indices at 192, four bytes per sampler register.
      for (uint32_t t = 0; t < draw.texture_count; ++t) {
        const TextureBinding& binding = frame.textures[draw.first_texture + t];
        const CachedTexture* texture = Texture(frame, binding.fetch, texture_uploads, budget);
        uint32_t heap = texture ? texture->heap : 0, index = texture ? texture->index : 0;
        uint32_t sampler = SamplerIndex(binding.fetch);
        std::memcpy(shared + heap * 64 + binding.sampler * 4, &index, 4);
        std::memcpy(shared + 192 + binding.sampler * 4, &sampler, 4);
      }
    }
    plan_resolves(UINT32_MAX);

    VkDeviceSize shared_offset = (frame.data.size() + 15) & ~VkDeviceSize(15);
    VkDeviceSize total = shared_offset + shared_.size();
    for (PendingUpload& pending : texture_uploads) {
      pending.offset = (total + 15) & ~VkDeviceSize(15);
      total = pending.offset + pending.decoded.data.size();
      textures_uploaded_bytes_ += pending.decoded.data.size();
    }
    // Cached vertex data new to the GPU goes through the upload buffer into
    // device-local memory; past the budget, or without memory, it is drawn
    // from the upload buffer this frame.
    struct VertexSource {
      VkBuffer buffer;  // VK_NULL_HANDLE: the upload buffer
      VkDeviceSize offset;
    };
    struct StagedVertices {
      size_t index;  // into frame.vertex_data
      VkDeviceSize offset;  // in the upload buffer
      const ResidentVertices* resident;  // where to copy it, or nullptr
    };
    std::vector<VertexSource> vertex_sources(frame.vertex_data.size());
    std::vector<StagedVertices> staged_vertices;
    size_t vertex_budget = kTextureUploadBudget;
    for (size_t i = 0; i < frame.vertex_data.size(); ++i) {
      const VertexData& data = *frame.vertex_data[i];
      auto it = vertex_data_.find(data.id);
      if (it != vertex_data_.end()) {
        it->second.last_used = frames_total_;
        vertex_sources[i] = {vertex_chunks_[it->second.chunk].buffer, it->second.offset};
        continue;
      }
      VkDeviceSize offset = (total + 15) & ~VkDeviceSize(15);
      total = offset + data.bytes.size();
      vertex_sources[i] = {VK_NULL_HANDLE, offset};
      ResidentVertices resident;
      if (data.bytes.size() <= vertex_budget &&
          AllocateVertexMemory(data.bytes.size(), resident)) {
        vertex_budget -= data.bytes.size();
        resident.last_used = frames_total_;
        it = vertex_data_.emplace(data.id, resident).first;
        vertex_sources[i] = {vertex_chunks_[resident.chunk].buffer, resident.offset};
        vertices_uploaded_bytes_ += data.bytes.size();
      }
      staged_vertices.push_back(
          {i, offset, it != vertex_data_.end() ? &it->second : nullptr});
    }
    // Without a submit, vertex data made resident here never gets copied.
    auto abandon_vertices = [&] {
      for (const StagedVertices& staged : staged_vertices) {
        if (staged.resident) {
          FreeVertexMemory(*staged.resident);
          vertex_data_.erase(frame.vertex_data[staged.index]->id);
        }
      }
    };
    Upload& upload = uploads_[frame_index_];
    if (!ReserveUpload(upload, std::max<VkDeviceSize>(total, 1))) {
      REXLOG_ERROR("[native view] upload buffer allocation failed");
      abandon_vertices();
      return;
    }
    uint32_t image_index = 0;
    VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                              image_acquired_[frame_index_], VK_NULL_HANDLE,
                                              &image_index);
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
      abandon_vertices();
      Recreate();
      return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
      abandon_vertices();
      return;
    }
    vkResetFences(device_, 1, &fence);
    std::memcpy(upload.mapped, frame.data.data(), frame.data.size());
    std::memcpy(upload.mapped + shared_offset, shared_.data(), shared_.size());
    for (const PendingUpload& pending : texture_uploads) {
      std::memcpy(upload.mapped + pending.offset, pending.decoded.data.data(),
                  pending.decoded.data.size());
    }
    for (const StagedVertices& staged : staged_vertices) {
      const std::vector<uint8_t>& bytes = frame.vertex_data[staged.index]->bytes;
      std::memcpy(upload.mapped + staged.offset, bytes.data(), bytes.size());
    }

    VkCommandBuffer cmd = command_buffers_[frame_index_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    uint32_t timestamps = StartTimestamps(cmd, pass_count + 2);
    std::vector<size_t> statistics_draws;
    if (snapshots && statistics_) {
      if (!statistics_pool_) {
        VkQueryPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        pool_info.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
        pool_info.queryCount = kStatisticsQueries;
        pool_info.pipelineStatistics =
            VK_QUERY_PIPELINE_STATISTIC_INPUT_ASSEMBLY_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_INVOCATIONS_BIT |
            VK_QUERY_PIPELINE_STATISTIC_CLIPPING_PRIMITIVES_BIT |
            VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
        vkCreateQueryPool(device_, &pool_info, nullptr, &statistics_pool_);
      }
      if (statistics_pool_) {
        vkCmdResetQueryPool(cmd, statistics_pool_, 0, kStatisticsQueries);
      }
    }

    // New render targets and resolve destinations start cleared, in GENERAL.
    for (const NewImage& image : new_images_) {
      VkImageAspectFlags aspect = image.depth ? VK_IMAGE_ASPECT_DEPTH_BIT
                                              : VK_IMAGE_ASPECT_COLOR_BIT;
      VkImageSubresourceRange range{aspect, 0, 1, 0, image.layers};
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = image.image;
      barrier.subresourceRange = range;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
      if (image.depth) {
        VkClearDepthStencilValue clear{1.0f, 0};
        vkCmdClearDepthStencilImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
      } else {
        VkClearColorValue clear{};
        vkCmdClearColorImage(cmd, image.image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
      }
    }
    for (const PendingUpload& pending : texture_uploads) {
      const texture_decode::Texture& t = pending.decoded;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, t.layers};
      VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = pending.texture->image.image;
      barrier.subresourceRange = range;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
      VkBufferImageCopy copy{};
      copy.bufferOffset = pending.offset;
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, t.layers};
      copy.imageExtent = {t.width, t.height, t.depth};
      vkCmdCopyBufferToImage(cmd, upload.buffer, barrier.image,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &barrier);
    }
    for (const StagedVertices& staged : staged_vertices) {
      if (staged.resident) {
        VkBufferCopy copy{staged.offset, staged.resident->offset,
                          frame.vertex_data[staged.index]->bytes.size()};
        vkCmdCopyBuffer(cmd, upload.buffer, vertex_chunks_[staged.resident->chunk].buffer, 1,
                        &copy);
      }
    }
    Barrier(vkCmdPipelineBarrier, cmd);

    // The passes, each followed by the resolves that end it.
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 4,
                            descriptor_sets_.data(), 0, nullptr);
    uint32_t drawn = 0;
    size_t d = 0;
    next_resolve = 0;
    size_t next_clear = 0;
    std::set<const RenderTarget*> depth_cleared;  // debug bit 8 only
    for (uint32_t pass = 0; pass < pass_count; ++pass) {
      WriteTimestamp(cmd, timestamps, pass);
      size_t first = d;
      while (d < frame.captured.size() && frame.captured[d].pass <= pass) {
        ++d;
      }
      const PassTargets& pt = targets[pass];
      bool renderable = d > first && (pt.color_count || pt.depth) && pt.extent.width != UINT32_MAX;
      bool rendering = false;
      auto begin_rendering = [&] {
        std::array<VkRenderingAttachmentInfo, 4> colors{};
        for (uint32_t i = 0; i < pt.color_count; ++i) {
          colors[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
          colors[i].imageView = pt.color[i] ? pt.color[i]->image.view : VK_NULL_HANDLE;
          colors[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (pt.depth) {
          depth.imageView = pt.depth->image.view;
          depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
          depth.loadOp = (DebugFlags() & 8) && depth_cleared.insert(pt.depth).second
                             ? VK_ATTACHMENT_LOAD_OP_CLEAR
                             : VK_ATTACHMENT_LOAD_OP_LOAD;
          depth.clearValue.depthStencil = {1.0f, 0};
          depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, pt.extent};
        info.layerCount = 1;
        info.colorAttachmentCount = pt.color_count;
        info.pColorAttachments = colors.data();
        info.pDepthAttachment = pt.depth ? &depth : nullptr;
        vkCmdBeginRendering(cmd, &info);
        VkRect2D scissor{{0, 0}, pt.extent};
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        rendering = true;
      };
      // Clears go between draws and before the pass's resolves. Inside
      // rendering, the pass's own attachments are cleared in place; ending
      // rendering there can stall the NVIDIA driver.
      auto clear_until = [&](uint32_t up_to_pass, size_t before_draw) {
        bool cleared = false;
        for (; next_clear < frame.clears.size() && frame.clears[next_clear].pass <= up_to_pass &&
               frame.clears[next_clear].before_draw <= before_draw;
             ++next_clear) {
          const Clear& clear = frame.clears[next_clear];
          if (DebugFlags() & 8) {
            continue;
          }
          VkRect2D area = ClearArea(clear);
          if (rendering && ClearAttachments(cmd, clear, area, pt)) {
            continue;
          }
          if (rendering) {
            vkCmdEndRendering(cmd);
            rendering = false;
          }
          if (!cleared) {
            Barrier(vkCmdPipelineBarrier, cmd);
          }
          RecordClear(cmd, clear, area);
          cleared = true;
        }
        if (cleared) {
          Barrier(vkCmdPipelineBarrier, cmd);
        }
        return cleared;
      };
      VkPipeline bound = VK_NULL_HANDLE;
      for (size_t i = first; renderable && i < d; ++i) {
        if (clear_until(pass, i)) {
          bound = VK_NULL_HANDLE;
        }
        const Draw& draw = frame.captured[i];
        VkViewport viewport = Viewport(draw);
        if (!draw.index_count || viewport.width <= 0.0f || viewport.height == 0.0f) {
          continue;
        }
        VkPipeline pipeline = Pipeline(frame, draw, pt);
        if (!pipeline) {
          continue;
        }
        if (!rendering) {
          begin_rendering();
          bound = VK_NULL_HANDLE;
        }
        if (pipeline != bound) {
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
          bound = pipeline;
        }
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        std::array<VkBuffer, 16> buffers;
        std::array<VkDeviceSize, 16> offsets;
        for (uint32_t b = 0; b < draw.binding_count; ++b) {
          const VertexBinding& binding = frame.bindings[draw.first_binding + b];
          buffers[b] = upload.buffer;
          offsets[b] = binding.data_offset;
          if (binding.cached >= 0) {
            const VertexSource& source = vertex_sources[binding.cached];
            buffers[b] = source.buffer ? source.buffer : upload.buffer;
            offsets[b] += source.offset;
          }
        }
        if (draw.binding_count) {
          vkCmdBindVertexBuffers(cmd, 0, draw.binding_count, buffers.data(), offsets.data());
        }
        vkCmdBindIndexBuffer(cmd, upload.buffer, draw.index_offset, VK_INDEX_TYPE_UINT32);
        PushConstants push{upload.address + draw.vertex_constants,
                           upload.address + draw.pixel_constants,
                           upload.address + shared_offset + i * kSharedConstantsSize};
        vkCmdPushConstants(cmd, pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(push), &push);
        bool measured = statistics_pool_ && snapshots && DumpedVertexShader(draw) &&
                        statistics_draws.size() < kStatisticsQueries;
        if (measured) {
          vkCmdBeginQuery(cmd, statistics_pool_, uint32_t(statistics_draws.size()), 0);
        }
        vkCmdDrawIndexed(cmd, draw.index_count, 1, 0, 0, 0);
        if (measured) {
          vkCmdEndQuery(cmd, statistics_pool_, uint32_t(statistics_draws.size()));
          statistics_draws.push_back(i);
        }
        drawn++;
        if (snapshots && pass == draw_pass && pt.color_count && pt.color[0]) {
          vkCmdEndRendering(cmd);
          rendering = false;
          Barrier(vkCmdPipelineBarrier, cmd);
          for (uint32_t c = 0; c < 2 && c < pt.color_count; ++c) {
            if (pt.color[c]) {
              RecordSnapshot(cmd, pt.color[c]->image.image,
                             {pt.color[c]->width, pt.color[c]->height},
                             pass_count + 1 + 2 * uint32_t(i - first) + c);
            }
          }
        }
      }
      if (rendering) {
        vkCmdEndRendering(cmd);
        rendering = false;
        Barrier(vkCmdPipelineBarrier, cmd);
      }
      clear_until(pass, SIZE_MAX);
      for (; next_resolve < frame.resolves.size() && frame.resolves[next_resolve].pass <= pass;
           ++next_resolve) {
        const Resolve& r = frame.resolves[next_resolve];
        ResolvedTexture* dest = resolve_destinations[next_resolve];
        auto source = render_targets_.find(TargetKey(r.surface_info, r.source_info,
                                                     (r.flags & 7) == 4));
        if (dest && source != render_targets_.end()) {
          RecordResolve(cmd, r, source->second, *dest);
          Barrier(vkCmdPipelineBarrier, cmd);
        }
      }
      if (snapshots) {
        for (uint32_t i = 0; i < pt.color_count; ++i) {
          if (pt.color[i]) {
            RecordSnapshot(cmd, pt.color[i]->image.image, {pt.color[i]->width, pt.color[i]->height},
                           pass);
            break;
          }
        }
      }
    }

    WriteTimestamp(cmd, timestamps, pass_count);

    // Present: the front buffer texture, or a pass's render target.
    VkImage present_source = VK_NULL_HANDLE;
    VkExtent2D present_extent{};
    std::string shown;
    if (shown_pass == UINT32_MAX) {
      auto front = frame.has_front_buffer ? resolved_.find(frame.front_buffer[1] & 0x1FFFF000)
                                          : resolved_.end();
      if (front != resolved_.end()) {
        present_source = front->second.entry.image.image;
        present_extent = {front->second.width, front->second.height};
      }
      shown = "presented frame";
    } else {
      const PassTargets& pt = targets[shown_pass];
      RenderTarget* target = nullptr;
      for (uint32_t i = 0; i < pt.color_count && !target; ++i) {
        target = pt.color[i];
      }
      if (target) {
        present_source = target->image.image;
        present_extent = {target->width, target->height};
      }
      shown = fmt::format("pass {}/{}", shown_pass + 1, pass_count);
    }
    SetTitle(fmt::format("Naruto - native renderer | {} | {} draws, {} resolves", shown,
                         frame.draws, frame.resolves.size()));
    if (snapshots && present_source) {
      RecordSnapshot(cmd, present_source, present_extent, pass_count);
    }
    VkImage swap_image = targets_[image_index].image;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = swap_image;
    to_dst.subresourceRange = range;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &to_dst);
    VkClearColorValue background = {{0.08f, 0.08f, 0.1f, 1.0f}};
    vkCmdClearColorImage(cmd, swap_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &background, 1,
                         &range);
    if (present_source) {
      // Fit the source's aspect ratio inside the window.
      float scale = std::min(float(extent_.width) / present_extent.width,
                             float(extent_.height) / present_extent.height);
      int32_t w = int32_t(present_extent.width * scale), h = int32_t(present_extent.height * scale);
      int32_t x = (int32_t(extent_.width) - w) / 2, y = (int32_t(extent_.height) - h) / 2;
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {int32_t(present_extent.width), int32_t(present_extent.height), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[0] = {x, y, 0};
      blit.dstOffsets[1] = {x + w, y + h, 1};
      vkCmdBlitImage(cmd, present_source, VK_IMAGE_LAYOUT_GENERAL, swap_image,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_present);
    WriteTimestamp(cmd, timestamps, pass_count + 1);
    vkEndCommandBuffer(cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSemaphore render_done = targets_[image_index].render_done;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_acquired_[frame_index_];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    VkResult submitted = vkQueueSubmit(queue_, 1, &submit, fence);
    if (submitted != VK_SUCCESS) {
      REXLOG_ERROR("[native view] submitting frame {} failed: {}", frames_total_, int(submitted));
    }
    cpu_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - cpu_start).count();
    capture_seconds_ += frame.capture_seconds;
    if (snapshots) {
      vkQueueWaitIdle(queue_);
      for (uint32_t q = 0; q < statistics_draws.size(); ++q) {
        uint64_t values[5] = {};
        if (vkGetQueryPoolResults(device_, statistics_pool_, q, 1, sizeof(values), values,
                                  sizeof(values), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
          REXLOG_INFO("[native view] draw {} statistics: {} primitives in, {} vertex shader runs, "
                      "{} clipping in, {} clipping out, {} fragment shader runs",
                      statistics_draws[q], values[0], values[1], values[2], values[3],
                      values[4]);
        }
      }
      WriteSnapshots(snapshot_dir, pass_count + 1 + draw_snapshots, pass_count + 1, draw_pass);
    }

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
          "[native view] {:.1f} fps, {:.1f} draws/frame, {} passes, {} resolves, "
          "{:.1f} MB captured/frame, {} pipelines, {} render targets, {} resolved textures, "
          "{} textures ({} not decodable), {:.1f} MB of textures uploaded, {:.1f} MB of "
          "vertices cached ({} ranges in {} chunks)",
          frames_presented_ / seconds, double(draws_drawn_) / std::max<uint64_t>(frames_presented_, 1),
          pass_count, frame.resolves.size(), frame.data.size() / 1048576.0, pipelines_.size(),
          render_targets_.size(), resolved_.size(), textures_.size(), textures_failed_,
          textures_uploaded_bytes_ / 1048576.0, vertices_uploaded_bytes_ / 1048576.0,
          vertex_data_.size(), vertex_chunks_.size());
      vertices_uploaded_bytes_ = 0;
      uint64_t submitted = frames_submitted_.exchange(0);
      uint64_t presented = std::max<uint64_t>(frames_presented_, 1);
      REXLOG_INFO("[native view] timing: {:.1f} game frames/s, capture {:.2f} ms/frame (game "
                  "thread), view CPU {:.2f} ms/frame, GPU wait {:.2f} ms/frame",
                  submitted / seconds, capture_seconds_ * 1000 / presented,
                  cpu_seconds_ * 1000 / presented, wait_seconds_ * 1000 / presented);
      wait_seconds_ = 0;
      if (gpu_frames_) {
        // The costliest passes; the last entry is the present.
        std::vector<uint32_t> order(pass_gpu_ms_.size());
        for (uint32_t i = 0; i < order.size(); ++i) {
          order[i] = i;
        }
        std::sort(order.begin(), order.end(),
                  [&](uint32_t a, uint32_t b) { return pass_gpu_ms_[a] > pass_gpu_ms_[b]; });
        std::string top;
        for (uint32_t i = 0; i < order.size() && i < 40; ++i) {
          top += fmt::format(" {}{}: {:.2f}", order[i] + 1 == order.size() ? "present" : "pass ",
                             order[i] + 1 == order.size() ? "" : std::to_string(order[i]),
                             pass_gpu_ms_[order[i]] / gpu_frames_);
        }
        REXLOG_INFO("[native view] GPU {:.2f} ms/frame; costliest (ms):{}", gpu_ms_ / gpu_frames_,
                    top);
        pass_gpu_ms_.clear();
        gpu_ms_ = 0;
        gpu_frames_ = 0;
      }
      capture_seconds_ = 0;
      cpu_seconds_ = 0;
      textures_uploaded_bytes_ = 0;
      frames_presented_ = 0;
      draws_drawn_ = 0;
      log_start_ = now;
    }
  }

  struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkSemaphore render_done = VK_NULL_HANDLE;
  };

  struct NewImage {
    VkImage image;
    bool depth;
    uint32_t layers;
  };

  struct Scratch {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
  };

  std::once_flag started_;
  std::mutex mutex_;
  std::condition_variable wake_;
  bool pending_ = false;
  FrameInfo latest_;
  std::vector<std::vector<uint8_t>> spare_data_;

  SDL_Window* window_ = nullptr;
  std::string title_;
  int64_t pass_offset_ = 0;
  bool key_left_ = false;
  bool key_right_ = false;
  bool key_f12_ = false;
  bool snapshot_requested_ = false;
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
  std::array<VkSampler, kSamplerCount> samplers_{};
  bool mirror_clamp_ = false;
  // Pipeline statistics for the draws NARUTO_NATIVE_VIEW_DUMP_VS names, on
  // snapshot frames.
  bool statistics_ = false;
  VkQueryPool statistics_pool_ = VK_NULL_HANDLE;
  static constexpr uint32_t kStatisticsQueries = 64;
  std::map<std::array<uint32_t, 5>, CachedTexture> textures_;
  std::array<std::vector<uint32_t>, 3> free_indices_;
  std::vector<Retired> retired_;
  uint64_t textures_failed_ = 0;
  uint64_t textures_uploaded_bytes_ = 0;
  std::unordered_map<uint64_t, ResidentVertices> vertex_data_;  // by VertexData::id
  std::vector<VertexChunk> vertex_chunks_;
  uint64_t vertices_uploaded_bytes_ = 0;
  std::atomic<uint64_t> frames_submitted_ = 0;
  double capture_seconds_ = 0;  // of the presented frames
  double cpu_seconds_ = 0;
  double wait_seconds_ = 0;  // for the frame's fence
  static constexpr uint32_t kTimestamps = 128;  // per frame in flight
  VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
  float timestamp_period_ = 0;  // ns per tick, 0 without timestamps
  uint32_t timestamp_counts_[kFramesInFlight] = {};
  std::vector<double> pass_gpu_ms_;  // per pass, summed over the log period
  double gpu_ms_ = 0;
  uint64_t gpu_frames_ = 0;
  std::vector<uint8_t> shared_;
  std::set<std::pair<std::string, uint32_t>> warned_;
  VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
  std::array<VkDescriptorSetLayout, 4> set_layouts_{};
  std::array<VkDescriptorSet, 4> descriptor_sets_{};
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::vector<VkShaderModule> shader_modules_;
  std::map<std::vector<uint32_t>, VkPipeline> pipelines_;

  std::map<uint64_t, RenderTarget> render_targets_;
  std::map<uint32_t, ResolvedTexture> resolved_;
  std::vector<NewImage> new_images_;
  Scratch scratch_;
  Image snapshot_image_;
  struct {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    uint8_t* mapped = nullptr;
  } snapshot_buffer_;

  VkSurfaceFormatKHR format_{};
  VkExtent2D extent_{};
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
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

std::vector<uint8_t> RecycledData() {
  return Enabled() ? GetView().RecycledData() : std::vector<uint8_t>();
}

}  // namespace native_view
