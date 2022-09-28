static const char triangle_hlsl[] =
"struct ps_in\n"
"{\n"
"    float4 position : SV_POSITION;\n"
"    float4 colour : COLOR;\n"
"};\n"
"\n"
"struct ps_in vs_main(float4 position : POSITION, float4 colour : COLOR)\n"
"{\n"
"    struct ps_in o;\n"
"\n"
"    o.position = position;\n"
"    o.colour = colour;\n"
"\n"
"    return o;\n"
"}\n"
"\n"
"float4 ps_main(struct ps_in i) : SV_TARGET\n"
"{\n"
"    return i.colour;\n"
"}\n";
