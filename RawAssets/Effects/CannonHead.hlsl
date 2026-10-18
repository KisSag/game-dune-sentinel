// Original circular projectile head: compact hot core and soft amber halo.
float2 p = (UV - 0.5) * 2.0;
float radius = length(p);
float core = exp(-radius * radius * 46.0);
float halo = exp(-radius * radius * 6.0);
float mask = saturate(core * 0.68 + halo * 0.42);
mask *= 1.0 - smoothstep(0.80, 0.98, radius);
float3 heat = lerp(Color.rgb, float3(1.0, 0.98, 0.90), saturate(core * 1.25));
return float4(heat, mask);
