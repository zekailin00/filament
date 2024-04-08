#include "backend/platforms/VulkanOpenxrPlatform.h"

#include <utils/Systrace.h>

#define CHK_XRCMD(result) do { if (XR_FAILED(result)) {     \
    char resultBuffer[XR_MAX_STRUCTURE_NAME_SIZE];          \
    XrResult res;                                           \
    xrResultToString(xrInstance, res, resultBuffer);        \
    utils::slog.i << "[OpenXR] API call error: "            \
        << std::string(resultBuffer);                       \
} } while(0)

#define CREATE_ACTION(actName, actType) do {                            \
    XrActionCreateInfo actioninfo{XR_TYPE_ACTION_CREATE_INFO};          \
    utility::strcpy_s(actioninfo.actionName, #actName);                 \
    utility::strcpy_s(actioninfo.localizedActionName, #actName);        \
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
    while(src[i] != 0)
    {
        dest[i] = src[i];
        i++;
    }
    
    dest[i] = 0;
}
 
} // namespace utility
       

VulkanOpenxrPlatform* VulkanOpenxrPlatform::Initialize()
{
    SYSTRACE_NAME("OpenxrPlatform::Initialize");

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
    instanceInfo.enabledExtensionCount = enabledExt.size();
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

void VulkanOpenxrPlatform::LoadViewConfig()
{
    unsigned int count;
    CHK_XRCMD(xrEnumerateViewConfigurations
        (xrInstance, xrSystemId, 0, &count, nullptr));

    viewConfigTypeList.resize(count);
    CHK_XRCMD(xrEnumerateViewConfigurations(
        xrInstance, xrSystemId, count, &count, viewConfigTypeList.data()));
    
    bool configFound = false;
    for (auto& configType: viewConfigTypeList)
    {
        if (configType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        {
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

    for(const char* extension: vulkanInstanceExt)
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

    for(const char* extension: vulkanDeviceExt)
        utils::slog.i << "[OpenXR] vkDevice extension: "
            << std::string(extension) << utils::io::endl;
}

void VulkanOpenxrPlatform::InitializeActions()
{
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
    suggestedBindings.countSuggestedBindings = bindings.size();
    CHK_XRCMD(xrSuggestInteractionProfileBindings(
        xrInstance, &suggestedBindings));
}

}// namespace filament::backend