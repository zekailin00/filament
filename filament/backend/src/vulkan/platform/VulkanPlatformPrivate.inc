using SwapChainPtr = VulkanPlatform::SwapChainPtr;

constexpr uint32_t const INVALID_VK_INDEX = 0xFFFFFFFF;

struct VulkanPlatformPrivate {
    VkInstance mInstance = VK_NULL_HANDLE;
    VkPhysicalDevice mPhysicalDevice = VK_NULL_HANDLE;
    VkDevice mDevice = VK_NULL_HANDLE;
    uint32_t mGraphicsQueueFamilyIndex = INVALID_VK_INDEX;
    uint32_t mGraphicsQueueIndex = INVALID_VK_INDEX;
    VkQueue mGraphicsQueue = VK_NULL_HANDLE;
    VulkanContext mContext = {};

    // We use a map to both map a handle (i.e. SwapChainPtr) to the concrete type and also to
    // store the actual swapchain struct, which is either backed-by-surface or headless.
    std::unordered_set<SwapChainPtr> mSurfaceSwapChains;
    std::unordered_set<SwapChainPtr> mHeadlessSwapChains;

    bool mSharedContext = false;
};