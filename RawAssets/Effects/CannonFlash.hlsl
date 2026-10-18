// Original radial muzzle-flash artwork. Billboard UV is [0,1], centre at 0.5.
float2 p = (UV - 0.5) * 2.0;
float radius = length(p);
float angle = atan2(p.y, p.x) + Seed;
float core = exp(-radius * radius * 42.0);
float spokes = pow(abs(cos(angle * 3.0)), 18.0);
float secondary = pow(abs(cos(angle * 2.0 + 0.65)), 24.0);
float rays = exp(-radius * 4.3) * (0.15 + 0.85 * spokes);
rays += exp(-radius * 6.0) * secondary * 0.27;
float mask = saturate(core + rays * 0.82);
mask *= 1.0 - smoothstep(0.73, 0.98, radius);
float3 heat = lerp(Color.rgb, float3(1.0, 0.97, 0.82), saturate(core * 1.6));
return float4(heat, mask);
