/*
 * Copyright 2025 Henri Verbeet
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

cbuffer teapot_cb : register(b0)
{
    float4x4 mvp_matrix;
    float level;
};

struct control_point
{
    float4 position : SV_POSITION;
};

struct patch_constant_data
{
    float edges[4] : SV_TessFactor;
    float inside[2] : SV_InsideTessFactor;
};

struct ps_in
{
    float4 position : SV_POSITION;
};

float4 vs_main(float4 position : POSITION, uint id : SV_InstanceID) : SV_POSITION
{
    /* Mirror/flip patches based on the instance ID. */
    position.w = -1.0;
    if (id & 1)
        position.yw = -position.yw;
    if (id & 2)
        position.xw = -position.xw;

    return position;
}

struct patch_constant_data patch_constant(InputPatch<control_point, 16> input)
{
    struct patch_constant_data output;

    output.edges[0] = level;
    output.edges[1] = level;
    output.edges[2] = level;
    output.edges[3] = level;
    output.inside[0] = level;
    output.inside[1] = level;

    return output;
}

[domain("quad")]
[outputcontrolpoints(16)]
[outputtopology("triangle_ccw")]
[partitioning("integer")]
[patchconstantfunc("patch_constant")]
struct control_point hs_main(InputPatch<control_point, 16> input, uint i : SV_OutputControlPointID)
{
    /* Reorder mirrored/flipped control points. */
    if (input[0].position.w < 0.0)
    {
        uint u = i % 4, v = i / 4;
        return input[v * 4 + (3 - u)];
    }

    return input[i];
}

float3 eval_quadratic(float3 p0, float3 p1, float3 p2, float t)
{
    return lerp(lerp(p0, p1, t), lerp(p1, p2, t), t);
}

float3 eval_cubic(float3 p0, float3 p1, float3 p2, float3 p3, float t)
{
    return lerp(eval_quadratic(p0, p1, p2, t),
            eval_quadratic(p1, p2, p3, t), t);
}

struct ps_in eval_patch(float2 t, float4 p[16])
{
    float3 position, q[4];
    struct ps_in o;

    q[0] = eval_cubic(p[0].xyz, p[4].xyz,  p[8].xyz, p[12].xyz, t.y);
    q[1] = eval_cubic(p[1].xyz, p[5].xyz,  p[9].xyz, p[13].xyz, t.y);
    q[2] = eval_cubic(p[2].xyz, p[6].xyz, p[10].xyz, p[14].xyz, t.y);
    q[3] = eval_cubic(p[3].xyz, p[7].xyz, p[11].xyz, p[15].xyz, t.y);

    position = eval_cubic(q[0], q[1], q[2], q[3], t.x);
    o.position = mul(mvp_matrix, float4(position, 1.0));

    return o;
}

[domain("quad")]
struct ps_in ds_main(struct patch_constant_data input,
        float2 tess_coord : SV_DomainLocation, const OutputPatch<control_point, 16> patch)
{
    return eval_patch(tess_coord, patch);
}

float4 ps_main(struct ps_in i) : SV_TARGET
{
    return float4(1.0, 0.69, 0.0, 1.0);
}
