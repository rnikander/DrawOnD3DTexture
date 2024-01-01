
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
};


Texture2D theTexture : register(t0);
SamplerState theSampler : register(s0);

// Things labled with "2" have texture coord.


struct VertexIn2
{
    float2 PosL : POSITION;
    float2 TexC : TEXCOORD;
};

struct VertexOut2
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};


VertexOut2 vert_shader(VertexIn2 vin)
{
    VertexOut2 vout = (VertexOut2) 0.0f;
	
    vout.PosH = mul(float4(vin.PosL, 0, 1.0f), gWorldViewProj);
    vout.TexC = vin.TexC;

    return vout;
}

float4 pix_shader(VertexOut2 pin) : SV_Target
{
    float4 color = theTexture.Sample(theSampler, pin.TexC);
    // color = float4(1, 0, 0, 1);
    return color;
}