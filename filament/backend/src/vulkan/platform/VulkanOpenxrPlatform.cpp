#include "backend/platforms/VulkanOpenxrPlatform.h"
#include "backend/platforms/VulkanPlatform.h"
#include "vulkan/platform/VulkanPlatformSwapChainImpl.h"
#include "vulkan/VulkanContext.h"

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

void OpenxrSession::PollActions()
{
    SYSTRACE_CALL();
}

bool OpenxrSession::XrBeginFrame()
{
    SYSTRACE_CALL();
    if (sessionState == XR_SESSION_STATE_READY ||
        sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
        sessionState == XR_SESSION_STATE_VISIBLE ||
        sessionState == XR_SESSION_STATE_FOCUSED) {
        
        XrResult result;
        XrFramePacer::State& state = pacer.NewState();

        // Wait for a new frame.
        XrFrameWaitInfo frameWaitInfo {XR_TYPE_FRAME_WAIT_INFO};
        state.frameState = {XR_TYPE_FRAME_STATE};
        CHK_XRCMD2(result = xrWaitFrame(xrSession, &frameWaitInfo, &state.frameState));

        if (result == XR_SESSION_LOSS_PENDING)
        {
            SetSessionState(XR_SESSION_STATE_LOSS_PENDING);
            return false;
        }

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

            // Input* input = Input::GetInstance();
            
            // input->xr_left_eye_fov =
            //     *reinterpret_cast<glm::vec4*>(&views[0].fov);
            // input->xr_right_eye_fov =
            //     *reinterpret_cast<glm::vec4*>(&views[1].fov);
            // input->xr_left_eye_pos = 
            //     *reinterpret_cast<glm::vec3*>(&views[0].pose.position);
            // input->xr_right_eye_pos = 
            //     *reinterpret_cast<glm::vec3*>(&views[1].pose.position);
            // input->xr_left_eye_quat = 
            //     *reinterpret_cast<glm::vec4*>(&views[0].pose.orientation);
            // input->xr_right_eye_quat = 
            //     *reinterpret_cast<glm::vec4*>(&views[1].pose.orientation);
        }

        {   // Locate hand aims
            XrSpaceLocation lAimPoseLocation{XR_TYPE_SPACE_LOCATION};
            xrLocateSpace(
                lAimPoseSpace, localSpace,
                state.frameState.predictedDisplayTime, &lAimPoseLocation
            );

            // EventLeftAimPose* eventLeftAimPose = new EventLeftAimPose();
            // math::XrToTransform(
            //     eventLeftAimPose->transform,
            //     reinterpret_cast<glm::vec4*>(&lAimPoseLocation.pose.orientation),
            //     reinterpret_cast<glm::vec3*>(&lAimPoseLocation.pose.position)
            // );

            // EventQueue::GetInstance()->Publish(
            //     EventQueue::InputXR, eventLeftAimPose);

            XrSpaceLocation rAimPoseLocation{XR_TYPE_SPACE_LOCATION};
            xrLocateSpace(
                rAimPoseSpace, localSpace,
                state.frameState.predictedDisplayTime, &rAimPoseLocation
            );

            // EventRightAimPose* eventRightAimPose = new EventRightAimPose();
            // math::XrToTransform(
            //     eventRightAimPose->transform,
            //     reinterpret_cast<glm::vec4*>(&rAimPoseLocation.pose.orientation),
            //     reinterpret_cast<glm::vec3*>(&rAimPoseLocation.pose.position)
            // );

            // EventQueue::GetInstance()->Publish(
            //     EventQueue::InputXR, eventRightAimPose);
        }

        {   // Locate hand grips
            XrSpaceLocation lGripPoseLocation{XR_TYPE_SPACE_LOCATION};
            xrLocateSpace(
                lGripPoseSpace, localSpace,
                state.frameState.predictedDisplayTime, &lGripPoseLocation
            );

            // EventLeftGripPose* eventLeftGripPose = new EventLeftGripPose();
            // math::XrToTransform(
            //     eventLeftGripPose->transform,
            //     reinterpret_cast<glm::vec4*>(&lGripPoseLocation.pose.orientation),
            //     reinterpret_cast<glm::vec3*>(&lGripPoseLocation.pose.position)
            // );

            // EventQueue::GetInstance()->Publish(
            //     EventQueue::InputXR, eventLeftGripPose);

            XrSpaceLocation rGripPoseLocation{XR_TYPE_SPACE_LOCATION};
            xrLocateSpace(
                rGripPoseSpace, localSpace,
                state.frameState.predictedDisplayTime, &rGripPoseLocation
            );

            // EventRightGripPose* eventRightGripPose = new EventRightGripPose();
            // math::XrToTransform(
            //     eventRightGripPose->transform,
            //     reinterpret_cast<glm::vec4*>(&rGripPoseLocation.pose.orientation),
            //     reinterpret_cast<glm::vec3*>(&rGripPoseLocation.pose.position)
            // );

            // EventQueue::GetInstance()->Publish(
            //     EventQueue::InputXR, eventRightGripPose);
        }

#if FVK_ENABLED(FVK_DEBUG_OPENXR)
        std::string report = "[XrSession] BeginSyncFrame: " + std::to_string(syncBeginID++);
        SYSTRACE_TEXT(report.c_str());
        utils::slog.i << report << utils::io::endl;
#endif

        driverApi->xrBeginFrame(0);
        return true;
    }

    return false;
}

void OpenxrSession::XrEndFrame()
{
    SYSTRACE_CALL();
    if (sessionState == XR_SESSION_STATE_READY ||
        sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
        sessionState == XR_SESSION_STATE_VISIBLE ||
        sessionState == XR_SESSION_STATE_FOCUSED)
    {
        driverApi->xrEndFrame(0);

#if FVK_ENABLED(FVK_DEBUG_OPENXR)
        std::string report = "[XrSession] EndSyncFrame: " + std::to_string(syncEndID++);
        SYSTRACE_TEXT(report.c_str());
        utils::slog.i << report << utils::io::endl;
#endif
    }
}

void OpenxrSession::AsyncXrBeginFrame()
{
    SYSTRACE_CALL();
#if FVK_ENABLED(FVK_DEBUG_OPENXR)
    std::string report = "[XrSession] BeginAsyncFrame: " + std::to_string(asyncBeginID++);
    SYSTRACE_TEXT(report.c_str());
    utils::slog.i << report << utils::io::endl;
#endif

    XrResult result;
    XrFrameBeginInfo frameBeginInfo {XR_TYPE_FRAME_BEGIN_INFO};
    CHK_XRCMD2(result = xrBeginFrame(xrSession, &frameBeginInfo));

    if (result == XR_SESSION_LOSS_PENDING) {
        SetSessionState(XR_SESSION_STATE_LOSS_PENDING);
        return;
    }
}

void OpenxrSession::AsyncXrEndFrame()
{
    SYSTRACE_CALL();
#if FVK_ENABLED(FVK_DEBUG_OPENXR)
    std::string report = "[XrSession] endAsync: " + std::to_string(asyncEndID++);
    SYSTRACE_TEXT(report.c_str());
    utils::slog.i << report << utils::io::endl;
#endif

    if (!(sessionState == XR_SESSION_STATE_READY ||
        sessionState == XR_SESSION_STATE_SYNCHRONIZED ||
        sessionState == XR_SESSION_STATE_VISIBLE ||
        sessionState == XR_SESSION_STATE_FOCUSED)) {
        return;
    }

    XrResult result;
    XrFramePacer::State& state = pacer.GetLastState();

    XrCompositionLayerProjection layer;
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    layer.next = 0;
    layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    layer.space = localSpace;
    layer.viewCount = 2;
    layer.views = state.layerViews;

    const XrCompositionLayerBaseHeader* pLayer =
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);

    XrFrameEndInfo frameEndInfo {XR_TYPE_FRAME_END_INFO};
    frameEndInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    frameEndInfo.displayTime = state.frameState.predictedDisplayTime;
    frameEndInfo.layerCount = 1;
    frameEndInfo.layers = &pLayer;
    
    CHK_XRCMD2(result = xrEndFrame(xrSession, &frameEndInfo));
    pacer.ReleaseLastState();

    if (result == XR_SESSION_LOSS_PENDING)
    {
        SetSessionState(XR_SESSION_STATE_LOSS_PENDING);
        return;
    }
}

void OpenxrSession::Initialize(VulkanOpenxrPlatform* platform, CommandStream* driverApi)
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

    assert(OpenxrSession::ShouldCloseSession());

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
    
    if (sessionState == XR_SESSION_STATE_EXITING ||
        sessionState == XR_SESSION_STATE_LOSS_PENDING)
    {
        return true;
    }

    return false;
}

void OpenxrSession::RequestCloseSession()
{
    SYSTRACE_CALL();
    xrRequestExitSession(xrSession);
}

void OpenxrSession::SetSessionState(XrSessionState newState)
{
    SYSTRACE_CALL();

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
            pacer.Reset();
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