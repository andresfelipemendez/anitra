// Vertex shader for vector font rendering
// Set 1 = vertex uniform buffers (matches sprite_vs convention)

[[vk::binding(0, 1)]]
cbuffer Constants : register(b0) {
    float4x4 projection;
    float4x4 view;
};

struct VSInput {
    float2 pos : POSITION;       // screen-space glyph quad corner
    float2 uv : TEXCOORD0;      // normalized [0,1] within glyph bbox
    float4 color : COLOR0;      // text color + alpha
    uint glyph_id : BLENDINDICES0;
};

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation uint glyph_id : BLENDINDICES0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.pos = mul(mul(projection, view), float4(input.pos, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    output.glyph_id = input.glyph_id;
    return output;
}
