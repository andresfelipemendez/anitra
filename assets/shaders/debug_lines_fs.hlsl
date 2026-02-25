struct VSOutput {
    float4 pos : SV_Position;
    float3 color : COLOR0;
};

float4 PSMain(VSOutput input) : SV_Target {
    return float4(input.color, 1.0);
}
