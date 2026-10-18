# Third-party notices

## Aircraft models

Aircraft source models by **Captain_Ahab_62**, published on OpenGameArt.org:

- [Fighter jets](https://opengameart.org/content/fighter-jets)
- [Adversary aircraft](https://opengameart.org/content/adversary-aircraft)

License: [Creative Commons Zero 1.0](https://creativecommons.org/publicdomain/zero/1.0/).
The F-15, Eurofighter, Rafale, F-111 and J-20 source meshes were selected,
reoriented, rescaled and assigned new surface materials. The bomber's wings and
fuselage were combined. Game aircraft have fictional names and performance.

## Desert surface

**Aerial Ground Rock** by **Rob Tuytel**, from
[Poly Haven](https://polyhaven.com/a/aerial_ground_rock).
License: [Creative Commons Zero 1.0](https://polyhaven.com/license).
The 2K base colour, DirectX normal and AO/roughness/metallic maps use world-scaled
coordinates for desert ground and weathered airbase concrete. Close surface
detail fades into broader ground variation with viewing distance.

## Typography

**Orbitron**, from [Google Fonts](https://fonts.google.com/specimen/Orbitron),
is licensed under the SIL Open Font License 1.1. The complete license is retained
at `RawAssets/Fonts/OFL.txt`.

**Rajdhani**, Copyright (c) 2014 Indian Type Foundry, from
[Google Fonts](https://fonts.google.com/specimen/Rajdhani), is licensed under the
SIL Open Font License 1.1. The complete license is retained at
`RawAssets/Fonts/Rajdhani-OFL.txt`.

## Original work

Airbase structures, terrain and distant ground geometry, equipment, missile and undercarriage models,
aircraft panel artwork, asphalt surface artwork and nonvoice sound effects were
created for this game. No recorded speech or synthesized speech is used.

Aircraft engine exhaust uses project-authored animated shader artwork, retained in
`RawAssets/Effects/aircraft_exhaust_*.hlsl`, with nozzle placement measured from the credited aircraft models.

## Desert settlement and oasis textures

The following ambientCG textures are CC0 1.0. Original selected maps and SHA-256 hashes are retained in `RawAssets/Textures/Environment/` and its `sources.json`.

- [PaintedPlaster017](https://ambientcg.com/view?id=PaintedPlaster017) — Plaster surface.
- [CorrugatedSteel005](https://ambientcg.com/view?id=CorrugatedSteel005) — Roof surface.
- [Rock030](https://ambientcg.com/view?id=Rock030) — Rock surface.
- [Grass004](https://ambientcg.com/view?id=Grass004) — Grass surface.
- [LeafSet014](https://ambientcg.com/view?id=LeafSet014) — Leaf surface.
- [Bark012](https://ambientcg.com/view?id=Bark012) — Bark surface.

[License](https://docs.ambientcg.com/license/). The earlier revision's settlement architecture and plants were
project-authored geometry and remain as retained source assets. The current map uses the downloaded model
derivatives credited below. Terrain, lakes and roads remain project-authored geometry.

## Downloaded environment model revision

The revised town and oasis use the following Poly Haven model packs under [CC0 1.0](https://polyhaven.com/license). Original model files, texture dependencies, publisher checksums and SHA-256 digests are retained in `RawAssets/Downloaded/PolyHaven/`. The assembled buildings and adapted natural models are in `RawAssets/Environment/Downloaded/`.

- [Grass Bermuda 01](https://polyhaven.com/a/grass_bermuda_01) — Rico Cilliers.
- [Island Tree 01](https://polyhaven.com/a/island_tree_01) — Rob Tuytel, Rico Cilliers.
- [Island Tree 02](https://polyhaven.com/a/island_tree_02) — Rob Tuytel, Rico Cilliers.
- [Jacaranda Tree](https://polyhaven.com/a/jacaranda_tree) — Rob Tuytel, Rico Cilliers.
- [Modular Factory Facade](https://polyhaven.com/a/modular_factory_facade) — James Ray Cock.
- [Modular Urban Apartments Facade](https://polyhaven.com/a/modular_urban_apartments_facade) — James Ray Cock.
- [Namaqualand Boulder 02](https://polyhaven.com/a/namaqualand_boulder_02) — Greg Zaal, Rico Cilliers.
- [Namaqualand Cliff 02](https://polyhaven.com/a/namaqualand_cliff_02) — Dario Barresi, Rico Cilliers.
- [Wild Rooibos Bush](https://polyhaven.com/a/wild_rooibos_bush) — James Ray Cock, Jenelle van Heerden.

The earlier authored town and plants remain in source history and retained raw assets; the current map replaces them with these downloadable model derivatives. Terrain, roads and lake surfaces remain project-authored geometry.

## Detailed aircraft and airbase sources

The following source models are licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Original downloaded files, source links, author credits, exact texture bytes and SHA-256 hashes are retained in `RawAssets/Downloaded/Aviation/`.

- [F-16 Fighting Falcon](https://sketchfab.com/3d-models/f-16-fighting-falcon-f4a4568d8a624026a675090852fb9546) — k.botond431.
- [F22 Raptor FREE](https://sketchfab.com/3d-models/f22-raptor-free-2a64abf0866a405c865466c7642ca689) — NLM.
- [Dassault Rafale](https://sketchfab.com/3d-models/dassault-rafale-d8bbfb0970ca4128b73e7e5364828fd3) — quick_loop.
- [Aircraft Hangar](https://sketchfab.com/3d-models/aircraft-hangar-a5b17ba2e3494873abd313f720097614) — jimbogies.
- [Control Tower San Francisco](https://sketchfab.com/3d-models/control-tower-san-francisco-96e0b2ef0b784a48968634583617d208) — Lucas Sobral.

The original hangar archive also contains [Diamond Da42 "Twin Star"](https://skfb.ly/AZU6) by helijah, under CC BY 4.0. It remains in the retained original but is removed from the derived airport building. Source presentation transforms, unrelated small props and detached accessories are removed where documented. Aircraft derivatives preserve textured materials and use measured physical scaling, normalized axes and separate flight/parked poses. The Rafale material conversion preserves its source dielectric reflectance and derives roughness from glossiness. Its parked gear adapts the credited F-16 wheel and strut geometry. Native asset acceptance is still in progress.

## Detailed bomber source

[Tupolev-Tu22-Blinder](https://sketchfab.com/3d-models/tupolev-tu22-blinder-eb6fe03160f943c4adc5c731a435124e), by **helijah**, is used under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The separately downloaded GLB carries this credit and license in its embedded metadata. Its exact original bytes, 23 embedded color images and source hashes are retained in `RawAssets/Downloaded/Aviation/tu22/`. No files from another source edition are incorporated.

The airborne derivative preserves the authored metre scale, texture coordinates and split normals. Exposed wheels, struts and links are removed; static gear-bay doors, nacelles, detailed cockpit and both engine outlets remain. Measured sockets locate the twin exhausts, rear defensive gun and bomb release. Original material color maps, scalar roughness/metallic factors and glass transparency are retained.

The exterior atlas is **1024×1024**; the other source images are 256–1024 pixels. This edition supplies no normal, roughness, metallic or occlusion maps. The model is a textured bomber baseline; it is not a claim of high-resolution PBR or AAA quality.
