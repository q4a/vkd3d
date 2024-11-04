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
#define COBJMACROS
#define VKD3D_TEST_NO_DEFS
/* Avoid conflicts with the Objective C BOOL definition. */
#define BOOL VKD3D_BOOLEAN
#include "shader_runner.h"
#include "vkd3d_d3dcommon.h"
#undef BOOL

struct metal_runner
{
    struct shader_runner r;
    struct shader_runner_caps caps;

    id<MTLDevice> device;

    ID3D10Blob *d3d_blobs[SHADER_TYPE_COUNT];
    struct vkd3d_shader_scan_signature_info signatures[SHADER_TYPE_COUNT];
};

static MTLVertexFormat get_metal_attribute_format(DXGI_FORMAT format)
{
    switch (format)
    {
        case DXGI_FORMAT_R32G32_FLOAT:
            return MTLVertexFormatFloat2;
        default:
            return MTLVertexFormatInvalid;
    }
}

static void trace_messages(const char *messages)
{
    const char *p, *end, *line;

    if (!vkd3d_test_state.debug_level)
        return;

    p = messages;
    end = &p[strlen(p)];

    trace("Received messages:\n");
    while (p < end)
    {
        line = p;
        if ((p = memchr(line, '\n', end - line)))
            ++p;
        else
            p = end;
        trace("    %.*s", (int)(p - line), line);
    }
}

static struct metal_runner *metal_runner(struct shader_runner *r)
{
    return CONTAINING_RECORD(r, struct metal_runner, r);
}

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

static bool compile_shader(struct metal_runner *runner, enum shader_type type, struct vkd3d_shader_code *out)
{
    struct vkd3d_shader_interface_info interface_info = {.type = VKD3D_SHADER_STRUCTURE_TYPE_INTERFACE_INFO};
    struct vkd3d_shader_compile_info info = {.type = VKD3D_SHADER_STRUCTURE_TYPE_COMPILE_INFO};
    struct vkd3d_shader_resource_binding bindings[MAX_RESOURCES + MAX_SAMPLERS];
    struct vkd3d_shader_resource_binding *binding;
    unsigned int descriptor_binding = 0;
    char *messages;
    int ret;

    const struct vkd3d_shader_compile_option options[] =
    {
        {VKD3D_SHADER_COMPILE_OPTION_API_VERSION, VKD3D_SHADER_API_VERSION_1_13},
        {VKD3D_SHADER_COMPILE_OPTION_FEATURE, shader_runner_caps_get_feature_flags(&runner->caps)},
    };

    if (!(runner->d3d_blobs[type] = compile_hlsl(&runner->r, type)))
        return false;

    info.next = &interface_info;
    info.source.code = ID3D10Blob_GetBufferPointer(runner->d3d_blobs[type]);
    info.source.size = ID3D10Blob_GetBufferSize(runner->d3d_blobs[type]);
    info.source_type = VKD3D_SHADER_SOURCE_DXBC_TPF;
    info.target_type = VKD3D_SHADER_TARGET_MSL;
    info.options = options;
    info.option_count = ARRAY_SIZE(options);
    info.log_level = VKD3D_SHADER_LOG_WARNING;

    if (runner->r.uniform_count)
    {
        binding = &bindings[interface_info.binding_count++];
        binding->type = VKD3D_SHADER_DESCRIPTOR_TYPE_CBV;
        binding->register_space = 0;
        binding->register_index = 0;
        binding->shader_visibility = VKD3D_SHADER_VISIBILITY_ALL;
        binding->flags = VKD3D_SHADER_BINDING_FLAG_BUFFER;
        binding->binding.set = 0;
        binding->binding.binding = descriptor_binding++;
        binding->binding.count = 1;
    }

    interface_info.bindings = bindings;
    interface_info.next = &runner->signatures[type];
    runner->signatures[type].type = VKD3D_SHADER_STRUCTURE_TYPE_SCAN_SIGNATURE_INFO;
    runner->signatures[type].next = NULL;

    ret = vkd3d_shader_compile(&info, out, &messages);
    if (messages)
        trace_messages(messages);
    vkd3d_shader_free_messages(messages);

    return ret >= 0;
}

static id<MTLFunction> compile_stage(struct metal_runner *runner, enum shader_type type)
{
    struct vkd3d_shader_code out;
    id<MTLFunction> function;
    id<MTLLibrary> library;
    NSString *src;
    NSError *err;

    if (!compile_shader(runner, type, &out))
        return nil;
    src = [[[NSString alloc] initWithBytes:out.code length:out.size encoding:NSUTF8StringEncoding] autorelease];
    library = [[runner->device newLibraryWithSource:src options:nil error:&err] autorelease];
    ok(library, "Failed to create MTLLibrary.\n");
    if (err)
        trace_messages([err.localizedDescription UTF8String]);
    function = [library newFunctionWithName:@"shader_entry"];
    ok(function, "Failed to create MTLFunction.\n");
    vkd3d_shader_free_shader_code(&out);

    return [function autorelease];
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
    struct metal_runner *runner = metal_runner(r);
    MTLRenderPipelineDescriptor *pipeline_desc;
    MTLVertexBufferLayoutDescriptor *binding;
    id<MTLDevice> device = runner->device;
    size_t attribute_offsets[32], stride;
    MTLVertexDescriptor *vertex_desc;
    id<MTLRenderPipelineState> pso;
    struct resource *resource;
    unsigned int vb_idx, i, j;
    NSError *err;

    struct
    {
        unsigned int idx;
    } vb_info[MAX_RESOURCES];

    @autoreleasepool
    {
        pipeline_desc = [[[MTLRenderPipelineDescriptor alloc] init] autorelease];
        vertex_desc = [MTLVertexDescriptor vertexDescriptor];

        if (!(pipeline_desc.vertexFunction = compile_stage(runner, SHADER_TYPE_VS)))
        {
            trace("Failed to compile vertex function.\n");
            goto done;
        }

        if (!(pipeline_desc.fragmentFunction = compile_stage(runner, SHADER_TYPE_PS)))
        {
            trace("Failed to compile fragment function.\n");
            goto done;
        }

        /* [[buffer(0)]] is used for the descriptor argument buffer. */
        vb_idx = 1;
        for (i = 0; i < runner->r.resource_count; ++i)
        {
            resource = runner->r.resources[i];
            switch (resource->desc.type)
            {
                case RESOURCE_TYPE_VERTEX_BUFFER:
                    assert(resource->desc.slot < ARRAY_SIZE(vb_info));
                    for (j = 0, stride = 0; j < runner->r.input_element_count; ++j)
                    {
                        if (runner->r.input_elements[j].slot != resource->desc.slot)
                            continue;
                        assert(j < ARRAY_SIZE(attribute_offsets));
                        attribute_offsets[j] = stride;
                        stride += runner->r.input_elements[j].texel_size;
                    }
                    if (!stride)
                        break;
                    vb_info[resource->desc.slot].idx = vb_idx;
                    binding = [vertex_desc.layouts objectAtIndexedSubscript:vb_idx];
                    binding.stepFunction = MTLVertexStepFunctionPerVertex;
                    binding.stride = stride;
                    ++vb_idx;
                    break;

                default:
                    break;
            }
        }

        if (runner->r.input_element_count > 32)
            fatal_error("Unsupported input element count %zu.\n", runner->r.input_element_count);

        for (i = 0; i < runner->r.input_element_count; ++i)
        {
            const struct input_element *element = &runner->r.input_elements[i];
            const struct vkd3d_shader_signature_element *signature_element;
            MTLVertexAttributeDescriptor *attribute;

            signature_element = vkd3d_shader_find_signature_element(&runner->signatures[SHADER_TYPE_VS].input,
                    element->name, element->index, 0);
            ok(signature_element, "Cannot find signature element %s%u.\n", element->name, element->index);

            attribute = [vertex_desc.attributes objectAtIndexedSubscript:signature_element->register_index];
            attribute.bufferIndex = vb_info[element->slot].idx;
            attribute.format = get_metal_attribute_format(element->format);
            ok(attribute.format != MTLVertexFormatInvalid, "Unhandled attribute format %#x.\n", element->format);
            attribute.offset = attribute_offsets[i];
        }

        pipeline_desc.vertexDescriptor = vertex_desc;

        if (!(pso = [[device newRenderPipelineStateWithDescriptor:pipeline_desc error:&err] autorelease]))
        {
            trace("Failed to compile pipeline state.\n");
            if (err)
                trace_messages([err.localizedDescription UTF8String]);
            goto done;
        }
    }

done:
    for (i = 0; i < SHADER_TYPE_COUNT; ++i)
    {
        if (!runner->d3d_blobs[i])
            continue;

        vkd3d_shader_free_scan_signature_info(&runner->signatures[i]);
        ID3D10Blob_Release(runner->d3d_blobs[i]);
        runner->d3d_blobs[i] = NULL;
    }

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

static bool check_msl_support(void)
{
    const enum vkd3d_shader_target_type *target_types;
    unsigned int count, i;

    target_types = vkd3d_shader_get_supported_target_types(VKD3D_SHADER_SOURCE_DXBC_TPF, &count);
    for (i = 0; i < count; ++i)
    {
        if (target_types[i] == VKD3D_SHADER_TARGET_MSL)
            return true;
    }

    return false;
}

static bool metal_runner_init(struct metal_runner *runner)
{
    NSArray<id<MTLDevice>> *devices;
    id<MTLDevice> device;

    static const char *const tags[] =
    {
        "msl",
    };

    if (!check_msl_support())
    {
        skip("MSL support is not enabled. If this is unintentional, "
                "add -DVKD3D_SHADER_UNSUPPORTED_MSL to CPPFLAGS.\n");
        return false;
    }

    memset(runner, 0, sizeof(*runner));

    devices = MTLCopyAllDevices();
    if (![devices count])
    {
        skip("Failed to find a usable Metal device.\n");
        [devices release];
        return false;
    }
    device = [devices objectAtIndex:0];
    runner->device = [device retain];
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
    [runner->device release];
}

void run_shader_tests_metal(void)
{
    struct metal_runner runner;

    if (!metal_runner_init(&runner))
        return;
    run_shader_tests(&runner.r, &runner.caps, &metal_runner_ops, NULL);
    metal_runner_cleanup(&runner);
}
