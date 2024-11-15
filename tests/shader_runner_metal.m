/*
 * Copyright 2024 Feifan He for CodeWeavers
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

#include "config.h"
#import <Metal/Metal.h>
#define VKD3D_TEST_NO_DEFS
/* Avoid conflicts with the Objective C BOOL definition. */
#define BOOL VKD3D_BOOLEAN
#include "shader_runner.h"
#undef BOOL

struct metal_runner
{
    struct shader_runner r;
    struct shader_runner_caps caps;
};

static struct resource *metal_runner_create_resource(struct shader_runner *r, const struct resource_params *params)
{
    struct resource *resource;

    resource = calloc(1, sizeof(*resource));
    init_resource(resource, params);

    return resource;
}

static void metal_runner_destroy_resource(struct shader_runner *r, struct resource *res)
{
    free(res);
}

static bool metal_runner_dispatch(struct shader_runner *r, unsigned int x, unsigned int y, unsigned int z)
{
    return false;
}

static void metal_runner_clear(struct shader_runner *r, struct resource *res, const struct vec4 *clear_value)
{
    return;
}

static bool metal_runner_draw(struct shader_runner *r, D3D_PRIMITIVE_TOPOLOGY primitive_topology,
        unsigned int vertex_count, unsigned int instance_count)
{
    return false;
}

static bool metal_runner_copy(struct shader_runner *r, struct resource *src, struct resource *dst)
{
    return false;
}

static struct resource_readback *metal_runner_get_resource_readback(struct shader_runner *r, struct resource *res)
{
    return NULL;
}

static void metal_runner_release_readback(struct shader_runner *r, struct resource_readback *rb)
{
    free(rb->data);
    free(rb);
}

static const struct shader_runner_ops metal_runner_ops =
{
    .create_resource = metal_runner_create_resource,
    .destroy_resource = metal_runner_destroy_resource,
    .dispatch = metal_runner_dispatch,
    .clear = metal_runner_clear,
    .draw = metal_runner_draw,
    .copy = metal_runner_copy,
    .get_resource_readback = metal_runner_get_resource_readback,
    .release_readback = metal_runner_release_readback,
};

static bool metal_runner_init(struct metal_runner *runner)
{
    NSArray<id<MTLDevice>> *devices;
    id<MTLDevice> device;

    static const char *const tags[] =
    {
        "msl",
    };

    memset(runner, 0, sizeof(*runner));

    devices = MTLCopyAllDevices();
    if (![devices count])
    {
        skip("Failed to find a usable Metal device.\n");
        [devices release];
        return false;
    }
    device = [devices objectAtIndex:0];
    [devices release];

    trace("GPU: %s\n", [[device name] UTF8String]);

    runner->caps.runner = "Metal";
    runner->caps.tags = tags;
    runner->caps.tag_count = ARRAY_SIZE(tags);
    runner->caps.minimum_shader_model = SHADER_MODEL_4_0;
    runner->caps.maximum_shader_model = SHADER_MODEL_5_0;

    return true;
}

static void metal_runner_cleanup(struct metal_runner *runner)
{
    /* Nothing to do. */
}

void run_shader_tests_metal(void)
{
    struct metal_runner runner;

    if (!metal_runner_init(&runner))
        return;
    run_shader_tests(&runner.r, &runner.caps, &metal_runner_ops, NULL);
    metal_runner_cleanup(&runner);
}
