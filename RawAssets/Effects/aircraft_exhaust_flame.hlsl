// A 100 cm Engine Plane: +50 X is the engine lip, -50 X the plume tip.
// Local coordinates keep the tail correct regardless of mesh UV orientation.
float downstream = saturate((50.0 - LocalPosition.x) * 0.01);
float across = LocalPosition.y * 0.02;
float clock = Clock + Phase;

// Turbulence is deliberately small at the nozzle and grows toward the tip.
float curl = sin(downstream * 22.0 - clock * 13.0)
           + 0.45 * sin(downstream * 49.0 - clock * 21.0 + 1.8);
float centre = curl * 0.026 * downstream * downstream;
float expansion = 0.62 + 0.12 * sin(downstream * 3.14159265);
float radius = expansion * pow(max(0.0, 1.0 - downstream), 0.64) + 0.035;
float q = (across - centre) / radius;
float halo = exp(-q * q * 3.8);
float hotCore = exp(-q * q * 22.0);
float fade = (1.0 - smoothstep(0.68, 1.0, downstream))
           * smoothstep(0.0, 0.014, downstream);
float edge = 1.0 - smoothstep(0.76, 0.98, abs(across));

// Compression diamonds repeat inside the continuous, soft outer flame.
float cell = frac(downstream * 5.2 - clock * 0.06);
float diamondWidth = 0.16 + 0.56 * (1.0 - abs(cell * 2.0 - 1.0));
float diamond = exp(-pow(q / diamondWidth, 2.0) * 4.0)
              * pow(sin(cell * 3.14159265), 2.0);
float flicker = 0.94 + 0.035 * sin(clock * 23.0 + downstream * 31.0)
                    + 0.025 * sin(clock * 37.0 - downstream * 17.0);
float alpha = saturate((halo * 0.58 + hotCore * 0.48 + diamond * 0.16)
                      * fade * edge * flicker);

float3 blueBase = float3(0.12, 0.38, 1.0);
float3 amberTip = float3(1.0, 0.31, 0.055);
float3 temperature = lerp(blueBase, amberTip, smoothstep(0.10, 0.80, downstream));
float whiteHeat = saturate(hotCore * (0.90 - downstream * 0.48) + diamond * 0.32);
temperature = lerp(temperature, float3(1.0, 0.92, 0.76), whiteHeat);
return float4(temperature * Color.rgb, alpha);
