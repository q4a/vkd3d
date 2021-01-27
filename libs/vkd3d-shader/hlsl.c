/*
 * Copyright 2012 Matteo Bruni for CodeWeavers
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

BOOL add_declaration(struct hlsl_scope *scope, struct hlsl_ir_var *decl, BOOL local_var)
{
    struct hlsl_ir_var *var;

    LIST_FOR_EACH_ENTRY(var, &scope->vars, struct hlsl_ir_var, scope_entry)
    {
        if (!strcmp(decl->name, var->name))
            return FALSE;
    }
    if (local_var && scope->upper->upper == hlsl_ctx.globals)
    {
        /* Check whether the variable redefines a function parameter. */
        LIST_FOR_EACH_ENTRY(var, &scope->upper->vars, struct hlsl_ir_var, scope_entry)
        {
            if (!strcmp(decl->name, var->name))
                return FALSE;
        }
    }

    list_add_tail(&scope->vars, &decl->scope_entry);
    return TRUE;
}

struct hlsl_ir_var *get_variable(struct hlsl_scope *scope, const char *name)
{
    struct hlsl_ir_var *var;

    LIST_FOR_EACH_ENTRY(var, &scope->vars, struct hlsl_ir_var, scope_entry)
    {
        if (!strcmp(name, var->name))
            return var;
    }
    if (!scope->upper)
        return NULL;
    return get_variable(scope->upper, name);
}

void free_declaration(struct hlsl_ir_var *decl)
{
    vkd3d_free((void *)decl->name);
    vkd3d_free((void *)decl->semantic);
    vkd3d_free((void *)decl->reg_reservation);
    vkd3d_free(decl);
}

struct hlsl_type *new_hlsl_type(const char *name, enum hlsl_type_class type_class,
        enum hlsl_base_type base_type, unsigned dimx, unsigned dimy)
{
    struct hlsl_type *type;

    if (!(type = vkd3d_calloc(1, sizeof(*type))))
        return NULL;
    type->name = name;
    type->type = type_class;
    type->base_type = base_type;
    type->dimx = dimx;
    type->dimy = dimy;
    if (type_class == HLSL_CLASS_MATRIX)
        type->reg_size = is_row_major(type) ? dimy : dimx;
    else
        type->reg_size = 1;

    list_add_tail(&hlsl_ctx.types, &type->entry);

    return type;
}

struct hlsl_type *new_array_type(struct hlsl_type *basic_type, unsigned int array_size)
{
    struct hlsl_type *type = new_hlsl_type(NULL, HLSL_CLASS_ARRAY, HLSL_TYPE_FLOAT, 1, 1);

    if (!type)
        return NULL;

    type->modifiers = basic_type->modifiers;
    type->e.array.elements_count = array_size;
    type->e.array.type = basic_type;
    type->reg_size = basic_type->reg_size * array_size;
    type->dimx = basic_type->dimx;
    type->dimy = basic_type->dimy;
    return type;
}

struct hlsl_type *get_type(struct hlsl_scope *scope, const char *name, BOOL recursive)
{
    struct rb_entry *entry = rb_get(&scope->types, name);

    if (entry)
        return RB_ENTRY_VALUE(entry, struct hlsl_type, scope_entry);

    if (recursive && scope->upper)
        return get_type(scope->upper, name, recursive);
    return NULL;
}

BOOL find_function(const char *name)
{
    return rb_get(&hlsl_ctx.functions, name) != NULL;
}

unsigned int components_count_type(struct hlsl_type *type)
{
    struct hlsl_struct_field *field;
    unsigned int count = 0;

    if (type->type <= HLSL_CLASS_LAST_NUMERIC)
    {
        return type->dimx * type->dimy;
    }
    if (type->type == HLSL_CLASS_ARRAY)
    {
        return components_count_type(type->e.array.type) * type->e.array.elements_count;
    }
    if (type->type != HLSL_CLASS_STRUCT)
    {
        ERR("Unexpected data type %s.\n", debug_hlsl_type(type));
        return 0;
    }

    LIST_FOR_EACH_ENTRY(field, type->e.elements, struct hlsl_struct_field, entry)
    {
        count += components_count_type(field->type);
    }
    return count;
}

BOOL compare_hlsl_types(const struct hlsl_type *t1, const struct hlsl_type *t2)
{
    if (t1 == t2)
        return TRUE;

    if (t1->type != t2->type)
        return FALSE;
    if (t1->base_type != t2->base_type)
        return FALSE;
    if (t1->base_type == HLSL_TYPE_SAMPLER && t1->sampler_dim != t2->sampler_dim)
        return FALSE;
    if ((t1->modifiers & HLSL_MODIFIERS_MAJORITY_MASK)
            != (t2->modifiers & HLSL_MODIFIERS_MAJORITY_MASK))
        return FALSE;
    if (t1->dimx != t2->dimx)
        return FALSE;
    if (t1->dimy != t2->dimy)
        return FALSE;
    if (t1->type == HLSL_CLASS_STRUCT)
    {
        struct list *t1cur, *t2cur;
        struct hlsl_struct_field *t1field, *t2field;

        t1cur = list_head(t1->e.elements);
        t2cur = list_head(t2->e.elements);
        while (t1cur && t2cur)
        {
            t1field = LIST_ENTRY(t1cur, struct hlsl_struct_field, entry);
            t2field = LIST_ENTRY(t2cur, struct hlsl_struct_field, entry);
            if (!compare_hlsl_types(t1field->type, t2field->type))
                return FALSE;
            if (strcmp(t1field->name, t2field->name))
                return FALSE;
            t1cur = list_next(t1->e.elements, t1cur);
            t2cur = list_next(t2->e.elements, t2cur);
        }
        if (t1cur != t2cur)
            return FALSE;
    }
    if (t1->type == HLSL_CLASS_ARRAY)
        return t1->e.array.elements_count == t2->e.array.elements_count
                && compare_hlsl_types(t1->e.array.type, t2->e.array.type);

    return TRUE;
}

struct hlsl_type *clone_hlsl_type(struct hlsl_type *old, unsigned int default_majority)
{
    struct hlsl_struct_field *old_field, *field;
    struct hlsl_type *type;

    if (!(type = vkd3d_calloc(1, sizeof(*type))))
        return NULL;

    if (old->name)
    {
        type->name = vkd3d_strdup(old->name);
        if (!type->name)
        {
            vkd3d_free(type);
            return NULL;
        }
    }
    type->type = old->type;
    type->base_type = old->base_type;
    type->dimx = old->dimx;
    type->dimy = old->dimy;
    type->modifiers = old->modifiers;
    if (!(type->modifiers & HLSL_MODIFIERS_MAJORITY_MASK))
        type->modifiers |= default_majority;
    type->sampler_dim = old->sampler_dim;
    switch (old->type)
    {
        case HLSL_CLASS_ARRAY:
            type->e.array.type = clone_hlsl_type(old->e.array.type, default_majority);
            type->e.array.elements_count = old->e.array.elements_count;
            type->reg_size = type->e.array.elements_count * type->e.array.type->reg_size;
            break;

        case HLSL_CLASS_STRUCT:
        {
            unsigned int reg_size = 0;

            if (!(type->e.elements = vkd3d_malloc(sizeof(*type->e.elements))))
            {
                vkd3d_free((void *)type->name);
                vkd3d_free(type);
                return NULL;
            }
            list_init(type->e.elements);
            LIST_FOR_EACH_ENTRY(old_field, old->e.elements, struct hlsl_struct_field, entry)
            {
                if (!(field = vkd3d_calloc(1, sizeof(*field))))
                {
                    LIST_FOR_EACH_ENTRY_SAFE(field, old_field, type->e.elements, struct hlsl_struct_field, entry)
                    {
                        vkd3d_free((void *)field->semantic);
                        vkd3d_free((void *)field->name);
                        vkd3d_free(field);
                    }
                    vkd3d_free(type->e.elements);
                    vkd3d_free((void *)type->name);
                    vkd3d_free(type);
                    return NULL;
                }
                field->type = clone_hlsl_type(old_field->type, default_majority);
                field->name = vkd3d_strdup(old_field->name);
                if (old_field->semantic)
                    field->semantic = vkd3d_strdup(old_field->semantic);
                field->modifiers = old_field->modifiers;
                field->reg_offset = reg_size;
                reg_size += field->type->reg_size;
                list_add_tail(type->e.elements, &field->entry);
            }
            type->reg_size = reg_size;
            break;
        }

        case HLSL_CLASS_MATRIX:
            type->reg_size = is_row_major(type) ? type->dimy : type->dimx;
            break;

        default:
            type->reg_size = 1;
            break;
    }

    list_add_tail(&hlsl_ctx.types, &type->entry);
    return type;
}

static BOOL convertible_data_type(struct hlsl_type *type)
{
    return type->type != HLSL_CLASS_OBJECT;
}

BOOL compatible_data_types(struct hlsl_type *t1, struct hlsl_type *t2)
{
   if (!convertible_data_type(t1) || !convertible_data_type(t2))
        return FALSE;

    if (t1->type <= HLSL_CLASS_LAST_NUMERIC)
    {
        /* Scalar vars can be cast to pretty much everything */
        if (t1->dimx == 1 && t1->dimy == 1)
            return TRUE;

        if (t1->type == HLSL_CLASS_VECTOR && t2->type == HLSL_CLASS_VECTOR)
            return t1->dimx >= t2->dimx;
    }

    /* The other way around is true too i.e. whatever to scalar */
    if (t2->type <= HLSL_CLASS_LAST_NUMERIC && t2->dimx == 1 && t2->dimy == 1)
        return TRUE;

    if (t1->type == HLSL_CLASS_ARRAY)
    {
        if (compare_hlsl_types(t1->e.array.type, t2))
            /* e.g. float4[3] to float4 is allowed */
            return TRUE;

        if (t2->type == HLSL_CLASS_ARRAY || t2->type == HLSL_CLASS_STRUCT)
            return components_count_type(t1) >= components_count_type(t2);
        else
            return components_count_type(t1) == components_count_type(t2);
    }

    if (t1->type == HLSL_CLASS_STRUCT)
        return components_count_type(t1) >= components_count_type(t2);

    if (t2->type == HLSL_CLASS_ARRAY || t2->type == HLSL_CLASS_STRUCT)
        return components_count_type(t1) == components_count_type(t2);

    if (t1->type == HLSL_CLASS_MATRIX || t2->type == HLSL_CLASS_MATRIX)
    {
        if (t1->type == HLSL_CLASS_MATRIX && t2->type == HLSL_CLASS_MATRIX && t1->dimx >= t2->dimx && t1->dimy >= t2->dimy)
            return TRUE;

        /* Matrix-vector conversion is apparently allowed if they have the same components count */
        if ((t1->type == HLSL_CLASS_VECTOR || t2->type == HLSL_CLASS_VECTOR)
                && components_count_type(t1) == components_count_type(t2))
            return TRUE;
        return FALSE;
    }

    if (components_count_type(t1) >= components_count_type(t2))
        return TRUE;
    return FALSE;
}

static BOOL implicit_compatible_data_types(struct hlsl_type *t1, struct hlsl_type *t2)
{
    if (!convertible_data_type(t1) || !convertible_data_type(t2))
        return FALSE;

    if (t1->type <= HLSL_CLASS_LAST_NUMERIC)
    {
        /* Scalar vars can be converted to any other numeric data type */
        if (t1->dimx == 1 && t1->dimy == 1 && t2->type <= HLSL_CLASS_LAST_NUMERIC)
            return TRUE;
        /* The other way around is true too */
        if (t2->dimx == 1 && t2->dimy == 1 && t2->type <= HLSL_CLASS_LAST_NUMERIC)
            return TRUE;
    }

    if (t1->type == HLSL_CLASS_ARRAY && t2->type == HLSL_CLASS_ARRAY)
    {
        return components_count_type(t1) == components_count_type(t2);
    }

    if ((t1->type == HLSL_CLASS_ARRAY && t2->type <= HLSL_CLASS_LAST_NUMERIC)
            || (t1->type <= HLSL_CLASS_LAST_NUMERIC && t2->type == HLSL_CLASS_ARRAY))
    {
        /* e.g. float4[3] to float4 is allowed */
        if (t1->type == HLSL_CLASS_ARRAY && compare_hlsl_types(t1->e.array.type, t2))
            return TRUE;
        if (components_count_type(t1) == components_count_type(t2))
            return TRUE;
        return FALSE;
    }

    if (t1->type <= HLSL_CLASS_VECTOR && t2->type <= HLSL_CLASS_VECTOR)
    {
        if (t1->dimx >= t2->dimx)
            return TRUE;
        return FALSE;
    }

    if (t1->type == HLSL_CLASS_MATRIX || t2->type == HLSL_CLASS_MATRIX)
    {
        if (t1->type == HLSL_CLASS_MATRIX && t2->type == HLSL_CLASS_MATRIX
                && t1->dimx >= t2->dimx && t1->dimy >= t2->dimy)
            return TRUE;

        /* Matrix-vector conversion is apparently allowed if they have the same components count */
        if ((t1->type == HLSL_CLASS_VECTOR || t2->type == HLSL_CLASS_VECTOR)
                && components_count_type(t1) == components_count_type(t2))
            return TRUE;
        return FALSE;
    }

    if (t1->type == HLSL_CLASS_STRUCT && t2->type == HLSL_CLASS_STRUCT)
        return compare_hlsl_types(t1, t2);

    return FALSE;
}

static BOOL expr_compatible_data_types(struct hlsl_type *t1, struct hlsl_type *t2)
{
    if (t1->base_type > HLSL_TYPE_LAST_SCALAR || t2->base_type > HLSL_TYPE_LAST_SCALAR)
        return FALSE;

    /* Scalar vars can be converted to pretty much everything */
    if ((t1->dimx == 1 && t1->dimy == 1) || (t2->dimx == 1 && t2->dimy == 1))
        return TRUE;

    if (t1->type == HLSL_CLASS_VECTOR && t2->type == HLSL_CLASS_VECTOR)
        return TRUE;

    if (t1->type == HLSL_CLASS_MATRIX || t2->type == HLSL_CLASS_MATRIX)
    {
        /* Matrix-vector conversion is apparently allowed if either they have the same components
           count or the matrix is nx1 or 1xn */
        if (t1->type == HLSL_CLASS_VECTOR || t2->type == HLSL_CLASS_VECTOR)
        {
            if (components_count_type(t1) == components_count_type(t2))
                return TRUE;

            return (t1->type == HLSL_CLASS_MATRIX && (t1->dimx == 1 || t1->dimy == 1))
                    || (t2->type == HLSL_CLASS_MATRIX && (t2->dimx == 1 || t2->dimy == 1));
        }

        /* Both matrices */
        if ((t1->dimx >= t2->dimx && t1->dimy >= t2->dimy)
                || (t1->dimx <= t2->dimx && t1->dimy <= t2->dimy))
            return TRUE;
    }

    return FALSE;
}

static enum hlsl_base_type expr_common_base_type(enum hlsl_base_type t1, enum hlsl_base_type t2)
{
    static const enum hlsl_base_type types[] =
    {
        HLSL_TYPE_BOOL,
        HLSL_TYPE_INT,
        HLSL_TYPE_UINT,
        HLSL_TYPE_HALF,
        HLSL_TYPE_FLOAT,
        HLSL_TYPE_DOUBLE,
    };
    int t1_idx = -1, t2_idx = -1, i;

    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        /* Always convert away from HLSL_TYPE_HALF */
        if (t1 == types[i])
            t1_idx = t1 == HLSL_TYPE_HALF ? i + 1 : i;
        if (t2 == types[i])
            t2_idx = t2 == HLSL_TYPE_HALF ? i + 1 : i;

        if (t1_idx != -1 && t2_idx != -1)
            break;
    }
    if (t1_idx == -1 || t2_idx == -1)
    {
        FIXME("Unexpected base type.\n");
        return HLSL_TYPE_FLOAT;
    }
    return t1_idx >= t2_idx ? t1 : t2;
}

static struct hlsl_type *expr_common_type(struct hlsl_type *t1, struct hlsl_type *t2,
        struct source_location *loc)
{
    enum hlsl_type_class type;
    enum hlsl_base_type base;
    unsigned int dimx, dimy;

    if (t1->type > HLSL_CLASS_LAST_NUMERIC || t2->type > HLSL_CLASS_LAST_NUMERIC)
    {
        hlsl_report_message(*loc, HLSL_LEVEL_ERROR, "non scalar/vector/matrix data type in expression");
        return NULL;
    }

    if (compare_hlsl_types(t1, t2))
        return t1;

    if (!expr_compatible_data_types(t1, t2))
    {
        hlsl_report_message(*loc, HLSL_LEVEL_ERROR, "expression data types are incompatible");
        return NULL;
    }

    if (t1->base_type == t2->base_type)
        base = t1->base_type;
    else
        base = expr_common_base_type(t1->base_type, t2->base_type);

    if (t1->dimx == 1 && t1->dimy == 1)
    {
        type = t2->type;
        dimx = t2->dimx;
        dimy = t2->dimy;
    }
    else if (t2->dimx == 1 && t2->dimy == 1)
    {
        type = t1->type;
        dimx = t1->dimx;
        dimy = t1->dimy;
    }
    else if (t1->type == HLSL_CLASS_MATRIX && t2->type == HLSL_CLASS_MATRIX)
    {
        type = HLSL_CLASS_MATRIX;
        dimx = min(t1->dimx, t2->dimx);
        dimy = min(t1->dimy, t2->dimy);
    }
    else
    {
        /* Two vectors or a vector and a matrix (matrix must be 1xn or nx1) */
        unsigned int max_dim_1, max_dim_2;

        max_dim_1 = max(t1->dimx, t1->dimy);
        max_dim_2 = max(t2->dimx, t2->dimy);
        if (t1->dimx * t1->dimy == t2->dimx * t2->dimy)
        {
            type = HLSL_CLASS_VECTOR;
            dimx = max(t1->dimx, t2->dimx);
            dimy = 1;
        }
        else if (max_dim_1 <= max_dim_2)
        {
            type = t1->type;
            if (type == HLSL_CLASS_VECTOR)
            {
                dimx = max_dim_1;
                dimy = 1;
            }
            else
            {
                dimx = t1->dimx;
                dimy = t1->dimy;
            }
        }
        else
        {
            type = t2->type;
            if (type == HLSL_CLASS_VECTOR)
            {
                dimx = max_dim_2;
                dimy = 1;
            }
            else
            {
                dimx = t2->dimx;
                dimy = t2->dimy;
            }
        }
    }

    if (type == HLSL_CLASS_SCALAR)
        return hlsl_ctx.builtin_types.scalar[base];
    if (type == HLSL_CLASS_VECTOR)
        return hlsl_ctx.builtin_types.vector[base][dimx - 1];
    return new_hlsl_type(NULL, type, base, dimx, dimy);
}

struct hlsl_ir_node *add_implicit_conversion(struct list *instrs, struct hlsl_ir_node *node,
        struct hlsl_type *dst_type, struct source_location *loc)
{
    struct hlsl_type *src_type = node->data_type;
    struct hlsl_ir_expr *cast;

    if (compare_hlsl_types(src_type, dst_type))
        return node;

    if (!implicit_compatible_data_types(src_type, dst_type))
    {
        hlsl_report_message(*loc, HLSL_LEVEL_ERROR, "can't implicitly convert %s to %s",
                debug_hlsl_type(src_type), debug_hlsl_type(dst_type));
        return NULL;
    }

    if (dst_type->dimx * dst_type->dimy < src_type->dimx * src_type->dimy)
        hlsl_report_message(*loc, HLSL_LEVEL_WARNING, "implicit truncation of vector type");

    TRACE("Implicit conversion from %s to %s.\n", debug_hlsl_type(src_type), debug_hlsl_type(dst_type));

    if (!(cast = new_cast(node, dst_type, loc)))
        return NULL;
    list_add_tail(instrs, &cast->node.entry);
    return &cast->node;
}

struct hlsl_ir_expr *add_expr(struct list *instrs, enum hlsl_ir_expr_op op, struct hlsl_ir_node *operands[3],
        struct source_location *loc)
{
    struct hlsl_ir_expr *expr;
    struct hlsl_type *type;
    unsigned int i;

    type = operands[0]->data_type;
    for (i = 1; i <= 2; ++i)
    {
        if (!operands[i])
            break;
        type = expr_common_type(type, operands[i]->data_type, loc);
        if (!type)
            return NULL;
    }
    for (i = 0; i <= 2; ++i)
    {
        struct hlsl_ir_expr *cast;

        if (!operands[i])
            break;
        if (compare_hlsl_types(operands[i]->data_type, type))
            continue;
        TRACE("Implicitly converting %s into %s in an expression.\n", debug_hlsl_type(operands[i]->data_type), debug_hlsl_type(type));
        if (operands[i]->data_type->dimx * operands[i]->data_type->dimy != 1
                && operands[i]->data_type->dimx * operands[i]->data_type->dimy != type->dimx * type->dimy)
        {
            hlsl_report_message(operands[i]->loc, HLSL_LEVEL_WARNING, "implicit truncation of vector/matrix type");
        }

        if (!(cast = new_cast(operands[i], type, &operands[i]->loc)))
            return NULL;
        list_add_after(&operands[i]->entry, &cast->node.entry);
        operands[i] = &cast->node;
    }

    if (!(expr = vkd3d_calloc(1, sizeof(*expr))))
        return NULL;
    init_node(&expr->node, HLSL_IR_EXPR, type, *loc);
    expr->op = op;
    for (i = 0; i <= 2; ++i)
        hlsl_src_from_node(&expr->operands[i], operands[i]);
    list_add_tail(instrs, &expr->node.entry);

    return expr;
}

struct hlsl_ir_expr *new_cast(struct hlsl_ir_node *node, struct hlsl_type *type,
        struct source_location *loc)
{
    struct hlsl_ir_node *cast;

    cast = new_unary_expr(HLSL_IR_UNOP_CAST, node, *loc);
    if (cast)
        cast->data_type = type;
    return expr_from_node(cast);
}

static enum hlsl_ir_expr_op op_from_assignment(enum parse_assign_op op)
{
    static const enum hlsl_ir_expr_op ops[] =
    {
        0,
        HLSL_IR_BINOP_ADD,
        HLSL_IR_BINOP_SUB,
        HLSL_IR_BINOP_MUL,
        HLSL_IR_BINOP_DIV,
        HLSL_IR_BINOP_MOD,
        HLSL_IR_BINOP_LSHIFT,
        HLSL_IR_BINOP_RSHIFT,
        HLSL_IR_BINOP_BIT_AND,
        HLSL_IR_BINOP_BIT_OR,
        HLSL_IR_BINOP_BIT_XOR,
    };

    return ops[op];
}

static BOOL invert_swizzle(unsigned int *swizzle, unsigned int *writemask, unsigned int *ret_width)
{
    unsigned int i, j, bit = 0, inverted = 0, width, new_writemask = 0, new_swizzle = 0;

    /* Apply the writemask to the swizzle to get a new writemask and swizzle. */
    for (i = 0; i < 4; ++i)
    {
        if (*writemask & (1 << i))
        {
            unsigned int s = (*swizzle >> (i * 2)) & 3;
            new_swizzle |= s << (bit++ * 2);
            if (new_writemask & (1 << s))
                return FALSE;
            new_writemask |= 1 << s;
        }
    }
    width = bit;

    /* Invert the swizzle. */
    bit = 0;
    for (i = 0; i < 4; ++i)
    {
        for (j = 0; j < width; ++j)
        {
            unsigned int s = (new_swizzle >> (j * 2)) & 3;
            if (s == i)
                inverted |= j << (bit++ * 2);
        }
    }

    *swizzle = inverted;
    *writemask = new_writemask;
    *ret_width = width;
    return TRUE;
}

struct hlsl_ir_node *add_assignment(struct list *instrs, struct hlsl_ir_node *lhs,
        enum parse_assign_op assign_op, struct hlsl_ir_node *rhs)
{
    struct hlsl_ir_assignment *assign;
    struct hlsl_type *lhs_type;
    DWORD writemask = 0;

    lhs_type = lhs->data_type;
    if (lhs_type->type <= HLSL_CLASS_LAST_NUMERIC)
    {
        writemask = (1 << lhs_type->dimx) - 1;

        if (!(rhs = add_implicit_conversion(instrs, rhs, lhs_type, &rhs->loc)))
            return NULL;
    }

    if (!(assign = vkd3d_malloc(sizeof(*assign))))
        return NULL;

    while (lhs->type != HLSL_IR_LOAD)
    {
        struct hlsl_ir_node *lhs_inner;

        if (lhs->type == HLSL_IR_EXPR && expr_from_node(lhs)->op == HLSL_IR_UNOP_CAST)
        {
            FIXME("Cast on the lhs.\n");
            vkd3d_free(assign);
            return NULL;
        }
        else if (lhs->type == HLSL_IR_SWIZZLE)
        {
            struct hlsl_ir_swizzle *swizzle = swizzle_from_node(lhs);
            const struct hlsl_type *swizzle_type = swizzle->node.data_type;
            unsigned int width;

            if (lhs->data_type->type == HLSL_CLASS_MATRIX)
                FIXME("Assignments with writemasks and matrices on lhs are not supported yet.\n");

            lhs_inner = swizzle->val.node;
            hlsl_src_remove(&swizzle->val);
            list_remove(&lhs->entry);

            list_add_after(&rhs->entry, &lhs->entry);
            hlsl_src_from_node(&swizzle->val, rhs);
            if (!invert_swizzle(&swizzle->swizzle, &writemask, &width))
            {
                hlsl_report_message(lhs->loc, HLSL_LEVEL_ERROR, "invalid writemask");
                vkd3d_free(assign);
                return NULL;
            }
            assert(swizzle_type->type == HLSL_CLASS_VECTOR);
            if (swizzle_type->dimx != width)
                swizzle->node.data_type = hlsl_ctx.builtin_types.vector[swizzle_type->base_type][width - 1];
            rhs = &swizzle->node;
        }
        else
        {
            hlsl_report_message(lhs->loc, HLSL_LEVEL_ERROR, "invalid lvalue");
            vkd3d_free(assign);
            return NULL;
        }

        lhs = lhs_inner;
    }

    init_node(&assign->node, HLSL_IR_ASSIGNMENT, lhs_type, lhs->loc);
    assign->writemask = writemask;
    assign->lhs.var = load_from_node(lhs)->src.var;
    hlsl_src_from_node(&assign->lhs.offset, load_from_node(lhs)->src.offset.node);
    if (assign_op != ASSIGN_OP_ASSIGN)
    {
        enum hlsl_ir_expr_op op = op_from_assignment(assign_op);
        struct hlsl_ir_node *expr;

        TRACE("Adding an expression for the compound assignment.\n");
        expr = new_binary_expr(op, lhs, rhs);
        list_add_after(&rhs->entry, &expr->entry);
        rhs = expr;
    }
    hlsl_src_from_node(&assign->rhs, rhs);
    list_add_tail(instrs, &assign->node.entry);

    return &assign->node;
}

static int compare_hlsl_types_rb(const void *key, const struct rb_entry *entry)
{
    const struct hlsl_type *type = RB_ENTRY_VALUE(entry, const struct hlsl_type, scope_entry);
    const char *name = key;

    if (name == type->name)
        return 0;

    if (!name || !type->name)
    {
        ERR("hlsl_type without a name in a scope?\n");
        return -1;
    }
    return strcmp(name, type->name);
}

void push_scope(struct hlsl_parse_ctx *ctx)
{
    struct hlsl_scope *new_scope;

    if (!(new_scope = vkd3d_malloc(sizeof(*new_scope))))
        return;
    TRACE("Pushing a new scope.\n");
    list_init(&new_scope->vars);
    rb_init(&new_scope->types, compare_hlsl_types_rb);
    new_scope->upper = ctx->cur_scope;
    ctx->cur_scope = new_scope;
    list_add_tail(&ctx->scopes, &new_scope->entry);
}

BOOL pop_scope(struct hlsl_parse_ctx *ctx)
{
    struct hlsl_scope *prev_scope = ctx->cur_scope->upper;

    if (!prev_scope)
        return FALSE;
    TRACE("Popping current scope.\n");
    ctx->cur_scope = prev_scope;
    return TRUE;
}

static int compare_param_hlsl_types(const struct hlsl_type *t1, const struct hlsl_type *t2)
{
    if (t1->type != t2->type)
    {
        if (!((t1->type == HLSL_CLASS_SCALAR && t2->type == HLSL_CLASS_VECTOR)
                || (t1->type == HLSL_CLASS_VECTOR && t2->type == HLSL_CLASS_SCALAR)))
            return t1->type - t2->type;
    }
    if (t1->base_type != t2->base_type)
        return t1->base_type - t2->base_type;
    if (t1->base_type == HLSL_TYPE_SAMPLER && t1->sampler_dim != t2->sampler_dim)
        return t1->sampler_dim - t2->sampler_dim;
    if (t1->dimx != t2->dimx)
        return t1->dimx - t2->dimx;
    if (t1->dimy != t2->dimy)
        return t1->dimx - t2->dimx;
    if (t1->type == HLSL_CLASS_STRUCT)
    {
        struct list *t1cur, *t2cur;
        struct hlsl_struct_field *t1field, *t2field;
        int r;

        t1cur = list_head(t1->e.elements);
        t2cur = list_head(t2->e.elements);
        while (t1cur && t2cur)
        {
            t1field = LIST_ENTRY(t1cur, struct hlsl_struct_field, entry);
            t2field = LIST_ENTRY(t2cur, struct hlsl_struct_field, entry);
            if ((r = compare_param_hlsl_types(t1field->type, t2field->type)))
                return r;
            if ((r = strcmp(t1field->name, t2field->name)))
                return r;
            t1cur = list_next(t1->e.elements, t1cur);
            t2cur = list_next(t2->e.elements, t2cur);
        }
        if (t1cur != t2cur)
            return t1cur ? 1 : -1;
        return 0;
    }
    if (t1->type == HLSL_CLASS_ARRAY)
    {
        if (t1->e.array.elements_count != t2->e.array.elements_count)
            return t1->e.array.elements_count - t2->e.array.elements_count;
        return compare_param_hlsl_types(t1->e.array.type, t2->e.array.type);
    }

    return 0;
}

static int compare_function_decl_rb(const void *key, const struct rb_entry *entry)
{
    const struct list *params = key;
    const struct hlsl_ir_function_decl *decl = RB_ENTRY_VALUE(entry, const struct hlsl_ir_function_decl, entry);
    int decl_params_count = decl->parameters ? list_count(decl->parameters) : 0;
    int params_count = params ? list_count(params) : 0;
    struct list *p1cur, *p2cur;
    int r;

    if (params_count != decl_params_count)
        return params_count - decl_params_count;

    p1cur = params ? list_head(params) : NULL;
    p2cur = decl->parameters ? list_head(decl->parameters) : NULL;
    while (p1cur && p2cur)
    {
        struct hlsl_ir_var *p1, *p2;
        p1 = LIST_ENTRY(p1cur, struct hlsl_ir_var, param_entry);
        p2 = LIST_ENTRY(p2cur, struct hlsl_ir_var, param_entry);
        if ((r = compare_param_hlsl_types(p1->data_type, p2->data_type)))
            return r;
        p1cur = list_next(params, p1cur);
        p2cur = list_next(decl->parameters, p2cur);
    }
    return 0;
}

static int compare_function_rb(const void *key, const struct rb_entry *entry)
{
    const char *name = key;
    const struct hlsl_ir_function *func = RB_ENTRY_VALUE(entry, const struct hlsl_ir_function,entry);

    return strcmp(name, func->name);
}

void init_functions_tree(struct rb_tree *funcs)
{
    rb_init(&hlsl_ctx.functions, compare_function_rb);
}

const char *debug_base_type(const struct hlsl_type *type)
{
    const char *name = "(unknown)";

    switch (type->base_type)
    {
        case HLSL_TYPE_FLOAT:        name = "float";         break;
        case HLSL_TYPE_HALF:         name = "half";          break;
        case HLSL_TYPE_DOUBLE:       name = "double";        break;
        case HLSL_TYPE_INT:          name = "int";           break;
        case HLSL_TYPE_UINT:         name = "uint";          break;
        case HLSL_TYPE_BOOL:         name = "bool";          break;
        case HLSL_TYPE_SAMPLER:
            switch (type->sampler_dim)
            {
                case HLSL_SAMPLER_DIM_GENERIC: name = "sampler";       break;
                case HLSL_SAMPLER_DIM_1D:      name = "sampler1D";     break;
                case HLSL_SAMPLER_DIM_2D:      name = "sampler2D";     break;
                case HLSL_SAMPLER_DIM_3D:      name = "sampler3D";     break;
                case HLSL_SAMPLER_DIM_CUBE:    name = "samplerCUBE";   break;
            }
            break;
        default:
            FIXME("Unhandled case %u.\n", type->base_type);
    }
    return name;
}

const char *debug_hlsl_type(const struct hlsl_type *type)
{
    const char *name;

    if (type->name)
        return debugstr_a(type->name);

    if (type->type == HLSL_CLASS_STRUCT)
        return "<anonymous struct>";

    if (type->type == HLSL_CLASS_ARRAY)
    {
        name = debug_base_type(type->e.array.type);
        return vkd3d_dbg_sprintf("%s[%u]", name, type->e.array.elements_count);
    }

    name = debug_base_type(type);

    if (type->type == HLSL_CLASS_SCALAR)
        return vkd3d_dbg_sprintf("%s", name);
    if (type->type == HLSL_CLASS_VECTOR)
        return vkd3d_dbg_sprintf("%s%u", name, type->dimx);
    if (type->type == HLSL_CLASS_MATRIX)
        return vkd3d_dbg_sprintf("%s%ux%u", name, type->dimx, type->dimy);
    return "unexpected_type";
}

const char *debug_modifiers(DWORD modifiers)
{
    char string[110];

    string[0] = 0;
    if (modifiers & HLSL_STORAGE_EXTERN)
        strcat(string, " extern");                       /* 7 */
    if (modifiers & HLSL_STORAGE_NOINTERPOLATION)
        strcat(string, " nointerpolation");              /* 16 */
    if (modifiers & HLSL_MODIFIER_PRECISE)
        strcat(string, " precise");                      /* 8 */
    if (modifiers & HLSL_STORAGE_SHARED)
        strcat(string, " shared");                       /* 7 */
    if (modifiers & HLSL_STORAGE_GROUPSHARED)
        strcat(string, " groupshared");                  /* 12 */
    if (modifiers & HLSL_STORAGE_STATIC)
        strcat(string, " static");                       /* 7 */
    if (modifiers & HLSL_STORAGE_UNIFORM)
        strcat(string, " uniform");                      /* 8 */
    if (modifiers & HLSL_STORAGE_VOLATILE)
        strcat(string, " volatile");                     /* 9 */
    if (modifiers & HLSL_MODIFIER_CONST)
        strcat(string, " const");                        /* 6 */
    if (modifiers & HLSL_MODIFIER_ROW_MAJOR)
        strcat(string, " row_major");                    /* 10 */
    if (modifiers & HLSL_MODIFIER_COLUMN_MAJOR)
        strcat(string, " column_major");                 /* 13 */
    if ((modifiers & (HLSL_STORAGE_IN | HLSL_STORAGE_OUT)) == (HLSL_STORAGE_IN | HLSL_STORAGE_OUT))
        strcat(string, " inout");                        /* 6 */
    else if (modifiers & HLSL_STORAGE_IN)
        strcat(string, " in");                           /* 3 */
    else if (modifiers & HLSL_STORAGE_OUT)
        strcat(string, " out");                          /* 4 */

    return vkd3d_dbg_sprintf("%s", string[0] ? string + 1 : "");
}

const char *debug_node_type(enum hlsl_ir_node_type type)
{
    static const char * const names[] =
    {
        "HLSL_IR_ASSIGNMENT",
        "HLSL_IR_CONSTANT",
        "HLSL_IR_EXPR",
        "HLSL_IR_IF",
        "HLSL_IR_LOAD",
        "HLSL_IR_LOOP",
        "HLSL_IR_JUMP",
        "HLSL_IR_SWIZZLE",
    };

    if (type >= ARRAY_SIZE(names))
        return "Unexpected node type";
    return names[type];
}

static void debug_dump_instr(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_node *instr);

static void debug_dump_instr_list(struct vkd3d_string_buffer *buffer, const struct list *list)
{
    struct hlsl_ir_node *instr;

    LIST_FOR_EACH_ENTRY(instr, list, struct hlsl_ir_node, entry)
    {
        debug_dump_instr(buffer, instr);
        vkd3d_string_buffer_printf(buffer, "\n");
    }
}

static void debug_dump_src(struct vkd3d_string_buffer *buffer, const struct hlsl_src *src)
{
    if (src->node->index)
        vkd3d_string_buffer_printf(buffer, "@%u", src->node->index);
    else
        vkd3d_string_buffer_printf(buffer, "%p", src->node);
}

static void debug_dump_ir_var(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_var *var)
{
    if (var->modifiers)
        vkd3d_string_buffer_printf(buffer, "%s ", debug_modifiers(var->modifiers));
    vkd3d_string_buffer_printf(buffer, "%s %s", debug_hlsl_type(var->data_type), var->name);
    if (var->semantic)
        vkd3d_string_buffer_printf(buffer, " : %s", var->semantic);
}

static void debug_dump_deref(struct vkd3d_string_buffer *buffer, const struct hlsl_deref *deref)
{
    if (deref->offset.node)
        /* Print the variable's type for convenience. */
        vkd3d_string_buffer_printf(buffer, "(%s %s)", debug_hlsl_type(deref->var->data_type), deref->var->name);
    else
        vkd3d_string_buffer_printf(buffer, "%s", deref->var->name);
    if (deref->offset.node)
    {
        vkd3d_string_buffer_printf(buffer, "[");
        debug_dump_src(buffer, &deref->offset);
        vkd3d_string_buffer_printf(buffer, "]");
    }
}

static const char *debug_writemask(DWORD writemask)
{
    static const char components[] = {'x', 'y', 'z', 'w'};
    char string[5];
    unsigned int i = 0, pos = 0;

    assert(!(writemask & ~VKD3DSP_WRITEMASK_ALL));

    while (writemask)
    {
        if (writemask & 1)
            string[pos++] = components[i];
        writemask >>= 1;
        i++;
    }
    string[pos] = '\0';
    return vkd3d_dbg_sprintf(".%s", string);
}

static void debug_dump_ir_assignment(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_assignment *assign)
{
    vkd3d_string_buffer_printf(buffer, "= (");
    debug_dump_deref(buffer, &assign->lhs);
    if (assign->writemask != VKD3DSP_WRITEMASK_ALL)
        vkd3d_string_buffer_printf(buffer, "%s", debug_writemask(assign->writemask));
    vkd3d_string_buffer_printf(buffer, " ");
    debug_dump_src(buffer, &assign->rhs);
    vkd3d_string_buffer_printf(buffer, ")");
}

static void debug_dump_ir_constant(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_constant *constant)
{
    struct hlsl_type *type = constant->node.data_type;
    unsigned int x;

    if (type->dimx != 1)
        vkd3d_string_buffer_printf(buffer, "{");
    for (x = 0; x < type->dimx; ++x)
    {
        switch (type->base_type)
        {
            case HLSL_TYPE_BOOL:
                vkd3d_string_buffer_printf(buffer, "%s ", constant->value.b[x] ? "true" : "false");
                break;

            case HLSL_TYPE_DOUBLE:
                vkd3d_string_buffer_printf(buffer, "%.16e ", constant->value.d[x]);
                break;

            case HLSL_TYPE_FLOAT:
                vkd3d_string_buffer_printf(buffer, "%.8e ", constant->value.f[x]);
                break;

            case HLSL_TYPE_INT:
                vkd3d_string_buffer_printf(buffer, "%d ", constant->value.i[x]);
                break;

            case HLSL_TYPE_UINT:
                vkd3d_string_buffer_printf(buffer, "%u ", constant->value.u[x]);
                break;

            default:
                vkd3d_string_buffer_printf(buffer, "Constants of type %s not supported\n", debug_base_type(type));
        }
    }
    if (type->dimx != 1)
        vkd3d_string_buffer_printf(buffer, "}");
}

static const char *debug_expr_op(const struct hlsl_ir_expr *expr)
{
    static const char * const op_names[] =
    {
        "~",
        "!",
        "-",
        "abs",
        "sign",
        "rcp",
        "rsq",
        "sqrt",
        "nrm",
        "exp2",
        "log2",

        "cast",

        "fract",

        "sin",
        "cos",
        "sin_reduced",
        "cos_reduced",

        "dsx",
        "dsy",

        "sat",

        "pre++",
        "pre--",
        "post++",
        "post--",

        "+",
        "-",
        "*",
        "/",

        "%",

        "<",
        ">",
        "<=",
        ">=",
        "==",
        "!=",

        "&&",
        "||",

        "<<",
        ">>",
        "&",
        "|",
        "^",

        "dot",
        "crs",
        "min",
        "max",

        "pow",

        "lerp",

        ",",
    };

    if (expr->op == HLSL_IR_UNOP_CAST)
        return debug_hlsl_type(expr->node.data_type);

    return op_names[expr->op];
}

/* Dumps the expression in a prefix "operator (operands)" form */
static void debug_dump_ir_expr(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_expr *expr)
{
    unsigned int i;

    vkd3d_string_buffer_printf(buffer, "%s (", debug_expr_op(expr));
    for (i = 0; i < 3 && expr->operands[i].node; ++i)
    {
        debug_dump_src(buffer, &expr->operands[i]);
        vkd3d_string_buffer_printf(buffer, " ");
    }
    vkd3d_string_buffer_printf(buffer, ")");
}

static void debug_dump_ir_if(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_if *if_node)
{
    vkd3d_string_buffer_printf(buffer, "if (");
    debug_dump_src(buffer, &if_node->condition);
    vkd3d_string_buffer_printf(buffer, ")\n{\n");
    debug_dump_instr_list(buffer, &if_node->then_instrs);
    vkd3d_string_buffer_printf(buffer, "}\nelse\n{\n");
    debug_dump_instr_list(buffer, &if_node->else_instrs);
    vkd3d_string_buffer_printf(buffer, "}\n");
}

static void debug_dump_ir_jump(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_jump *jump)
{
    switch (jump->type)
    {
        case HLSL_IR_JUMP_BREAK:
            vkd3d_string_buffer_printf(buffer, "break");
            break;

        case HLSL_IR_JUMP_CONTINUE:
            vkd3d_string_buffer_printf(buffer, "continue");
            break;

        case HLSL_IR_JUMP_DISCARD:
            vkd3d_string_buffer_printf(buffer, "discard");
            break;

        case HLSL_IR_JUMP_RETURN:
            vkd3d_string_buffer_printf(buffer, "return");
            break;
    }
}

static void debug_dump_ir_loop(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_loop *loop)
{
    vkd3d_string_buffer_printf(buffer, "for (;;)\n{\n");
    debug_dump_instr_list(buffer, &loop->body);
    vkd3d_string_buffer_printf(buffer, "}\n");
}

static void debug_dump_ir_swizzle(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_swizzle *swizzle)
{
    unsigned int i;

    debug_dump_src(buffer, &swizzle->val);
    vkd3d_string_buffer_printf(buffer, ".");
    if (swizzle->val.node->data_type->dimy > 1)
    {
        for (i = 0; i < swizzle->node.data_type->dimx; ++i)
            vkd3d_string_buffer_printf(buffer, "_m%u%u", (swizzle->swizzle >> i * 8) & 0xf, (swizzle->swizzle >> (i * 8 + 4)) & 0xf);
    }
    else
    {
        static const char c[] = {'x', 'y', 'z', 'w'};

        for (i = 0; i < swizzle->node.data_type->dimx; ++i)
            vkd3d_string_buffer_printf(buffer, "%c", c[(swizzle->swizzle >> i * 2) & 0x3]);
    }
}

static void debug_dump_instr(struct vkd3d_string_buffer *buffer, const struct hlsl_ir_node *instr)
{
    if (instr->index)
        vkd3d_string_buffer_printf(buffer, "%4u: ", instr->index);
    else
        vkd3d_string_buffer_printf(buffer, "%p: ", instr);

    vkd3d_string_buffer_printf(buffer, "%10s | ", instr->data_type ? debug_hlsl_type(instr->data_type) : "");

    switch (instr->type)
    {
        case HLSL_IR_ASSIGNMENT:
            debug_dump_ir_assignment(buffer, assignment_from_node(instr));
            break;

        case HLSL_IR_CONSTANT:
            debug_dump_ir_constant(buffer, constant_from_node(instr));
            break;

        case HLSL_IR_EXPR:
            debug_dump_ir_expr(buffer, expr_from_node(instr));
            break;

        case HLSL_IR_IF:
            debug_dump_ir_if(buffer, if_from_node(instr));
            break;

        case HLSL_IR_JUMP:
            debug_dump_ir_jump(buffer, jump_from_node(instr));
            break;

        case HLSL_IR_LOAD:
            debug_dump_deref(buffer, &load_from_node(instr)->src);
            break;

        case HLSL_IR_LOOP:
            debug_dump_ir_loop(buffer, loop_from_node(instr));
            break;

        case HLSL_IR_SWIZZLE:
            debug_dump_ir_swizzle(buffer, swizzle_from_node(instr));
            break;

        default:
            vkd3d_string_buffer_printf(buffer, "<No dump function for %s>", debug_node_type(instr->type));
    }
}

void debug_dump_ir_function_decl(const struct hlsl_ir_function_decl *func)
{
    struct vkd3d_string_buffer buffer;
    struct hlsl_ir_var *param;

    vkd3d_string_buffer_init(&buffer);
    vkd3d_string_buffer_printf(&buffer, "Dumping function %s.\n", func->func->name);
    vkd3d_string_buffer_printf(&buffer, "Function parameters:\n");
    LIST_FOR_EACH_ENTRY(param, func->parameters, struct hlsl_ir_var, param_entry)
    {
        debug_dump_ir_var(&buffer, param);
        vkd3d_string_buffer_printf(&buffer, "\n");
    }
    if (func->semantic)
        vkd3d_string_buffer_printf(&buffer, "Function semantic: %s\n", func->semantic);
    if (func->body)
        debug_dump_instr_list(&buffer, func->body);

    vkd3d_string_buffer_trace(&buffer);
    vkd3d_string_buffer_cleanup(&buffer);
}

void free_hlsl_type(struct hlsl_type *type)
{
    struct hlsl_struct_field *field, *next_field;

    vkd3d_free((void *)type->name);
    if (type->type == HLSL_CLASS_STRUCT)
    {
        LIST_FOR_EACH_ENTRY_SAFE(field, next_field, type->e.elements, struct hlsl_struct_field, entry)
        {
            vkd3d_free((void *)field->name);
            vkd3d_free((void *)field->semantic);
            vkd3d_free(field);
        }
    }
    vkd3d_free(type);
}

void free_instr_list(struct list *list)
{
    struct hlsl_ir_node *node, *next_node;

    if (!list)
        return;
    /* Iterate in reverse, to avoid use-after-free when unlinking sources from
     * the "uses" list. */
    LIST_FOR_EACH_ENTRY_SAFE_REV(node, next_node, list, struct hlsl_ir_node, entry)
        free_instr(node);
    vkd3d_free(list);
}

static void free_ir_assignment(struct hlsl_ir_assignment *assignment)
{
    hlsl_src_remove(&assignment->rhs);
    hlsl_src_remove(&assignment->lhs.offset);
    vkd3d_free(assignment);
}

static void free_ir_constant(struct hlsl_ir_constant *constant)
{
    vkd3d_free(constant);
}

static void free_ir_expr(struct hlsl_ir_expr *expr)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(expr->operands); ++i)
        hlsl_src_remove(&expr->operands[i]);
    vkd3d_free(expr);
}

static void free_ir_if(struct hlsl_ir_if *if_node)
{
    struct hlsl_ir_node *node, *next_node;

    LIST_FOR_EACH_ENTRY_SAFE(node, next_node, &if_node->then_instrs, struct hlsl_ir_node, entry)
        free_instr(node);
    LIST_FOR_EACH_ENTRY_SAFE(node, next_node, &if_node->else_instrs, struct hlsl_ir_node, entry)
        free_instr(node);
    hlsl_src_remove(&if_node->condition);
    vkd3d_free(if_node);
}

static void free_ir_jump(struct hlsl_ir_jump *jump)
{
    vkd3d_free(jump);
}

static void free_ir_load(struct hlsl_ir_load *load)
{
    hlsl_src_remove(&load->src.offset);
    vkd3d_free(load);
}

static void free_ir_loop(struct hlsl_ir_loop *loop)
{
    struct hlsl_ir_node *node, *next_node;

    LIST_FOR_EACH_ENTRY_SAFE(node, next_node, &loop->body, struct hlsl_ir_node, entry)
        free_instr(node);
    vkd3d_free(loop);
}

static void free_ir_swizzle(struct hlsl_ir_swizzle *swizzle)
{
    hlsl_src_remove(&swizzle->val);
    vkd3d_free(swizzle);
}

void free_instr(struct hlsl_ir_node *node)
{
    switch (node->type)
    {
        case HLSL_IR_ASSIGNMENT:
            free_ir_assignment(assignment_from_node(node));
            break;

        case HLSL_IR_CONSTANT:
            free_ir_constant(constant_from_node(node));
            break;

        case HLSL_IR_EXPR:
            free_ir_expr(expr_from_node(node));
            break;

        case HLSL_IR_IF:
            free_ir_if(if_from_node(node));
            break;

        case HLSL_IR_JUMP:
            free_ir_jump(jump_from_node(node));
            break;

        case HLSL_IR_LOAD:
            free_ir_load(load_from_node(node));
            break;

        case HLSL_IR_LOOP:
            free_ir_loop(loop_from_node(node));
            break;

        case HLSL_IR_SWIZZLE:
            free_ir_swizzle(swizzle_from_node(node));
            break;

        default:
            FIXME("Unsupported node type %s.\n", debug_node_type(node->type));
    }
}

static void free_function_decl(struct hlsl_ir_function_decl *decl)
{
    vkd3d_free((void *)decl->semantic);
    vkd3d_free(decl->parameters);
    free_instr_list(decl->body);
    vkd3d_free(decl);
}

static void free_function_decl_rb(struct rb_entry *entry, void *context)
{
    free_function_decl(RB_ENTRY_VALUE(entry, struct hlsl_ir_function_decl, entry));
}

static void free_function(struct hlsl_ir_function *func)
{
    rb_destroy(&func->overloads, free_function_decl_rb, NULL);
    vkd3d_free((void *)func->name);
    vkd3d_free(func);
}

void free_function_rb(struct rb_entry *entry, void *context)
{
    free_function(RB_ENTRY_VALUE(entry, struct hlsl_ir_function, entry));
}

void add_function_decl(struct rb_tree *funcs, char *name, struct hlsl_ir_function_decl *decl, BOOL intrinsic)
{
    struct hlsl_ir_function *func;
    struct rb_entry *func_entry, *old_entry;

    func_entry = rb_get(funcs, name);
    if (func_entry)
    {
        func = RB_ENTRY_VALUE(func_entry, struct hlsl_ir_function, entry);
        if (intrinsic != func->intrinsic)
        {
            if (intrinsic)
            {
                ERR("Redeclaring a user defined function as an intrinsic.\n");
                return;
            }
            TRACE("Function %s redeclared as a user defined function.\n", debugstr_a(name));
            func->intrinsic = intrinsic;
            rb_destroy(&func->overloads, free_function_decl_rb, NULL);
            rb_init(&func->overloads, compare_function_decl_rb);
        }
        decl->func = func;
        if ((old_entry = rb_get(&func->overloads, decl->parameters)))
        {
            struct hlsl_ir_function_decl *old_decl =
                    RB_ENTRY_VALUE(old_entry, struct hlsl_ir_function_decl, entry);

            if (!decl->body)
            {
                free_function_decl(decl);
                vkd3d_free(name);
                return;
            }
            rb_remove(&func->overloads, old_entry);
            free_function_decl(old_decl);
        }
        rb_put(&func->overloads, decl->parameters, &decl->entry);
        vkd3d_free(name);
        return;
    }
    func = vkd3d_malloc(sizeof(*func));
    func->name = name;
    rb_init(&func->overloads, compare_function_decl_rb);
    decl->func = func;
    rb_put(&func->overloads, decl->parameters, &decl->entry);
    func->intrinsic = intrinsic;
    rb_put(funcs, func->name, &func->entry);
}

struct hlsl_profile_info
{
    const char *name;
    enum vkd3d_shader_type type;
    DWORD sm_major;
    DWORD sm_minor;
    DWORD level_major;
    DWORD level_minor;
    BOOL sw;
};

static const struct hlsl_profile_info *get_target_info(const char *target)
{
    unsigned int i;

    static const struct hlsl_profile_info profiles[] =
    {
        {"cs_4_0",              VKD3D_SHADER_TYPE_COMPUTE,  4, 0, 0, 0, FALSE},
        {"cs_4_1",              VKD3D_SHADER_TYPE_COMPUTE,  4, 1, 0, 0, FALSE},
        {"cs_5_0",              VKD3D_SHADER_TYPE_COMPUTE,  5, 0, 0, 0, FALSE},
        {"ds_5_0",              VKD3D_SHADER_TYPE_DOMAIN,   5, 0, 0, 0, FALSE},
        {"fx_2_0",              VKD3D_SHADER_TYPE_EFFECT,   2, 0, 0, 0, FALSE},
        {"fx_4_0",              VKD3D_SHADER_TYPE_EFFECT,   4, 0, 0, 0, FALSE},
        {"fx_4_1",              VKD3D_SHADER_TYPE_EFFECT,   4, 1, 0, 0, FALSE},
        {"fx_5_0",              VKD3D_SHADER_TYPE_EFFECT,   5, 0, 0, 0, FALSE},
        {"gs_4_0",              VKD3D_SHADER_TYPE_GEOMETRY, 4, 0, 0, 0, FALSE},
        {"gs_4_1",              VKD3D_SHADER_TYPE_GEOMETRY, 4, 1, 0, 0, FALSE},
        {"gs_5_0",              VKD3D_SHADER_TYPE_GEOMETRY, 5, 0, 0, 0, FALSE},
        {"hs_5_0",              VKD3D_SHADER_TYPE_HULL,     5, 0, 0, 0, FALSE},
        {"ps.1.0",              VKD3D_SHADER_TYPE_PIXEL,    1, 0, 0, 0, FALSE},
        {"ps.1.1",              VKD3D_SHADER_TYPE_PIXEL,    1, 1, 0, 0, FALSE},
        {"ps.1.2",              VKD3D_SHADER_TYPE_PIXEL,    1, 2, 0, 0, FALSE},
        {"ps.1.3",              VKD3D_SHADER_TYPE_PIXEL,    1, 3, 0, 0, FALSE},
        {"ps.1.4",              VKD3D_SHADER_TYPE_PIXEL,    1, 4, 0, 0, FALSE},
        {"ps.2.0",              VKD3D_SHADER_TYPE_PIXEL,    2, 0, 0, 0, FALSE},
        {"ps.2.a",              VKD3D_SHADER_TYPE_PIXEL,    2, 1, 0, 0, FALSE},
        {"ps.2.b",              VKD3D_SHADER_TYPE_PIXEL,    2, 2, 0, 0, FALSE},
        {"ps.2.sw",             VKD3D_SHADER_TYPE_PIXEL,    2, 0, 0, 0, TRUE},
        {"ps.3.0",              VKD3D_SHADER_TYPE_PIXEL,    3, 0, 0, 0, FALSE},
        {"ps_1_0",              VKD3D_SHADER_TYPE_PIXEL,    1, 0, 0, 0, FALSE},
        {"ps_1_1",              VKD3D_SHADER_TYPE_PIXEL,    1, 1, 0, 0, FALSE},
        {"ps_1_2",              VKD3D_SHADER_TYPE_PIXEL,    1, 2, 0, 0, FALSE},
        {"ps_1_3",              VKD3D_SHADER_TYPE_PIXEL,    1, 3, 0, 0, FALSE},
        {"ps_1_4",              VKD3D_SHADER_TYPE_PIXEL,    1, 4, 0, 0, FALSE},
        {"ps_2_0",              VKD3D_SHADER_TYPE_PIXEL,    2, 0, 0, 0, FALSE},
        {"ps_2_a",              VKD3D_SHADER_TYPE_PIXEL,    2, 1, 0, 0, FALSE},
        {"ps_2_b",              VKD3D_SHADER_TYPE_PIXEL,    2, 2, 0, 0, FALSE},
        {"ps_2_sw",             VKD3D_SHADER_TYPE_PIXEL,    2, 0, 0, 0, TRUE},
        {"ps_3_0",              VKD3D_SHADER_TYPE_PIXEL,    3, 0, 0, 0, FALSE},
        {"ps_3_sw",             VKD3D_SHADER_TYPE_PIXEL,    3, 0, 0, 0, TRUE},
        {"ps_4_0",              VKD3D_SHADER_TYPE_PIXEL,    4, 0, 0, 0, FALSE},
        {"ps_4_0_level_9_0",    VKD3D_SHADER_TYPE_PIXEL,    4, 0, 9, 0, FALSE},
        {"ps_4_0_level_9_1",    VKD3D_SHADER_TYPE_PIXEL,    4, 0, 9, 1, FALSE},
        {"ps_4_0_level_9_3",    VKD3D_SHADER_TYPE_PIXEL,    4, 0, 9, 3, FALSE},
        {"ps_4_1",              VKD3D_SHADER_TYPE_PIXEL,    4, 1, 0, 0, FALSE},
        {"ps_5_0",              VKD3D_SHADER_TYPE_PIXEL,    5, 0, 0, 0, FALSE},
        {"tx_1_0",              VKD3D_SHADER_TYPE_TEXTURE,  1, 0, 0, 0, FALSE},
        {"vs.1.0",              VKD3D_SHADER_TYPE_VERTEX,   1, 0, 0, 0, FALSE},
        {"vs.1.1",              VKD3D_SHADER_TYPE_VERTEX,   1, 1, 0, 0, FALSE},
        {"vs.2.0",              VKD3D_SHADER_TYPE_VERTEX,   2, 0, 0, 0, FALSE},
        {"vs.2.a",              VKD3D_SHADER_TYPE_VERTEX,   2, 1, 0, 0, FALSE},
        {"vs.2.sw",             VKD3D_SHADER_TYPE_VERTEX,   2, 0, 0, 0, TRUE},
        {"vs.3.0",              VKD3D_SHADER_TYPE_VERTEX,   3, 0, 0, 0, FALSE},
        {"vs.3.sw",             VKD3D_SHADER_TYPE_VERTEX,   3, 0, 0, 0, TRUE},
        {"vs_1_0",              VKD3D_SHADER_TYPE_VERTEX,   1, 0, 0, 0, FALSE},
        {"vs_1_1",              VKD3D_SHADER_TYPE_VERTEX,   1, 1, 0, 0, FALSE},
        {"vs_2_0",              VKD3D_SHADER_TYPE_VERTEX,   2, 0, 0, 0, FALSE},
        {"vs_2_a",              VKD3D_SHADER_TYPE_VERTEX,   2, 1, 0, 0, FALSE},
        {"vs_2_sw",             VKD3D_SHADER_TYPE_VERTEX,   2, 0, 0, 0, TRUE},
        {"vs_3_0",              VKD3D_SHADER_TYPE_VERTEX,   3, 0, 0, 0, FALSE},
        {"vs_3_sw",             VKD3D_SHADER_TYPE_VERTEX,   3, 0, 0, 0, TRUE},
        {"vs_4_0",              VKD3D_SHADER_TYPE_VERTEX,   4, 0, 0, 0, FALSE},
        {"vs_4_0_level_9_0",    VKD3D_SHADER_TYPE_VERTEX,   4, 0, 9, 0, FALSE},
        {"vs_4_0_level_9_1",    VKD3D_SHADER_TYPE_VERTEX,   4, 0, 9, 1, FALSE},
        {"vs_4_0_level_9_3",    VKD3D_SHADER_TYPE_VERTEX,   4, 0, 9, 3, FALSE},
        {"vs_4_1",              VKD3D_SHADER_TYPE_VERTEX,   4, 1, 0, 0, FALSE},
        {"vs_5_0",              VKD3D_SHADER_TYPE_VERTEX,   5, 0, 0, 0, FALSE},
    };

    for (i = 0; i < ARRAY_SIZE(profiles); ++i)
    {
        if (!strcmp(target, profiles[i].name))
            return &profiles[i];
    }

    return NULL;
}

int hlsl_compile_shader(const char *text, const struct vkd3d_shader_compile_info *compile_info,
        struct vkd3d_shader_code *dxbc, struct vkd3d_shader_message_context *message_context)
{
    const struct vkd3d_shader_hlsl_source_info *hlsl_source_info;
    const struct hlsl_profile_info *profile;

    if (!(hlsl_source_info = vkd3d_find_struct(compile_info->next, HLSL_SOURCE_INFO)))
    {
        ERR("No HLSL source info given.\n");
        return VKD3D_ERROR_INVALID_ARGUMENT;
    }

    if (!(profile = get_target_info(hlsl_source_info->profile)))
    {
        FIXME("Unknown compilation target %s.\n", debugstr_a(hlsl_source_info->profile));
        return VKD3D_ERROR_NOT_IMPLEMENTED;
    }

    vkd3d_shader_dump_shader(profile->type, &compile_info->source);

    return hlsl_lexer_compile(text, profile->type, profile->sm_major, profile->sm_minor,
            hlsl_source_info->entry_point ? hlsl_source_info->entry_point : "main", dxbc, message_context);
}
