//
// Copyright 2016 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// RendererVk.cpp:
//    Implements the class methods for RendererVk.
//

#include "libANGLE/renderer/vulkan/RendererVk.h"

// Placing this first seems to solve an intellisense bug.
#include "libANGLE/renderer/vulkan/renderervk_utils.h"

#include <EGL/eglext.h>

#include "common/debug.h"
#include "libANGLE/renderer/driver_utils.h"
#include "libANGLE/renderer/vulkan/CompilerVk.h"
#include "libANGLE/renderer/vulkan/FramebufferVk.h"
#include "libANGLE/renderer/vulkan/TextureVk.h"
#include "libANGLE/renderer/vulkan/VertexArrayVk.h"
#include "platform/Platform.h"

namespace rx
{

namespace
{

VkResult VerifyExtensionsPresent(const std::vector<VkExtensionProperties> &extensionProps,
                                 const std::vector<const char *> &enabledExtensionNames)
{
    // Compile the extensions names into a set.
    std::set<std::string> extensionNames;
    for (const auto &extensionProp : extensionProps)
    {
        extensionNames.insert(extensionProp.extensionName);
    }

    for (const auto &extensionName : enabledExtensionNames)
    {
        if (extensionNames.count(extensionName) == 0)
        {
            return VK_ERROR_EXTENSION_NOT_PRESENT;
        }
    }

    return VK_SUCCESS;
}

VkBool32 VKAPI_CALL DebugReportCallback(VkDebugReportFlagsEXT flags,
                                        VkDebugReportObjectTypeEXT objectType,
                                        uint64_t object,
                                        size_t location,
                                        int32_t messageCode,
                                        const char *layerPrefix,
                                        const char *message,
                                        void *userData)
{
    if ((flags & VK_DEBUG_REPORT_ERROR_BIT_EXT) != 0)
    {
        ANGLEPlatformCurrent()->logError(message);
#if !defined(NDEBUG)
        // Abort the call in Debug builds.
        return VK_TRUE;
#endif
    }
    else if ((flags & VK_DEBUG_REPORT_WARNING_BIT_EXT) != 0)
    {
        ANGLEPlatformCurrent()->logWarning(message);
    }
    else
    {
        ANGLEPlatformCurrent()->logInfo(message);
    }

    return VK_FALSE;
}

}  // anonymous namespace

RendererVk::RendererVk()
    : mCapsInitialized(false),
      mInstance(VK_NULL_HANDLE),
      mEnableValidationLayers(false),
      mDebugReportCallback(VK_NULL_HANDLE),
      mPhysicalDevice(VK_NULL_HANDLE),
      mQueue(VK_NULL_HANDLE),
      mCurrentQueueFamilyIndex(std::numeric_limits<uint32_t>::max()),
      mDevice(VK_NULL_HANDLE),
      mCommandPool(VK_NULL_HANDLE)
{
}

RendererVk::~RendererVk()
{
    if (mDebugReportCallback)
    {
        ASSERT(mInstance);
        auto destroyDebugReportCallback = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(
            vkGetInstanceProcAddr(mInstance, "vkDestroyDebugReportCallbackEXT"));
        ASSERT(destroyDebugReportCallback);
        destroyDebugReportCallback(mInstance, mDebugReportCallback, nullptr);
    }

    mCommandBuffer.reset(nullptr);

    if (mCommandPool)
    {
        vkDestroyCommandPool(mDevice, mCommandPool, nullptr);
        mCommandPool = VK_NULL_HANDLE;
    }

    if (mDevice)
    {
        vkDestroyDevice(mDevice, nullptr);
        mDevice = VK_NULL_HANDLE;
    }

    if (mInstance)
    {
        vkDestroyInstance(mInstance, nullptr);
        mInstance = VK_NULL_HANDLE;
    }
}

vk::Error RendererVk::initialize(const egl::AttributeMap &attribs)
{
    // Gather global layer properties.
    uint32_t instanceLayerCount = 0;
    ANGLE_VK_TRY(vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr));

    std::vector<VkLayerProperties> instanceLayerProps(instanceLayerCount);
    if (instanceLayerCount > 0)
    {
        ANGLE_VK_TRY(
            vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayerProps.data()));
    }

    uint32_t instanceExtensionCount = 0;
    ANGLE_VK_TRY(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr));

    std::vector<VkExtensionProperties> instanceExtensionProps(instanceExtensionCount);
    if (instanceExtensionCount > 0)
    {
        ANGLE_VK_TRY(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount,
                                                            instanceExtensionProps.data()));
    }

#if !defined(NDEBUG)
    // Validation layers enabled by default in Debug.
    mEnableValidationLayers = true;
#endif

    // If specified in the attributes, override the default.
    if (attribs.contains(EGL_PLATFORM_ANGLE_ENABLE_VALIDATION_LAYER_ANGLE))
    {
        mEnableValidationLayers =
            (attribs.get(EGL_PLATFORM_ANGLE_ENABLE_VALIDATION_LAYER_ANGLE, EGL_FALSE) == EGL_TRUE);
    }

    if (mEnableValidationLayers)
    {
        // Verify the standard validation layers are available.
        if (!HasStandardValidationLayer(instanceLayerProps))
        {
            // Generate an error if the attribute was requested, warning otherwise.
            if (attribs.contains(EGL_PLATFORM_ANGLE_ENABLE_VALIDATION_LAYER_ANGLE))
            {
                ANGLEPlatformCurrent()->logError("Vulkan standard validation layers are missing.");
            }
            else
            {
                ANGLEPlatformCurrent()->logWarning(
                    "Vulkan standard validation layers are missing.");
            }
            mEnableValidationLayers = false;
        }
    }

    std::vector<const char *> enabledInstanceExtensions;
    enabledInstanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
#if defined(ANGLE_PLATFORM_WINDOWS)
    enabledInstanceExtensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#else
#error Unsupported Vulkan platform.
#endif  // defined(ANGLE_PLATFORM_WINDOWS)

    // TODO(jmadill): Should be able to continue initialization if debug report ext missing.
    if (mEnableValidationLayers)
    {
        enabledInstanceExtensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
    }

    // Verify the required extensions are in the extension names set. Fail if not.
    ANGLE_VK_TRY(VerifyExtensionsPresent(instanceExtensionProps, enabledInstanceExtensions));

    VkApplicationInfo applicationInfo;
    applicationInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pNext              = nullptr;
    applicationInfo.pApplicationName   = "ANGLE";
    applicationInfo.applicationVersion = 1;
    applicationInfo.pEngineName        = "ANGLE";
    applicationInfo.engineVersion      = 1;
    applicationInfo.apiVersion         = VK_API_VERSION_1_0;

    VkInstanceCreateInfo instanceInfo;
    instanceInfo.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pNext            = nullptr;
    instanceInfo.flags            = 0;
    instanceInfo.pApplicationInfo = &applicationInfo;

    // Enable requested layers and extensions.
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledInstanceExtensions.size());
    instanceInfo.ppEnabledExtensionNames =
        enabledInstanceExtensions.empty() ? nullptr : enabledInstanceExtensions.data();
    instanceInfo.enabledLayerCount = mEnableValidationLayers ? 1u : 0u;
    instanceInfo.ppEnabledLayerNames =
        mEnableValidationLayers ? &g_VkStdValidationLayerName : nullptr;

    ANGLE_VK_TRY(vkCreateInstance(&instanceInfo, nullptr, &mInstance));

    if (mEnableValidationLayers)
    {
        VkDebugReportCallbackCreateInfoEXT debugReportInfo;

        debugReportInfo.sType = VK_STRUCTURE_TYPE_DEBUG_REPORT_CREATE_INFO_EXT;
        debugReportInfo.pNext = nullptr;
        debugReportInfo.flags = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT |
                                VK_DEBUG_REPORT_PERFORMANCE_WARNING_BIT_EXT |
                                VK_DEBUG_REPORT_INFORMATION_BIT_EXT | VK_DEBUG_REPORT_DEBUG_BIT_EXT;
        debugReportInfo.pfnCallback = &DebugReportCallback;
        debugReportInfo.pUserData   = this;

        auto createDebugReportCallback = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(
            vkGetInstanceProcAddr(mInstance, "vkCreateDebugReportCallbackEXT"));
        ASSERT(createDebugReportCallback);
        ANGLE_VK_TRY(
            createDebugReportCallback(mInstance, &debugReportInfo, nullptr, &mDebugReportCallback));
    }

    uint32_t physicalDeviceCount = 0;
    ANGLE_VK_TRY(vkEnumeratePhysicalDevices(mInstance, &physicalDeviceCount, nullptr));
    ANGLE_VK_CHECK(physicalDeviceCount > 0, VK_ERROR_INITIALIZATION_FAILED);

    // TODO(jmadill): Handle multiple physical devices. For now, use the first device.
    physicalDeviceCount = 1;
    ANGLE_VK_TRY(vkEnumeratePhysicalDevices(mInstance, &physicalDeviceCount, &mPhysicalDevice));

    vkGetPhysicalDeviceProperties(mPhysicalDevice, &mPhysicalDeviceProperties);

    // Ensure we can find a graphics queue family.
    uint32_t queueCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &queueCount, nullptr);

    ANGLE_VK_CHECK(queueCount > 0, VK_ERROR_INITIALIZATION_FAILED);

    mQueueFamilyProperties.resize(queueCount);
    vkGetPhysicalDeviceQueueFamilyProperties(mPhysicalDevice, &queueCount,
                                             mQueueFamilyProperties.data());

    size_t graphicsQueueFamilyCount   = false;
    uint32_t firstGraphicsQueueFamily = 0;
    for (uint32_t queueIndex = 0; queueIndex < queueCount; ++queueIndex)
    {
        const auto &queueInfo = mQueueFamilyProperties[queueIndex];
        if ((queueInfo.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            ASSERT(queueInfo.queueCount > 0);
            graphicsQueueFamilyCount++;
            if (firstGraphicsQueueFamily == 0)
            {
                firstGraphicsQueueFamily = queueIndex;
            }
            break;
        }
    }

    ANGLE_VK_CHECK(graphicsQueueFamilyCount > 0, VK_ERROR_INITIALIZATION_FAILED);

    // If only one queue family, go ahead and initialize the device. If there is more than one
    // queue, we'll have to wait until we see a WindowSurface to know which supports present.
    if (graphicsQueueFamilyCount == 1)
    {
        ANGLE_TRY(initializeDevice(firstGraphicsQueueFamily));
    }

    return vk::NoError();
}

vk::Error RendererVk::initializeDevice(uint32_t queueFamilyIndex)
{
    uint32_t deviceLayerCount = 0;
    ANGLE_VK_TRY(vkEnumerateDeviceLayerProperties(mPhysicalDevice, &deviceLayerCount, nullptr));

    std::vector<VkLayerProperties> deviceLayerProps(deviceLayerCount);
    if (deviceLayerCount > 0)
    {
        ANGLE_VK_TRY(vkEnumerateDeviceLayerProperties(mPhysicalDevice, &deviceLayerCount,
                                                      deviceLayerProps.data()));
    }

    uint32_t deviceExtensionCount = 0;
    ANGLE_VK_TRY(vkEnumerateDeviceExtensionProperties(mPhysicalDevice, nullptr,
                                                      &deviceExtensionCount, nullptr));

    std::vector<VkExtensionProperties> deviceExtensionProps(deviceExtensionCount);
    if (deviceExtensionCount > 0)
    {
        ANGLE_VK_TRY(vkEnumerateDeviceExtensionProperties(
            mPhysicalDevice, nullptr, &deviceExtensionCount, deviceExtensionProps.data()));
    }

    if (mEnableValidationLayers)
    {
        if (!HasStandardValidationLayer(deviceLayerProps))
        {
            ANGLEPlatformCurrent()->logWarning("Vulkan standard validation layer is missing.");
            mEnableValidationLayers = false;
        }
    }

    std::vector<const char *> enabledDeviceExtensions;
    enabledDeviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);

    ANGLE_VK_TRY(VerifyExtensionsPresent(deviceExtensionProps, enabledDeviceExtensions));

    VkDeviceQueueCreateInfo queueCreateInfo;

    float zeroPriority = 0.0f;

    queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueCreateInfo.pNext            = nullptr;
    queueCreateInfo.flags            = 0;
    queueCreateInfo.queueFamilyIndex = queueFamilyIndex;
    queueCreateInfo.queueCount       = 1;
    queueCreateInfo.pQueuePriorities = &zeroPriority;

    // Initialize the device
    VkDeviceCreateInfo createInfo;

    createInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    createInfo.pNext                = nullptr;
    createInfo.flags                = 0;
    createInfo.queueCreateInfoCount = 1;
    createInfo.pQueueCreateInfos    = &queueCreateInfo;
    createInfo.enabledLayerCount    = mEnableValidationLayers ? 1u : 0u;
    createInfo.ppEnabledLayerNames =
        mEnableValidationLayers ? &g_VkStdValidationLayerName : nullptr;
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions.size());
    createInfo.ppEnabledExtensionNames =
        enabledDeviceExtensions.empty() ? nullptr : enabledDeviceExtensions.data();
    createInfo.pEnabledFeatures = nullptr;  // TODO(jmadill): features

    ANGLE_VK_TRY(vkCreateDevice(mPhysicalDevice, &createInfo, nullptr, &mDevice));

    mCurrentQueueFamilyIndex = queueFamilyIndex;

    vkGetDeviceQueue(mDevice, mCurrentQueueFamilyIndex, 0, &mQueue);

    // Initialize the command pool now that we know the queue family index.
    VkCommandPoolCreateInfo commandPoolInfo;
    commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolInfo.pNext = nullptr;
    // TODO(jmadill): Investigate transient command buffers.
    commandPoolInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolInfo.queueFamilyIndex = mCurrentQueueFamilyIndex;

    ANGLE_VK_TRY(vkCreateCommandPool(mDevice, &commandPoolInfo, nullptr, &mCommandPool));

    mCommandBuffer.reset(new vk::CommandBuffer(mDevice, mCommandPool));

    return vk::NoError();
}

vk::Error RendererVk::selectGraphicsQueue()
{
    uint32_t queueCount = static_cast<uint32_t>(mQueueFamilyProperties.size());
    for (uint32_t queueIndex = 0; queueIndex < queueCount; ++queueIndex)
    {
        const auto &queueInfo = mQueueFamilyProperties[queueIndex];
        if ((queueInfo.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            ANGLE_TRY(initializeDevice(queueIndex));
            return vk::NoError();
        }
    }

    return ANGLE_VK_UNREACHABLE();
}

vk::ErrorOrResult<uint32_t> RendererVk::selectPresentQueueForSurface(VkSurfaceKHR surface)
{
    // We've already initialized a device, and can't re-create it unless it's never been used.
    // TODO(jmadill): Handle the re-creation case if necessary.
    if (mDevice != VK_NULL_HANDLE)
    {
        ASSERT(mCurrentQueueFamilyIndex != std::numeric_limits<uint32_t>::max());

        // Check if the current device supports present on this surface.
        VkBool32 supportsPresent = VK_FALSE;
        ANGLE_VK_TRY(vkGetPhysicalDeviceSurfaceSupportKHR(mPhysicalDevice, mCurrentQueueFamilyIndex,
                                                          surface, &supportsPresent));

        return (supportsPresent == VK_TRUE);
    }

    // Find a graphics and present queue.
    Optional<uint32_t> newPresentQueue;
    uint32_t queueCount = static_cast<uint32_t>(mQueueFamilyProperties.size());
    for (uint32_t queueIndex = 0; queueIndex < queueCount; ++queueIndex)
    {
        const auto &queueInfo = mQueueFamilyProperties[queueIndex];
        if ((queueInfo.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            VkBool32 supportsPresent = VK_FALSE;
            ANGLE_VK_TRY(vkGetPhysicalDeviceSurfaceSupportKHR(mPhysicalDevice, queueIndex, surface,
                                                              &supportsPresent));

            if (supportsPresent == VK_TRUE)
            {
                newPresentQueue = queueIndex;
                break;
            }
        }
    }

    ANGLE_VK_CHECK(newPresentQueue.valid(), VK_ERROR_INITIALIZATION_FAILED);
    ANGLE_TRY(initializeDevice(newPresentQueue.value()));

    return newPresentQueue.value();
}

std::string RendererVk::getVendorString() const
{
    switch (mPhysicalDeviceProperties.vendorID)
    {
        case VENDOR_ID_AMD:
            return "Advanced Micro Devices";
        case VENDOR_ID_NVIDIA:
            return "NVIDIA";
        case VENDOR_ID_INTEL:
            return "Intel";
        default:
        {
            // TODO(jmadill): More vendor IDs.
            std::stringstream strstr;
            strstr << "Vendor ID: " << mPhysicalDeviceProperties.vendorID;
            return strstr.str();
        }
    }
}

std::string RendererVk::getRendererDescription() const
{
    std::stringstream strstr;

    uint32_t apiVersion = mPhysicalDeviceProperties.apiVersion;

    strstr << "Vulkan ";
    strstr << VK_VERSION_MAJOR(apiVersion) << ".";
    strstr << VK_VERSION_MINOR(apiVersion) << ".";
    strstr << VK_VERSION_PATCH(apiVersion);

    strstr << "(" << mPhysicalDeviceProperties.deviceName << ")";

    return strstr.str();
}

void RendererVk::ensureCapsInitialized() const
{
    if (!mCapsInitialized)
    {
        generateCaps(&mNativeCaps, &mNativeTextureCaps, &mNativeExtensions, &mNativeLimitations);
        mCapsInitialized = true;
    }
}

void RendererVk::generateCaps(gl::Caps * /*outCaps*/,
                              gl::TextureCapsMap * /*outTextureCaps*/,
                              gl::Extensions * /*outExtensions*/,
                              gl::Limitations * /* outLimitations */) const
{
    // TODO(jmadill): Caps.
}

const gl::Caps &RendererVk::getNativeCaps() const
{
    ensureCapsInitialized();
    return mNativeCaps;
}

const gl::TextureCapsMap &RendererVk::getNativeTextureCaps() const
{
    ensureCapsInitialized();
    return mNativeTextureCaps;
}

const gl::Extensions &RendererVk::getNativeExtensions() const
{
    ensureCapsInitialized();
    return mNativeExtensions;
}

const gl::Limitations &RendererVk::getNativeLimitations() const
{
    ensureCapsInitialized();
    return mNativeLimitations;
}

vk::CommandBuffer *RendererVk::getCommandBuffer()
{
    return mCommandBuffer.get();
}

vk::Error RendererVk::queueAndFinishCommandBuffer(const vk::CommandBuffer &commandBuffer,
                                                  uint64_t timeoutMS)
{
    VkFenceCreateInfo fenceInfo;
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.pNext = nullptr;
    fenceInfo.flags = 0;

    VkFence commandFence = VK_NULL_HANDLE;
    ANGLE_VK_TRY(vkCreateFence(mDevice, &fenceInfo, nullptr, &commandFence));

    VkCommandBuffer commandBufferHandle = commandBuffer.getHandle();

    VkSubmitInfo submitInfo;
    submitInfo.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.pNext                = nullptr;
    submitInfo.waitSemaphoreCount   = 0;
    submitInfo.pWaitSemaphores      = nullptr;
    submitInfo.commandBufferCount   = 1;
    submitInfo.pCommandBuffers      = &commandBufferHandle;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores    = nullptr;

    ANGLE_VK_TRY(vkQueueSubmit(mQueue, 1, &submitInfo, commandFence));
    ANGLE_VK_TRY(vkWaitForFences(mDevice, 1, &commandFence, VK_TRUE, timeoutMS * 1000ull));
    vkDestroyFence(mDevice, commandFence, nullptr);

    return vk::NoError();
}

}  // namespace rx
