/*
 * vulkan_x11_stub.c
 * Stub for X11 Vulkan symbols that GTK4 references but we don't need
 * (we only use Wayland Vulkan)
 */
#include <stdint.h>
#include <stddef.h>

typedef void* VkInstance;
typedef uint64_t VkSurfaceKHR;
typedef int VkResult;
typedef struct { int ignored; } Display;
typedef unsigned long Window;
typedef struct {
    int sType;
    const void *pNext;
    int flags;
    Display *dpy;
    Window window;
} VkXlibSurfaceCreateInfoKHR;
typedef struct {
    int sType;
    const void *pNext;
    uint32_t minImageCount;
} VkSurfaceCapabilitiesKHR;

__attribute__((visibility("default")))
VkResult vkCreateXlibSurfaceKHR(VkInstance instance,
                                  const VkXlibSurfaceCreateInfoKHR *pCreateInfo,
                                  const void *pAllocator,
                                  VkSurfaceKHR *pSurface) {
    return -1000066001; /* VK_ERROR_EXTENSION_NOT_PRESENT */
}

__attribute__((visibility("default")))
unsigned int vkGetPhysicalDeviceXlibPresentationSupportKHR(void *physicalDevice,
                                                             uint32_t queueFamilyIndex,
                                                             Display *dpy,
                                                             unsigned long visualID) {
    return 0;
}
