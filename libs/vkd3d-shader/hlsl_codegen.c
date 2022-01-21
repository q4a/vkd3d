/*
 * HLSL optimization and code generation
 *
 * Copyright 2019-2020 Zebediah Figura for CodeWeavers
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

#include "hlsl.h"
#include <stdio.h>

/* Split uniforms into two variables representing the constant and temp
 * registers, and copy the former to the latter, so that writes to uniforms
 * work. */
static void prepend_uniform_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *temp)
{
    struct vkd3d_string_buffer *name;
    struct hlsl_ir_var *uniform;
    struct hlsl_ir_store *store;
    struct hlsl_ir_load *load;

    /* Use the synthetic name for the temp, rather than the uniform, so that we
     * can write the uniform name into the shader reflection data. */

    if (!(uniform = hlsl_new_var(ctx, temp->name, temp->data_type, temp->loc, NULL, 0, &temp->reg_reservation)))
        return;
    list_add_before(&temp->scope_entry, &uniform->scope_entry);
    list_add_tail(&ctx->extern_vars, &uniform->extern_entry);
    uniform->is_uniform = 1;
    uniform->is_param = temp->is_param;
    uniform->buffer = temp->buffer;

    if (!(name = hlsl_get_string_buffer(ctx)))
        return;
    vkd3d_string_buffer_printf(name, "<temp-%s>", temp->name);
    temp->name = hlsl_strdup(ctx, name->buffer);
    hlsl_release_string_buffer(ctx, name);

    if (!(load = hlsl_new_var_load(ctx, uniform, temp->loc)))
        return;
    list_add_head(instrs, &load->node.entry);

    if (!(store = hlsl_new_simple_store(ctx, temp, &load->node)))
        return;
    list_add_after(&load->node.entry, &store->node.entry);
}

static void prepend_input_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset, const struct hlsl_semantic *semantic)
{
    struct vkd3d_string_buffer *name;
    struct hlsl_semantic new_semantic;
    struct hlsl_ir_constant *offset;
    struct hlsl_ir_store *store;
    struct hlsl_ir_load *load;
    struct hlsl_ir_var *input;

    if (!(name = hlsl_get_string_buffer(ctx)))
        return;
    vkd3d_string_buffer_printf(name, "<input-%s%u>", semantic->name, semantic->index);
    if (!(new_semantic.name = hlsl_strdup(ctx, semantic->name)))
    {
        hlsl_release_string_buffer(ctx, name);
        return;
    }
    new_semantic.index = semantic->index;
    if (!(input = hlsl_new_var(ctx, hlsl_strdup(ctx, name->buffer), type, var->loc, &new_semantic, 0, NULL)))
    {
        hlsl_release_string_buffer(ctx, name);
        vkd3d_free((void *)new_semantic.name);
        return;
    }
    hlsl_release_string_buffer(ctx, name);
    input->is_input_semantic = 1;
    input->is_param = var->is_param;
    list_add_before(&var->scope_entry, &input->scope_entry);
    list_add_tail(&ctx->extern_vars, &input->extern_entry);

    if (!(load = hlsl_new_var_load(ctx, input, var->loc)))
        return;
    list_add_head(instrs, &load->node.entry);

    if (!(offset = hlsl_new_uint_constant(ctx, field_offset, var->loc)))
        return;
    list_add_after(&load->node.entry, &offset->node.entry);

    if (!(store = hlsl_new_store(ctx, var, &offset->node, &load->node, 0, var->loc)))
        return;
    list_add_after(&offset->node.entry, &store->node.entry);
}

static void prepend_input_struct_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset)
{
    struct hlsl_struct_field *field;

    LIST_FOR_EACH_ENTRY(field, type->e.elements, struct hlsl_struct_field, entry)
    {
        if (field->type->type == HLSL_CLASS_STRUCT)
            prepend_input_struct_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset);
        else if (field->semantic.name)
            prepend_input_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset, &field->semantic);
        else
            hlsl_error(ctx, &field->loc, VKD3D_SHADER_ERROR_HLSL_MISSING_SEMANTIC,
                    "Field '%s' is missing a semantic.", field->name);
    }
}

/* Split inputs into two variables representing the semantic and temp registers,
 * and copy the former to the latter, so that writes to input variables work. */
static void prepend_input_var_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var)
{
    if (var->data_type->type == HLSL_CLASS_STRUCT)
        prepend_input_struct_copy(ctx, instrs, var, var->data_type, 0);
    else if (var->semantic.name)
        prepend_input_copy(ctx, instrs, var, var->data_type, 0, &var->semantic);
}

static void append_output_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset, const struct hlsl_semantic *semantic)
{
    struct vkd3d_string_buffer *name;
    struct hlsl_semantic new_semantic;
    struct hlsl_ir_constant *offset;
    struct hlsl_ir_store *store;
    struct hlsl_ir_var *output;
    struct hlsl_ir_load *load;

    if (!(name = hlsl_get_string_buffer(ctx)))
        return;
    vkd3d_string_buffer_printf(name, "<output-%s%u>", semantic->name, semantic->index);
    if (!(new_semantic.name = hlsl_strdup(ctx, semantic->name)))
    {
        hlsl_release_string_buffer(ctx, name);
        return;
    }
    new_semantic.index = semantic->index;
    if (!(output = hlsl_new_var(ctx, hlsl_strdup(ctx, name->buffer), type, var->loc, &new_semantic, 0, NULL)))
    {
        vkd3d_free((void *)new_semantic.name);
        hlsl_release_string_buffer(ctx, name);
        return;
    }
    hlsl_release_string_buffer(ctx, name);
    output->is_output_semantic = 1;
    output->is_param = var->is_param;
    list_add_before(&var->scope_entry, &output->scope_entry);
    list_add_tail(&ctx->extern_vars, &output->extern_entry);

    if (!(offset = hlsl_new_uint_constant(ctx, field_offset, var->loc)))
        return;
    list_add_tail(instrs, &offset->node.entry);

    if (!(load = hlsl_new_load(ctx, var, &offset->node, type, var->loc)))
        return;
    list_add_after(&offset->node.entry, &load->node.entry);

    if (!(store = hlsl_new_store(ctx, output, NULL, &load->node, 0, var->loc)))
        return;
    list_add_after(&load->node.entry, &store->node.entry);
}

static void append_output_struct_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var,
        struct hlsl_type *type, unsigned int field_offset)
{
    struct hlsl_struct_field *field;

    LIST_FOR_EACH_ENTRY(field, type->e.elements, struct hlsl_struct_field, entry)
    {
        if (field->type->type == HLSL_CLASS_STRUCT)
            append_output_struct_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset);
        else if (field->semantic.name)
            append_output_copy(ctx, instrs, var, field->type, field_offset + field->reg_offset, &field->semantic);
        else
            hlsl_error(ctx, &field->loc, VKD3D_SHADER_ERROR_HLSL_MISSING_SEMANTIC,
                    "Field '%s' is missing a semantic.", field->name);
    }
}

/* Split outputs into two variables representing the temp and semantic
 * registers, and copy the former to the latter, so that reads from output
 * variables work. */
static void append_output_var_copy(struct hlsl_ctx *ctx, struct list *instrs, struct hlsl_ir_var *var)
{
    if (var->data_type->type == HLSL_CLASS_STRUCT)
        append_output_struct_copy(ctx, instrs, var, var->data_type, 0);
    else if (var->semantic.name)
        append_output_copy(ctx, instrs, var, var->data_type, 0, &var->semantic);
}

static bool transform_ir(struct hlsl_ctx *ctx, bool (*func)(struct hlsl_ctx *ctx, struct hlsl_ir_node *, void *),
        struct hlsl_block *block, void *context)
{
    struct hlsl_ir_node *instr, *next;
    bool progress = false;

    LIST_FOR_EACH_ENTRY_SAFE(instr, next, &block->instrs, struct hlsl_ir_node, entry)
    {
        if (instr->type == HLSL_IR_IF)
        {
            struct hlsl_ir_if *iff = hlsl_ir_if(instr);

            progress |= transform_ir(ctx, func, &iff->then_instrs, context);
            progress |= transform_ir(ctx, func, &iff->else_instrs, context);
        }
        else if (instr->type == HLSL_IR_LOOP)
            progress |= transform_ir(ctx, func, &hlsl_ir_loop(instr)->body, context);

        progress |= func(ctx, instr, context);
    }

    return progress;
}

static void replace_node(struct hlsl_ir_node *old, struct hlsl_ir_node *new)
{
    struct hlsl_src *src, *next;

    LIST_FOR_EACH_ENTRY_SAFE(src, next, &old->uses, struct hlsl_src, entry)
    {
        hlsl_src_remove(src);
        hlsl_src_from_node(src, new);
    }
    list_remove(&old->entry);
    hlsl_free_instr(old);
}

/* Lower casts from vec1 to vecN to swizzles. */
static bool lower_broadcasts(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    const struct hlsl_type *src_type, *dst_type;
    struct hlsl_type *dst_scalar_type;
    struct hlsl_ir_expr *cast;

    if (instr->type != HLSL_IR_EXPR)
        return false;
    cast = hlsl_ir_expr(instr);
    src_type = cast->operands[0].node->data_type;
    dst_type = cast->node.data_type;

    if (cast->op == HLSL_OP1_CAST
            && src_type->type <= HLSL_CLASS_VECTOR && dst_type->type <= HLSL_CLASS_VECTOR
            && src_type->dimx == 1)
    {
        struct hlsl_ir_swizzle *swizzle;
        struct hlsl_ir_expr *new_cast;

        dst_scalar_type = hlsl_get_scalar_type(ctx, dst_type->base_type);
        /* We need to preserve the cast since it might be doing more than just
         * turning the scalar into a vector. */
        if (!(new_cast = hlsl_new_cast(ctx, cast->operands[0].node, dst_scalar_type, &cast->node.loc)))
            return false;
        list_add_after(&cast->node.entry, &new_cast->node.entry);
        if (!(swizzle = hlsl_new_swizzle(ctx, HLSL_SWIZZLE(X, X, X, X), dst_type->dimx, &new_cast->node, &cast->node.loc)))
            return false;
        list_add_after(&new_cast->node.entry, &swizzle->node.entry);

        replace_node(&cast->node, &swizzle->node);
        return true;
    }

    return false;
}

struct copy_propagation_value
{
    struct hlsl_ir_node *node;
    unsigned int component;
};

struct copy_propagation_var_def
{
    struct rb_entry entry;
    struct hlsl_ir_var *var;
    struct copy_propagation_value values[];
};

struct copy_propagation_state
{
    struct rb_tree var_defs;
};

static int copy_propagation_var_def_compare(const void *key, const struct rb_entry *entry)
{
    struct copy_propagation_var_def *var_def = RB_ENTRY_VALUE(entry, struct copy_propagation_var_def, entry);
    uintptr_t key_int = (uintptr_t)key, entry_int = (uintptr_t)var_def->var;

    return (key_int > entry_int) - (key_int < entry_int);
}

static void copy_propagation_var_def_destroy(struct rb_entry *entry, void *context)
{
    struct copy_propagation_var_def *var_def = RB_ENTRY_VALUE(entry, struct copy_propagation_var_def, entry);

    vkd3d_free(var_def);
}

static struct copy_propagation_var_def *copy_propagation_get_var_def(const struct copy_propagation_state *state,
        const struct hlsl_ir_var *var)
{
    struct rb_entry *entry = rb_get(&state->var_defs, var);

    if (entry)
        return RB_ENTRY_VALUE(entry, struct copy_propagation_var_def, entry);
    else
        return NULL;
}

static struct copy_propagation_var_def *copy_propagation_create_var_def(struct hlsl_ctx *ctx,
        struct copy_propagation_state *state, struct hlsl_ir_var *var)
{
    struct rb_entry *entry = rb_get(&state->var_defs, var);
    struct copy_propagation_var_def *var_def;
    int res;

    if (entry)
        return RB_ENTRY_VALUE(entry, struct copy_propagation_var_def, entry);

    if (!(var_def = hlsl_alloc(ctx, offsetof(struct copy_propagation_var_def, values[var->data_type->reg_size]))))
        return NULL;

    var_def->var = var;

    res = rb_put(&state->var_defs, var, &var_def->entry);
    assert(!res);

    return var_def;
}

static void copy_propagation_invalidate_whole_variable(struct copy_propagation_var_def *var_def)
{
    TRACE("Invalidate variable %s.\n", var_def->var->name);
    memset(var_def->values, 0, sizeof(*var_def->values) * var_def->var->data_type->reg_size);
}

static void copy_propagation_set_value(struct copy_propagation_var_def *var_def, unsigned int offset,
        unsigned char writemask, struct hlsl_ir_node *node)
{
    unsigned int i, j = 0;

    for (i = 0; i < 4; ++i)
    {
        if (writemask & (1u << i))
        {
            TRACE("Variable %s[%u] is written by instruction %p%s.\n",
                    var_def->var->name, offset + i, node, debug_hlsl_writemask(1u << i));
            var_def->values[offset + i].node = node;
            var_def->values[offset + i].component = j++;
        }
    }
}

static struct hlsl_ir_node *copy_propagation_compute_replacement(const struct copy_propagation_state *state,
        const struct hlsl_deref *deref, unsigned int count, unsigned int *swizzle)
{
    const struct hlsl_ir_var *var = deref->var;
    struct copy_propagation_var_def *var_def;
    struct hlsl_ir_node *node = NULL;
    unsigned int offset, i;

    if (!hlsl_offset_from_deref(deref, &offset))
        return NULL;

    if (!(var_def = copy_propagation_get_var_def(state, var)))
        return NULL;

    assert(offset + count <= var_def->var->data_type->reg_size);

    *swizzle = 0;

    for (i = 0; i < count; ++i)
    {
        if (!node)
        {
            node = var_def->values[offset + i].node;
        }
        else if (node != var_def->values[offset + i].node)
        {
            TRACE("No single source for propagating load from %s[%u-%u].\n", var->name, offset, offset + count);
            return NULL;
        }
        *swizzle |= var_def->values[offset + i].component << i * 2;
    }

    TRACE("Load from %s[%u-%u] propagated as instruction %p%s.\n",
            var->name, offset, offset + count, node, debug_hlsl_swizzle(*swizzle, count));
    return node;
}

static bool copy_propagation_analyze_load(struct hlsl_ctx *ctx, struct hlsl_ir_load *load,
        struct copy_propagation_state *state)
{
    struct hlsl_ir_node *node = &load->node, *new_node;
    struct hlsl_type *type = node->data_type;
    struct hlsl_ir_swizzle *swizzle_node;
    unsigned int dimx = 0;
    unsigned int swizzle;

    switch (type->type)
    {
        case HLSL_CLASS_SCALAR:
        case HLSL_CLASS_VECTOR:
            dimx = type->dimx;
            break;

        case HLSL_CLASS_OBJECT:
            dimx = 1;
            break;

        case HLSL_CLASS_MATRIX:
        case HLSL_CLASS_ARRAY:
        case HLSL_CLASS_STRUCT:
            /* FIXME: Actually we shouldn't even get here, but we don't split
             * matrices yet. */
            return false;
    }

    if (!(new_node = copy_propagation_compute_replacement(state, &load->src, dimx, &swizzle)))
        return false;

    if (type->type != HLSL_CLASS_OBJECT)
    {
        if (!(swizzle_node = hlsl_new_swizzle(ctx, swizzle, dimx, new_node, &node->loc)))
            return false;
        list_add_before(&node->entry, &swizzle_node->node.entry);
        new_node = &swizzle_node->node;
    }
    replace_node(node, new_node);
    return true;
}

static void copy_propagation_record_store(struct hlsl_ctx *ctx, struct hlsl_ir_store *store,
        struct copy_propagation_state *state)
{
    struct copy_propagation_var_def *var_def;
    struct hlsl_deref *lhs = &store->lhs;
    struct hlsl_ir_var *var = lhs->var;
    unsigned int offset;

    if (!(var_def = copy_propagation_create_var_def(ctx, state, var)))
        return;

    if (hlsl_offset_from_deref(lhs, &offset))
    {
        unsigned int writemask = store->writemask;

        if (store->rhs.node->data_type->type == HLSL_CLASS_OBJECT)
            writemask = VKD3DSP_WRITEMASK_0;
        copy_propagation_set_value(var_def, offset, writemask, store->rhs.node);
    }
    else
    {
        copy_propagation_invalidate_whole_variable(var_def);
    }
}

static bool copy_propagation_transform_block(struct hlsl_ctx *ctx, struct hlsl_block *block,
        struct copy_propagation_state *state)
{
    struct hlsl_ir_node *instr, *next;
    bool progress = false;

    LIST_FOR_EACH_ENTRY_SAFE(instr, next, &block->instrs, struct hlsl_ir_node, entry)
    {
        switch (instr->type)
        {
            case HLSL_IR_LOAD:
                progress |= copy_propagation_analyze_load(ctx, hlsl_ir_load(instr), state);
                break;

            case HLSL_IR_STORE:
                copy_propagation_record_store(ctx, hlsl_ir_store(instr), state);
                break;

            case HLSL_IR_IF:
                FIXME("Copy propagation doesn't support conditionals yet, leaving.\n");
                return progress;

            case HLSL_IR_LOOP:
                FIXME("Copy propagation doesn't support loops yet, leaving.\n");
                return progress;

            default:
                break;
        }
    }

    return progress;
}

static bool copy_propagation_execute(struct hlsl_ctx *ctx, struct hlsl_block *block)
{
    struct copy_propagation_state state;
    bool progress;

    rb_init(&state.var_defs, copy_propagation_var_def_compare);

    progress = copy_propagation_transform_block(ctx, block, &state);

    rb_destroy(&state.var_defs, copy_propagation_var_def_destroy, NULL);

    return progress;
}

static bool is_vec1(const struct hlsl_type *type)
{
    return (type->type == HLSL_CLASS_SCALAR) || (type->type == HLSL_CLASS_VECTOR && type->dimx == 1);
}

static bool fold_redundant_casts(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    if (instr->type == HLSL_IR_EXPR)
    {
        struct hlsl_ir_expr *expr = hlsl_ir_expr(instr);
        const struct hlsl_type *src_type = expr->operands[0].node->data_type;
        const struct hlsl_type *dst_type = expr->node.data_type;

        if (expr->op != HLSL_OP1_CAST)
            return false;

        if (hlsl_types_are_equal(src_type, dst_type)
                || (src_type->base_type == dst_type->base_type && is_vec1(src_type) && is_vec1(dst_type)))
        {
            replace_node(&expr->node, expr->operands[0].node);
            return true;
        }
    }

    return false;
}

/* Helper for split_array_copies() and split_struct_copies(). Inserts new
 * instructions right before "store". */
static bool split_copy(struct hlsl_ctx *ctx, struct hlsl_ir_store *store,
        const struct hlsl_ir_load *load, const unsigned int offset, struct hlsl_type *type)
{
    struct hlsl_ir_node *offset_instr, *add;
    struct hlsl_ir_store *split_store;
    struct hlsl_ir_load *split_load;
    struct hlsl_ir_constant *c;

    if (!(c = hlsl_new_uint_constant(ctx, offset, store->node.loc)))
        return false;
    list_add_before(&store->node.entry, &c->node.entry);

    offset_instr = &c->node;
    if (load->src.offset.node)
    {
        if (!(add = hlsl_new_binary_expr(ctx, HLSL_OP2_ADD, load->src.offset.node, &c->node)))
            return false;
        list_add_before(&store->node.entry, &add->entry);
        offset_instr = add;
    }
    if (!(split_load = hlsl_new_load(ctx, load->src.var, offset_instr, type, store->node.loc)))
        return false;
    list_add_before(&store->node.entry, &split_load->node.entry);

    offset_instr = &c->node;
    if (store->lhs.offset.node)
    {
        if (!(add = hlsl_new_binary_expr(ctx, HLSL_OP2_ADD, store->lhs.offset.node, &c->node)))
            return false;
        list_add_before(&store->node.entry, &add->entry);
        offset_instr = add;
    }

    if (!(split_store = hlsl_new_store(ctx, store->lhs.var, offset_instr, &split_load->node, 0, store->node.loc)))
        return false;
    list_add_before(&store->node.entry, &split_store->node.entry);

    return true;
}

static bool split_array_copies(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    const struct hlsl_ir_node *rhs;
    struct hlsl_type *element_type;
    const struct hlsl_type *type;
    unsigned int element_size, i;
    struct hlsl_ir_store *store;

    if (instr->type != HLSL_IR_STORE)
        return false;

    store = hlsl_ir_store(instr);
    rhs = store->rhs.node;
    type = rhs->data_type;
    if (type->type != HLSL_CLASS_ARRAY)
        return false;
    element_type = type->e.array.type;
    element_size = element_type->reg_size;

    for (i = 0; i < type->e.array.elements_count; ++i)
    {
        if (!split_copy(ctx, store, hlsl_ir_load(rhs), i * element_size, element_type))
            return false;
    }

    /* Remove the store instruction, so that we can split structs which contain
     * other structs. Although assignments produce a value, we don't allow
     * HLSL_IR_STORE to be used as a source. */
    list_remove(&store->node.entry);
    hlsl_free_instr(&store->node);
    return true;
}

static bool split_struct_copies(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    const struct hlsl_struct_field *field;
    const struct hlsl_ir_node *rhs;
    const struct hlsl_type *type;
    struct hlsl_ir_store *store;

    if (instr->type != HLSL_IR_STORE)
        return false;

    store = hlsl_ir_store(instr);
    rhs = store->rhs.node;
    type = rhs->data_type;
    if (type->type != HLSL_CLASS_STRUCT)
        return false;

    LIST_FOR_EACH_ENTRY(field, type->e.elements, struct hlsl_struct_field, entry)
    {
        if (!split_copy(ctx, store, hlsl_ir_load(rhs), field->reg_offset, field->type))
            return false;
    }

    /* Remove the store instruction, so that we can split structs which contain
     * other structs. Although assignments produce a value, we don't allow
     * HLSL_IR_STORE to be used as a source. */
    list_remove(&store->node.entry);
    hlsl_free_instr(&store->node);
    return true;
}

static bool lower_narrowing_casts(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    const struct hlsl_type *src_type, *dst_type;
    struct hlsl_type *dst_vector_type;
    struct hlsl_ir_expr *cast;

    if (instr->type != HLSL_IR_EXPR)
        return false;
    cast = hlsl_ir_expr(instr);
    src_type = cast->operands[0].node->data_type;
    dst_type = cast->node.data_type;

    if (cast->op == HLSL_OP1_CAST
            && src_type->type <= HLSL_CLASS_VECTOR && dst_type->type <= HLSL_CLASS_VECTOR
            && dst_type->dimx < src_type->dimx)
    {
        struct hlsl_ir_swizzle *swizzle;
        struct hlsl_ir_expr *new_cast;

        dst_vector_type = hlsl_get_vector_type(ctx, dst_type->base_type, src_type->dimx);
        /* We need to preserve the cast since it might be doing more than just
         * narrowing the vector. */
        if (!(new_cast = hlsl_new_cast(ctx, cast->operands[0].node, dst_vector_type, &cast->node.loc)))
            return false;
        list_add_after(&cast->node.entry, &new_cast->node.entry);
        if (!(swizzle = hlsl_new_swizzle(ctx, HLSL_SWIZZLE(X, Y, Z, W), dst_type->dimx, &new_cast->node, &cast->node.loc)))
            return false;
        list_add_after(&new_cast->node.entry, &swizzle->node.entry);

        replace_node(&cast->node, &swizzle->node);
        return true;
    }

    return false;
}

static bool fold_constants(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    struct hlsl_ir_constant *arg1, *arg2 = NULL, *res;
    struct hlsl_ir_expr *expr;
    unsigned int i, dimx;

    if (instr->type != HLSL_IR_EXPR)
        return false;
    expr = hlsl_ir_expr(instr);

    for (i = 0; i < ARRAY_SIZE(expr->operands); ++i)
    {
        if (expr->operands[i].node && expr->operands[i].node->type != HLSL_IR_CONSTANT)
            return false;
    }
    arg1 = hlsl_ir_constant(expr->operands[0].node);
    if (expr->operands[1].node)
        arg2 = hlsl_ir_constant(expr->operands[1].node);
    dimx = instr->data_type->dimx;

    if (!(res = hlsl_alloc(ctx, sizeof(*res))))
        return false;
    init_node(&res->node, HLSL_IR_CONSTANT, instr->data_type, instr->loc);

    switch (instr->data_type->base_type)
    {
        case HLSL_TYPE_FLOAT:
        {
            switch (expr->op)
            {
                case HLSL_OP1_CAST:
                    if (instr->data_type->dimx != arg1->node.data_type->dimx
                            || instr->data_type->dimy != arg1->node.data_type->dimy)
                    {
                        FIXME("Cast from %s to %s.\n", debug_hlsl_type(ctx, arg1->node.data_type),
                                debug_hlsl_type(ctx, instr->data_type));
                        vkd3d_free(res);
                        return false;
                    }

                    switch (arg1->node.data_type->base_type)
                    {
                        case HLSL_TYPE_INT:
                            for (i = 0; i < dimx; ++i)
                                res->value[i].f = arg1->value[i].i;
                            break;

                        case HLSL_TYPE_UINT:
                            for (i = 0; i < dimx; ++i)
                                res->value[i].f = arg1->value[i].u;
                            break;

                        default:
                            FIXME("Cast from %s to %s.\n", debug_hlsl_type(ctx, arg1->node.data_type),
                                    debug_hlsl_type(ctx, instr->data_type));
                            vkd3d_free(res);
                            return false;
                    }
                    break;

                default:
                    FIXME("Fold float op %#x.\n", expr->op);
                    vkd3d_free(res);
                    return false;
            }
            break;
        }

        case HLSL_TYPE_UINT:
        {
            switch (expr->op)
            {
                case HLSL_OP1_CAST:
                    if (instr->data_type->dimx != arg1->node.data_type->dimx
                            || instr->data_type->dimy != arg1->node.data_type->dimy)
                    {
                        FIXME("Cast from %s to %s.\n", debug_hlsl_type(ctx, arg1->node.data_type),
                                debug_hlsl_type(ctx, instr->data_type));
                        vkd3d_free(res);
                        return false;
                    }

                    switch (arg1->node.data_type->base_type)
                    {
                        case HLSL_TYPE_INT:
                            for (i = 0; i < dimx; ++i)
                                res->value[i].i = arg1->value[i].u;
                            break;

                        default:
                            FIXME("Cast from %s to %s.\n", debug_hlsl_type(ctx, arg1->node.data_type),
                                    debug_hlsl_type(ctx, instr->data_type));
                            vkd3d_free(res);
                            return false;
                    }
                    break;

                case HLSL_OP1_NEG:
                    for (i = 0; i < instr->data_type->dimx; ++i)
                        res->value[i].u = -arg1->value[i].u;
                    break;

                case HLSL_OP2_ADD:
                    for (i = 0; i < instr->data_type->dimx; ++i)
                        res->value[i].u = arg1->value[i].u + arg2->value[i].u;
                    break;

                case HLSL_OP2_MUL:
                    for (i = 0; i < instr->data_type->dimx; ++i)
                        res->value[i].u = arg1->value[i].u * arg2->value[i].u;
                    break;

                default:
                    FIXME("Fold uint op %#x.\n", expr->op);
                    vkd3d_free(res);
                    return false;
            }
            break;
        }

        default:
            FIXME("Fold type %#x op %#x.\n", instr->data_type->base_type, expr->op);
            vkd3d_free(res);
            return false;
    }

    list_add_before(&expr->node.entry, &res->node.entry);
    replace_node(&expr->node, &res->node);
    return true;
}

static bool remove_trivial_swizzles(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    struct hlsl_ir_swizzle *swizzle;
    unsigned int i;

    if (instr->type != HLSL_IR_SWIZZLE)
        return false;
    swizzle = hlsl_ir_swizzle(instr);

    if (instr->data_type->dimx != swizzle->val.node->data_type->dimx)
        return false;

    for (i = 0; i < instr->data_type->dimx; ++i)
        if (((swizzle->swizzle >> (2 * i)) & 3) != i)
            return false;

    replace_node(instr, swizzle->val.node);

    return true;
}

/* Lower DIV to RCP + MUL. */
static bool lower_division(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    struct hlsl_ir_expr *expr;
    struct hlsl_ir_node *rcp;

    if (instr->type != HLSL_IR_EXPR)
        return false;
    expr = hlsl_ir_expr(instr);
    if (expr->op != HLSL_OP2_DIV)
        return false;

    if (!(rcp = hlsl_new_unary_expr(ctx, HLSL_OP1_RCP, expr->operands[1].node, instr->loc)))
        return false;
    list_add_before(&expr->node.entry, &rcp->entry);
    expr->op = HLSL_OP2_MUL;
    hlsl_src_remove(&expr->operands[1]);
    hlsl_src_from_node(&expr->operands[1], rcp);
    return true;
}

static bool dce(struct hlsl_ctx *ctx, struct hlsl_ir_node *instr, void *context)
{
    switch (instr->type)
    {
        case HLSL_IR_CONSTANT:
        case HLSL_IR_EXPR:
        case HLSL_IR_LOAD:
        case HLSL_IR_RESOURCE_LOAD:
        case HLSL_IR_SWIZZLE:
            if (list_empty(&instr->uses))
            {
                list_remove(&instr->entry);
                hlsl_free_instr(instr);
                return true;
            }
            break;

        case HLSL_IR_STORE:
        {
            struct hlsl_ir_store *store = hlsl_ir_store(instr);
            struct hlsl_ir_var *var = store->lhs.var;

            if (var->last_read < instr->index)
            {
                list_remove(&instr->entry);
                hlsl_free_instr(instr);
                return true;
            }
            break;
        }

        case HLSL_IR_IF:
        case HLSL_IR_JUMP:
        case HLSL_IR_LOOP:
            break;
    }

    return false;
}

/* Allocate a unique, ordered index to each instruction, which will be used for
 * computing liveness ranges. */
static unsigned int index_instructions(struct hlsl_block *block, unsigned int index)
{
    struct hlsl_ir_node *instr;

    LIST_FOR_EACH_ENTRY(instr, &block->instrs, struct hlsl_ir_node, entry)
    {
        instr->index = index++;

        if (instr->type == HLSL_IR_IF)
        {
            struct hlsl_ir_if *iff = hlsl_ir_if(instr);
            index = index_instructions(&iff->then_instrs, index);
            index = index_instructions(&iff->else_instrs, index);
        }
        else if (instr->type == HLSL_IR_LOOP)
        {
            index = index_instructions(&hlsl_ir_loop(instr)->body, index);
            hlsl_ir_loop(instr)->next_index = index;
        }
    }

    return index;
}

static void dump_function_decl(struct rb_entry *entry, void *context)
{
    struct hlsl_ir_function_decl *func = RB_ENTRY_VALUE(entry, struct hlsl_ir_function_decl, entry);
    struct hlsl_ctx *ctx = context;

    if (func->has_body)
        hlsl_dump_function(ctx, func);
}

static void dump_function(struct rb_entry *entry, void *context)
{
    struct hlsl_ir_function *func = RB_ENTRY_VALUE(entry, struct hlsl_ir_function, entry);
    struct hlsl_ctx *ctx = context;

    rb_for_each_entry(&func->overloads, dump_function_decl, ctx);
}

/* Compute the earliest and latest liveness for each variable. In the case that
 * a variable is accessed inside of a loop, we promote its liveness to extend
 * to at least the range of the entire loop. Note that we don't need to do this
 * for anonymous nodes, since there's currently no way to use a node which was
 * calculated in an earlier iteration of the loop. */
static void compute_liveness_recurse(struct hlsl_block *block, unsigned int loop_first, unsigned int loop_last)
{
    struct hlsl_ir_node *instr;
    struct hlsl_ir_var *var;

    LIST_FOR_EACH_ENTRY(instr, &block->instrs, struct hlsl_ir_node, entry)
    {
        const unsigned int var_last_read = loop_last ? max(instr->index, loop_last) : instr->index;

        switch (instr->type)
        {
        case HLSL_IR_STORE:
        {
            struct hlsl_ir_store *store = hlsl_ir_store(instr);

            var = store->lhs.var;
            if (!var->first_write)
                var->first_write = loop_first ? min(instr->index, loop_first) : instr->index;
            store->rhs.node->last_read = instr->index;
            if (store->lhs.offset.node)
                store->lhs.offset.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_EXPR:
        {
            struct hlsl_ir_expr *expr = hlsl_ir_expr(instr);
            unsigned int i;

            for (i = 0; i < ARRAY_SIZE(expr->operands) && expr->operands[i].node; ++i)
                expr->operands[i].node->last_read = instr->index;
            break;
        }
        case HLSL_IR_IF:
        {
            struct hlsl_ir_if *iff = hlsl_ir_if(instr);

            compute_liveness_recurse(&iff->then_instrs, loop_first, loop_last);
            compute_liveness_recurse(&iff->else_instrs, loop_first, loop_last);
            iff->condition.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_LOAD:
        {
            struct hlsl_ir_load *load = hlsl_ir_load(instr);

            var = load->src.var;
            var->last_read = max(var->last_read, var_last_read);
            if (load->src.offset.node)
                load->src.offset.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_LOOP:
        {
            struct hlsl_ir_loop *loop = hlsl_ir_loop(instr);

            compute_liveness_recurse(&loop->body, loop_first ? loop_first : instr->index,
                    loop_last ? loop_last : loop->next_index);
            break;
        }
        case HLSL_IR_RESOURCE_LOAD:
        {
            struct hlsl_ir_resource_load *load = hlsl_ir_resource_load(instr);

            var = load->resource.var;
            var->last_read = max(var->last_read, var_last_read);
            if (load->resource.offset.node)
                load->resource.offset.node->last_read = instr->index;

            if ((var = load->sampler.var))
            {
                var->last_read = max(var->last_read, var_last_read);
                if (load->sampler.offset.node)
                    load->sampler.offset.node->last_read = instr->index;
            }

            load->coords.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_SWIZZLE:
        {
            struct hlsl_ir_swizzle *swizzle = hlsl_ir_swizzle(instr);

            swizzle->val.node->last_read = instr->index;
            break;
        }
        case HLSL_IR_CONSTANT:
        case HLSL_IR_JUMP:
            break;
        }
    }
}

static void compute_liveness(struct hlsl_ctx *ctx, struct hlsl_ir_function_decl *entry_func)
{
    struct hlsl_scope *scope;
    struct hlsl_ir_var *var;

    /* Index 0 means unused; index 1 means function entry, so start at 2. */
    index_instructions(&entry_func->body, 2);

    LIST_FOR_EACH_ENTRY(scope, &ctx->scopes, struct hlsl_scope, entry)
    {
        LIST_FOR_EACH_ENTRY(var, &scope->vars, struct hlsl_ir_var, scope_entry)
            var->first_write = var->last_read = 0;
    }

    LIST_FOR_EACH_ENTRY(var, &ctx->extern_vars, struct hlsl_ir_var, extern_entry)
    {
        if (var->is_uniform || var->is_input_semantic)
            var->first_write = 1;
        else if (var->is_output_semantic)
            var->last_read = UINT_MAX;
    }

    compute_liveness_recurse(&entry_func->body, 0, 0);
}

struct liveness
{
    size_t size;
    uint32_t reg_count;
    struct
    {
        /* 0 if not live yet. */
        unsigned int last_read;
    } *regs;
};

static unsigned int get_available_writemask(struct liveness *liveness,
        unsigned int first_write, unsigned int component_idx, unsigned int component_count)
{
    unsigned int i, writemask = 0, count = 0;

    for (i = 0; i < 4; ++i)
    {
        if (liveness->regs[component_idx + i].last_read <= first_write)
        {
            writemask |= 1u << i;
            if (++count == component_count)
                return writemask;
        }
    }

    return 0;
}

static bool resize_liveness(struct hlsl_ctx *ctx, struct liveness *liveness, size_t new_count)
{
    size_t old_capacity = liveness->size;

    if (!hlsl_array_reserve(ctx, (void **)&liveness->regs, &liveness->size, new_count, sizeof(*liveness->regs)))
        return false;

    if (liveness->size > old_capacity)
        memset(liveness->regs + old_capacity, 0, (liveness->size - old_capacity) * sizeof(*liveness->regs));
    return true;
}

static struct hlsl_reg allocate_register(struct hlsl_ctx *ctx, struct liveness *liveness,
        unsigned int first_write, unsigned int last_read, unsigned int component_count)
{
    unsigned int component_idx, writemask, i;
    struct hlsl_reg ret = {0};

    for (component_idx = 0; component_idx < liveness->size; component_idx += 4)
    {
        if ((writemask = get_available_writemask(liveness, first_write, component_idx, component_count)))
            break;
    }
    if (component_idx == liveness->size)
    {
        if (!resize_liveness(ctx, liveness, component_idx + 4))
            return ret;
        writemask = (1u << component_count) - 1;
    }
    for (i = 0; i < 4; ++i)
    {
        if (writemask & (1u << i))
            liveness->regs[component_idx + i].last_read = last_read;
    }
    ret.id = component_idx / 4;
    ret.writemask = writemask;
    ret.allocated = true;
    liveness->reg_count = max(liveness->reg_count, ret.id + 1);
    return ret;
}

static bool is_range_available(struct liveness *liveness, unsigned int first_write,
        unsigned int component_idx, unsigned int component_count)
{
    unsigned int i;

    for (i = 0; i < component_count; i += 4)
    {
        if (!get_available_writemask(liveness, first_write, component_idx + i, 4))
            return false;
    }
    return true;
}

static struct hlsl_reg allocate_range(struct hlsl_ctx *ctx, struct liveness *liveness,
        unsigned int first_write, unsigned int last_read, unsigned int component_count)
{
    unsigned int i, component_idx;
    struct hlsl_reg ret = {0};

    for (component_idx = 0; component_idx < liveness->size; component_idx += 4)
    {
        if (is_range_available(liveness, first_write, component_idx,
                min(component_count, liveness->size - component_idx)))
            break;
    }
    if (!resize_liveness(ctx, liveness, component_idx + component_count))
        return ret;

    for (i = 0; i < component_count; ++i)
        liveness->regs[component_idx + i].last_read = last_read;
    ret.id = component_idx / 4;
    ret.allocated = true;
    liveness->reg_count = max(liveness->reg_count, ret.id + align(component_count, 4));
    return ret;
}

static const char *debug_register(char class, struct hlsl_reg reg, const struct hlsl_type *type)
{
    static const char writemask_offset[] = {'w','x','y','z'};

    if (type->reg_size > 4)
    {
        if (type->reg_size & 3)
            return vkd3d_dbg_sprintf("%c%u-%c%u.%c", class, reg.id, class,
                    reg.id + (type->reg_size / 4), writemask_offset[type->reg_size & 3]);

        return vkd3d_dbg_sprintf("%c%u-%c%u", class, reg.id, class,
                reg.id + (type->reg_size / 4) - 1);
    }
    return vkd3d_dbg_sprintf("%c%u%s", class, reg.id, debug_hlsl_writemask(reg.writemask));
}

static void allocate_variable_temp_register(struct hlsl_ctx *ctx, struct hlsl_ir_var *var, struct liveness *liveness)
{
    if (var->is_input_semantic || var->is_output_semantic || var->is_uniform)
        return;

    if (!var->reg.allocated && var->last_read)
    {
        if (var->data_type->reg_size > 4)
            var->reg = allocate_range(ctx, liveness, var->first_write,
                    var->last_read, var->data_type->reg_size);
        else
            var->reg = allocate_register(ctx, liveness, var->first_write,
                    var->last_read, var->data_type->dimx);
        TRACE("Allocated %s to %s (liveness %u-%u).\n", var->name,
                debug_register('r', var->reg, var->data_type), var->first_write, var->last_read);
    }
}

static void allocate_temp_registers_recurse(struct hlsl_ctx *ctx, struct hlsl_block *block, struct liveness *liveness)
{
    struct hlsl_ir_node *instr;

    LIST_FOR_EACH_ENTRY(instr, &block->instrs, struct hlsl_ir_node, entry)
    {
        if (!instr->reg.allocated && instr->last_read)
        {
            if (instr->data_type->reg_size > 4)
                instr->reg = allocate_range(ctx, liveness, instr->index,
                        instr->last_read, instr->data_type->reg_size);
            else
                instr->reg = allocate_register(ctx, liveness, instr->index,
                        instr->last_read, instr->data_type->dimx);
            TRACE("Allocated anonymous expression @%u to %s (liveness %u-%u).\n", instr->index,
                    debug_register('r', instr->reg, instr->data_type), instr->index, instr->last_read);
        }

        switch (instr->type)
        {
            case HLSL_IR_IF:
            {
                struct hlsl_ir_if *iff = hlsl_ir_if(instr);
                allocate_temp_registers_recurse(ctx, &iff->then_instrs, liveness);
                allocate_temp_registers_recurse(ctx, &iff->else_instrs, liveness);
                break;
            }

            case HLSL_IR_LOAD:
            {
                struct hlsl_ir_load *load = hlsl_ir_load(instr);
                /* We need to at least allocate a variable for undefs.
                 * FIXME: We should probably find a way to remove them instead. */
                allocate_variable_temp_register(ctx, load->src.var, liveness);
                break;
            }

            case HLSL_IR_LOOP:
            {
                struct hlsl_ir_loop *loop = hlsl_ir_loop(instr);
                allocate_temp_registers_recurse(ctx, &loop->body, liveness);
                break;
            }

            case HLSL_IR_STORE:
            {
                struct hlsl_ir_store *store = hlsl_ir_store(instr);
                allocate_variable_temp_register(ctx, store->lhs.var, liveness);
                break;
            }

            default:
                break;
        }
    }
}

static void allocate_const_registers_recurse(struct hlsl_ctx *ctx, struct hlsl_block *block, struct liveness *liveness)
{
    struct hlsl_constant_defs *defs = &ctx->constant_defs;
    struct hlsl_ir_node *instr;

    LIST_FOR_EACH_ENTRY(instr, &block->instrs, struct hlsl_ir_node, entry)
    {
        switch (instr->type)
        {
            case HLSL_IR_CONSTANT:
            {
                struct hlsl_ir_constant *constant = hlsl_ir_constant(instr);
                const struct hlsl_type *type = instr->data_type;
                unsigned int x, y, i, writemask, end_reg;
                unsigned int reg_size = type->reg_size;

                if (reg_size > 4)
                    constant->reg = allocate_range(ctx, liveness, 1, UINT_MAX, reg_size);
                else
                    constant->reg = allocate_register(ctx, liveness, 1, UINT_MAX, type->dimx);
                TRACE("Allocated constant @%u to %s.\n", instr->index, debug_register('c', constant->reg, type));

                if (!hlsl_array_reserve(ctx, (void **)&defs->values, &defs->size,
                        constant->reg.id + reg_size / 4, sizeof(*defs->values)))
                    return;
                end_reg = constant->reg.id + reg_size / 4;
                if (end_reg > defs->count)
                {
                    memset(&defs->values[defs->count], 0, sizeof(*defs->values) * (end_reg - defs->count));
                    defs->count = end_reg;
                }

                assert(type->type <= HLSL_CLASS_LAST_NUMERIC);

                if (!(writemask = constant->reg.writemask))
                    writemask = (1u << type->dimx) - 1;

                for (y = 0; y < type->dimy; ++y)
                {
                    for (x = 0, i = 0; x < 4; ++x)
                    {
                        const union hlsl_constant_value *value;
                        float f;

                        if (!(writemask & (1u << x)))
                            continue;
                        value = &constant->value[i++];

                        switch (type->base_type)
                        {
                            case HLSL_TYPE_BOOL:
                                f = value->b;
                                break;

                            case HLSL_TYPE_FLOAT:
                            case HLSL_TYPE_HALF:
                                f = value->f;
                                break;

                            case HLSL_TYPE_INT:
                                f = value->i;
                                break;

                            case HLSL_TYPE_UINT:
                                f = value->u;
                                break;

                            case HLSL_TYPE_DOUBLE:
                                FIXME("Double constant.\n");
                                return;

                            default:
                                assert(0);
                                return;
                        }
                        defs->values[constant->reg.id + y].f[x] = f;
                    }
                }

                break;
            }

            case HLSL_IR_IF:
            {
                struct hlsl_ir_if *iff = hlsl_ir_if(instr);
                allocate_const_registers_recurse(ctx, &iff->then_instrs, liveness);
                allocate_const_registers_recurse(ctx, &iff->else_instrs, liveness);
                break;
            }

            case HLSL_IR_LOOP:
            {
                struct hlsl_ir_loop *loop = hlsl_ir_loop(instr);
                allocate_const_registers_recurse(ctx, &loop->body, liveness);
                break;
            }

            default:
                break;
        }
    }
}

static void allocate_const_registers(struct hlsl_ctx *ctx, struct hlsl_ir_function_decl *entry_func)
{
    struct liveness liveness = {0};
    struct hlsl_ir_var *var;

    allocate_const_registers_recurse(ctx, &entry_func->body, &liveness);

    LIST_FOR_EACH_ENTRY(var, &ctx->extern_vars, struct hlsl_ir_var, extern_entry)
    {
        if (var->is_uniform && var->last_read)
        {
            if (var->data_type->reg_size > 4)
                var->reg = allocate_range(ctx, &liveness, 1, UINT_MAX, var->data_type->reg_size);
            else
            {
                var->reg = allocate_register(ctx, &liveness, 1, UINT_MAX, 4);
                var->reg.writemask = (1u << var->data_type->dimx) - 1;
            }
            TRACE("Allocated %s to %s.\n", var->name, debug_register('c', var->reg, var->data_type));
        }
    }
}

/* Simple greedy temporary register allocation pass that just assigns a unique
 * index to all (simultaneously live) variables or intermediate values. Agnostic
 * as to how many registers are actually available for the current backend, and
 * does not handle constants. */
static void allocate_temp_registers(struct hlsl_ctx *ctx, struct hlsl_ir_function_decl *entry_func)
{
    struct liveness liveness = {0};
    allocate_temp_registers_recurse(ctx, &entry_func->body, &liveness);
    ctx->temp_count = liveness.reg_count;
    vkd3d_free(liveness.regs);
}

static void allocate_semantic_register(struct hlsl_ctx *ctx, struct hlsl_ir_var *var, unsigned int *counter, bool output)
{
    static const char *shader_names[] =
    {
        [VKD3D_SHADER_TYPE_PIXEL] = "Pixel",
        [VKD3D_SHADER_TYPE_VERTEX] = "Vertex",
        [VKD3D_SHADER_TYPE_GEOMETRY] = "Geometry",
        [VKD3D_SHADER_TYPE_HULL] = "Hull",
        [VKD3D_SHADER_TYPE_DOMAIN] = "Domain",
        [VKD3D_SHADER_TYPE_COMPUTE] = "Compute",
    };

    unsigned int type;
    uint32_t reg;
    bool builtin;

    assert(var->semantic.name);

    if (ctx->profile->major_version < 4)
    {
        D3DDECLUSAGE usage;
        uint32_t usage_idx;

        if (!hlsl_sm1_usage_from_semantic(&var->semantic, &usage, &usage_idx))
        {
            hlsl_error(ctx, &var->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_SEMANTIC,
                    "Invalid semantic '%s'.", var->semantic.name);
            return;
        }

        if ((!output && !var->last_read) || (output && !var->first_write))
            return;

        builtin = hlsl_sm1_register_from_semantic(ctx, &var->semantic, output, &type, &reg);
    }
    else
    {
        D3D_NAME usage;
        bool has_idx;

        if (!hlsl_sm4_usage_from_semantic(ctx, &var->semantic, output, &usage))
        {
            hlsl_error(ctx, &var->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_SEMANTIC,
                    "Invalid semantic '%s'.", var->semantic.name);
            return;
        }
        if ((builtin = hlsl_sm4_register_from_semantic(ctx, &var->semantic, output, &type, NULL, &has_idx)))
            reg = has_idx ? var->semantic.index : 0;
    }

    if (builtin)
    {
        TRACE("%s %s semantic %s[%u] matches predefined register %#x[%u].\n", shader_names[ctx->profile->type],
                output ? "output" : "input", var->semantic.name, var->semantic.index, type, reg);
    }
    else
    {
        var->reg.allocated = true;
        var->reg.id = (*counter)++;
        var->reg.writemask = (1 << var->data_type->dimx) - 1;
        TRACE("Allocated %s to %s.\n", var->name, debug_register(output ? 'o' : 'v', var->reg, var->data_type));
    }
}

static void allocate_semantic_registers(struct hlsl_ctx *ctx)
{
    unsigned int input_counter = 0, output_counter = 0;
    struct hlsl_ir_var *var;

    LIST_FOR_EACH_ENTRY(var, &ctx->extern_vars, struct hlsl_ir_var, extern_entry)
    {
        if (var->is_input_semantic)
            allocate_semantic_register(ctx, var, &input_counter, false);
        if (var->is_output_semantic)
            allocate_semantic_register(ctx, var, &output_counter, true);
    }
}

static const struct hlsl_buffer *get_reserved_buffer(struct hlsl_ctx *ctx, uint32_t index)
{
    const struct hlsl_buffer *buffer;

    LIST_FOR_EACH_ENTRY(buffer, &ctx->buffers, const struct hlsl_buffer, entry)
    {
        if (buffer->used_size && buffer->reservation.type == 'b' && buffer->reservation.index == index)
            return buffer;
    }
    return NULL;
}

static void calculate_buffer_offset(struct hlsl_ir_var *var)
{
    struct hlsl_buffer *buffer = var->buffer;

    buffer->size = hlsl_type_get_sm4_offset(var->data_type, buffer->size);

    var->buffer_offset = buffer->size;
    TRACE("Allocated buffer offset %u to %s.\n", var->buffer_offset, var->name);
    buffer->size += var->data_type->reg_size;
    if (var->last_read)
        buffer->used_size = buffer->size;
}

static void allocate_buffers(struct hlsl_ctx *ctx)
{
    struct hlsl_buffer *buffer;
    struct hlsl_ir_var *var;
    uint32_t index = 0;

    LIST_FOR_EACH_ENTRY(var, &ctx->extern_vars, struct hlsl_ir_var, extern_entry)
    {
        if (var->is_uniform && var->data_type->type != HLSL_CLASS_OBJECT)
        {
            if (var->is_param)
                var->buffer = ctx->params_buffer;

            calculate_buffer_offset(var);
        }
    }

    LIST_FOR_EACH_ENTRY(buffer, &ctx->buffers, struct hlsl_buffer, entry)
    {
        if (!buffer->used_size)
            continue;

        if (buffer->type == HLSL_BUFFER_CONSTANT)
        {
            if (buffer->reservation.type == 'b')
            {
                const struct hlsl_buffer *reserved_buffer = get_reserved_buffer(ctx, buffer->reservation.index);

                if (reserved_buffer && reserved_buffer != buffer)
                {
                    hlsl_error(ctx, &buffer->loc, VKD3D_SHADER_ERROR_HLSL_OVERLAPPING_RESERVATIONS,
                            "Multiple buffers bound to cb%u.", buffer->reservation.index);
                    hlsl_note(ctx, &reserved_buffer->loc, VKD3D_SHADER_LOG_ERROR,
                            "Buffer %s is already bound to cb%u.", reserved_buffer->name, buffer->reservation.index);
                }

                buffer->reg.id = buffer->reservation.index;
                buffer->reg.allocated = true;
                TRACE("Allocated reserved %s to cb%u.\n", buffer->name, index);
            }
            else if (!buffer->reservation.type)
            {
                while (get_reserved_buffer(ctx, index))
                    ++index;

                buffer->reg.id = index;
                buffer->reg.allocated = true;
                TRACE("Allocated %s to cb%u.\n", buffer->name, index);
                ++index;
            }
            else
            {
                hlsl_error(ctx, &buffer->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_RESERVATION,
                        "Constant buffers must be allocated to register type 'b'.");
            }
        }
        else
        {
            FIXME("Allocate registers for texture buffers.\n");
        }
    }
}

static const struct hlsl_ir_var *get_reserved_object(struct hlsl_ctx *ctx, char type, uint32_t index)
{
    const struct hlsl_ir_var *var;

    LIST_FOR_EACH_ENTRY(var, &ctx->extern_vars, const struct hlsl_ir_var, extern_entry)
    {
        if (var->last_read && var->reg_reservation.type == type && var->reg_reservation.index == index)
            return var;
    }
    return NULL;
}

static const struct object_type_info
{
    enum hlsl_base_type type;
    char reg_name;
}
object_types[] =
{
    { HLSL_TYPE_SAMPLER, 's' },
    { HLSL_TYPE_TEXTURE, 't' },
};

static const struct object_type_info *get_object_type_info(enum hlsl_base_type type)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(object_types); ++i)
        if (type == object_types[i].type)
            return &object_types[i];

    WARN("No type info for object type %u.\n", type);
    return NULL;
}

static void allocate_objects(struct hlsl_ctx *ctx, enum hlsl_base_type type)
{
    const struct object_type_info *type_info = get_object_type_info(type);
    struct hlsl_ir_var *var;
    uint32_t index = 0;

    LIST_FOR_EACH_ENTRY(var, &ctx->extern_vars, struct hlsl_ir_var, extern_entry)
    {
        if (!var->last_read || var->data_type->type != HLSL_CLASS_OBJECT
                || var->data_type->base_type != type)
            continue;

        if (var->reg_reservation.type == type_info->reg_name)
        {
            const struct hlsl_ir_var *reserved_object = get_reserved_object(ctx, type_info->reg_name,
                    var->reg_reservation.index);

            if (reserved_object && reserved_object != var)
            {
                hlsl_error(ctx, &var->loc, VKD3D_SHADER_ERROR_HLSL_OVERLAPPING_RESERVATIONS,
                        "Multiple objects bound to %c%u.", type_info->reg_name,
                        var->reg_reservation.index);
                hlsl_note(ctx, &reserved_object->loc, VKD3D_SHADER_LOG_ERROR,
                        "Object '%s' is already bound to %c%u.", reserved_object->name,
                        type_info->reg_name, var->reg_reservation.index);
            }

            var->reg.id = var->reg_reservation.index;
            var->reg.allocated = true;
            TRACE("Allocated reserved %s to %c%u.\n", var->name, type_info->reg_name, var->reg_reservation.index);
        }
        else if (!var->reg_reservation.type)
        {
            while (get_reserved_object(ctx, type_info->reg_name, index))
                ++index;

            var->reg.id = index;
            var->reg.allocated = true;
            TRACE("Allocated object to %c%u.\n", type_info->reg_name, index);
            ++index;
        }
        else
        {
            struct vkd3d_string_buffer *type_string;

            type_string = hlsl_type_to_string(ctx, var->data_type);
            hlsl_error(ctx, &var->loc, VKD3D_SHADER_ERROR_HLSL_INVALID_RESERVATION,
                    "Object of type '%s' must be bound to register type '%c'.",
                    type_string->buffer, type_info->reg_name);
            hlsl_release_string_buffer(ctx, type_string);
        }
    }
}

static bool type_is_single_reg(const struct hlsl_type *type)
{
    return type->type == HLSL_CLASS_SCALAR || type->type == HLSL_CLASS_VECTOR;
}

bool hlsl_offset_from_deref(const struct hlsl_deref *deref, unsigned int *offset)
{
    struct hlsl_ir_node *offset_node = deref->offset.node;

    if (!offset_node)
    {
        *offset = 0;
        return true;
    }

    /* We should always have generated a cast to UINT. */
    assert(offset_node->data_type->type == HLSL_CLASS_SCALAR
            && offset_node->data_type->base_type == HLSL_TYPE_UINT);

    if (offset_node->type != HLSL_IR_CONSTANT)
        return false;

    *offset = hlsl_ir_constant(offset_node)->value[0].u;
    return true;
}

unsigned int hlsl_offset_from_deref_safe(struct hlsl_ctx *ctx, const struct hlsl_deref *deref)
{
    unsigned int offset;

    if (hlsl_offset_from_deref(deref, &offset))
        return offset;

    hlsl_fixme(ctx, &deref->offset.node->loc, "Dereference with non-constant offset of type %s.",
            hlsl_node_type_to_string(deref->offset.node->type));

    return 0;
}

struct hlsl_reg hlsl_reg_from_deref(struct hlsl_ctx *ctx, const struct hlsl_deref *deref,
        const struct hlsl_type *type)
{
    const struct hlsl_ir_var *var = deref->var;
    struct hlsl_reg ret = var->reg;
    unsigned int offset = hlsl_offset_from_deref_safe(ctx, deref);

    ret.id += offset / 4;

    if (type_is_single_reg(var->data_type))
    {
        assert(!offset);
        ret.writemask = var->reg.writemask;
    }
    else
    {
        assert(type_is_single_reg(type));
        ret.writemask = ((1 << type->dimx) - 1) << (offset % 4);
    }
    return ret;
}

int hlsl_emit_dxbc(struct hlsl_ctx *ctx, struct hlsl_ir_function_decl *entry_func, struct vkd3d_shader_code *out)
{
    struct hlsl_block *const body = &entry_func->body;
    struct hlsl_ir_var *var;
    bool progress;

    list_move_head(&body->instrs, &ctx->static_initializers);

    LIST_FOR_EACH_ENTRY(var, &ctx->globals->vars, struct hlsl_ir_var, scope_entry)
    {
        if (var->modifiers & HLSL_STORAGE_UNIFORM)
            prepend_uniform_copy(ctx, &body->instrs, var);
    }

    LIST_FOR_EACH_ENTRY(var, entry_func->parameters, struct hlsl_ir_var, param_entry)
    {
        if (var->data_type->type == HLSL_CLASS_OBJECT || (var->modifiers & HLSL_STORAGE_UNIFORM))
        {
            prepend_uniform_copy(ctx, &body->instrs, var);
        }
        else
        {
            if (var->data_type->type != HLSL_CLASS_STRUCT && !var->semantic.name)
                hlsl_error(ctx, &var->loc, VKD3D_SHADER_ERROR_HLSL_MISSING_SEMANTIC,
                        "Parameter \"%s\" is missing a semantic.", var->name);

            if (var->modifiers & HLSL_STORAGE_IN)
                prepend_input_var_copy(ctx, &body->instrs, var);
            if (var->modifiers & HLSL_STORAGE_OUT)
                append_output_var_copy(ctx, &body->instrs, var);
        }
    }
    if (entry_func->return_var)
    {
        if (entry_func->return_var->data_type->type != HLSL_CLASS_STRUCT && !entry_func->return_var->semantic.name)
            hlsl_error(ctx, &entry_func->loc, VKD3D_SHADER_ERROR_HLSL_MISSING_SEMANTIC,
                    "Entry point \"%s\" is missing a return value semantic.", entry_func->func->name);

        append_output_var_copy(ctx, &body->instrs, entry_func->return_var);
    }

    transform_ir(ctx, lower_broadcasts, body, NULL);
    while (transform_ir(ctx, fold_redundant_casts, body, NULL));
    do
    {
        progress = transform_ir(ctx, split_array_copies, body, NULL);
        progress |= transform_ir(ctx, split_struct_copies, body, NULL);
    }
    while (progress);
    transform_ir(ctx, lower_narrowing_casts, body, NULL);
    do
    {
        progress = transform_ir(ctx, fold_constants, body, NULL);
        progress |= copy_propagation_execute(ctx, body);
    }
    while (progress);
    transform_ir(ctx, remove_trivial_swizzles, body, NULL);

    if (ctx->profile->major_version < 4)
        transform_ir(ctx, lower_division, body, NULL);

    do
        compute_liveness(ctx, entry_func);
    while (transform_ir(ctx, dce, body, NULL));

    compute_liveness(ctx, entry_func);

    if (TRACE_ON())
        rb_for_each_entry(&ctx->functions, dump_function, ctx);

    allocate_temp_registers(ctx, entry_func);
    if (ctx->profile->major_version < 4)
    {
        allocate_const_registers(ctx, entry_func);
    }
    else
    {
        allocate_buffers(ctx);
        allocate_objects(ctx, HLSL_TYPE_TEXTURE);
    }
    allocate_semantic_registers(ctx);
    allocate_objects(ctx, HLSL_TYPE_SAMPLER);

    if (ctx->result)
        return ctx->result;

    if (ctx->profile->major_version < 4)
        return hlsl_sm1_write(ctx, entry_func, out);
    else
        return hlsl_sm4_write(ctx, entry_func, out);
}
