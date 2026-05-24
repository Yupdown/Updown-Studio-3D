struct VS_OUT
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD0;
};

VS_OUT VS(uint vid : SV_VertexID)
{
    static const float2 verts[6] =
    {
        float2(-1.0f,  1.0f),
        float2( 1.0f,  1.0f),
        float2(-1.0f, -1.0f),
        float2(-1.0f, -1.0f),
        float2( 1.0f,  1.0f),
        float2( 1.0f, -1.0f)
    };

    VS_OUT output;
    output.PosH = float4(verts[vid], 0.0f, 1.0f);
    output.TexC = verts[vid] * float2(0.5f, -0.5f) + 0.5f;
    return output;
}
