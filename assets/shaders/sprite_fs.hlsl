struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
    float4 tint : COLOR0;
};

[[vk::binding(0, 2)]]
Texture2D tex : register(t0);
[[vk::binding(0, 2)]]
SamplerState samp : register(s0);

float4 PSMain(VSOutput input) : SV_Target {
    float4 texColor = tex.Sample(samp, input.uv);
    return texColor * input.tint;
}
