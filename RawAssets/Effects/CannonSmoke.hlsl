// Original three-octave smoke density; alpha reaches zero before card edges.
float2 p = (UV - 0.5) * 2.0;
float2 q = p * 3.8 + float2(Seed, Seed * 0.37);
float density = 0.0;
float amplitude = 0.57;
[unroll]
for (int octave = 0; octave < 3; ++octave)
{
    float2 cell = floor(q);
    float2 f = frac(q);
    f = f * f * (3.0 - 2.0 * f);
    float4 h = float4(dot(cell, float2(127.1, 311.7)),
                     dot(cell + float2(1, 0), float2(127.1, 311.7)),
                     dot(cell + float2(0, 1), float2(127.1, 311.7)),
                     dot(cell + float2(1, 1), float2(127.1, 311.7)));
    h = frac(sin(h) * 43758.5453);
    density += lerp(lerp(h.x, h.y, f.x), lerp(h.z, h.w, f.x), f.y) * amplitude;
    q = q * 2.07 + 17.1;
    amplitude *= 0.5;
}
float angle = atan2(p.y, p.x);
float boundary = 0.67 + 0.09 * sin(angle * 5.0 + Seed) + 0.055 * sin(angle * 9.0 - Seed * 0.31);
float mask = 1.0 - smoothstep(boundary - 0.20, boundary + 0.12, length(p));
mask *= lerp(0.28, 1.0, saturate(density)) * 0.78;
float3 shade = Color.rgb * lerp(0.72, 1.08, saturate(density));
return float4(shade, mask);
