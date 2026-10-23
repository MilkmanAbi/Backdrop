// Backdrop blur: two separable Gaussian passes over a captured copy of the screen (blur.cpp).
// PSH blurs along the screen's x into a float texture (and undoes the output's rotation), PSV blurs along y and
// pre-compensates for the colour filter DWM applies on top of the overlay again.
cbuffer Params : register(b0) {
    float4 mapU, mapV;   // texture uv = (dot(mapU.xy, uv) + mapU.z, dot(mapV.xy, uv) + mapV.z)
    float2 stepUV;       // texture uv per screen pixel along the blur axis
    int taps;
    int pad;
    float4 tap[12];      // x = weight, y = offset in pixels (pairs merged into one bilinear fetch)
    float4 fix[3];       // output channel i = dot(fix[i].xyz, rgb) + fix[i].w
};
Texture2D src : register(t0);
SamplerState smp : register(s0);

struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

V VS(uint id : SV_VertexID) {   // one triangle covering the target
    V o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float3 Blur(float2 uv) {
    float2 t = float2(dot(mapU.xy, uv) + mapU.z, dot(mapV.xy, uv) + mapV.z);
    float3 s = src.SampleLevel(smp, t, 0).rgb * tap[0].x;
    for (int k = 1; k < taps; k++) {
        float2 o = stepUV * tap[k].y;
        s += (src.SampleLevel(smp, t + o, 0).rgb + src.SampleLevel(smp, t - o, 0).rgb) * tap[k].x;
    }
    return s;
}

float4 PSH(V i) : SV_Target { return float4(Blur(i.uv), 1); }

float4 PSV(V i) : SV_Target {
    float3 c = Blur(i.uv);
    return float4(saturate(float3(dot(fix[0].xyz, c) + fix[0].w, dot(fix[1].xyz, c) + fix[1].w, dot(fix[2].xyz, c) + fix[2].w)), 1);
}

// change detection: draws only where the new copy (t0) differs from the one last blurred (t1) by more than a few
// levels; an occlusion query per tile counts the survivors
Texture2D prev : register(t1);

float4 PSD(V i) : SV_Target {
    int3 p = int3(i.pos.xy, 0);
    if (all(abs(src.Load(p).rgb - prev.Load(p).rgb) < 5.5 / 255)) discard;   // the filter's rounding jitters by a few levels
    return 1;
}
