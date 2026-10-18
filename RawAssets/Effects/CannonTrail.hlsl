// Engine Plane is 100 cm across, with its length along component-local X.
// Derive UV from local position so import UV orientation cannot reverse the tail.
// +X=+50 is head, -X=-50 is tail; component centre is half its length behind bullet.
float u = saturate((LocalPosition.x + 50.0) * 0.01);
float v = LocalPosition.y * 0.02;
float width = lerp(0.18, 1.0, pow(u, 0.65));
float core = exp(-pow(v * 12.0 / width, 2.0));
float halo = exp(-pow(v * 3.4 / width, 2.0));
float lengthFade = pow(u, 1.45) * (1.0 - smoothstep(0.94, 1.0, u));
float sideFade = 1.0 - smoothstep(0.78, 0.99, abs(v));
float mask = saturate(core * 0.64 + halo * 0.48) * lengthFade * sideFade;
float3 heat = lerp(Color.rgb, float3(1.0, 0.97, 0.86), saturate(core * 0.95));
return float4(heat, mask);
