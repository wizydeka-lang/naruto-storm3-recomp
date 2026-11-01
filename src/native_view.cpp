// naruto - native renderer view
//
// Runs on its own thread: the game's Swap hook only hands over the latest
// frame's data, so presenting here never stalls the game. The window is
// created on the main thread (SDL's requirement); Vulkan runs on the view
// thread. Each frame clears to the game's clear color and draws a graph of
// the last 256 frames' draw counts with clear rectangles, so no shaders are
// needed yet.

#include "native_view.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <rex/logging.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace native_view {

namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr size_t kHistoryFrames = 256;
// Draw count that fills the graph's height; fights reach 350-450.
constexpr float kGraphFullDraws = 600.0f;

#define NV_INSTANCE_FUNCTIONS(X)                \
  X(vkEnumeratePhysicalDevices)                 \
  X(vkGetPhysicalDeviceProperties)              \
  X(vkGetPhysicalDeviceQueueFamilyProperties)   \
  X(vkGetPhysicalDeviceSurfaceSupportKHR)       \
  X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)  \
  X(vkGetPhysicalDeviceSurfaceFormatsKHR)       \
  X(vkGetPhysicalDeviceSurfacePresentModesKHR)  \
  X(vkCreateDevice)                             \
  X(vkGetDeviceProcAddr)

#define NV_DEVICE_FUNCTIONS(X) \
  X(vkGetDeviceQueue)          \
  X(vkCreateSwapchainKHR)      \
  X(vkDestroySwapchainKHR)     \
  X(vkGetSwapchainImagesKHR)   \
  X(vkAcquireNextImageKHR)     \
  X(vkQueuePresentKHR)         \
  X(vkQueueSubmit)             \
  X(vkCreateImageView)         \
  X(vkDestroyImageView)        \
  X(vkCreateRenderPass)        \
  X(vkCreateFramebuffer)       \
  X(vkDestroyFramebuffer)      \
  X(vkCreateCommandPool)       \
  X(vkAllocateCommandBuffers)  \
  X(vkBeginCommandBuffer)      \
  X(vkEndCommandBuffer)        \
  X(vkResetCommandBuffer)      \
  X(vkCmdBeginRenderPass)      \
  X(vkCmdEndRenderPass)        \
  X(vkCmdClearAttachments)     \
  X(vkCreateSemaphore)         \
  X(vkDestroySemaphore)        \
  X(vkCreateFence)             \
  X(vkWaitForFences)           \
  X(vkResetFences)             \
  X(vkDeviceWaitIdle)

class View {
 public:
  void Submit(const FrameInfo& frame) {
    std::call_once(started_, [this] { std::thread([this] { Run(); }).detach(); });
    std::lock_guard<std::mutex> lock(mutex_);
    latest_ = frame;
    history_.push_back(frame.draws);
    if (history_.size() > kHistoryFrames) {
      history_.pop_front();
    }
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
      std::vector<uint32_t> history;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] { return pending_; });
        pending_ = false;
        frame = latest_;
        history.assign(history_.begin(), history_.end());
      }
      DrawFrame(frame, history);
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
    app.apiVersion = VK_API_VERSION_1_1;
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
    if (!PickDevice() || !CreateDevice()) {
      return false;
    }
    return CreateSwapchain();
  }

  // Prefers a discrete GPU with a queue that both draws and presents.
  bool PickDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    int best_score = -1;
    for (VkPhysicalDevice device : devices) {
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
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(device, &properties);
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
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
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

    if (!render_pass_) {
      uint32_t count = 0;
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, nullptr);
      std::vector<VkSurfaceFormatKHR> formats(count);
      vkGetPhysicalDeviceSurfaceFormatsKHR(physical_device_, surface_, &count, formats.data());
      format_ = formats[0];
      for (const auto& f : formats) {
        // UNORM, not sRGB: the game's clear colors are shown as given.
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
          format_ = f;
          break;
        }
      }
      VkAttachmentDescription color{};
      color.format = format_.format;
      color.samples = VK_SAMPLE_COUNT_1_BIT;
      color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
      VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{};
      subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      subpass.colorAttachmentCount = 1;
      subpass.pColorAttachments = &color_ref;
      VkSubpassDependency dependency{};
      dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
      dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
      dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      VkRenderPassCreateInfo pass_info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
      pass_info.attachmentCount = 1;
      pass_info.pAttachments = &color;
      pass_info.subpassCount = 1;
      pass_info.pSubpasses = &subpass;
      pass_info.dependencyCount = 1;
      pass_info.pDependencies = &dependency;
      if (vkCreateRenderPass(device_, &pass_info, nullptr, &render_pass_) != VK_SUCCESS) {
        return false;
      }
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
      VkFramebufferCreateInfo fb_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb_info.renderPass = render_pass_;
      fb_info.attachmentCount = 1;
      fb_info.pAttachments = &target.view;
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
  }

  void Recreate() {
    vkDeviceWaitIdle(device_);
    CreateSwapchain();
  }

  void DrawFrame(const FrameInfo& frame, const std::vector<uint32_t>& history) {
    if (!swapchain_ && !CreateSwapchain()) {
      return;
    }
    VkFence fence = frame_done_[frame_index_];
    vkWaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX);
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

    VkCommandBuffer cmd = command_buffers_[frame_index_];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    VkClearValue clear{};
    if (frame.has_clear_color) {
      std::copy(frame.clear_color, frame.clear_color + 4, clear.color.float32);
    } else {
      clear.color = {{0.08f, 0.08f, 0.1f, 1.0f}};
    }
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = render_pass_;
    pass.framebuffer = targets_[image_index].framebuffer;
    pass.renderArea = {{0, 0}, extent_};
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

    // Draw-count graph along the bottom third, newest frame on the right.
    std::vector<VkClearRect> bars;
    float bar_width = float(extent_.width) / kHistoryFrames;
    float graph_height = extent_.height * 0.33f;
    size_t first = kHistoryFrames - history.size();
    for (size_t i = 0; i < history.size(); ++i) {
      uint32_t height = uint32_t(std::min(history[i] / kGraphFullDraws, 1.0f) * graph_height);
      if (!height) {
        continue;
      }
      int32_t x = int32_t((first + i) * bar_width);
      uint32_t width = std::max(1u, uint32_t((first + i + 1) * bar_width) - uint32_t(x));
      VkClearRect rect{};
      rect.rect = {{x, int32_t(extent_.height - height)}, {width, height}};
      rect.layerCount = 1;
      bars.push_back(rect);
    }
    if (!bars.empty()) {
      VkClearAttachment bar_color{};
      bar_color.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      bar_color.clearValue.color = {{0.95f, 0.6f, 0.1f, 1.0f}};
      vkCmdClearAttachments(cmd, 1, &bar_color, uint32_t(bars.size()), bars.data());
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
  std::deque<uint32_t> history_;

  SDL_Window* window_ = nullptr;
  std::string device_name_;
  PFN_vkGetInstanceProcAddr vk_get_instance_proc_addr_ = nullptr;
#define X(name) PFN_##name name = nullptr;
  NV_INSTANCE_FUNCTIONS(X)
  NV_DEVICE_FUNCTIONS(X)
#undef X
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkDevice device_ = VK_NULL_HANDLE;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool command_pool_ = VK_NULL_HANDLE;
  std::array<VkCommandBuffer, kFramesInFlight> command_buffers_{};
  std::array<VkSemaphore, kFramesInFlight> image_acquired_{};
  std::array<VkFence, kFramesInFlight> frame_done_{};
  uint32_t frame_index_ = 0;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
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

void SubmitFrame(const FrameInfo& frame) {
  if (Enabled()) {
    GetView().Submit(frame);
  }
}

}  // namespace native_view
