// Rear-facing hot nozzle with a soft optical halo, never a rectangular card.
float2 p = (UV - 0.5) * 2.0;
float radiusSquared = dot(p, p);
float radius = sqrt(radiusSquared);
float clock = Clock + Phase;
float angle = atan2(p.y, p.x);
float filament = 0.96 + 0.025 * sin(angle * 11.0 + clock * 7.0)
                         * smoothstep(0.12, 0.60, radius);
float whiteCore = exp(-radiusSquared * 17.0);
float blueHalo = exp(-radiusSquared * 4.2);
float edge = 1.0 - smoothstep(0.72, 0.98, radius);
float pulse = 0.975 + 0.025 * sin(clock * 19.0);
float alpha = saturate((whiteCore * 0.78 + blueHalo * 0.44) * edge * filament * pulse);
float3 heat = lerp(float3(0.10, 0.34, 1.0), float3(1.0, 0.95, 0.83),
                  saturate(whiteCore * 1.18));
return float4(heat * Color.rgb, alpha);
