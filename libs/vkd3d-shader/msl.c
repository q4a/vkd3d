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

#include "vkd3d_shader_private.h"

struct msl_generator
{
    struct vsir_program *program;
    struct vkd3d_string_buffer_cache string_buffers;
    struct vkd3d_string_buffer *buffer;
    struct vkd3d_shader_location location;
    struct vkd3d_shader_message_context *message_context;
    unsigned int indent;
};

static void VKD3D_PRINTF_FUNC(3, 4) msl_compiler_error(struct msl_generator *gen,
        enum vkd3d_shader_error error, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vkd3d_shader_verror(gen->message_context, &gen->location, error, fmt, args);
    va_end(args);
}

static void msl_print_indent(struct vkd3d_string_buffer *buffer, unsigned int indent)
{
    vkd3d_string_buffer_printf(buffer, "%*s", 4 * indent, "");
}


static void msl_unhandled(struct msl_generator *gen, const struct vkd3d_shader_instruction *ins)
{
    msl_print_indent(gen->buffer, gen->indent);
    vkd3d_string_buffer_printf(gen->buffer, "/* <unhandled instruction %#x> */\n", ins->opcode);
    msl_compiler_error(gen, VKD3D_SHADER_ERROR_MSL_INTERNAL,
            "Internal compiler error: Unhandled instruction %#x.", ins->opcode);
}

static void msl_handle_instruction(struct msl_generator *gen, const struct vkd3d_shader_instruction *ins)
{
    gen->location = ins->location;
    msl_unhandled(gen, ins);
}

static void msl_generator_generate(struct msl_generator *gen)
{
    const struct vkd3d_shader_instruction_array *instructions = &gen->program->instructions;
    unsigned int i;

    MESSAGE("Generating a MSL shader. This is unsupported; you get to keep all the pieces if it breaks.\n");

    vkd3d_string_buffer_printf(gen->buffer, "void shader_main()\n{\n");

    ++gen->indent;
    for (i = 0; i < instructions->count; ++i)
    {
        msl_handle_instruction(gen, &instructions->elements[i]);
    }

    vkd3d_string_buffer_printf(gen->buffer, "}\n");

    if (TRACE_ON())
        vkd3d_string_buffer_trace(gen->buffer);
}

static void msl_generator_cleanup(struct msl_generator *gen)
{
    vkd3d_string_buffer_release(&gen->string_buffers, gen->buffer);
    vkd3d_string_buffer_cache_cleanup(&gen->string_buffers);
}

static int msl_generator_init(struct msl_generator *gen, struct vsir_program *program,
        struct vkd3d_shader_message_context *message_context)
{
    memset(gen, 0, sizeof(*gen));
    gen->program = program;
    vkd3d_string_buffer_cache_init(&gen->string_buffers);
    if (!(gen->buffer = vkd3d_string_buffer_get(&gen->string_buffers)))
    {
        vkd3d_string_buffer_cache_cleanup(&gen->string_buffers);
        return VKD3D_ERROR_OUT_OF_MEMORY;
    }
    gen->message_context = message_context;

    return VKD3D_OK;
}

int msl_compile(struct vsir_program *program, uint64_t config_flags,
        const struct vkd3d_shader_compile_info *compile_info, struct vkd3d_shader_message_context *message_context)
{
    struct msl_generator generator;
    int ret;

    if ((ret = vsir_program_transform(program, config_flags, compile_info, message_context)) < 0)
        return ret;

    if ((ret = msl_generator_init(&generator, program, message_context)) < 0)
        return ret;
    msl_generator_generate(&generator);
    msl_generator_cleanup(&generator);

    return VKD3D_ERROR_INVALID_SHADER;
}
