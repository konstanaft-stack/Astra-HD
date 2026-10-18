# CET vehicle grant for three requested cars

Prepared artifact: [`CyberpunkVRPort_VehicleGrant/init.lua`](CyberpunkVRPort_VehicleGrant/init.lua). This directory is a staging area; nothing here has been copied into the live game. The mod needs the user's existing Cyber Engine Tweaks installation and the corresponding vehicle mod files. It adds no other dependency and changes no money, quests, or save files directly.

After the three vehicle mods are installed and the game is closed, copy the `CyberpunkVRPort_VehicleGrant` directory into `Cyberpunk 2077/bin/x64/plugins/cyber_engine_tweaks/mods/`. The game launch and in-game call-menu check belong to the user/root workflow.

The exact author commands are:

| Vehicle | Record passed to `EnablePlayerVehicle(id, true, false)` | Author page |
| --- | --- | --- |
| Toyota GR Yaris | `Vehicle.toyota_gr_yaris_purchasable` | [Nexus 34357](https://www.nexusmods.com/cyberpunk2077/mods/34357) |
| Audi RS6 Performance | `Vehicle.rs6_performance` | [Nexus 33903](https://www.nexusmods.com/cyberpunk2077/mods/33903) |
| Nomad Warlock Conversion | `Vehicle.v_standard25_thorton_merrimac_player` | [Nexus 33970](https://www.nexusmods.com/cyberpunk2077/mods/33970) |

The mod waits for `Game.GetPlayer()` and `Game.GetVehicleSystem()` after a short settling period. `PlayerPuppet.OnGameAttached` re-arms it for a save loaded within the same process; a player disappearing and returning also starts a new local session. Record existence is checked with CET's `TweakDB:GetRecord` before each grant. There are at most six record/API attempts per vehicle, spaced two seconds apart, and at most twelve VehicleSystem readiness checks per local session. Completed grants do not repeat during that session. CET console/log entries begin `[CyberpunkVR VehicleGrant]` and distinguish **invoked**, **record unavailable**, and **failed**. A nil result from `EnablePlayerVehicle` is logged as an invocation with menu appearance unverified, not as proof of success.

The Warlock is a replacer of an existing vehicle. Its base `Vehicle.v_standard25_thorton_merrimac_player` record may exist even when the replacement assets are missing, so TweakDB cannot verify the conversion. All three entries require a user-visible call-menu and vehicle-appearance check after launch.

Offline verification: `tests/run.py` uses the already staged Lupa 2.8 LuaJIT 2.1 runtime and mocks CET/Game/TweakDB. It checks main-menu gating, exact command arguments, save/load re-arming, late records, VehicleSystem readiness, bounded retries, and truthful logs. Run with the bundled Python:

```powershell
python .\vehicle-tools\tests\run.py
```

Result on 10 Oct 2026: `PASS: LuaJIT 2.1 syntax and CET mock session/vehicle behavior`. This is an offline component check; no live CET, save, or call-menu behavior has been verified.
