// Vertex shader for UI rectangles (Clay)
// Set 1 = vertex uniform buffers

[[vk::binding(0, 1)]]
cbuffer Constants : register(b0) {
    float4x4 projection;
    float4x4 view;
};

struct VSInput {
    float2 pos : POSITION;
    float4 color : COLOR0;
};

struct VSOutput {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.pos = mul(mul(projection, view), float4(input.pos, 0.0, 1.0));
    output.color = input.color;
    return output;
}
