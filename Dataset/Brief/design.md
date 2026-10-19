# Dune Sentinel design

Sentinel is an isolated forward airbase in a desert basin. A clear daytime sky, warm ground colours and distant ridges support long sightlines and readable combat.

The player flies one of three modern fighters in third person. Each airframe trades durability, speed, handling and ammunition. Independent pitch, roll and yaw support expert flight, with throttle, braking, cannon fire, missile fire and target selection. The camera keeps the aircraft below the centre of the view.

Start at the airbase menu, select an aircraft and launch. Enemy fighters attack the player; bombers fly toward the airbase, bomb it and depart. Incoming waves repeat every five minutes. Flying near the base replenishes ammunition. Aircraft destruction and survival award points; losing the player aircraft or the base ends the sortie.

Native gameplay classes implement flight, weapons, opponents, score and the interface. `/Game/Maps/DesertDefense` is the play map. A Blueprint game-mode subclass holds editable base health and wave timing. Existing Unreal Engine 5.7 components are adopted with exact asset hashes and recorded changes.

The menu, pause screen and HUD use a shared avionics style. Clear markings, bright weapon feedback, layered ground materials and engine audio support orientation. Keep the source and licence of every third-party asset with the project.

Acceptance covers launch, three aircraft, flight controls, weapons and ammunition, replenishment, fighters and bombers, scoring and defeat. Validate repeatable keyboard routes and Windows packaging. Keep exact-byte reuse verification separate from new runtime checks and retain the identity of earlier evidence when it is reused.
