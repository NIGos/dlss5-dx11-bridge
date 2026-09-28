// ReShade skips begin/finish-effects events when it has no techniques.
// This technique exists only to make the documented callback available.
// ColorWriteMask=0 preserves the application's colour, including alpha.
float4 AdapterVS(uint id : SV_VertexID) : SV_Position
{
    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}
float4 AdapterPS(float4 position : SV_Position) : SV_Target
{
    return 0.0;
}
technique AdapterEvents
{
    pass
    {
        VertexShader = AdapterVS;
        PixelShader = AdapterPS;
        ColorWriteMask = 0;
    }
}
