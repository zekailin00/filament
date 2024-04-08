#ifndef TNT_FILAMENT_BACKEND_PLATFORMS_VULKANOPENXRPLATFORM_H
#define TNT_FILAMENT_BACKEND_PLATFORMS_VULKANOPENXRPLATFORM_H

#include "backend/platforms/VulkanPlatform.h"
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace filament::backend {


class OpenxrSession;
class VulkanOpenxrPlatform : public VulkanPlatform
{
public:
    // /**
    //  * @brief Get an instance of the OpenXR platform.
    //  * If openxr instance cannot be created, NULL is returned.
    //  * @return OpenxrPlatform* 
    //  */
    static VulkanOpenxrPlatform* Initialize();

    OpenxrSession* NewSession();
    void Destroy();

    void PollEvents();
    void PollActions();

    std::vector<const char*> GetVkInstanceExt() {return vulkanInstanceExt;}
    std::vector<const char*> GetVkDeviceExt() {return vulkanDeviceExt;}

    const VulkanOpenxrPlatform& operator=(const VulkanOpenxrPlatform&) = delete;
    VulkanOpenxrPlatform(const VulkanOpenxrPlatform&) = delete;

    ~VulkanOpenxrPlatform() override = default;

    friend OpenxrSession;

private:
    VulkanOpenxrPlatform() = default;

    bool TryReadNextEvent(XrEventDataBuffer* eventDataBuffer);

    void InitializeActions();
    void LoadViewConfig();
    void LoadVulkanRequirements();
    std::vector<const char*> ParseExtensionString(char* names);

private:
    //------- Instance data -------//
    std::vector<XrApiLayerProperties> layerList{};
    std::vector<XrExtensionProperties> extensionList{};
    std::vector<XrViewConfigurationType> viewConfigTypeList{};
    std::vector<XrViewConfigurationView> viewConfigViewList{};

    XrGraphicsRequirementsVulkanKHR vkRequirements{};
    std::vector<char> vulkanInstanceExtStr;
    std::vector<const char*> vulkanInstanceExt{};
    std::vector<char> vulkanDeviceExtStr;
    std::vector<const char*> vulkanDeviceExt{};

    XrInstance xrInstance = XR_NULL_HANDLE;
    XrSystemId xrSystemId = XR_NULL_SYSTEM_ID;
    XrActionSet inputActionSet = XR_NULL_HANDLE;
    OpenxrSession* activeSession = nullptr;

    //--------- Actions ----------//
    XrAction lSqueezeValueAction, rSqueezeValueAction;
    XrAction lTriggerValueAction, rTriggerValueAction;
    XrAction lTriggerTouchAction, rTriggerTouchAction;

    XrAction lThumbstickXAction, rThumbstickXAction;
    XrAction lThumbstickYAction, rThumbstickYAction;
    XrAction lThumbstickClickAction, rThumbstickClickAction;
    XrAction lThumbstickTouchAction, rThumbstickTouchAction;

    XrAction lXClickAction, lXTouchAction, lYClickAction, lYTouchAction;
    XrAction rAClickAction, rATouchAction, rBClickAction, rBTouchAction;
    XrAction lMenuClickAction, rSystemClickAction;

    XrAction lGripPoseAction, rGripPoseAction;
    XrAction lAimPoseAction, rAimPoseAction;
};


class OpenxrSession
{
public:
    void SetOpenxrContext(VulkanOpenxrPlatform* platform)
    {
        this->platform = platform;
    }

    void SetSessionState(XrSessionState newState);
    bool ShouldCloseSession();
    void RequestCloseSession();
    bool IsSessionRunning();

    void BeginFrame();
    void EndFrame();

    XrSessionState GetSessionState() {return sessionState;}
    XrSession GetSession() {return xrSession;}

    // XrSpace GetViewSpace() {return viewSpace;}
    // XrSpace GetLocalSpace() {return localSpace;}
    // XrSpace GetStageSpace() {return stageSpace;}

    // XrSpace GetLGripPoseSpace() {return lGripPoseSpace;}
    // XrSpace GetRGripPoseSpace() {return rGripPoseSpace;}
    // XrSpace GetLAimPoseSpace() {return lAimPoseSpace;}
    // XrSpace GetRAimPoseSpace() {return rAimPoseSpace;}

public:
    // // Swapchain interface
    // VkFormat GetImageFormat() override;
    // uint32_t GetImageCount() override;
    // uint32_t GetWidth() override;
    // uint32_t GetHeight() override;

    // VkImage GetImage(int index) override;
    // VkImageView GetImageView(int index) override;
    // VkFramebuffer* GetFramebuffer(int index) override;

    void Initialize(VulkanDevice* vulkanDevice);
    void Destroy(VulkanDevice* vulkanDevice);

    // uint32_t GetNextImageIndex(VulkanDevice* vulkanDevice,
    //     VkSemaphore imageAcquiredSemaphores) override;
    // void PresentImage(VulkanDevice* vulkanDevice,
    //     VkSemaphore renderFinishedSemaphores, uint32_t imageIndex) override;
    // bool ShouldRender() override;

    // void RebuildSwapchain(VulkanDevice* vulkanDevice) override;

private:
    // Helper methods
    void CreateSession(VulkanDevice* vukanDevice);
    void InitializeSpaces();
    bool SelectImageFormat(VkFormat format);
    void PrintErrorMsg(XrResult result);

private:    
    VulkanOpenxrPlatform* platform = nullptr;


    XrSession xrSession = XR_NULL_HANDLE;
    std::vector<XrSwapchain> swapchainList{};
    std::vector<VkImage> images{};
    std::vector<VkImageView> imageViews{};
    std::vector<VkFramebuffer> framebuffers{};

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;

    int64_t imageFormat = 0;
    uint32_t totalImageCount = 0; //Total images (per eye * 2)
    uint32_t imageCount = 0;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;


    XrSpace viewSpace = VK_NULL_HANDLE;
    XrSpace localSpace = VK_NULL_HANDLE;
    XrSpace stageSpace = VK_NULL_HANDLE;

    XrSpace lGripPoseSpace = VK_NULL_HANDLE;
    XrSpace rGripPoseSpace = VK_NULL_HANDLE;
    XrSpace lAimPoseSpace = VK_NULL_HANDLE;
    XrSpace rAimPoseSpace = VK_NULL_HANDLE;


    // Render loop context
    // reset when enter XR_SESSION_STATE_READY
    // index 0 = left eye, index 1 = right eye
    XrFrameState frameState{};
    XrCompositionLayerProjection layer;
    XrCompositionLayerProjectionView layerViews[2]; 
    XrView views[2];
    int eye;
};

}// namespace filament::backend

#endif TNT_FILAMENT_BACKEND_PLATFORMS_VULKANOPENXRPLATFORM_H