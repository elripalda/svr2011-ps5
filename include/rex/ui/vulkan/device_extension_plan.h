#pragma once
// Optional device functionality for stutter-free pipelines and pacing: graphics pipeline
// libraries, the Vulkan 1.3 extended dynamic states and present-wait (v2, then v1).
// Free of Vulkan types so the choice can be tested without a GPU.

#include <string>
#include <vector>

namespace rex {
namespace ui {
namespace vulkan {

// Extensions the device (and, for present_id2, the instance) lists.
struct DeviceOffer {
  bool vulkan_1_3 = false;
  bool surface_capabilities2 = false;      // instance: VK_KHR_get_surface_capabilities2
  bool pipeline_library = false;           // VK_KHR_pipeline_library
  bool graphics_pipeline_library = false;  // VK_EXT_graphics_pipeline_library
  bool present_id2 = false;                // VK_KHR_present_id2
  bool present_wait2 = false;              // VK_KHR_present_wait2
  bool present_id = false;                 // VK_KHR_present_id
  bool present_wait = false;               // VK_KHR_present_wait
};

// Extensions to enable.
struct DeviceRequest {
  bool pipeline_library = false;
  bool graphics_pipeline_library = false;
  bool present_id2 = false;
  bool present_wait2 = false;
  bool present_id = false;
  bool present_wait = false;
};

// `forced_off` is the developer switch that tests the paths for devices without them.
inline DeviceRequest RequestOptionalExtensions(const DeviceOffer& offer, bool with_gpu_emulation,
                                               bool with_swapchain, bool forced_off) {
  DeviceRequest request;
  if (forced_off) {
    return request;
  }
  if (with_gpu_emulation) {
    // VK_EXT_graphics_pipeline_library requires VK_KHR_pipeline_library.
    request.graphics_pipeline_library = offer.pipeline_library && offer.graphics_pipeline_library;
    request.pipeline_library = request.graphics_pipeline_library;
  }
  if (with_swapchain) {
    // Waiting needs the ids; present_id2 needs VK_KHR_get_surface_capabilities2.
    request.present_id2 = request.present_wait2 =
        offer.surface_capabilities2 && offer.present_id2 && offer.present_wait2;
    request.present_id = request.present_wait = offer.present_id && offer.present_wait;
  }
  return request;
}

// Feature bits and properties the driver reports for the requested extensions.
struct DeviceFeatureOffer {
  bool graphicsPipelineLibrary = false;
  bool graphicsPipelineLibraryFastLinking = false;
  bool graphicsPipelineLibraryIndependentInterpolationDecoration = false;
  bool presentId2 = false;
  bool presentWait2 = false;
  bool presentId = false;
  bool presentWait = false;
};

// What the device enables and the renderers may use.
struct OptionalFeatures {
  bool graphicsPipelineLibrary = false;
  bool graphicsPipelineLibraryFastLinking = false;
  bool graphicsPipelineLibraryIndependentInterpolationDecoration = false;
  // EDS1 and the core part of EDS2, both core in Vulkan 1.3.
  bool extendedDynamicState = false;
  bool presentId2 = false;
  bool presentWait2 = false;
  // v1 beside v2, for surfaces without v2.
  bool presentId = false;
  bool presentWait = false;
};

inline OptionalFeatures EnableOptionalFeatures(const DeviceRequest& request,
                                               const DeviceFeatureOffer& offer, bool vulkan_1_3,
                                               bool forced_off) {
  OptionalFeatures f;
  f.graphicsPipelineLibrary = request.graphics_pipeline_library && offer.graphicsPipelineLibrary;
  f.graphicsPipelineLibraryFastLinking =
      f.graphicsPipelineLibrary && offer.graphicsPipelineLibraryFastLinking;
  f.graphicsPipelineLibraryIndependentInterpolationDecoration =
      f.graphicsPipelineLibrary && offer.graphicsPipelineLibraryIndependentInterpolationDecoration;
  f.extendedDynamicState = vulkan_1_3 && !forced_off;
  f.presentId2 = f.presentWait2 = request.present_id2 && request.present_wait2 &&
                                  offer.presentId2 && offer.presentWait2;
  f.presentId = f.presentWait =
      request.present_id && request.present_wait && offer.presentId && offer.presentWait;
  return f;
}

// One "name: on|off" line per feature; present ids are implied by their waits.
inline std::vector<std::string> DescribeOptionalFeatures(const OptionalFeatures& f) {
  const auto line = [](const char* name, bool on) {
    return std::string(name) + (on ? ": on" : ": off");
  };
  return {
      line("graphicsPipelineLibrary", f.graphicsPipelineLibrary),
      line("graphicsPipelineLibraryFastLinking", f.graphicsPipelineLibraryFastLinking),
      line("graphicsPipelineLibraryIndependentInterpolationDecoration",
           f.graphicsPipelineLibraryIndependentInterpolationDecoration),
      line("extendedDynamicState", f.extendedDynamicState),
      line("presentWait2", f.presentWait2),
      line("presentWait", f.presentWait),
  };
}

}  // namespace vulkan
}  // namespace ui
}  // namespace rex
