#ifndef TNT_FILAMENT_BACKEND_PLATFORMS_VULKANOPENXRPLATFORM_H
#define TNT_FILAMENT_BACKEND_PLATFORMS_VULKANOPENXRPLATFORM_H

#include <backend/platforms/VulkanPlatform.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <filament/Engine.h>
#include <atomic>
#include <shared_mutex>
#include "private\backend\CommandStream.h"


namespace filament {
class FEngine; // sets command stream

namespace backend {

class OpenxrSession;
struct VulkanPlatformOpenxrSwapChain;

class VulkanOpenxrPlatform : public VulkanPlatform
{
public:
    // /**
    //  * @brief Get an instance of the OpenXR platform.
    //  * If openxr instance cannot be created, NULL is returned.
    //  * @return OpenxrPlatform* 
    //  */
    static VulkanOpenxrPlatform* Initialize();

    OpenxrSession* CreateSession();
    OpenxrSession* GetActiveSession() {return activeSession;}
    void DestroySession(OpenxrSession*& openxrSession);

    void PollEvents();

    const VulkanOpenxrPlatform& operator=(const VulkanOpenxrPlatform&) = delete;
    VulkanOpenxrPlatform(const VulkanOpenxrPlatform&) = delete;

    ~VulkanOpenxrPlatform() override {
        xrDestroyActionSet(inputActionSet);
        xrDestroyInstance(xrInstance);
    }

    virtual SwapChainPtr createSwapChain(void* nativeWindow,
        uint64_t flags = 0, VkExtent2D extent = {0, 0}) override;
    virtual SwapChainBundle getSwapChainBundle(SwapChainPtr handle) override;
    virtual VkResult acquire(SwapChainPtr handle,
        VkSemaphore clientSignal, uint32_t* index) override;
    virtual VkResult present(SwapChainPtr handle,
        uint32_t index, VkSemaphore finishedDrawing) override;
    virtual bool hasResized(SwapChainPtr handle) override;
    virtual VkResult recreate(SwapChainPtr handle) override;
    virtual void destroy(SwapChainPtr handle) override;

protected:
    ExtensionSet getInstanceExtensions() override;
    ExtensionSet getDeviceExtensions(VkPhysicalDevice device) override;

private:
    VulkanOpenxrPlatform() = default;

    bool TryReadNextEvent(XrEventDataBuffer* eventDataBuffer);

    void InitializeActions();
    void LoadViewConfig();
    void LoadVulkanRequirements();
    ExtensionSet ParseExtensionString(char* names);

private:
    //------- Instance data -------//
    std::vector<XrApiLayerProperties> layerList{};
    std::vector<XrExtensionProperties> extensionList{};
    std::vector<XrViewConfigurationType> viewConfigTypeList{};
    std::vector<XrViewConfigurationView> viewConfigViewList{};

    XrGraphicsRequirementsVulkanKHR vkRequirements{};
    std::vector<char> vulkanInstanceExtStr;
    ExtensionSet vulkanInstanceExt{};
    std::vector<char> vulkanDeviceExtStr;
    ExtensionSet vulkanDeviceExt{};
    VkExtent2D extent;

    XrInstance xrInstance = XR_NULL_HANDLE;
    XrSystemId xrSystemId = XR_NULL_SYSTEM_ID;
    XrActionSet inputActionSet = XR_NULL_HANDLE;
    OpenxrSession* activeSession = nullptr;

    friend OpenxrSession;
    friend FEngine;
    CommandStream* driverApi = nullptr;

    //--------- Actions ----------//
    XrAction lSqueezeValueAction, rSqueezeValueAction;
    XrAction lTriggerValueAction, rTriggerValueAction;
    XrAction lTriggerTouchAction, rTriggerTouchAction;

    XrAction lThumbstickXAction,     rThumbstickXAction;
    XrAction lThumbstickYAction,     rThumbstickYAction;
    XrAction lThumbstickClickAction, rThumbstickClickAction;
    XrAction lThumbstickTouchAction, rThumbstickTouchAction;

    XrAction lXClickAction, lXTouchAction, lYClickAction, lYTouchAction;
    XrAction rAClickAction, rATouchAction, rBClickAction, rBTouchAction;
    XrAction lMenuClickAction, rSystemClickAction;

    XrAction lGripPoseAction, rGripPoseAction;
    XrAction lAimPoseAction,  rAimPoseAction;
};


class OpenxrSession
{
public:
    class XrFramePacer
    {
    public:
        struct State {
            XrFrameState frameState{XR_TYPE_FRAME_STATE};
            XrCompositionLayerProjectionView layerViews[2] = {};
            XrView views[2];
        };

        int NumFrameQueued()            {return frameStates.size();}
        State& GetCurrState()           {return frameStates.front();}
        State& GetLastState()           {return frameStates.back();}
        void AddNewState(State& state)  {frameStates.push_front(state);}
        void PopLastState() {
            assert(!frameStates.empty());
            frameStates.pop_back();
        }
        int GetEyeIndex() {
            assert(eyeCreated < 2);
            return eyeCreated++;
        }
        void Reset() {
            eyeCreated = 0;
            frameStates.clear();
        }
    private:
        // Render loop context
        // reset when enter XR_SESSION_STATE_READY
        // index 0 = left eye, index 1 = right eye
        int eyeCreated = 0;
        std::list<State> frameStates{};

        /* Synchronization is done by the OpenXR runtime internally
         * through the calls of xrBeginFrame and xrWaitFrame
         * Spec: "A subsequent xrWaitFrame call must block until
         * the previous frame has been begun with xrBeginFrame"
        */
    };

public:
    void SetSessionState(XrSessionState newState);
    bool ShouldCloseSession();
    bool IsRunningSession();
    void RequestCloseSession();

    void PollActions();
    XrView* GetViews() {return pacer.GetCurrState().views;}
    VkExtent2D GetExtent() {return platform->extent;}

    bool XrBeginFrame();
    void XrEndFrame();
    void AsyncXrBeginFrame();
    void AsyncXrEndFrame();

    XrSessionState GetSessionState() {return sessionState;}
    XrSession GetSession() {return xrSession;}

    XrSpace GetViewSpace() {return viewSpace;}
    XrSpace GetLocalSpace() {return localSpace;}
    XrSpace GetStageSpace() {return stageSpace;}

    XrSpace GetLGripPoseSpace() {return lGripPoseSpace;}
    XrSpace GetRGripPoseSpace() {return rGripPoseSpace;}
    XrSpace GetLAimPoseSpace() {return lAimPoseSpace;}
    XrSpace GetRAimPoseSpace() {return rAimPoseSpace;}

private:
    // Helper methods called by Initialize()
    void InitializeSession();
    void InitializeSpaces();

    void Initialize(VulkanOpenxrPlatform* platform, CommandStream* driverApi);
    void Wait() {
        while (pacer.NumFrameQueued() != 0)
            /* wait until CMD thread flushes all frames*/;
        return;
    }
    void Destroy();

private: // VulkanPlatformOpenxrSwapChain
    friend VulkanPlatformOpenxrSwapChain;
    int GetSwapchainIndex() {return pacer.GetEyeIndex();}
    XrInstance GetXrInstance() {return platform->xrInstance;}

private:    
    friend VulkanOpenxrPlatform;
    VulkanOpenxrPlatform* platform = nullptr;
    CommandStream* driverApi = nullptr; // Owned by FEngine

    XrSession xrSession = XR_NULL_HANDLE;
    XrFramePacer pacer;
    std::atomic<XrSessionState> sessionState = XR_SESSION_STATE_UNKNOWN;
    mutable std::shared_mutex stateNotModified;

    XrSpace viewSpace = VK_NULL_HANDLE;
    XrSpace localSpace = VK_NULL_HANDLE;
    XrSpace stageSpace = VK_NULL_HANDLE;

    XrSpace lGripPoseSpace = VK_NULL_HANDLE;
    XrSpace rGripPoseSpace = VK_NULL_HANDLE;
    XrSpace lAimPoseSpace = VK_NULL_HANDLE;
    XrSpace rAimPoseSpace = VK_NULL_HANDLE;

    // Debug
    int syncBeginID = 0;
    int syncEndID = 0;
    int asyncBeginID = 0;
    int asyncEndID = 0;
};

}// namespace filament::backend

}// namspace filament

#endif TNT_FILAMENT_BACKEND_PLATFORMS_VULKANOPENXRPLATFORM_H