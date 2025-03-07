/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include <sstream>

#include "vk_backend.hh"
#include "vk_context.hh"
#include "vk_device.hh"
#include "vk_memory.hh"
#include "vk_state_manager.hh"
#include "vk_storage_buffer.hh"
#include "vk_texture.hh"
#include "vk_vertex_buffer.hh"

#include "GPU_capabilities.hh"

#include "BLI_math_matrix_types.hh"

#include "GHOST_C-api.h"

extern "C" char datatoc_glsl_shader_defines_glsl[];

namespace blender::gpu {

void VKDevice::reinit()
{
  samplers_.free();
  samplers_.init();
}

void VKDevice::deinit()
{
  if (!is_initialized()) {
    return;
  }

  dummy_buffer.free();
  samplers_.free();

  {
    while (!thread_data_.is_empty()) {
      VKThreadData *thread_data = thread_data_.pop_last();
      thread_data->deinit(*this);
      delete thread_data;
    }
    thread_data_.clear();
  }
  pipelines.free_data();
  descriptor_set_layouts_.deinit();
  vmaDestroyAllocator(mem_allocator_);
  mem_allocator_ = VK_NULL_HANDLE;

  debugging_tools_.deinit(vk_instance_);

  vk_instance_ = VK_NULL_HANDLE;
  vk_physical_device_ = VK_NULL_HANDLE;
  vk_device_ = VK_NULL_HANDLE;
  vk_queue_family_ = 0;
  vk_queue_ = VK_NULL_HANDLE;
  vk_physical_device_properties_ = {};
  glsl_patch_.clear();
}

bool VKDevice::is_initialized() const
{
  return vk_device_ != VK_NULL_HANDLE;
}

void VKDevice::init(void *ghost_context)
{
  BLI_assert(!is_initialized());
  GHOST_GetVulkanHandles((GHOST_ContextHandle)ghost_context,
                         &vk_instance_,
                         &vk_physical_device_,
                         &vk_device_,
                         &vk_queue_family_,
                         &vk_queue_);

  init_physical_device_properties();
  init_physical_device_memory_properties();
  init_physical_device_features();
  init_physical_device_extensions();
  VKBackend::platform_init(*this);
  VKBackend::capabilities_init(*this);
  init_functions();
  init_debug_callbacks();
  init_memory_allocator();
  pipelines.init();

  samplers_.init();
  init_dummy_buffer();

  debug::object_label(vk_handle(), "LogicalDevice");
  debug::object_label(queue_get(), "GenericQueue");
  init_glsl_patch();
}

void VKDevice::init_functions()
{
#define LOAD_FUNCTION(name) (PFN_##name) vkGetInstanceProcAddr(vk_instance_, STRINGIFY(name))
  /* VK_KHR_dynamic_rendering */
  functions.vkCmdBeginRendering = LOAD_FUNCTION(vkCmdBeginRenderingKHR);
  functions.vkCmdEndRendering = LOAD_FUNCTION(vkCmdEndRenderingKHR);

  /* VK_EXT_debug_utils */
  functions.vkCmdBeginDebugUtilsLabel = LOAD_FUNCTION(vkCmdBeginDebugUtilsLabelEXT);
  functions.vkCmdEndDebugUtilsLabel = LOAD_FUNCTION(vkCmdEndDebugUtilsLabelEXT);
  functions.vkSetDebugUtilsObjectName = LOAD_FUNCTION(vkSetDebugUtilsObjectNameEXT);
  functions.vkCreateDebugUtilsMessenger = LOAD_FUNCTION(vkCreateDebugUtilsMessengerEXT);
  functions.vkDestroyDebugUtilsMessenger = LOAD_FUNCTION(vkDestroyDebugUtilsMessengerEXT);
#undef LOAD_FUNCTION
}

void VKDevice::init_debug_callbacks()
{
  debugging_tools_.init(vk_instance_);
}

void VKDevice::init_physical_device_properties()
{
  BLI_assert(vk_physical_device_ != VK_NULL_HANDLE);
  vkGetPhysicalDeviceProperties(vk_physical_device_, &vk_physical_device_properties_);
}

void VKDevice::init_physical_device_memory_properties()
{
  BLI_assert(vk_physical_device_ != VK_NULL_HANDLE);
  vkGetPhysicalDeviceMemoryProperties(vk_physical_device_, &vk_physical_device_memory_properties_);
}

void VKDevice::init_physical_device_features()
{
  BLI_assert(vk_physical_device_ != VK_NULL_HANDLE);

  VkPhysicalDeviceFeatures2 features = {};
  features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  vk_physical_device_vulkan_11_features_.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  vk_physical_device_vulkan_12_features_.sType =
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

  features.pNext = &vk_physical_device_vulkan_11_features_;
  vk_physical_device_vulkan_11_features_.pNext = &vk_physical_device_vulkan_12_features_;

  vkGetPhysicalDeviceFeatures2(vk_physical_device_, &features);
  vk_physical_device_features_ = features.features;
}

void VKDevice::init_physical_device_extensions()
{
  uint32_t count = 0;
  vkEnumerateDeviceExtensionProperties(vk_physical_device_, nullptr, &count, nullptr);
  device_extensions_ = Array<VkExtensionProperties>(count);
  vkEnumerateDeviceExtensionProperties(
      vk_physical_device_, nullptr, &count, device_extensions_.data());
}

bool VKDevice::supports_extension(const char *extension_name) const
{
  for (const VkExtensionProperties &vk_extension_properties : device_extensions_) {
    if (STREQ(vk_extension_properties.extensionName, extension_name)) {
      return true;
    }
  }
  return false;
}

void VKDevice::init_memory_allocator()
{
  VK_ALLOCATION_CALLBACKS;
  VmaAllocatorCreateInfo info = {};
  info.vulkanApiVersion = VK_API_VERSION_1_2;
  info.physicalDevice = vk_physical_device_;
  info.device = vk_device_;
  info.instance = vk_instance_;
  info.pAllocationCallbacks = vk_allocation_callbacks;
  vmaCreateAllocator(&info, &mem_allocator_);
}

void VKDevice::init_dummy_buffer()
{
  dummy_buffer.create(sizeof(float4x4),
                      GPU_USAGE_DEVICE_ONLY,
                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  debug::object_label(dummy_buffer.vk_handle(), "DummyBuffer");
  /* Default dummy buffer. Set the 4th element to 1 to fix missing orcos. */
  float data[16] = {
      0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
  dummy_buffer.update(static_cast<void *>(data));
}

void VKDevice::init_glsl_patch()
{
  std::stringstream ss;

  ss << "#version 450\n";
  if (GPU_shader_draw_parameters_support()) {
    ss << "#extension GL_ARB_shader_draw_parameters : enable\n";
    ss << "#define GPU_ARB_shader_draw_parameters\n";
    ss << "#define gpu_BaseInstance (gl_BaseInstanceARB)\n";
  }

  ss << "#define gl_VertexID gl_VertexIndex\n";
  ss << "#define gpu_InstanceIndex (gl_InstanceIndex)\n";
  ss << "#define gl_InstanceID (gpu_InstanceIndex - gpu_BaseInstance)\n";

  ss << "#extension GL_ARB_shader_viewport_layer_array: enable\n";
  if (GPU_stencil_export_support()) {
    ss << "#extension GL_ARB_shader_stencil_export: enable\n";
    ss << "#define GPU_ARB_shader_stencil_export 1\n";
  }
  if (!workarounds_.shader_output_layer) {
    ss << "#define gpu_Layer gl_Layer\n";
  }
  if (!workarounds_.shader_output_viewport_index) {
    ss << "#define gpu_ViewportIndex gl_ViewportIndex\n";
  }

  ss << "#define DFDX_SIGN 1.0\n";
  ss << "#define DFDY_SIGN 1.0\n";

  /* GLSL Backend Lib. */
  ss << datatoc_glsl_shader_defines_glsl;
  glsl_patch_ = ss.str();
}

const char *VKDevice::glsl_patch_get() const
{
  BLI_assert(!glsl_patch_.empty());
  return glsl_patch_.c_str();
}

/* -------------------------------------------------------------------- */
/** \name Platform/driver/device information
 * \{ */

constexpr int32_t PCI_ID_NVIDIA = 0x10de;
constexpr int32_t PCI_ID_INTEL = 0x8086;
constexpr int32_t PCI_ID_AMD = 0x1002;
constexpr int32_t PCI_ID_ATI = 0x1022;
constexpr int32_t PCI_ID_APPLE = 0x106b;

eGPUDeviceType VKDevice::device_type() const
{
  /* According to the vulkan specifications:
   *
   * If the vendor has a PCI vendor ID, the low 16 bits of vendorID must contain that PCI vendor
   * ID, and the remaining bits must be set to zero. Otherwise, the value returned must be a valid
   * Khronos vendor ID.
   */
  switch (vk_physical_device_properties_.vendorID) {
    case PCI_ID_NVIDIA:
      return GPU_DEVICE_NVIDIA;
    case PCI_ID_INTEL:
      return GPU_DEVICE_INTEL;
    case PCI_ID_AMD:
    case PCI_ID_ATI:
      return GPU_DEVICE_ATI;
    case PCI_ID_APPLE:
      return GPU_DEVICE_APPLE;
    default:
      break;
  }

  return GPU_DEVICE_UNKNOWN;
}

eGPUDriverType VKDevice::driver_type() const
{
  /* It is unclear how to determine the driver type, but it is required to extract the correct
   * driver version. */
  return GPU_DRIVER_ANY;
}

std::string VKDevice::vendor_name() const
{
  /* Below 0x10000 are the PCI vendor IDs (https://pcisig.com/membership/member-companies) */
  if (vk_physical_device_properties_.vendorID < 0x10000) {
    switch (vk_physical_device_properties_.vendorID) {
      case PCI_ID_AMD:
        return "Advanced Micro Devices";
      case PCI_ID_NVIDIA:
        return "NVIDIA Corporation";
      case PCI_ID_INTEL:
        return "Intel Corporation";
      case PCI_ID_APPLE:
        return "Apple";
      default:
        return std::to_string(vk_physical_device_properties_.vendorID);
    }
  }
  else {
    /* above 0x10000 should be vkVendorIDs
     * NOTE: When debug_messaging landed we can use something similar to
     * vk::to_string(vk::VendorId(properties.vendorID));
     */
    return std::to_string(vk_physical_device_properties_.vendorID);
  }
}

std::string VKDevice::driver_version() const
{
  /*
   * NOTE: this depends on the driver type and is currently incorrect. Idea is to use a default per
   * OS.
   */
  const uint32_t driver_version = vk_physical_device_properties_.driverVersion;
  switch (vk_physical_device_properties_.vendorID) {
    case PCI_ID_NVIDIA:
      return std::to_string((driver_version >> 22) & 0x3FF) + "." +
             std::to_string((driver_version >> 14) & 0xFF) + "." +
             std::to_string((driver_version >> 6) & 0xFF) + "." +
             std::to_string(driver_version & 0x3F);
    case PCI_ID_INTEL: {
      const uint32_t major = VK_VERSION_MAJOR(driver_version);
      /* When using Mesa driver we should use VK_VERSION_*. */
      if (major > 30) {
        return std::to_string((driver_version >> 14) & 0x3FFFF) + "." +
               std::to_string(driver_version & 0x3FFF);
      }
      break;
    }
    default:
      break;
  }

  return std::to_string(VK_VERSION_MAJOR(driver_version)) + "." +
         std::to_string(VK_VERSION_MINOR(driver_version)) + "." +
         std::to_string(VK_VERSION_PATCH(driver_version));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name VKThreadData
 * \{ */

VKThreadData::VKThreadData(VKDevice &device,
                           pthread_t thread_id,
                           std::unique_ptr<render_graph::VKCommandBufferInterface> command_buffer,
                           render_graph::VKResourceStateTracker &resources)
    : thread_id(thread_id), render_graph(std::move(command_buffer), resources)
{
  for (VKResourcePool &resource_pool : resource_pools) {
    resource_pool.init(device);
  }
}

void VKThreadData::deinit(VKDevice &device)
{
  for (VKResourcePool &resource_pool : resource_pools) {
    resource_pool.deinit(device);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Resource management
 * \{ */

VKThreadData &VKDevice::current_thread_data()
{
  std::scoped_lock mutex(resources.mutex);
  pthread_t current_thread_id = pthread_self();

  for (VKThreadData *thread_data : thread_data_) {
    if (pthread_equal(thread_data->thread_id, current_thread_id)) {
      return *thread_data;
    }
  }

  VKThreadData *thread_data = new VKThreadData(
      *this,
      current_thread_id,
      std::make_unique<render_graph::VKCommandBufferWrapper>(),
      resources);
  thread_data_.append(thread_data);
  return *thread_data;
}

VKDiscardPool &VKDevice::discard_pool_for_current_thread()
{
  std::scoped_lock mutex(resources.mutex);
  pthread_t current_thread_id = pthread_self();
  for (VKThreadData *thread_data : thread_data_) {
    if (pthread_equal(thread_data->thread_id, current_thread_id)) {
      return thread_data->resource_pool_get().discard_pool;
    }
  }

  return orphaned_data;
}

void VKDevice::context_register(VKContext &context)
{
  contexts_.append(std::reference_wrapper(context));
}

void VKDevice::context_unregister(VKContext &context)
{
  contexts_.remove(contexts_.first_index_of(std::reference_wrapper(context)));
}
Span<std::reference_wrapper<VKContext>> VKDevice::contexts_get() const
{
  return contexts_;
};

void VKDevice::memory_statistics_get(int *r_total_mem_kb, int *r_free_mem_kb) const
{
  VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
  vmaGetHeapBudgets(mem_allocator_get(), budgets);
  VkDeviceSize total_mem = 0;
  VkDeviceSize used_mem = 0;

  for (int memory_heap_index : IndexRange(vk_physical_device_memory_properties_.memoryHeapCount)) {
    const VkMemoryHeap &memory_heap =
        vk_physical_device_memory_properties_.memoryHeaps[memory_heap_index];
    const VmaBudget &budget = budgets[memory_heap_index];

    /* Skip host memory-heaps. */
    if (!bool(memory_heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) {
      continue;
    }

    total_mem += memory_heap.size;
    used_mem += budget.usage;
  }

  *r_total_mem_kb = int(total_mem / 1024);
  *r_free_mem_kb = int((total_mem - used_mem) / 1024);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Debugging/statistics
 * \{ */

void VKDevice::debug_print()
{
  std::ostream &os = std::cout;

  os << "Pipelines\n";
  os << " Graphics: " << pipelines.graphic_pipelines_.size() << "\n";
  os << " Compute: " << pipelines.compute_pipelines_.size() << "\n";
  os << "Descriptor sets\n";
  os << " VkDescriptorSetLayouts: " << descriptor_set_layouts_.size() << "\n";
  os << "\n";
}

/** \} */

}  // namespace blender::gpu
