#include "backend/platforms/VulkanOpenxrPlatform.h"
#include "backend/platforms/VulkanPlatform.h"
#include "vulkan/platform/VulkanPlatformSwapChainImpl.h"
#include "vulkan/VulkanContext.h"

#include "private/backend/CommandStream.h"

#include <utils/Systrace.h>

#define CHK_XRCMD(result) do {                                          \
    XrResult res = result;                                              \
    if (XR_FAILED(res)) {                                               \
    char resultBuffer[XR_MAX_STRUCTURE_NAME_SIZE];                      \
    xrResultToString(xrInstance, res, resultBuffer);                    \
    utils::slog.i << "[OpenXR] API call error: "                        \
        << std::string(resultBuffer)                                    \
        << utils::io::endl;                                             \
} } while(0)

#define CHK_XRCMD2(result) do {                                         \
    XrResult res = result;                                              \
    if (XR_FAILED(res)) {                                               \
    char resultBuffer[XR_MAX_STRUCTURE_NAME_SIZE];                      \
    xrResultToString(platform->xrInstance, res, resultBuffer);          \
    utils::slog.i << "[OpenXR] API call error: "                        \
        << std::string(resultBuffer)                                    \
        << utils::io::endl;                                             \
} } while(0)

#define CREATE_ACTION(actName, actType) do {                            \
    XrActionCreateInfo actioninfo{XR_TYPE_ACTION_CREATE_INFO};          \
    std::string str = #actName;                                         \
    std::transform(str.begin(), str.end(), str.begin(), ::tolower);     \
    utility::strcpy_s(actioninfo.actionName, str.c_str());              \
    utility::strcpy_s(actioninfo.localizedActionName, str.c_str());     \
    actioninfo.actionType = XR_ACTION_TYPE_##actType##_INPUT;           \
    CHK_XRCMD(xrCreateAction(inputActionSet, &actioninfo, &actName));   \
} while(0)

#define ADD_BINDING(bindName, bindPath, actType) do {                   \
    CREATE_ACTION(bindName##Action, actType);                           \
    XrActionSuggestedBinding binding;                                   \
    XrPath bindName##Path;                                              \
    CHK_XRCMD(xrStringToPath(xrInstance, bindPath, &bindName##Path));   \
    binding.action = bindName##Action;                                  \
    binding.binding = bindName##Path;                                   \
    bindings.push_back(binding);                                        \
} while(0)

namespace filament::backend {

// OSX C standard library does not have strcpy_s
// helper for cross-platform 
namespace utility
{

/**
 * dest buffer size MUST be larger than src buffer size.
 * src MUST be null terminated.
*/
static void strcpy_s(char* dest, const char* src)
{
    int i = 0;
    while(src[i] != 0) {
        dest[i] = src[i];
        i++;
    }
    
    dest[i] = 0;
}
 
} // namespace utility
       
#include "VulkanPlatformPrivate.inc"

VulkanOpenxrPlatform* VulkanOpenxrPlatform::Initialize()
{
    SYSTRACE_CALL();

    unsigned int layerCount;
    std::vector<XrApiLayerProperties> layerList;
    unsigned int extensionCount;
    std::vector<XrExtensionProperties> extensionList;

    xrEnumerateApiLayerProperties(0, &layerCount, nullptr);
    layerList.resize(layerCount, {XR_TYPE_API_LAYER_PROPERTIES});
    xrEnumerateApiLayerProperties(layerCount, &layerCount, layerList.data());

    utils::slog.i << "[OpenXR] Layer count: "
        << std::to_string(layerCount) << utils::io::endl;

    for (auto& layer: layerList)
        utils::slog.i << layer.layerName << utils::io::endl;

    xrEnumerateInstanceExtensionProperties(
        nullptr, 0, &extensionCount, nullptr
    );
    extensionList.resize(extensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(
        nullptr, extensionCount, &extensionCount, extensionList.data()
    );

    utils::slog.i << "[OpenXR] Extension count: "
        << std::to_string(extensionCount) << utils::io::endl;

    for (auto& extension: extensionList)
        utils::slog.i << extension.extensionName << utils::io::endl;

    XrInstanceCreateInfo instanceInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.applicationInfo = {
        "Filament", 1, "Filament-renderer", 1,
        XR_CURRENT_API_VERSION
    };

    std::vector<const char*> enabledExt = {
        "XR_KHR_vulkan_enable", "XR_EXT_debug_utils"
    };
    instanceInfo.enabledExtensionCount = (uint32_t) enabledExt.size();
    instanceInfo.enabledExtensionNames = enabledExt.data();

    XrInstance xrInstance;
    XrResult result = xrCreateInstance(&instanceInfo, &xrInstance);

    if (XR_FAILED(result)) {
        utils::slog.i << "[OpenXR] Failed to load OpenXR runtime: "
            << std::to_string(result) << utils::io::endl;
        return nullptr;
    }

    XrInstanceProperties instanceProp{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(xrInstance, &instanceProp);

    utils::slog.i << "[OpenXR] Runtime: "
        << std::string(instanceProp.runtimeName) << utils::io::endl;

    XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

    // Will fail if VR headset is not plugged into the computer
    XrSystemId xrSystemId;
    result = xrGetSystem(xrInstance, &systemInfo, &xrSystemId);

    if (XR_FAILED(result))
    {
        char resultBuffer[XR_MAX_STRUCTURE_NAME_SIZE];
        xrResultToString(xrInstance, result, resultBuffer);
        utils::slog.i << "[OpenXR] Failed to load OpenXR system: "
            << std::string(resultBuffer) << utils::io::endl;
        xrDestroyInstance(xrInstance);
        return nullptr;
    }

    XrSystemProperties systemProp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(xrInstance, xrSystemId, &systemProp);

    utils::slog.i << "[OpenXR] System name: "
        << std::string(systemProp.systemName) << utils::io::endl;

    // Only allocate an object when system is available
    VulkanOpenxrPlatform* platform = new VulkanOpenxrPlatform();
    platform->layerList = layerList;
    platform->extensionList = extensionList;
    platform->xrInstance = xrInstance;
    platform->xrSystemId = xrSystemId;

    platform->LoadViewConfig();
    platform->LoadVulkanRequirements();
    platform->InitializeActions();

    return platform;
}

OpenxrSession* VulkanOpenxrPlatform::CreateSession()
{
    SYSTRACE_CALL();
    assert(activeSession == nullptr);

    activeSession = new OpenxrSession();
    activeSession->Initialize(this, driverApi);

    return activeSession;
}

void VulkanOpenxrPlatform::DestroySession(OpenxrSession*& openxrSession)
{
    SYSTRACE_CALL();
    assert(activeSession != nullptr);
    
    openxrSession->Wait();
    openxrSession->Destroy();
    delete openxrSession;
    openxrSession = nullptr;
}

ExtensionSet VulkanOpenxrPlatform::ParseExtensionString(char* names)
{
    ExtensionSet list;
    while (*names != 0)
    {
        list.emplace(names);
        while (*(++names) != 0)
        {
            if (*names == ' ')
            {
                *names++ = '\0';
                break;
            }
        }
    }
    return list;
}

ExtensionSet VulkanOpenxrPlatform::getInstanceExtensions()
{
    ExtensionSet extensions = VulkanPlatform::getInstanceExtensions();
    extensions.insert(vulkanInstanceExt.begin(), vulkanInstanceExt.end());
    return extensions;
}

ExtensionSet VulkanOpenxrPlatform::getDeviceExtensions(VkPhysicalDevice device)
{
    ExtensionSet extensions = VulkanPlatform::getDeviceExtensions(device);
    extensions.insert(vulkanDeviceExt.begin(), vulkanDeviceExt.end());
    return extensions;
}

void VulkanOpenxrPlatform::LoadViewConfig()
{
    SYSTRACE_CALL();

    unsigned int count;
    CHK_XRCMD(xrEnumerateViewConfigurations
        (xrInstance, xrSystemId, 0, &count, nullptr));

    viewConfigTypeList.resize(count);
    CHK_XRCMD(xrEnumerateViewConfigurations(
        xrInstance, xrSystemId, count, &count, viewConfigTypeList.data()));
    
    bool configFound = false;
    for (auto& configType: viewConfigTypeList) {
        if (configType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
            configFound = true;
            break;
        }
    }

    if (!configFound)
        utils::slog.i << "[OpenXR] The system does not support stereo views"
            << utils::io::endl;
    
    CHK_XRCMD(xrEnumerateViewConfigurationViews(
        xrInstance, xrSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        0, &count, nullptr));
    
    viewConfigViewList.resize(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    CHK_XRCMD(xrEnumerateViewConfigurationViews(
        xrInstance, xrSystemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        count, &count, viewConfigViewList.data()));
    
    extent.width = viewConfigViewList[0].recommendedImageRectWidth;
    extent.height = viewConfigViewList[0].recommendedImageRectHeight;

    utils::slog.i << "[OpenXR] Number of views: " 
        << std::to_string(viewConfigViewList.size())
        << "\n[OpenXR] recommendedImageRectWidth: "
        << std::to_string(viewConfigViewList[0].recommendedImageRectWidth)
        << "\n[OpenXR] recommendedImageRectHeight: "
        << std::to_string(viewConfigViewList[0].recommendedImageRectHeight)
        << "\n[OpenXR] recommendedSwapchainSampleCount: "
        << std::to_string(viewConfigViewList[0].recommendedSwapchainSampleCount)
        << utils::io::endl;
}

void VulkanOpenxrPlatform::LoadVulkanRequirements()
{
    SYSTRACE_CALL();

    PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsReqKHR;
    CHK_XRCMD(xrGetInstanceProcAddr(
        xrInstance, "xrGetVulkanGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsReqKHR)));
    vkRequirements.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR;
    xrGetVulkanGraphicsReqKHR(xrInstance, xrSystemId, &vkRequirements);

    PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtKHR;
    CHK_XRCMD(xrGetInstanceProcAddr(
        xrInstance, "xrGetVulkanInstanceExtensionsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanInstanceExtKHR)));

    unsigned int count;
    xrGetVulkanInstanceExtKHR(xrInstance, xrSystemId, 0, &count, nullptr);
    vulkanInstanceExtStr.resize(count);
    xrGetVulkanInstanceExtKHR(
        xrInstance, xrSystemId, count, &count, vulkanInstanceExtStr.data());
    vulkanInstanceExt = ParseExtensionString(vulkanInstanceExtStr.data());

    for(std::string_view extension: vulkanInstanceExt)
        utils::slog.i << "[OpenXR] vkInstance extension: "
            << std::string(extension) << utils::io::endl;

    PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtKHR;
    CHK_XRCMD(xrGetInstanceProcAddr(
        xrInstance, "xrGetVulkanDeviceExtensionsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanDeviceExtKHR)));

    xrGetVulkanDeviceExtKHR(xrInstance, xrSystemId, 0, &count, nullptr);
    vulkanDeviceExtStr.resize(count);
    xrGetVulkanDeviceExtKHR(
        xrInstance, xrSystemId, count, &count, vulkanDeviceExtStr.data());
    vulkanDeviceExt = ParseExtensionString(vulkanDeviceExtStr.data());

    for(std::string_view extension: vulkanDeviceExt)
        utils::slog.i << "[OpenXR] vkDevice extension: "
            << std::string(extension) << utils::io::endl;
}

void VulkanOpenxrPlatform::InitializeActions()
{
    SYSTRACE_CALL();
    // Create an action set
    XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    utility::strcpy_s(actionSetInfo.actionSetName, "input");
    utility::strcpy_s(actionSetInfo.localizedActionSetName, "input");
    actionSetInfo.priority = 0;
    CHK_XRCMD(xrCreateActionSet(xrInstance, &actionSetInfo, &inputActionSet));

    std::vector<XrActionSuggestedBinding> bindings;

    ADD_BINDING(lSqueezeValue, "/user/hand/left/input/squeeze/value",  FLOAT);
    ADD_BINDING(rSqueezeValue, "/user/hand/right/input/squeeze/value", FLOAT);
    ADD_BINDING(lTriggerValue, "/user/hand/left/input/trigger/value",  FLOAT);
    ADD_BINDING(rTriggerValue, "/user/hand/right/input/trigger/value", FLOAT);
    ADD_BINDING(lTriggerTouch, "/user/hand/left/input/trigger/touch",  BOOLEAN);
    ADD_BINDING(rTriggerTouch, "/user/hand/right/input/trigger/touch", BOOLEAN);

    ADD_BINDING(lThumbstickX, "/user/hand/left/input/thumbstick/x",  FLOAT);
    ADD_BINDING(rThumbstickX, "/user/hand/right/input/thumbstick/x", FLOAT);
    ADD_BINDING(lThumbstickY, "/user/hand/left/input/thumbstick/y",  FLOAT);
    ADD_BINDING(rThumbstickY, "/user/hand/right/input/thumbstick/y", FLOAT);

    ADD_BINDING(lThumbstickClick, "/user/hand/left/input/thumbstick/click",  BOOLEAN);
    ADD_BINDING(rThumbstickClick, "/user/hand/right/input/thumbstick/click", BOOLEAN);
    ADD_BINDING(lThumbstickTouch, "/user/hand/left/input/thumbstick/touch",  BOOLEAN);
    ADD_BINDING(rThumbstickTouch, "/user/hand/right/input/thumbstick/touch", BOOLEAN);

    ADD_BINDING(lXClick, "/user/hand/left/input/x/click", BOOLEAN);
    ADD_BINDING(lXTouch, "/user/hand/left/input/x/touch", BOOLEAN);
    ADD_BINDING(lYClick, "/user/hand/left/input/y/click", BOOLEAN);
    ADD_BINDING(lYTouch, "/user/hand/left/input/y/touch", BOOLEAN);
    ADD_BINDING(lMenuClick, "/user/hand/left/input/menu/click", BOOLEAN);

    ADD_BINDING(rAClick, "/user/hand/right/input/a/click", BOOLEAN);
    ADD_BINDING(rATouch, "/user/hand/right/input/a/touch", BOOLEAN);
    ADD_BINDING(rBClick, "/user/hand/right/input/b/click", BOOLEAN);
    ADD_BINDING(rBTouch, "/user/hand/right/input/b/touch", BOOLEAN);
    ADD_BINDING(rSystemClick, "/user/hand/right/input/system/click", BOOLEAN);

    ADD_BINDING(lGripPose, "/user/hand/left/input/grip/pose",  POSE);
    ADD_BINDING(rGripPose, "/user/hand/right/input/grip/pose", POSE);
    ADD_BINDING(lAimPose, "/user/hand/left/input/aim/pose",  POSE);
    ADD_BINDING(rAimPose, "/user/hand/right/input/aim/pose", POSE);

    XrPath oculusTouchInteractionProfilePath;
    CHK_XRCMD(xrStringToPath(xrInstance,
        "/interaction_profiles/oculus/touch_controller",
        &oculusTouchInteractionProfilePath
    ));

    XrInteractionProfileSuggestedBinding suggestedBindings{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggestedBindings.interactionProfile = oculusTouchInteractionProfilePath;
    suggestedBindings.suggestedBindings = bindings.data();
    suggestedBindings.countSuggestedBindings = (uint32_t) bindings.size();
    CHK_XRCMD(xrSuggestInteractionProfileBindings(
        xrInstance, &suggestedBindings));
}


void VulkanOpenxrPlatform::PollEvents()
{
    SYSTRACE_CALL();

    XrEventDataBuffer event{};
    while (TryReadNextEvent(&event))
    {
        switch (event.type)
        {
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            {
                const XrEventDataInstanceLossPending& instanceLossPending = 
                    *reinterpret_cast<const XrEventDataInstanceLossPending*>(&event);

                utils::slog.i << "[OpenXR] XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING"
                    << instanceLossPending.lossTime
                    << utils::io::endl;
                break;
            }
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            {
                const XrEventDataSessionStateChanged& sessionStateChangedEvent =
                    *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);

                if (activeSession)
                    activeSession->SetSessionState(sessionStateChangedEvent.state);
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            {
                utils::slog.i << "[OpenXR] XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED"
                    << utils::io::endl;
                break;
            }
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
            {
                utils::slog.i << "[OpenXR] XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING"
                    << utils::io::endl;
                break;
            }
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            {
                const XrEventDataEventsLost& eventsLost =
                    *reinterpret_cast<const XrEventDataEventsLost*>(&event);

                utils::slog.i << "[OpenXR] Events lost: "
                    << std::to_string(eventsLost.lostEventCount)
                    << utils::io::endl;
                break;
            }
            default:
            {
                utils::slog.i << "[OpenXR] Unknown event type "
                    << std::to_string(event.type)
                    << utils::io::endl;
                break;
            }
        }
    }
    return;
}

bool VulkanOpenxrPlatform::TryReadNextEvent(XrEventDataBuffer* eventDataBuffer)
{
    SYSTRACE_NAME("OpenxrPlatform::TryReadNextEvent");

    XrEventDataBaseHeader* baseHeader =
        reinterpret_cast<XrEventDataBaseHeader*>(eventDataBuffer);

    *baseHeader = {XR_TYPE_EVENT_DATA_BUFFER};
    const XrResult result = xrPollEvent(xrInstance, eventDataBuffer);
    CHK_XRCMD(result);

    if (result == XR_SUCCESS)
        return true;

    return false;
}

SwapChainPtr VulkanOpenxrPlatform::createSwapChain(void* nativeWindow,
    uint64_t flags, VkExtent2D extent)
{
    SYSTRACE_CALL();
    if (flags == backend::SWAP_CHAIN_CONFIG_OPENXR_SESSION)
    {
        OpenxrSession* session = static_cast<OpenxrSession*>(nativeWindow);
        VkExtent2D extent = {
            viewConfigViewList[0].recommendedImageRectWidth,
            viewConfigViewList[0].recommendedImageRectHeight};
        uint32_t sampleCount = viewConfigViewList[0].maxSwapchainSampleCount;
        VulkanPlatformOpenxrSwapChain* swapchain = new VulkanPlatformOpenxrSwapChain(
                mImpl->mContext, mImpl->mDevice, mImpl->mGraphicsQueue,
                session, extent, sampleCount, flags);
        mImpl->mOpenxrSwapchains.insert(swapchain);
        assert(mImpl->mOpenxrSwapchains.size() <= 2);
        return swapchain;
    } else {
        return VulkanPlatform::createSwapChain(nativeWindow, flags, extent);
    }
}

VulkanPlatform::SwapChainBundle VulkanOpenxrPlatform::getSwapChainBundle(SwapChainPtr handle) {
    SYSTRACE_CALL();
    if (mImpl->mOpenxrSwapchains.find(handle) != mImpl->mOpenxrSwapchains.end()) {
        return static_cast<VulkanPlatformOpenxrSwapChain*>(handle)->getSwapChainBundle();
    } else {
        return VulkanPlatform::getSwapChainBundle(handle);
    }
}

VkResult VulkanOpenxrPlatform::acquire(SwapChainPtr handle, VkSemaphore clientSignal, uint32_t* index) {
    SYSTRACE_CALL();
    if (mImpl->mOpenxrSwapchains.find(handle) != mImpl->mOpenxrSwapchains.end()) {
        return static_cast<VulkanPlatformOpenxrSwapChain*>(handle)
            ->acquire(clientSignal, index);
    } else {
        return VulkanPlatform::acquire(handle, clientSignal, index);
    }
}

VkResult VulkanOpenxrPlatform::present(SwapChainPtr handle, uint32_t index,
        VkSemaphore finishedDrawing) {
    SYSTRACE_CALL();
    if (mImpl->mOpenxrSwapchains.find(handle) != mImpl->mOpenxrSwapchains.end()) {
        return static_cast<VulkanPlatformOpenxrSwapChain*>(handle)
            ->present(index, finishedDrawing);
    } else {
        return VulkanPlatform::present(handle, index, finishedDrawing);
    }
}

bool VulkanOpenxrPlatform::hasResized(SwapChainPtr handle) {
    SYSTRACE_CALL();
    if (mImpl->mOpenxrSwapchains.find(handle) != mImpl->mOpenxrSwapchains.end()) {
        return static_cast<VulkanPlatformOpenxrSwapChain*>(handle)->hasResized();
    } else {
        return VulkanPlatform::hasResized(handle);
    }
}

VkResult VulkanOpenxrPlatform::recreate(SwapChainPtr handle) {
    SYSTRACE_CALL();
    if (mImpl->mOpenxrSwapchains.find(handle) != mImpl->mOpenxrSwapchains.end()) {
        return static_cast<VulkanPlatformOpenxrSwapChain*>(handle)->recreate();
    } else {
        return VulkanPlatform::recreate(handle);
    }
}

void VulkanOpenxrPlatform::destroy(SwapChainPtr handle) {
    SYSTRACE_CALL();
    if (mImpl->mOpenxrSwapchains.erase(handle)) {
        delete static_cast<VulkanPlatformOpenxrSwapChain*>(handle);;
    } else {
        VulkanPlatform::destroy(handle);
    }
}

void OpenxrSession::PollAction(XrFramePacer::State& state)
{ 
    {   // Locate eyes
        XrViewLocateInfo locateInfo {XR_TYPE_VIEW_LOCATE_INFO};
        locateInfo.viewConfigurationType = 
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locateInfo.displayTime = state.frameState.predictedDisplayTime;
        locateInfo.space = localSpace;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        state.views[0] = {XR_TYPE_VIEW};
        state.views[1] = {XR_TYPE_VIEW};
        uint32_t count;
        CHK_XRCMD2(xrLocateViews(
            xrSession, &locateInfo, &viewState, 2, &count, state.views));
        device.poseMap["Left Eye"] = state.views[0].pose;
        device.poseMap["Right Eye"] = state.views[1].pose;
    }

    {   // Locate hand aims
        XrSpaceLocation lAimPoseLocation{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(
            lAimPoseSpace, localSpace,
            state.frameState.predictedDisplayTime, &lAimPoseLocation
        );
        XrSpaceLocation rAimPoseLocation{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(
            rAimPoseSpace, localSpace,
            state.frameState.predictedDisplayTime, &rAimPoseLocation
        );

        device.poseMap["Left Aim"] = lAimPoseLocation.pose;
        device.poseMap["Right Aim"] = rAimPoseLocation.pose;
    }

    {   // Locate hand grips
        XrSpaceLocation lGripPoseLocation{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(
            lGripPoseSpace, localSpace,
            state.frameState.predictedDisplayTime, &lGripPoseLocation
        );
        XrSpaceLocation rGripPoseLocation{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(
            rGripPoseSpace, localSpace,
            state.frameState.predictedDisplayTime, &rGripPoseLocation
        );

        device.poseMap["Left Grip"] = lGripPoseLocation.pose;
        device.poseMap["Right Grip"] = rGripPoseLocation.pose;
    }

    {
        XrActiveActionSet activeActionSet{platform->inputActionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeActionSet;
        CHK_XRCMD2(xrSyncActions(xrSession, &syncInfo));

        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};

#define GET_ACTION_FLOAT(var, name)                                                     \
    getInfo.action = platform->var##Action;                                             \
    CHK_XRCMD2(xrGetActionStateFloat(xrSession, &getInfo, &device.floatMap[name]))

#define GET_ACTION_BOOLEAN(var, name)                                                   \
    getInfo.action = platform->var##Action;                                             \
    CHK_XRCMD2(xrGetActionStateBoolean(xrSession, &getInfo, &device.boolMap[name]))

        GET_ACTION_FLOAT(lSqueezeValue, "Left Squeeze");
        GET_ACTION_FLOAT(rSqueezeValue, "Right Squeeze");
        GET_ACTION_FLOAT(lTriggerValue, "Left Trigger");
        GET_ACTION_FLOAT(rTriggerValue, "Right Trigger");

        GET_ACTION_FLOAT(lThumbstickX, "Left Thumbstick X");
        GET_ACTION_FLOAT(rThumbstickX, "Right Thumbstick X");
        GET_ACTION_FLOAT(lThumbstickY, "Left Thumbstick Y");
        GET_ACTION_FLOAT(rThumbstickY, "Right Thumbstick Y");

        GET_ACTION_BOOLEAN(lTriggerTouch, "Left Trigger Touch");
        GET_ACTION_BOOLEAN(rTriggerTouch, "Right Trigger Touch");

        GET_ACTION_BOOLEAN(lThumbstickClick, "Left Thumbstick Click");
        GET_ACTION_BOOLEAN(rThumbstickClick, "Right Thumbstick Click");
        GET_ACTION_BOOLEAN(lThumbstickTouch, "Left Thumbstick Touch");
        GET_ACTION_BOOLEAN(rThumbstickTouch, "Right Thumbstick Touch");

        GET_ACTION_BOOLEAN(lXClick, "Left X Click");
        GET_ACTION_BOOLEAN(lXTouch, "Left X Touch");
        GET_ACTION_BOOLEAN(lYClick, "Left Y Click");
        GET_ACTION_BOOLEAN(lYTouch, "Left Y Touch");
        GET_ACTION_BOOLEAN(lMenuClick, "Left Menu Click");

        GET_ACTION_BOOLEAN(rAClick, "Right A Click");
        GET_ACTION_BOOLEAN(rATouch, "Right A Touch");
        GET_ACTION_BOOLEAN(rBClick, "Right B Click");
        GET_ACTION_BOOLEAN(rBTouch, "Right B Touch");
        GET_ACTION_BOOLEAN(rSystemClick, "Right System Click");

#undef GET_ACTION_FLOAT
#undef GET_ACTION_BOOLEAN
    }
}

void OpenxrSession::SyncFrame()
{
    SYSTRACE_CALL();

    XrResult result = XR_SUCCESS;
    XrFramePacer::State state = XrFramePacer::State();
    {   // State cannot change between session state check and API call
        std::shared_lock lock(stateNotModified);
        if (!IsRunningSession()) return;

        // Wait for a new frame.
        XrFrameWaitInfo frameWaitInfo {XR_TYPE_FRAME_WAIT_INFO};
        state.frameState = {XR_TYPE_FRAME_STATE};
        CHK_XRCMD2(result = xrWaitFrame(xrSession, &frameWaitInfo, &state.frameState));
    }

    if (result == XR_SESSION_LOSS_PENDING)
    {
        SetSessionState(XR_SESSION_STATE_LOSS_PENDING);
        return;
    }

    PollAction(state);
    pacer.AddNewState(state);
}

bool OpenxrSession::XrBeginFrame() {
    throw;
    return true;
}

void OpenxrSession::XrEndFrame() {
    throw;
}

void OpenxrSession::AsyncXrBeginFrame()
{
    SYSTRACE_CALL();
#if FVK_ENABLED(FVK_DEBUG_OPENXR)
    std::string report = "[XrSession] BeginAsyncFrame: " + std::to_string(asyncBeginID++);
    SYSTRACE_TEXT(report.c_str());
    utils::slog.i << report << utils::io::endl;
#endif

    XrResult result = XR_SUCCESS;
    {   // State cannot change between session state check and API call
        std::shared_lock lock(stateNotModified);
        if (!IsRunningSession())
            return;

        XrFrameBeginInfo frameBeginInfo {XR_TYPE_FRAME_BEGIN_INFO};
        CHK_XRCMD2(result = xrBeginFrame(xrSession, &frameBeginInfo));
    }

    if (result == XR_SESSION_LOSS_PENDING)
        SetSessionState(XR_SESSION_STATE_LOSS_PENDING);
}

void OpenxrSession::AsyncXrEndFrame()
{
    SYSTRACE_CALL();
#if FVK_ENABLED(FVK_DEBUG_OPENXR)
    std::string report = "[XrSession] EndAsyncFrame: " + std::to_string(asyncEndID++);
    SYSTRACE_TEXT(report.c_str());
    utils::slog.i << report << utils::io::endl;
#endif

    XrResult result = XR_SUCCESS;
    XrFramePacer::State state = pacer.GetLastState();
    pacer.PopLastState();
    {   // State cannot change between session state check and API call
        std::shared_lock lock(stateNotModified);
        if (!IsRunningSession())
            return;

        XrCompositionLayerProjection layer;
        layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        layer.next = 0;
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layer.space = localSpace;
        layer.viewCount = 2;
        layer.views = state.layerViews;
        assert(state.layerViews[0].type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
        assert(state.layerViews[1].type == XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);

        const XrCompositionLayerBaseHeader* pLayer =
            reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);

        XrFrameEndInfo frameEndInfo {XR_TYPE_FRAME_END_INFO};
        frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        frameEndInfo.displayTime = state.frameState.predictedDisplayTime;
        frameEndInfo.layerCount = 1;
        frameEndInfo.layers = &pLayer;
        
        CHK_XRCMD2(result = xrEndFrame(xrSession, &frameEndInfo));
    }
    
    if (result == XR_SESSION_LOSS_PENDING)
        SetSessionState(XR_SESSION_STATE_LOSS_PENDING);
}

void OpenxrSession::Initialize(VulkanOpenxrPlatform* platform, void* driverApi)
{
    SYSTRACE_CALL();

    this->platform = platform;
    this->driverApi = driverApi;
    InitializeSession();
    InitializeSpaces();
    // Initialize actions?

    XrSessionActionSetsAttachInfo attachInfo
        {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attachInfo.countActionSets = 1;
    attachInfo.actionSets = &platform->inputActionSet;
    CHK_XRCMD2(xrAttachSessionActionSets(xrSession, &attachInfo));
}

void OpenxrSession::Destroy()
{
    SYSTRACE_CALL();

    assert(OpenxrSession::ShouldCloseSession() || sessionState == XR_SESSION_STATE_IDLE);

    xrDestroySpace(viewSpace);
    xrDestroySpace(localSpace);
    xrDestroySpace(stageSpace);

    xrDestroySpace(lGripPoseSpace);
    xrDestroySpace(rGripPoseSpace);
    xrDestroySpace(lAimPoseSpace);
    xrDestroySpace(rAimPoseSpace);
    
    xrDestroySession(xrSession);
}

void OpenxrSession::InitializeSession()
{
    SYSTRACE_CALL();
    // Excludes all methods that read session state
    std::unique_lock lock(stateNotModified);

    XrGraphicsBindingVulkanKHR vkBinding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    vkBinding.instance = platform->mImpl->mInstance;
    vkBinding.physicalDevice = platform->mImpl->mPhysicalDevice;
    vkBinding.device = platform->mImpl->mDevice;
    vkBinding.queueFamilyIndex = platform->mImpl->mGraphicsQueueIndex;
    vkBinding.queueIndex = 0;

    PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR;
    CHK_XRCMD2(xrGetInstanceProcAddr(
        platform->xrInstance, "xrGetVulkanGraphicsDeviceKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&xrGetVulkanGraphicsDeviceKHR)));

    // Ensure the physical device used by Vulkan renderer has a VR headset connected.
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    xrGetVulkanGraphicsDeviceKHR(platform->xrInstance, platform->xrSystemId,
        platform->mImpl->mInstance, &physicalDevice);
    if (physicalDevice != platform->mImpl->mPhysicalDevice)
    {
        utils::slog.e << "Physical devices used by OpenXR and Vulkan are different."
            << utils::io::endl;
        throw;
    }

    XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
    createInfo.next = &vkBinding;
    createInfo.systemId = platform->xrSystemId;
    CHK_XRCMD2(xrCreateSession(platform->xrInstance, &createInfo, &xrSession));

    sessionState = XR_SESSION_STATE_IDLE;
}

void OpenxrSession::InitializeSpaces()
{
    SYSTRACE_CALL();

    uint32_t spaceCount;
    CHK_XRCMD2(xrEnumerateReferenceSpaces(xrSession, 0, &spaceCount, nullptr));
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    CHK_XRCMD2(xrEnumerateReferenceSpaces(xrSession, spaceCount, &spaceCount, spaces.data()));

    utils::slog.i << "[OpenXR] Available reference spaces: " 
        << std::to_string(spaceCount)
        << utils::io::endl;

    for (XrReferenceSpaceType space: spaces)
    {
        std::string spaceName;
        switch (space)
        {
        case XR_REFERENCE_SPACE_TYPE_VIEW:
            spaceName = "XR_REFERENCE_SPACE_TYPE_VIEW";
            break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL:
            spaceName = "XR_REFERENCE_SPACE_TYPE_LOCAL";
            break;
        case XR_REFERENCE_SPACE_TYPE_STAGE:
            spaceName = "XR_REFERENCE_SPACE_TYPE_STAGE";
            break;
        default:
            spaceName = "Unknown reference space: " + std::to_string(space);
            break;
        }
        utils::slog.i << "[OpenXR] Referenece space: " << spaceName << utils::io::endl;
    }
    
    if (spaceCount != 3) {
        utils::slog.i << "[OpenXR] Reference space check failed."
            << utils::io::endl;
    }

    {   // Create reference spaces
        XrReferenceSpaceCreateInfo createInfo
            {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        createInfo.poseInReferenceSpace.orientation.w = 1.0f;
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        CHK_XRCMD2(xrCreateReferenceSpace(xrSession, &createInfo, &viewSpace));
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        CHK_XRCMD2(xrCreateReferenceSpace(xrSession, &createInfo, &localSpace));
        createInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        CHK_XRCMD2(xrCreateReferenceSpace(xrSession, &createInfo, &stageSpace));
    }

    {   // Create action spaces
        XrActionSpaceCreateInfo createInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        createInfo.poseInActionSpace.orientation.w = 1.0f;
        createInfo.action = platform->lGripPoseAction;
        CHK_XRCMD2(xrCreateActionSpace(xrSession, &createInfo, &lGripPoseSpace));
        createInfo.action = platform->rGripPoseAction;
        CHK_XRCMD2(xrCreateActionSpace(xrSession, &createInfo, &rGripPoseSpace));
        createInfo.action = platform->lAimPoseAction;
        CHK_XRCMD2(xrCreateActionSpace(xrSession, &createInfo, &lAimPoseSpace));
        createInfo.action = platform->rAimPoseAction;
        CHK_XRCMD2(xrCreateActionSpace(xrSession, &createInfo, &rAimPoseSpace));
    }
}

bool OpenxrSession::ShouldCloseSession()
{
    SYSTRACE_CALL();
    std::shared_lock lock(stateNotModified);
    return (sessionState == XR_SESSION_STATE_EXITING ||
        sessionState == XR_SESSION_STATE_LOSS_PENDING);
}

bool OpenxrSession::IsRunningSession()
{
    SYSTRACE_CALL();
    std::shared_lock lock(stateNotModified);
    return (sessionState == XR_SESSION_STATE_READY ||
        sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
        sessionState == XR_SESSION_STATE_VISIBLE ||
        sessionState == XR_SESSION_STATE_FOCUSED);
}

void OpenxrSession::RequestCloseSession()
{
    SYSTRACE_CALL();
    xrRequestExitSession(xrSession);
}

void OpenxrSession::SetSessionState(XrSessionState newState)
{
    SYSTRACE_CALL();
    // Excludes all methods that read session state
    std::unique_lock lock(stateNotModified);

    static const std::vector<std::string> stateNames
    {
        "XR_SESSION_STATE_UNKNOWN",
        "XR_SESSION_STATE_IDLE",
        "XR_SESSION_STATE_READY",
        "XR_SESSION_STATE_SYNCHRONIZED",
        "XR_SESSION_STATE_VISIBLE",
        "XR_SESSION_STATE_FOCUSED",
        "XR_SESSION_STATE_STOPPING",
        "XR_SESSION_STATE_LOSS_PENDING",
        "XR_SESSION_STATE_EXITING",
    };

    utils::slog.i << "[OpenXR] Session state changes from "
        << stateNames[sessionState] << " to " << stateNames[newState]
        << utils::io::endl;

    sessionState = newState;

    switch (sessionState)
    {
        case XR_SESSION_STATE_READY:
        {
            XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
            info.primaryViewConfigurationType = 
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            CHK_XRCMD2(xrBeginSession(xrSession, &info));
            pacer.Reset();
            break;
        }
        case XR_SESSION_STATE_STOPPING:
        {
            CHK_XRCMD2(xrEndSession(xrSession));
            break;
        }
        case XR_SESSION_STATE_EXITING:
        {
            break;
        }
        case XR_SESSION_STATE_LOSS_PENDING:
        {
            // could not be tested.
            break;
        }
        default:
            break;
    }
}

}// namespace filament::backend
