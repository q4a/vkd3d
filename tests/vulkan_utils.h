/*
 * Copyright 2020-2022 Zebediah Figura for CodeWeavers
 * Copyright 2024 Conor McCarthy for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifndef __VKD3D_VULKAN_UTILS_H
#define __VKD3D_VULKAN_UTILS_H

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "vulkan/vulkan.h"
#include "vkd3d_test.h"

/* The helpers in this file are not part of utils.h because vkd3d_api.c
 * needs its own Vulkan helpers specific to API tests. */

#define DECLARE_VK_PFN(name) PFN_##name name;

struct vulkan_test_context
{
    VkInstance instance;
    VkPhysicalDevice phys_device;
    VkDevice device;
    VkQueue queue;
    VkCommandPool command_pool;
    VkCommandBuffer cmd_buffer;
    VkDescriptorPool descriptor_pool;

    DECLARE_VK_PFN(vkCreateInstance);
    DECLARE_VK_PFN(vkEnumerateInstanceExtensionProperties);
#define VK_INSTANCE_PFN   DECLARE_VK_PFN
#define VK_DEVICE_PFN     DECLARE_VK_PFN
#include "vulkan_procs.h"
};

#define VK_CALL(f) (context->f)

static inline bool vk_extension_properties_contain(const VkExtensionProperties *extensions,
        uint32_t count, const char *extension_name)
{
    uint32_t i;

    for (i = 0; i < count; ++i)
    {
        if (!strcmp(extensions[i].extensionName, extension_name))
            return true;
    }
    return false;
}

struct vulkan_extension_list
{
    const char **names;
    size_t count;
};

static inline void check_instance_extensions(const struct vulkan_test_context *context,
        const char **instance_extensions, size_t instance_extension_count,
        struct vulkan_extension_list *enabled_extensions)
{
    VkExtensionProperties *extensions;
    uint32_t count, i;

    enabled_extensions->names = calloc(instance_extension_count, sizeof(*enabled_extensions->names));
    enabled_extensions->count = 0;

    VK_CALL(vkEnumerateInstanceExtensionProperties(NULL, &count, NULL));
    extensions = calloc(count, sizeof(*extensions));
    VK_CALL(vkEnumerateInstanceExtensionProperties(NULL, &count, extensions));

    for (i = 0; i < instance_extension_count; ++i)
    {
        const char *name = instance_extensions[i];

        if (vk_extension_properties_contain(extensions, count, name))
            enabled_extensions->names[enabled_extensions->count++] = name;
    }

    free(extensions);
}

static inline bool vulkan_test_context_init_instance(struct vulkan_test_context *context,
        const char **instance_extensions, size_t instance_extension_count)
{
    VkInstanceCreateInfo instance_desc = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    struct vulkan_extension_list enabled_extensions;
    DECLARE_VK_PFN(vkGetInstanceProcAddr)
    void *libvulkan;
    uint32_t count;
    VkResult vr;

    memset(context, 0, sizeof(*context));

    if (!(libvulkan = vkd3d_dlopen(SONAME_LIBVULKAN)))
    {
        skip("Failed to load %s: %s.\n", SONAME_LIBVULKAN, vkd3d_dlerror());
        return false;
    }
    vkGetInstanceProcAddr = vkd3d_dlsym(libvulkan, "vkGetInstanceProcAddr");

    context->vkCreateInstance = (void *)vkGetInstanceProcAddr(NULL, "vkCreateInstance");
    context->vkEnumerateInstanceExtensionProperties = (void *)vkGetInstanceProcAddr(NULL,
            "vkEnumerateInstanceExtensionProperties");

    check_instance_extensions(context, instance_extensions, instance_extension_count, &enabled_extensions);
    instance_desc.ppEnabledExtensionNames = enabled_extensions.names;
    instance_desc.enabledExtensionCount = enabled_extensions.count;
    vr = VK_CALL(vkCreateInstance(&instance_desc, NULL, &context->instance));
    free(enabled_extensions.names);
    if (vr < 0)
    {
        skip("Failed to create a Vulkan instance, vr %d.\n", vr);
        return false;
    }

#define VK_INSTANCE_PFN(name) context->name = (void *)vkGetInstanceProcAddr(context->instance, #name);
#include "vulkan_procs.h"

    count = 1;
    if ((vr = VK_CALL(vkEnumeratePhysicalDevices(context->instance, &count, &context->phys_device))) < 0)
    {
        skip("Failed to enumerate physical devices, vr %d.\n", vr);
        goto out_destroy_instance;
    }

    if (!count)
    {
        skip("No Vulkan devices are available.\n");
        goto out_destroy_instance;
    }

    return true;

out_destroy_instance:
    VK_CALL(vkDestroyInstance(context->instance, NULL));
    return false;
}

static inline bool get_vulkan_queue_index(const struct vulkan_test_context *context,
        VkQueueFlags queue_flag, uint32_t *index)
{
    VkQueueFamilyProperties *queue_properties;
    uint32_t count, i;

    count = 0;
    VK_CALL(vkGetPhysicalDeviceQueueFamilyProperties(context->phys_device, &count, NULL));
    queue_properties = malloc(count * sizeof(*queue_properties));
    VK_CALL(vkGetPhysicalDeviceQueueFamilyProperties(context->phys_device, &count, queue_properties));

    for (i = 0; i < count; ++i)
    {
        if (queue_properties[i].queueFlags & queue_flag)
        {
            free(queue_properties);
            *index = i;
            return true;
        }
    }

    free(queue_properties);
    return false;
}

static inline bool vulkan_test_context_init_device(struct vulkan_test_context *context,
        const VkDeviceCreateInfo *device_desc, uint32_t queue_index,
        uint32_t max_resource_count, uint32_t max_sampler_count)
{
    VkDescriptorPoolCreateInfo descriptor_pool_desc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    VkCommandBufferAllocateInfo cmd_buffer_desc = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    VkCommandPoolCreateInfo command_pool_desc = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    VkDescriptorPoolSize descriptor_pool_sizes[6];
    VkDevice device;
    VkResult vr;

    if ((vr = VK_CALL(vkCreateDevice(context->phys_device, device_desc, NULL, &device))))
    {
        skip("Failed to create device, vr %d.\n", vr);
        return false;
    }
    context->device = device;

#define VK_DEVICE_PFN(name) context->name = (void *)VK_CALL(vkGetDeviceProcAddr(device, #name));
#include "vulkan_procs.h"

    VK_CALL(vkGetDeviceQueue(device, queue_index, 0, &context->queue));

    command_pool_desc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    command_pool_desc.queueFamilyIndex = queue_index;

    VK_CALL(vkCreateCommandPool(device, &command_pool_desc, NULL, &context->command_pool));

    cmd_buffer_desc.commandPool = context->command_pool;
    cmd_buffer_desc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_buffer_desc.commandBufferCount = 1;

    VK_CALL(vkAllocateCommandBuffers(device, &cmd_buffer_desc, &context->cmd_buffer));

    assert(max_resource_count);

    descriptor_pool_sizes[0].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    descriptor_pool_sizes[0].descriptorCount = max_resource_count;
    descriptor_pool_sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptor_pool_sizes[1].descriptorCount = max_resource_count;
    descriptor_pool_sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptor_pool_sizes[2].descriptorCount = max_resource_count;
    descriptor_pool_sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    descriptor_pool_sizes[3].descriptorCount = max_resource_count;
    descriptor_pool_sizes[4].type = VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    descriptor_pool_sizes[4].descriptorCount = max_resource_count;
    descriptor_pool_sizes[5].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    descriptor_pool_sizes[5].descriptorCount = max_sampler_count;

    descriptor_pool_desc.maxSets = 1;
    descriptor_pool_desc.poolSizeCount = ARRAY_SIZE(descriptor_pool_sizes) - !max_sampler_count;
    descriptor_pool_desc.pPoolSizes = descriptor_pool_sizes;

    VK_CALL(vkCreateDescriptorPool(device, &descriptor_pool_desc, NULL, &context->descriptor_pool));

    return true;
}

static inline void vulkan_test_context_destroy(const struct vulkan_test_context *context)
{
    VkDevice device = context->device;

    VK_CALL(vkDestroyDescriptorPool(device, context->descriptor_pool, NULL));
    VK_CALL(vkFreeCommandBuffers(device, context->command_pool, 1, &context->cmd_buffer));
    VK_CALL(vkDestroyCommandPool(device, context->command_pool, NULL));
    VK_CALL(vkDestroyDevice(device, NULL));
    VK_CALL(vkDestroyInstance(context->instance, NULL));
}

#endif /* __VKD3D_VULKAN_UTILS_H */
