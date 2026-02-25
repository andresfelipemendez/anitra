// Fragment shader for UI rectangles (Clay)

struct VSOutput {
    float4 pos : SV_Position;
    float4 color : COLOR0;
};

float4 PSMain(VSOutput input) : SV_Target {
    return input.color;
}
