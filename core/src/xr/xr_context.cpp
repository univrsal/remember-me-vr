#include <cmath>
#include <cstring>
#include <sstream>

#include <openxr/openxr_reflection.h>

#include "vrcore/log.h"
#include "vrcore/xr.h"

namespace vrcore::xr {
namespace {

const char* StateName(XrSessionState state) {
  switch (state) {
#define VR_XR_ENUM_CASE(name, value) \
  case name:                         \
    return #name;
    XR_LIST_ENUM_XrSessionState(VR_XR_ENUM_CASE)
#undef VR_XR_ENUM_CASE
    default:
      return "XR_SESSION_STATE_?";
  }
}

std::vector<std::string> SplitSpaces(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string item; in >> item;) out.push_back(item);
  return out;
}

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
  const XrQuaternionf p{v.x, v.y, v.z, 0.0f};
  const XrQuaternionf conj{-q.x, -q.y, -q.z, q.w};
  const XrQuaternionf r = Multiply(Multiply(q, p), conj);
  return {r.x, r.y, r.z};
}

}  // namespace

const char* ResultName(XrResult result) {
  switch (result) {
#define VR_XR_ENUM_CASE(name, value) \
  case name:                         \
    return #name;
    XR_LIST_ENUM_XrResult(VR_XR_ENUM_CASE)
#undef VR_XR_ENUM_CASE
    default:
      return "XR_RESULT_?";
  }
}

bool Context::Init(const char* appName, const DxvkInterop& dxvk) {
  Shutdown();
  if (!CreateInstance(appName) || !CreateSession(dxvk)) {
    Shutdown();
    return false;
  }
  return true;
}

bool Context::CreateInstance(const char* appName) {
  getProcAddr_ = LoadRuntime();
  if (!getProcAddr_) return false;

  auto enumerateExtensions = Proc<PFN_xrEnumerateInstanceExtensionProperties>("xrEnumerateInstanceExtensionProperties");
  auto createInstance = Proc<PFN_xrCreateInstance>("xrCreateInstance");
  if (!enumerateExtensions || !createInstance) {
    VR_LOG_ERROR("xr: runtime is missing global functions");
    return false;
  }

  uint32_t count = 0;
  enumerateExtensions(nullptr, 0, &count, nullptr);
  std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
  enumerateExtensions(nullptr, count, &count, props.data());
  bool hasVulkan = false;
  for (const auto& p : props) {
    VR_LOG_TRACE("xr: runtime extension %s v%u", p.extensionName, p.extensionVersion);
    hasVulkan |= std::strcmp(p.extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME) == 0;
  }
  if (!hasVulkan) {
    VR_LOG_ERROR("xr: runtime lacks %s", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    return false;
  }

  const char* extensions[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
  XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
  std::strncpy(info.applicationInfo.applicationName, appName, XR_MAX_APPLICATION_NAME_SIZE - 1);
  std::strncpy(info.applicationInfo.engineName, "vrcore", XR_MAX_ENGINE_NAME_SIZE - 1);
  info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  info.enabledExtensionCount = 1;
  info.enabledExtensionNames = extensions;
  XrResult result = createInstance(&info, &instance_);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: xrCreateInstance failed: %s (is SteamVR running?)", ResultName(result));
    instance_ = XR_NULL_HANDLE;
    return false;
  }

#define VR_XR_LOAD(name)                                     \
  fn.name = Proc<PFN_xr##name>("xr" #name);                  \
  if (!fn.name) {                                            \
    VR_LOG_ERROR("xr: runtime is missing xr" #name);         \
    return false;                                            \
  }
  VR_XR_LOAD(DestroyInstance)
  VR_XR_LOAD(PollEvent)
  VR_XR_LOAD(BeginSession)
  VR_XR_LOAD(EndSession)
  VR_XR_LOAD(DestroySession)
  VR_XR_LOAD(CreateReferenceSpace)
  VR_XR_LOAD(DestroySpace)
  VR_XR_LOAD(LocateSpace)
  VR_XR_LOAD(WaitFrame)
  VR_XR_LOAD(BeginFrame)
  VR_XR_LOAD(EndFrame)
  VR_XR_LOAD(CreateSwapchain)
  VR_XR_LOAD(DestroySwapchain)
  VR_XR_LOAD(EnumerateSwapchainImages)
  VR_XR_LOAD(AcquireSwapchainImage)
  VR_XR_LOAD(WaitSwapchainImage)
  VR_XR_LOAD(ReleaseSwapchainImage)
  VR_XR_LOAD(EnumerateSwapchainFormats)
#undef VR_XR_LOAD

  XrInstanceProperties instanceProps{XR_TYPE_INSTANCE_PROPERTIES};
  if (auto getProps = Proc<PFN_xrGetInstanceProperties>("xrGetInstanceProperties");
      getProps && XR_SUCCEEDED(getProps(instance_, &instanceProps))) {
    VR_LOG_INFO("xr: runtime %s %u.%u.%u", instanceProps.runtimeName,
                XR_VERSION_MAJOR(instanceProps.runtimeVersion), XR_VERSION_MINOR(instanceProps.runtimeVersion),
                XR_VERSION_PATCH(instanceProps.runtimeVersion));
  }

  auto getSystem = Proc<PFN_xrGetSystem>("xrGetSystem");
  XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  result = getSystem ? getSystem(instance_, &systemInfo, &system_) : XR_ERROR_FUNCTION_UNSUPPORTED;
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: no HMD available: %s (headset connected and SteamVR running?)", ResultName(result));
    return false;
  }
  XrSystemProperties systemProps{XR_TYPE_SYSTEM_PROPERTIES};
  if (auto getSystemProps = Proc<PFN_xrGetSystemProperties>("xrGetSystemProperties");
      getSystemProps && XR_SUCCEEDED(getSystemProps(instance_, system_, &systemProps))) {
    VR_LOG_INFO("xr: system \"%s\" (vendor 0x%x), max swapchain %ux%u, %u layers", systemProps.systemName,
                systemProps.vendorId, systemProps.graphicsProperties.maxSwapchainImageWidth,
                systemProps.graphicsProperties.maxSwapchainImageHeight, systemProps.graphicsProperties.maxLayerCount);
  }
  return true;
}

bool Context::CreateSession(const DxvkInterop& dxvk) {
  // XR_KHR_vulkan_enable (v1) lets us hand over the VkDevice DXVK already created.
  auto getRequirements = Proc<PFN_xrGetVulkanGraphicsRequirementsKHR>("xrGetVulkanGraphicsRequirementsKHR");
  auto getGraphicsDevice = Proc<PFN_xrGetVulkanGraphicsDeviceKHR>("xrGetVulkanGraphicsDeviceKHR");
  auto getInstanceExts = Proc<PFN_xrGetVulkanInstanceExtensionsKHR>("xrGetVulkanInstanceExtensionsKHR");
  auto getDeviceExts = Proc<PFN_xrGetVulkanDeviceExtensionsKHR>("xrGetVulkanDeviceExtensionsKHR");
  if (!getRequirements || !getGraphicsDevice) {
    VR_LOG_ERROR("xr: XR_KHR_vulkan_enable functions missing");
    return false;
  }

  XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
  XrResult result = getRequirements(instance_, system_, &requirements);  // must be called before xrCreateSession
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: xrGetVulkanGraphicsRequirementsKHR failed: %s", ResultName(result));
    return false;
  }
  VR_LOG_DEBUG("xr: Vulkan API supported %u.%u - %u.%u", XR_VERSION_MAJOR(requirements.minApiVersionSupported),
               XR_VERSION_MINOR(requirements.minApiVersionSupported),
               XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
               XR_VERSION_MINOR(requirements.maxApiVersionSupported));

  // Diagnostics: DXVK enables the runtime's required extensions only if the
  // runtime was reachable when DXVK created its Vulkan instance.
  auto listExtensions = [&](auto fnPtr, const char* what) -> std::vector<std::string> {
    if (!fnPtr) return {};
    uint32_t len = 0;
    fnPtr(instance_, system_, 0, &len, nullptr);
    std::string buffer(len, '\0');
    if (len == 0 || XR_FAILED(fnPtr(instance_, system_, len, &len, buffer.data()))) return {};
    buffer.resize(std::strlen(buffer.c_str()));
    VR_LOG_DEBUG("xr: runtime requires Vulkan %s extensions: %s", what, buffer.c_str());
    return SplitSpaces(buffer);
  };
  const auto requiredInstance = listExtensions(getInstanceExts, "instance");
  listExtensions(getDeviceExts, "device");
  const auto& dxvkInstanceExts = DxvkInstanceExtensions();
  if (!dxvkInstanceExts.empty()) {
    for (const auto& ext : requiredInstance) {
      bool found = false;
      for (const auto& have : dxvkInstanceExts) found |= have == ext;
      if (!found) {
        VR_LOG_WARN("xr: DXVK's VkInstance lacks %s; was SteamVR started before the game?", ext.c_str());
      }
    }
  }

  VkPhysicalDevice xrPhysicalDevice = VK_NULL_HANDLE;
  result = getGraphicsDevice(instance_, system_, dxvk.instance, &xrPhysicalDevice);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: xrGetVulkanGraphicsDeviceKHR failed: %s", ResultName(result));
    return false;
  }
  if (xrPhysicalDevice != dxvk.physicalDevice) {
    VR_LOG_WARN("xr: runtime wants VkPhysicalDevice %p but DXVK uses %p; continuing",
                static_cast<void*>(xrPhysicalDevice), static_cast<void*>(dxvk.physicalDevice));
  }

  XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
  binding.instance = dxvk.instance;
  binding.physicalDevice = dxvk.physicalDevice;
  binding.device = dxvk.vkDevice;
  binding.queueFamilyIndex = dxvk.queueFamilyIndex;
  binding.queueIndex = dxvk.queueIndex;
  XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
  sessionInfo.next = &binding;
  sessionInfo.systemId = system_;
  auto createSession = Proc<PFN_xrCreateSession>("xrCreateSession");
  result = createSession ? createSession(instance_, &sessionInfo, &session_) : XR_ERROR_FUNCTION_UNSUPPORTED;
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: xrCreateSession failed: %s", ResultName(result));
    session_ = XR_NULL_HANDLE;
    return false;
  }
  VR_LOG_INFO("xr: session created on DXVK's VkDevice (queue family %u, index %u)", dxvk.queueFamilyIndex,
              dxvk.queueIndex);

  uint32_t count = 0;
  fn.EnumerateSwapchainFormats(session_, 0, &count, nullptr);
  formats_.resize(count);
  fn.EnumerateSwapchainFormats(session_, count, &count, formats_.data());
  std::string list;
  for (int64_t f : formats_) list += std::to_string(f) + " ";
  VR_LOG_DEBUG("xr: swapchain formats (VkFormat): %s", list.c_str());

  XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  viewInfo.poseInReferenceSpace.orientation.w = 1.0f;
  if (XR_FAILED(fn.CreateReferenceSpace(session_, &viewInfo, &viewSpace_))) return false;
  XrPosef identity{};
  identity.orientation.w = 1.0f;
  return CreateLocalSpace(identity);
}

bool Context::CreateLocalSpace(const XrPosef& pose) {
  XrSpace space = XR_NULL_HANDLE;
  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  info.poseInReferenceSpace = pose;
  const XrResult result = fn.CreateReferenceSpace(session_, &info, &space);
  if (XR_FAILED(result)) {
    VR_LOG_ERROR("xr: creating LOCAL space failed: %s", ResultName(result));
    return false;
  }
  if (localSpace_ != XR_NULL_HANDLE) fn.DestroySpace(localSpace_);
  localSpace_ = space;
  return true;
}

void Context::Recenter(XrTime time) {
  if (session_ == XR_NULL_HANDLE || time == 0) return;
  // Locate the head relative to the raw LOCAL origin, then rebase our space on
  // its position and yaw (ignoring pitch/roll so the screen stays upright).
  XrSpace raw = XR_NULL_HANDLE;
  XrReferenceSpaceCreateInfo rawInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rawInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rawInfo.poseInReferenceSpace.orientation.w = 1.0f;
  if (XR_FAILED(fn.CreateReferenceSpace(session_, &rawInfo, &raw))) return;
  XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
  const XrResult result = fn.LocateSpace(viewSpace_, raw, time, &head);
  fn.DestroySpace(raw);
  const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  if (XR_FAILED(result) || (head.locationFlags & needed) != needed) {
    VR_LOG_WARN("xr: recenter skipped, head pose not valid");
    return;
  }
  const XrVector3f forward = Rotate(head.pose.orientation, {0.0f, 0.0f, -1.0f});
  const float yaw = std::atan2(-forward.x, -forward.z);
  XrPosef pose{};
  pose.orientation = {0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
  pose.position = head.pose.position;
  if (CreateLocalSpace(pose)) VR_LOG_INFO("xr: recentered (yaw %.1f deg)", yaw * 57.29578f);
}

bool Context::PollEvents() {
  if (instance_ == XR_NULL_HANDLE) return false;
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (fn.PollEvent(instance_, &event) == XR_SUCCESS) {
    switch (event.type) {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
        const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
        state_ = changed.state;
        VR_LOG_INFO("xr: session state -> %s", StateName(state_));
        if (state_ == XR_SESSION_STATE_READY) {
          XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
          begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          const XrResult result = fn.BeginSession(session_, &begin);
          running_ = XR_SUCCEEDED(result);
          if (!running_) VR_LOG_ERROR("xr: xrBeginSession failed: %s", ResultName(result));
        } else if (state_ == XR_SESSION_STATE_STOPPING) {
          fn.EndSession(session_);
          running_ = false;
        } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
          running_ = false;
          return false;
        }
        break;
      }
      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        VR_LOG_WARN("xr: instance loss pending (runtime shutting down)");
        running_ = false;
        return false;
      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        VR_LOG_INFO("xr: reference space change pending");
        break;
      default:
        break;
    }
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
  return true;
}

void Context::Shutdown() {
  if (instance_ == XR_NULL_HANDLE) {
    getProcAddr_ = nullptr;
    return;
  }
  if (localSpace_ != XR_NULL_HANDLE) fn.DestroySpace(localSpace_);
  if (viewSpace_ != XR_NULL_HANDLE) fn.DestroySpace(viewSpace_);
  if (session_ != XR_NULL_HANDLE) fn.DestroySession(session_);
  if (fn.DestroyInstance) fn.DestroyInstance(instance_);
  localSpace_ = viewSpace_ = XR_NULL_HANDLE;
  session_ = XR_NULL_HANDLE;
  instance_ = XR_NULL_HANDLE;
  system_ = XR_NULL_SYSTEM_ID;
  state_ = XR_SESSION_STATE_UNKNOWN;
  running_ = false;
  formats_.clear();
  fn = {};
  getProcAddr_ = nullptr;
}

}  // namespace vrcore::xr
