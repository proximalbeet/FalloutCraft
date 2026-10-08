# MODLOG: real Minecraft inside Fallout 4 (passthrough)

Goal: Minecraft Java runs next to Fallout 4 and is drawn into FO4's frame. FO4's camera drives Minecraft's
camera. Minecraft TNT (and creepers) detonating = a real FO4 explosion at the same spot (damage, physics
impulse, FO4 VFX/SFX), scaled to the Minecraft explosion power.
Done = works in the real game + a 20-45 s clip.

## Recon (2026-10-05)
- Host: Fallout 4, Steam 377160, `~/.local/share/Steam/steamapps/common/Fallout 4`
  - **Runtime 1.11.240.0** (Aug 2026 update, buildid 24564252). D3D11. No anti-cheat. Single-player.
  - Runs under **Proton Experimental** (native Linux host, CachyOS, RTX 5070 Ti).
  - Prefix: `steamapps/compatdata/377160/pfx`, My Games: `.../Documents/My Games/Fallout4/` (Saves only).
  - No loaders installed yet.
  - **F4SE 0.7.9** supports 1.11.240 (f4se.silverlock.org; download only on Nexus, mod 42147, needs login).
- Guest: Minecraft Java: **not installed** (no ~/.minecraft, no Prism/MultiMC). JDK 27 on PATH.
- Toolchain: clang 22 + clang-cl + lld-link present. Need MSVC CRT/Windows SDK via `xwin`. No cmake/wine.
- Reference implementation: universal-modder `examples/minecraft-gta5-passthrough` (Windows/WSL; GTA V).

## Route (and why)
Pattern 2 passthrough, adapted from the GTA V example:
```
Fallout 4 (Proton)                                         Minecraft (native Linux) + Fabric
  MCPassthrough F4SE plugin  -- WebSocket 127.0.0.1:25599 -->  HostLink: cam / player / ground / input
                             <-----------------------------    explosion events {x,y,z,power,source}
  ReShade add-on compositor  <-- file mapping /dev/shm/mcpassthrough_frame --  FrameExporter: RGBA+depth, overlay
  MCPassthrough.fx: depth test vs FO4 depth, overlay on top
```
- Linux twist: Wine named mappings (`Local\...`) are invisible to Linux processes, so the frame buffer is a
  tmpfs file: Java `FileChannel.map` on `/dev/shm/...`; Wine side `CreateFile("Z:\\dev\\shm\\...")` +
  `CreateFileMapping`. WebSocket on 127.0.0.1 crosses Wine fine.
- ReShade (add-on build) loaded as `dxgi.dll` on top of DXVK with `WINEDLLOVERRIDES="dxgi=n,b"`.
- Units: FO4 1 unit ≈ 1.428 cm → 70 units = 1 m = 1 block. FO4 is Z-up (like GTA) →
  MC (x, z + yOffset, −y) after scaling; yaw/pitch mapping to verify in-game.
- Explosions: plugin places an FO4 `Explosion` form at the MC event position (PlaceAtMe on a moved marker /
  native PlaceObjectAtMe). Tiered by MC power: creeper (3) → frag grenade class, TNT (4) → frag mine class,
  chained/big → larger. Exact form IDs to read from Fallout4.esm.

## Next steps
1. User: Minecraft launcher + account; F4SE 0.7.9 download; OK to install F4SE + ReShade into the game folder.
2. `um backup` FO4 saves + My Games before the first modded launch.
3. Vertical slice: camera sync → one block drawn at the right place in FO4. Then depth, then TNT.

## Progress (2026-10-05)
- Project: `fo4-mc-passthrough/` (git). Backup of FO4 My Games: `~/.universal-modder/backups/fo4-mygames/20261005-210622.zip`.
- mc/: builds with Temurin 25 (`toolchain/jdk25`); Gradle 9.7.1 can't run on JDK 27 ("major version 71").
  Frames: `/dev/shm/mcpassthrough_frame` (FileChannel.map).
- f4se/: `build.sh` (clang-cl + lld-link + xwin CRT/SDK in `toolchain/msvc`) -> `build/MCPassthrough.dll`, exports OK.
  Addresses hard-coded from F4SE 0.7.9 source (`~/fo4-src/f4se`), runtime 1.11.240 only.
- FO4 1.11 input is Raw Input (USER32!GetRawInputData imported; no dinput8) -> IAT hook for Minecraft mode (F6).
- Explosions: fragGrenade 000E574F / fragMine 000EECE9 (TNT) / nukaGrenade 000E5752 / FatMan 001A7FF2 (8+ TNT chain).
- Prism flatpak: needed `flatpak remote-add --user flathub` (silent failure first time), and
  `flatpak override --user --device=shm` (sandbox /dev/shm is private otherwise). Verified with a probe file.
- Unverified until in game: NiCamera frustum scan, forward axis auto-check, FO4 depth not reversed, marker+PlaceAtMe.

## In game (2026-10-05 evening)
- Works: plugin load, link, 2560x1440, NiCamera + frustum at +0x160, composite, TNT -> fragMine (18-TNT chain ok).
- Camera axes: real answer is row 0 forward / row 1 up; the main menu gives a wrong one (column 2) -> re-check on load.
- Shader: ReShade turns [loop] into [fastopt]; Proton's d3dcompiler_47 (vkd3d-shader) rejects it (E5017). No [loop].
- Health AV = 000002D4 (AVIF). ActorValueOwner at REFR+0x58: vtbl 1 GetValue, 2 GetMaximum, 8 damage-modify.
- First-person skeleton at PlayerCharacter+0xB78; third-person root = LoadedData(+0xF0)+0x08. Hidden flag = bit 0 at +0x108.
- Fallout4Prefs.ini set to 2560x1440 borderless (backup .bak-mcpassthrough).
- Untested in game: body/arms hiding, shared-health damage into Fallout (vtbl 8), shots, mini-nuke threshold.

## Second evening (2026-10-05/06)
- Natives found in Fallout4.exe 1.11.240 (tools/find_native.py): SetPosition 115DED0 (takes a 24-byte ref record, not
  TESObjectREFR**: fault at +115DFBC otherwise), DamageValue 11543B0 (ref by value), Kill 10F9E90, IsDead = vfunc +0x600.
- Health modifier alone never kills; DamageValue does. Cryo-pod bodies report 3 health: use IsDead.
- Hiding the 3rd-person root (APP_CULLED) froze the animation graph (no run/jump): hide geometry only.
- Camera axes: row 0 forward / row 1 up, hard-coded (the auto check picked column 2 on the main menu / third person).
- Barriers are saved with the MC world; clear must sweep (8701 leftovers = "air is solid" in the wasteland).
- Terrain: Fallout4.esm LAND VHGT -> 80 MB int16 heightmap; matched feet within 8 units in game.
- Minecraft window shrank to 320x240 on Wayland after start: re-apply host size every 2 s.
- ReShade add-on registers after the runtime exists: take it from reshade_present. Before-UI render = before the
  back buffer's 2nd draw (log showed 2 draws/frame).
- Shader light match at mip 3.2 printed dark shapes onto a close-up Steve: mip 5.5, strength 0.5.
- Background "wait for exit" jobs: never `pgrep -f Fallout4.exe` inside them (matches their own command line).

## Paused 2026-10-06 (user switched projects)
- Installed, untested: solid placed blocks (cinder-block stand-ins), fireworks -> Fallout missiles, rounded floor.
- The new Minecraft jar (FireworkMixin) is in the Prism instance but Minecraft wasn't restarted (old process, PID 115181).
- Open: Minecraft inventory screen (cursor forwarding), "face in NPC conversations" (ask), Steve-textured arms (offered),
  structures not in the heightmap.
