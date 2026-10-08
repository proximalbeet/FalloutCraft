// MCPassthrough: the Fallout 4 half of the Minecraft passthrough (an F4SE plugin and a ReShade add-on in one DLL).
//
//   render thread (ReShade, once per frame)   Fallout's camera -> {"t":"cam"} for Minecraft, and the pose the
//                                             compositor re-projects Minecraft's frame to
//   main thread (F4SE permanent task)         hotkeys, the ground under the player as barrier blocks, and Minecraft's
//                                             explosions set off as real Fallout explosions
//   window thread (GetRawInputData hook)      with the weapon holstered, clicks, the wheel and 1-9 go to Minecraft
//                                             instead; with it drawn they are Fallout's, and a shot is also fired
//                                             into Minecraft
//
// Units: 70 Fallout units = 1 m = 1 block. Fallout (x east, y north, z up) -> Minecraft (x, z / 70 + yOffset, -y).
#include "compositor.h"
#include "game.h"
#include "ws.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
	// ---------------------------------------------------------------- F4SE plugin API (f4se/PluginAPI.h, 0.7.9)
	using PluginHandle = uint32_t;

	struct F4SEInterface
	{
		uint32_t f4seVersion, runtimeVersion, editorVersion, isEditor;
		void *(*QueryInterface)(uint32_t id);
		PluginHandle (*GetPluginHandle)();
		uint32_t (*GetReleaseIndex)();
	};

	constexpr uint32_t kInterface_Task = 5;

	class ITaskDelegate
	{
	public:
		virtual ~ITaskDelegate() = default;
		virtual void Run() = 0;
	};

	struct F4SETaskInterface
	{
		uint32_t interfaceVersion;
		void (*AddTask)(ITaskDelegate *task);
		void (*AddUITask)(ITaskDelegate *task);
		void (*AddTaskPermanent)(ITaskDelegate *task);
	};

	// ---------------------------------------------------------------- settings
	constexpr int kPort = 25599;
	constexpr double kUnitsPerBlock = 70.0;
	constexpr int kGroundRadius = 18;  // blocks around the player kept solid
	constexpr uint32_t kXMarker = 0x0000003B;
	// Minecraft explosion power -> the Fallout explosive that feels like it (Fallout4.esm EXPL records; tools/esm_expl.py)
	constexpr uint32_t kFragGrenade = 0x000E574F; // 150 dmg, 500u radius: creepers, fireballs
	constexpr uint32_t kFragMine = 0x000EECE9;    // 100 dmg, 500u, force 2000: TNT
	constexpr uint32_t kNukaGrenade = 0x000E5752; // 300 dmg, 700u, force 20000: charged creepers, beds, end crystals
	constexpr uint32_t kMiniNuke = 0x001A7FF2;    // FatManExplosion: a big TNT chain going off at once
	constexpr uint32_t kMissile = 0x000FD3AA;     // ExplosionMissileShell, 135 dmg, 500u: a firework rocket's burst
	constexpr int kChainCount = 8;                // TNT explosions ...
	constexpr double kChainWindow = 1.5;          // ... within this many seconds (chained TNT gets a random 0.5-1.5 s fuse) ...
	constexpr double kChainRadius = 12.0;         // ... and blocks of each other make a mini nuke
	constexpr int kMaxBlastsPerTick = 4;
	constexpr uint32_t kHealthAV = 0x000002D4;   // ActorValueInfo "Health" (Fallout4.esm AVIF)
	constexpr float kMcToFalloutDamage = 8.0f;    // Minecraft hit points -> Fallout health (a raider has ~100)
	constexpr double kPeopleRange = 64.0;         // blocks: actors this close get a Minecraft stand-in
	constexpr float kShotDamage = 7.0f;           // Minecraft hit points per Fallout round (a zombie has 20)
	constexpr double kAutoFireDelay = 0.3;        // holding the trigger this long counts as automatic fire ...
	constexpr double kAutoFireInterval = 0.12;    // ... at this rate

	// ---------------------------------------------------------------- state
	HMODULE g_module = nullptr;
	WsClient g_ws;
	F4SETaskInterface *g_tasks = nullptr;
	FILE *g_log = nullptr;
	std::mutex g_logLock;

	std::atomic<bool> g_enabled{true};      // F7
	std::atomic<bool> g_mcInput{false};     // mouse buttons, wheel and 1-9 go to Minecraft (weapon holstered, or F6)
	std::atomic<bool> g_forceMc{false};     // F6: swap who has the controls (holstered = Fallout, drawn = Minecraft)
	std::atomic<bool> g_menuOpen{false};    // a menu pauses the game (Pip-Boy, ...): nothing goes to Minecraft
	std::atomic<bool> g_solidReset{false};  // the ground moved: the blocks' Fallout stand-ins are rebuilt
	std::atomic<bool> g_pipboyOpen{false};  // the Pip-Boy is open: Minecraft isn't drawn
	std::atomic<bool> g_relevel{false};     // F8
	std::atomic<double> g_yOffset{0.0};
	std::atomic<bool> g_haveOffset{false};
	std::atomic<bool> g_cameraOk{false};
	std::atomic<long long> g_frame{0};
	std::atomic<bool> g_recheckAxes{false}; // set by the main thread on a new worldspace or interior
	std::atomic<bool> g_triggerDown{false};
	std::atomic<int> g_triggerPulls{0};

	void log(const char *fmt, ...)
	{
		std::lock_guard<std::mutex> lock(g_logLock);
		if (g_log == nullptr)
			return;
		SYSTEMTIME t;
		GetLocalTime(&t);
		std::fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list args;
		va_start(args, fmt);
		std::vfprintf(g_log, fmt, args);
		va_end(args);
		std::fputc('\n', g_log);
		std::fflush(g_log);
	}

	double now_seconds()
	{
		static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		return double(c.QuadPart) / double(freq.QuadPart);
	}

	struct V3
	{
		double x = 0, y = 0, z = 0;
	};

	V3 to_mc(double x, double y, double z)
	{
		return {x / kUnitsPerBlock, z / kUnitsPerBlock + g_yOffset.load(), -y / kUnitsPerBlock};
	}

	V3 to_fo4(double x, double y, double z)
	{
		return {x * kUnitsPerBlock, -z * kUnitsPerBlock, (y - g_yOffset.load()) * kUnitsPerBlock};
	}

	uintptr_t player()
	{
		return game::ptr(game::base() + game::addr::Player);
	}

	bool player_pos(game::Vec3 &pos)
	{
		const uintptr_t p = player();
		return p && game::read(p + game::off::Pos, pos);
	}

	// ---------------------------------------------------------------- camera (render thread)
	struct CameraInfo
	{
		uintptr_t camera = 0, vtable = 0;
		int frustumOffset = -1;
		// 0..5: columns 0-2, rows 0-2 of the NiCamera's world rotation. Fallout 4 1.11.240: row 0 looks forward, row 1 is
		// up (every in-game check agreed). The automatic check stays off: on the main menu, or in third person after a
		// load, it locked onto column 2, and Minecraft's camera then turned against Fallout's.
		int forwardAxis = 3;
		float forwardSign = 1.0f;
		int upAxis = 4;
		float upSign = 1.0f;
		bool axisChecked = true;
		int agree = 0;
	} g_cam;

	/// The NiCamera the world renders with: a child (or grandchild) of the player camera's node.
	uintptr_t find_nicamera(uintptr_t node, int depth)
	{
		if (!node || depth > 3)
			return 0;
		if (std::strstr(game::rtti_name(node), "NiCamera@@") != nullptr && std::strstr(game::rtti_name(node), "BSSceneGraph") == nullptr)
			return node;
		uint16_t count = 0;
		const uintptr_t children = game::ptr(node + game::off::Children);
		if (!children || !game::read(node + game::off::ChildCount, count))
			return 0;
		for (uint16_t i = 0; i < std::min<uint16_t>(count, 16); ++i)
			if (const uintptr_t found = find_nicamera(game::ptr(children + 8 * i), depth + 1))
				return found;
		return 0;
	}

	/// NiFrustum {left, right, top, bottom, near, far}: found by its shape rather than a fixed offset.
	int find_frustum(uintptr_t camera)
	{
		for (int o = 0x120; o <= 0x220; o += 4)
		{
			float f[6];
			if (!game::read_raw(camera + o, f, sizeof(f)))
				return -1;
			const bool sym = f[0] < 0 && std::fabs(f[0] + f[1]) < 1e-3f && f[2] > 0 && std::fabs(f[2] + f[3]) < 1e-3f;
			if (sym && f[2] < 5.0f && f[4] > 0.0f && f[5] > f[4] * 10.0f && f[5] < 1e7f)
				return o;
		}
		return -1;
	}

	void axis(const float m[3][4], int which, float out[3])
	{
		for (int i = 0; i < 3; ++i)
			out[i] = which < 3 ? m[i][which] : m[which - 3][i];
	}

	/// Which axis of the camera's world rotation points forward: checked against the player's heading in first person.
	void check_axes(const float m[3][4], bool firstPerson)
	{
		g_recheckAxes = false; // see CameraInfo: the axes are known
		if (g_cam.axisChecked || !firstPerson)
			return;
		const uintptr_t p = player();
		game::Vec3 rot{};
		if (!p || !game::read(p + game::off::Rot, rot) || std::fabs(rot.x) > 0.8f)
			return;
		const float hx = std::sin(rot.z), hy = std::cos(rot.z);
		int best = 0;
		float bestDot = -2.0f, bestSign = 1.0f;
		for (int a = 0; a < 6; ++a)
		{
			float v[3];
			axis(m, a, v);
			const float len = std::sqrt(v[0] * v[0] + v[1] * v[1]);
			if (len < 0.3f)
				continue;
			const float d = (v[0] * hx + v[1] * hy) / len;
			if (std::fabs(d) > bestDot)
			{
				bestDot = std::fabs(d);
				best = a;
				bestSign = d < 0 ? -1.0f : 1.0f;
			}
		}
		if (bestDot < 0.98f)
			return;
		if (best == g_cam.forwardAxis && bestSign == g_cam.forwardSign)
			++g_cam.agree;
		else
		{
			g_cam.forwardAxis = best;
			g_cam.forwardSign = bestSign;
			g_cam.agree = 0;
		}
		if (g_cam.agree < 30)
			return;
		// up: of the same kind (columns or rows), the other axis nearest world +z
		const int first = best < 3 ? 0 : 3;
		float upBest = -2.0f;
		for (int a = first; a < first + 3; ++a)
		{
			if (a == best)
				continue;
			float v[3];
			axis(m, a, v);
			if (std::fabs(v[2]) > upBest)
			{
				upBest = std::fabs(v[2]);
				g_cam.upAxis = a;
				g_cam.upSign = v[2] < 0 ? -1.0f : 1.0f;
			}
		}
		g_cam.axisChecked = true;
		log("camera axes: forward = %s %d (sign %+.0f), up = %s %d (sign %+.0f)", best < 3 ? "column" : "row", best % 3, bestSign,
			g_cam.upAxis < 3 ? "column" : "row", g_cam.upAxis % 3, g_cam.upSign);
	}

	struct Pose
	{
		V3 pos;
		float yaw = 0, pitch = 0, roll = 0, fov = 70, nearBlocks = 0.15f, farBlocks = 5000.0f;
		bool firstPerson = true;
	};

	bool sample_camera(Pose &out)
	{
		const uintptr_t pc = game::ptr(game::base() + game::addr::PlayerCamera);
		const uintptr_t node = pc ? game::ptr(pc + game::off::CameraNode) : 0;
		if (!node)
			return false;
		if (!g_cam.camera || game::ptr(g_cam.camera) != g_cam.vtable)
		{
			g_cam.camera = find_nicamera(node, 0);
			g_cam.vtable = g_cam.camera ? game::ptr(g_cam.camera) : 0;
			g_cam.frustumOffset = g_cam.camera ? find_frustum(g_cam.camera) : -1;
			log("camera: node %p, NiCamera %p (%s), frustum at +0x%X", (void *)node, (void *)g_cam.camera,
				g_cam.camera ? game::rtti_name(g_cam.camera) : "-", g_cam.frustumOffset);
		}
		const uintptr_t src = g_cam.camera ? g_cam.camera : node;
		float m[3][4];
		game::Vec3 pos{};
		if (!game::read(src + game::off::WorldRot, m) || !game::read(src + game::off::WorldPos, pos))
			return false;

		const uintptr_t state = game::ptr(pc + game::off::CameraState);
		const uintptr_t firstPersonState = game::ptr(pc + game::off::CameraStates);
		out.firstPerson = state != 0 && state == firstPersonState;
		check_axes(m, out.firstPerson);

		float f[3], u[3];
		axis(m, g_cam.forwardAxis, f);
		axis(m, g_cam.upAxis, u);
		for (int i = 0; i < 3; ++i)
		{
			f[i] *= g_cam.forwardSign;
			u[i] *= g_cam.upSign;
		}
		// Minecraft: forward = (-sin yaw cos pitch, -sin pitch, cos yaw cos pitch)
		const double mx = f[0], my = f[2], mz = -f[1];
		constexpr double r2d = 180.0 / 3.14159265358979;
		out.yaw = float(std::atan2(-mx, mz) * r2d);
		out.pitch = float(-std::asin(std::clamp(my, -1.0, 1.0)) * r2d);
		// roll: the camera's up against the up a level camera with this yaw and pitch would have
		{
			const double cy = std::cos(out.yaw / r2d), sy = std::sin(out.yaw / r2d), cp = std::cos(out.pitch / r2d), sp = std::sin(out.pitch / r2d);
			const double levelUp[3] = {-sy * sp, cp, cy * sp}; // Minecraft space
			const double up[3] = {u[0], u[2], -u[1]};
			const double fw[3] = {mx, my, mz};
			const double cr[3] = {levelUp[1] * up[2] - levelUp[2] * up[1], levelUp[2] * up[0] - levelUp[0] * up[2], levelUp[0] * up[1] - levelUp[1] * up[0]};
			const double s = cr[0] * fw[0] + cr[1] * fw[1] + cr[2] * fw[2];
			const double c = levelUp[0] * up[0] + levelUp[1] * up[1] + levelUp[2] * up[2];
			out.roll = float(-std::atan2(s, c) * r2d);
		}
		out.pos = to_mc(pos.x, pos.y, pos.z);

		float frustum[6];
		if (g_cam.frustumOffset >= 0 && game::read_raw(g_cam.camera + g_cam.frustumOffset, frustum, sizeof(frustum)))
		{
			const float top = frustum[2] < 5.0f ? frustum[2] : frustum[2] / frustum[4];
			out.fov = float(2.0 * std::atan(top) * r2d);
			out.nearBlocks = frustum[4] / float(kUnitsPerBlock);
			out.farBlocks = frustum[5] / float(kUnitsPerBlock);
		}
		else
		{
			// no frustum: fDefaultWorldFOV is horizontal; Minecraft wants vertical
			float hfov = 90.0f;
			game::read(pc + game::off::DefaultWorldFov, hfov);
			int w = 16, h = 9;
			compositor::backbuffer_size(w, h);
			out.fov = float(2.0 * std::atan(std::tan(hfov / r2d / 2.0) * h / std::max(w, 1)) * r2d);
		}
		return out.fov > 10.0f && out.fov < 150.0f;
	}

	std::mutex g_poseLock;
	Pose g_lastPose; // the newest camera, for shots fired from the main thread

	/// Once per frame on the render thread, just before Minecraft's frame is composited.
	void on_frame()
	{
		const bool live = g_enabled && g_ws.connected() && g_haveOffset && !g_pipboyOpen;
		Pose pose;
		const bool ok = live && sample_camera(pose);
		g_cameraOk = ok;
		compositor::set_active(ok);
		if (!ok)
			return;
		compositor::set_host_planes(pose.nearBlocks, pose.farBlocks);
		compositor::set_host_pose(pose.yaw, pose.pitch, pose.roll, pose.fov, pose.pos.x, pose.pos.y, pose.pos.z);
		{
			std::lock_guard<std::mutex> lock(g_poseLock);
			g_lastPose = pose;
		}

		game::Vec3 feet{}, rot{};
		const uintptr_t p = player();
		if (!p || !game::read(p + game::off::Pos, feet) || !game::read(p + game::off::Rot, rot))
			return;
		// the feet as drawn this frame (the body's root, same moment as the camera's world transform), not the
		// reference's position, which the game moves on its own tick: the two drift apart while running
		{
			const uintptr_t loaded = game::ptr(p + game::off::Loaded3D);
			const uintptr_t root = loaded ? game::ptr(loaded + 0x08) : 0;
			game::Vec3 drawn{};
			if (root && game::read(root + game::off::WorldPos, drawn))
			{
				const float dx = drawn.x - feet.x, dy = drawn.y - feet.y, dz = drawn.z - feet.z;
				if (dx * dx + dy * dy + dz * dz < 200.0f * 200.0f)
					feet = drawn;
			}
		}
		// third person: the camera follows the player, so Minecraft's frame isn't re-projected (that would drag Steve,
		// who moves with the camera, as if he were part of the static world)
		compositor::set_camera_locked(!pose.firstPerson);
		const V3 pl = to_mc(feet.x, feet.y, feet.z);
		const float bodyYaw = float(180.0 + rot.z * 180.0 / 3.14159265358979);
		char msg[400];
		std::snprintf(msg, sizeof(msg),
			"{\"t\":\"cam\",\"f\":%lld,\"p\":[%.4f,%.4f,%.4f],\"r\":[%.3f,%.3f,%.3f],\"fov\":%.3f,\"fp\":%s,\"pl\":[%.4f,%.4f,%.4f],\"h\":%.2f}",
			++g_frame, pose.pos.x, pose.pos.y, pose.pos.z, pose.yaw, pose.pitch, pose.roll, pose.fov, pose.firstPerson ? "true" : "false",
			pl.x, pl.y, pl.z, bodyYaw);
		g_ws.send(msg);
	}

	// ---------------------------------------------------------------- explosions (main thread)
	using LookupFormByID_t = void *(*)(uint32_t id);
	using PlaceAtMe_t = void *(*)(void *vm, uint32_t stackId, void **target, void *form, int32_t count, bool persist, bool disabled, bool deleteWhenAble);
	using MoveRefrToPosition_t = void (*)(void *ref, uint32_t *targetHandle, void *cell, void *worldspace, game::Vec3 *pos, game::Vec3 *rot);
	using GetWorldspace_t = void *(*)(void *ref);
	using SetPosition_t = bool (*)(void *vm, uint32_t stackId, void **ref, float x, float y, float z);
	using DamageValue_t = void (*)(void *vm, uint32_t stackId, void *ref, void *av, float amount);
	using Kill_t = void (*)(void *vm, uint32_t stackId, void *actor, void *killer);
	using SetScale_t = void (*)(void *vm, uint32_t stackId, void *ref, float scale);
	using Disable_t = void (*)(void *vm, uint32_t stackId, void *ref, bool fade);
	using Delete_t = void (*)(void *vm, uint32_t stackId, void *ref);

	template <typename T>
	T fn(uintptr_t offset)
	{
		return reinterpret_cast<T>(game::base() + offset);
	}

	struct Blast
	{
		V3 mc;
		float power;
		std::string source;
	};
	std::mutex g_blastLock;
	std::deque<Blast> g_blasts;
	struct RecentTnt
	{
		V3 at;
		double t;
	};
	std::deque<RecentTnt> g_recentTnt;
	double g_lastNuke = -100.0;
	void *g_marker = nullptr;

	void *vm()
	{
		const uintptr_t gvm = game::ptr(game::base() + game::addr::GameVM);
		return gvm ? reinterpret_cast<void *>(game::ptr(gvm + game::off::VirtualMachine)) : nullptr;
	}

	bool spawn_explosion(uint32_t formId, const V3 &mc)
	{
		void *const form = fn<LookupFormByID_t>(game::addr::LookupFormByID)(formId);
		void *const machine = vm();
		void *pl = reinterpret_cast<void *>(player());
		if (!form || !machine || !pl)
			return false;
		const V3 at = to_fo4(mc.x, mc.y, mc.z);
		__try
		{
			// an invisible marker, moved to the blast, is what the explosion is placed at
			if (g_marker == nullptr)
			{
				void *const xmarker = fn<LookupFormByID_t>(game::addr::LookupFormByID)(kXMarker);
				g_marker = xmarker ? fn<PlaceAtMe_t>(game::addr::PlaceAtMe)(machine, 0, &pl, xmarker, 1, true, false, false) : nullptr;
				if (g_marker == nullptr)
					return false;
			}
			uint32_t handle = *reinterpret_cast<uint32_t *>(game::base() + game::addr::InvalidRefHandle);
			void *const cell = reinterpret_cast<void *>(game::ptr(uintptr_t(pl) + game::off::ParentCell));
			void *const world = fn<GetWorldspace_t>(game::addr::GetWorldspace)(pl);
			game::Vec3 pos{float(at.x), float(at.y), float(at.z)}, rot{0, 0, 0};
			fn<MoveRefrToPosition_t>(game::addr::MoveRefrToPosition)(g_marker, &handle, cell, world, &pos, &rot);
			void *target = g_marker;
			return fn<PlaceAtMe_t>(game::addr::PlaceAtMe)(machine, 0, &target, form, 1, false, false, true) != nullptr;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			g_marker = nullptr;
			return false;
		}
	}

	void run_blasts()
	{
		std::deque<Blast> todo;
		{
			std::lock_guard<std::mutex> lock(g_blastLock);
			todo.swap(g_blasts);
		}
		const double t = now_seconds();
		while (!g_recentTnt.empty() && t - g_recentTnt.front().t > kChainWindow)
			g_recentTnt.pop_front();
		int spawned = 0;
		for (const Blast &b : todo)
		{
			const bool tnt = b.source.find("tnt") != std::string::npos;
			if (tnt)
			{
				g_recentTnt.push_back({b.mc, t});
				// a chain: enough TNT close together right now goes up as one mini nuke at its centre
				V3 c;
				int n = 0;
				for (const RecentTnt &r : g_recentTnt)
				{
					const double dx = r.at.x - b.mc.x, dy = r.at.y - b.mc.y, dz = r.at.z - b.mc.z;
					if (dx * dx + dy * dy + dz * dz < kChainRadius * kChainRadius)
					{
						c.x += r.at.x;
						c.y += r.at.y;
						c.z += r.at.z;
						++n;
					}
				}
				if (n >= kChainCount && t - g_lastNuke > 3.0)
				{
					g_lastNuke = t;
					c = {c.x / n, c.y / n, c.z / n};
					log("TNT chain of %d: mini nuke at %.1f %.1f %.1f -> %s", n, c.x, c.y, c.z, spawn_explosion(kMiniNuke, c) ? "ok" : "FAILED");
					++spawned;
				}
			}
			if (spawned >= kMaxBlastsPerTick)
				continue; // a big chain: Minecraft's own blasts still show; Fallout gets the first few and the nuke
			const bool firework = b.source.find("firework") != std::string::npos;
			const uint32_t form = firework ? kMissile : b.power >= 5.0f ? kNukaGrenade : tnt || b.power >= 3.5f ? kFragMine : kFragGrenade;
			const bool ok = spawn_explosion(form, b.mc);
			++spawned;
			log("explosion %s power %.1f at mc %.1f %.1f %.1f -> form %08X %s", b.source.c_str(), b.power, b.mc.x, b.mc.y, b.mc.z, form, ok ? "ok" : "FAILED");
		}
	}

	// ---------------------------------------------------------------- the ground (main thread)
	struct Ground
	{
		uintptr_t space = 0; // worldspace, or the cell for interiors
		int level = 0;       // Minecraft y of the barrier layer
		int candidate = 0;
		double candidateSince = 0;
		double lastSend = 0;
		std::unordered_set<int64_t> sent;
		uintptr_t checkedSpace = 0; // the worldspace whose terrain was checked against the player's feet
	} g_ground;

	void ground_reset(const game::Vec3 &feet, const char *why)
	{
		// the feet land on y = 64, exactly on a block
		g_yOffset = 64.0 - feet.z / kUnitsPerBlock;
		g_haveOffset = true;
		g_ground.level = 63;
		g_ground.candidate = 63;
		g_ground.sent.clear();
		g_ws.send("{\"t\":\"clear\"}");
		g_ground.lastSend = 0;
		g_solidReset = true;
		log("ground: levelled (%s), yOffset %.3f", why, g_yOffset.load());
	}

	/// The Commonwealth's terrain (tools/heightmap.py, from the user's Fallout4.esm): 33x33 int16 heights per cell,
	/// 128 units apart, in units of 2. Memory-mapped from MCPassthrough_heights.bin next to the plugin.
	struct Heightmap
	{
		const int16_t *cells = nullptr;
		uint32_t world = 0;
		int minX = 0, minY = 0, maxX = -1, maxY = -1;
		bool tried = false;

		void load()
		{
			tried = true;
			wchar_t path[MAX_PATH];
			GetModuleFileNameW(g_module, path, MAX_PATH);
			if (wchar_t *slash = wcsrchr(path, L'\\'))
				wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"MCPassthrough_heights.bin");
			HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
			if (file == INVALID_HANDLE_VALUE)
			{
				log("heightmap: none (flat ground outdoors too)");
				return;
			}
			HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
			CloseHandle(file);
			const auto *view = mapping ? static_cast<const uint8_t *>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0)) : nullptr;
			if (!view || std::memcmp(view, "MCHM", 4) != 0 || *reinterpret_cast<const int32_t *>(view + 4) != 2)
			{
				log("heightmap: unreadable");
				return;
			}
			world = *reinterpret_cast<const uint32_t *>(view + 8);
			minX = *reinterpret_cast<const int32_t *>(view + 12);
			minY = *reinterpret_cast<const int32_t *>(view + 16);
			maxX = *reinterpret_cast<const int32_t *>(view + 20);
			maxY = *reinterpret_cast<const int32_t *>(view + 24);
			cells = reinterpret_cast<const int16_t *>(view + 28);
			log("heightmap: worldspace %08X, cells %d..%d x %d..%d", world, minX, maxX, minY, maxY);
		}

		/// Terrain height (game units) at a point, NaN where there's none.
		float at(double x, double y) const
		{
			const int cx = int(std::floor(x / 4096.0)), cy = int(std::floor(y / 4096.0));
			if (!cells || cx < minX || cx > maxX || cy < minY || cy > maxY)
				return NAN;
			const int16_t *h = cells + (size_t(cy - minY) * size_t(maxX - minX + 1) + size_t(cx - minX)) * 1089;
			const double fx = (x - cx * 4096.0) / 128.0, fy = (y - cy * 4096.0) / 128.0;
			const int c = std::clamp(int(fx), 0, 31), r = std::clamp(int(fy), 0, 31);
			const double u = fx - c, v = fy - r;
			const int16_t a = h[r * 33 + c], b = h[r * 33 + c + 1], d = h[(r + 1) * 33 + c], e = h[(r + 1) * 33 + c + 1];
			if (a == -32768 || b == -32768 || d == -32768 || e == -32768)
				return NAN;
			return float(2.0 * ((a * (1 - u) + b * u) * (1 - v) + (d * (1 - u) + e * u) * v));
		}
	} g_heights;

	void ground_tick()
	{
		game::Vec3 feet{};
		const uintptr_t p = player();
		if (!p || !player_pos(feet))
			return;
		void *const world = fn<GetWorldspace_t>(game::addr::GetWorldspace)(reinterpret_cast<void *>(p));
		const uintptr_t space = world ? uintptr_t(world) : game::ptr(p + game::off::ParentCell);
		if (!g_haveOffset || space != g_ground.space || g_relevel.exchange(false))
		{
			const bool moved = g_haveOffset && space != g_ground.space;
			g_ground.space = space;
			g_recheckAxes = true;
			ground_reset(feet, moved ? "new worldspace or interior" : "start / F8");
		}
		const double t = now_seconds();
		const V3 mc = to_mc(feet.x, feet.y, feet.z);
		if (!g_heights.tried)
			g_heights.load();
		uint32_t worldId = 0;
		const bool terrain = world && g_heights.cells && game::read(uintptr_t(world) + game::off::FormID, worldId) && worldId == g_heights.world;
		if (terrain && g_ground.checkedSpace != space)
		{
			g_ground.checkedSpace = space;
			log("terrain under the player: %.0f, feet at %.0f (game units)", g_heights.at(feet.x, feet.y), feet.z);
		}
		if (terrain)
		{
			// outdoors: every column at the real terrain under it (2 blocks thick, just below the surface), sent once
			if (t - g_ground.lastSend < 0.2)
				return;
			g_ground.lastSend = t;
			const int cx = int(std::floor(mc.x)), cz = int(std::floor(mc.z));
			std::string msg = "{\"t\":\"ground\",\"c\":[";
			int n = 0;
			for (int dx = -kGroundRadius; dx <= kGroundRadius; ++dx)
				for (int dz = -kGroundRadius; dz <= kGroundRadius; ++dz)
				{
					if (dx * dx + dz * dz > kGroundRadius * kGroundRadius)
						continue;
					const int x = cx + dx, z = cz + dz;
					const int64_t key = (int64_t(x) << 32) ^ uint32_t(z);
					if (g_ground.sent.count(key))
						continue;
					const V3 at = to_fo4(x + 0.5, 0.0, z + 0.5);
					const float h = g_heights.at(at.x, at.y);
					if (std::isnan(h))
						continue;
					g_ground.sent.insert(key);
					// the nearest whole block: blocks put down sit at most half a block into or above the ground
					const int top = int(std::floor(to_mc(at.x, at.y, h).y + 0.5)) - 1;
					char col[64];
					std::snprintf(col, sizeof(col), "%s%d,%d,%d,%d", n ? "," : "", x, z, top - 1, top);
					msg += col;
					++n;
				}
			if (n > 0)
			{
				msg += "]}";
				g_ws.send(msg);
			}
			if (g_ground.sent.size() > 40000)
			{
				g_ground.sent.clear();
				g_ws.send("{\"t\":\"clear\"}");
			}
			return;
		}
		// walking up or down a slope: move the layer once the player has stayed at a new height for a moment
		const int want = int(std::floor(mc.y + 0.05)) - 1;
		if (want != g_ground.candidate)
		{
			g_ground.candidate = want;
			g_ground.candidateSince = t;
		}
		if (want != g_ground.level && t - g_ground.candidateSince > 0.6)
		{
			g_ground.level = want;
			g_ground.sent.clear();
			g_ws.send("{\"t\":\"clear\"}");
			g_ground.lastSend = 0; // the new layer right away, or what rests on the old one falls through
		}
		if (t - g_ground.lastSend < 0.2)
			return;
		g_ground.lastSend = t;
		const int cx = int(std::floor(mc.x)), cz = int(std::floor(mc.z));
		std::string msg = "{\"t\":\"ground\",\"c\":[";
		int n = 0;
		for (int dx = -kGroundRadius; dx <= kGroundRadius; ++dx)
			for (int dz = -kGroundRadius; dz <= kGroundRadius; ++dz)
			{
				if (dx * dx + dz * dz > kGroundRadius * kGroundRadius)
					continue;
				const int x = cx + dx, z = cz + dz;
				const int64_t key = (int64_t(x) << 32) ^ uint32_t(z);
				if (!g_ground.sent.insert(key).second)
					continue;
				char col[48];
				std::snprintf(col, sizeof(col), "%s%d,%d,%d,%d", n ? "," : "", x, z, g_ground.level, g_ground.level);
				msg += col;
				++n;
			}
		if (n == 0)
			return;
		msg += "]}";
		g_ws.send(msg);
		if (g_ground.sent.size() > 40000)
			g_ground.sent.clear();
	}

	// ---------------------------------------------------------------- the player's own body (main thread)
	/// The player's meshes we hide. Only the geometry is hidden, never the skeleton above it: a hidden (culled) root
	/// stops the animation graph, and with it running, jumping and the character's movement.
	struct Hidden
	{
		uintptr_t root = 0;
		std::unordered_set<uintptr_t> nodes; // geometry we set the hidden flag on
		double lastScan = 0;
		const char *what;
	};
	Hidden g_body{0, {}, 0, "third-person body"}, g_arms{0, {}, 0, "first-person arms"};

	bool is_geometry(const char *rtti)
	{
		return std::strstr(rtti, "TriShape") || std::strstr(rtti, "BSGeometry");
	}

	/// Every geometry node under root (NiNode children, depth-first), up to a sane limit.
	template <typename F>
	void each_geometry(uintptr_t node, int depth, int &budget, F &&f)
	{
		if (!node || depth > 32 || --budget < 0)
			return;
		const char *rtti = game::rtti_name(node);
		if (is_geometry(rtti))
		{
			f(node);
			return;
		}
		if (!std::strstr(rtti, "Node") && !std::strstr(rtti, "BoneTree"))
			return;
		uint16_t count = 0;
		const uintptr_t children = game::ptr(node + game::off::Children);
		if (!children || !game::read(node + game::off::ChildCount, count))
			return;
		for (uint16_t i = 0; i < std::min<uint16_t>(count, 256); ++i)
			each_geometry(game::ptr(children + 8 * i), depth + 1, budget, f);
	}

	void set_flag(uintptr_t node, bool hidden)
	{
		uint64_t flags = 0;
		if (game::read(node + game::off::AvFlags, flags))
			*reinterpret_cast<uint64_t *>(node + game::off::AvFlags) = hidden ? flags | 1 : flags & ~uint64_t(1);
	}

	/// Hide (or show again) the meshes under one of the player's roots. Showing only touches nodes still found under
	/// the current root, so a model the game rebuilt (new armour) is never written to after it was freed.
	void set_hidden(Hidden &h, uintptr_t root, bool hide)
	{
		const double t = now_seconds();
		if (!h.nodes.empty() && (!hide || root != h.root))
		{
			int budget = 6000;
			if (h.root && h.root == root)
				each_geometry(root, 0, budget, [&](uintptr_t g) { if (h.nodes.count(g)) set_flag(g, false); });
			h.nodes.clear();
		}
		h.root = hide ? root : 0;
		if (!hide || !root || t - h.lastScan < 0.1)
			return;
		h.lastScan = t; // ten times a second: armour and weapons put on later, and anything the game shows again
		const bool first = h.nodes.empty();
		int budget = 6000;
		each_geometry(root, 0, budget, [&](uintptr_t g) {
			uint64_t flags = 0;
			// also ones we hid before: drawing or holstering a weapon shows the arms again
			if (game::read(g + game::off::AvFlags, flags) && !(flags & 1))
			{
				set_flag(g, true);
				h.nodes.insert(g);
			}
		});
		if (first && !h.nodes.empty())
			log("player %s hidden (%zu meshes)", h.what, h.nodes.size());
	}

	/// Steve stands in for the player's body in third person, and his hand for Fallout's arms while Minecraft has
	/// the controls. Everything comes back with F7 or when Minecraft goes away.
	void player_visibility(bool live, bool thirdPerson, bool mcControls)
	{
		const uintptr_t p = player();
		const uintptr_t loaded = p ? game::ptr(p + game::off::Loaded3D) : 0;
		set_hidden(g_body, loaded ? game::ptr(loaded + 0x08) : 0, live && thirdPerson);
		// the first-person model as the game itself gives it (TESObjectREFR vfunc 0x8B, GetActorRootNode(true)), else
		// PlayerCharacter's firstPersonSkeleton field
		uintptr_t arms = 0;
		if (p)
		{
			using Root_t = uintptr_t (*)(uintptr_t ref, bool firstPerson);
			const uintptr_t fnp = game::ptr(game::ptr(p) + 0x8B * 8);
			arms = fnp ? reinterpret_cast<Root_t>(fnp)(p, true) : 0;
			static uintptr_t logged = 1;
			const uintptr_t field = game::ptr(p + game::off::FirstPersonSkeleton);
			if (arms != logged)
			{
				logged = arms;
				log("first-person root: vfunc %p (%s), field %p (%s)", (void *)arms, arms ? game::rtti_name(arms) : "-", (void *)field,
					field ? game::rtti_name(field) : "-");
			}
			if (!arms)
				arms = field;
		}
		set_hidden(g_arms, arms, live && !thirdPerson && mcControls);
	}

	void set_mc_input(bool on);

	bool weapon_drawn()
	{
		const uintptr_t p = player();
		uint32_t flags = 0;
		return p && game::read(p + game::off::ActorStateFlags, flags) && ((flags >> 1) & 7) >= 3;
	}

	/// The Pip-Boy is one of the open menus (UI menuStack, tArray<IMenu *> at +0x190): Minecraft isn't drawn over it at
	/// all (its camera moves close in; the pause menu and others keep Minecraft, behind them).
	bool pipboy_open()
	{
		const uintptr_t ui = game::ptr(game::base() + game::addr::UI);
		const uintptr_t menus = ui ? game::ptr(ui + 0x190) : 0;
		uint32_t count = 0;
		if (!menus || !game::read(ui + 0x190 + 0x10, count))
			return false;
		for (uint32_t i = 0; i < std::min<uint32_t>(count, 64); ++i)
			if (const uintptr_t menu = game::ptr(menus + 8 * i); menu && std::strstr(game::rtti_name(menu), "PipboyMenu"))
				return true;
		return false;
	}

	bool menu_open()
	{
		const uintptr_t ui = game::ptr(game::base() + game::addr::UI);
		uint32_t n = 0;
		return ui && game::read(ui + game::off::NumPauseGame, n) && n > 0;
	}

	/// Who gets the mouse: Minecraft with the weapon holstered (or F6), Fallout with it drawn. Menus get it all, and
	/// hide Minecraft's HUD (the hotbar would sit over the Pip-Boy).
	void controls_tick()
	{
		const bool menu = menu_open();
		g_pipboyOpen = menu && pipboy_open();
		if (menu != g_menuOpen.exchange(menu))
		{
			if (menu)
			{
				g_ws.send("{\"t\":\"key\",\"k\":\"attack\",\"down\":false}");
				g_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":false}");
				g_triggerDown = false;
			}
			g_ws.send(menu ? "{\"t\":\"hud\",\"hidden\":true}" : "{\"t\":\"hud\",\"hidden\":false}");
			log("menu %s", menu ? "open" : "closed");
		}
		if (!menu)
			set_mc_input(g_forceMc ? weapon_drawn() : !weapon_drawn());
	}

	bool third_person()
	{
		const uintptr_t pc = game::ptr(game::base() + game::addr::PlayerCamera);
		const uintptr_t state = pc ? game::ptr(pc + game::off::CameraState) : 0;
		return state != 0 && state != game::ptr(pc + game::off::CameraStates);
	}

	// ---------------------------------------------------------------- input
	void mc_text(const char *text)
	{
		char msg[256];
		std::snprintf(msg, sizeof(msg), "{\"t\":\"cmd\",\"c\":\"title @a actionbar {\\\"text\\\":\\\"%s\\\"}\"}", text);
		g_ws.send(msg);
	}

	void set_mc_input(bool on)
	{
		if (g_mcInput.exchange(on) == on)
			return;
		if (!on)
		{
			g_ws.send("{\"t\":\"key\",\"k\":\"attack\",\"down\":false}");
			g_ws.send("{\"t\":\"key\",\"k\":\"use\",\"down\":false}");
		}
		g_ws.send(on ? "{\"t\":\"hand\",\"show\":true}" : "{\"t\":\"hand\",\"show\":false}");
		log("Minecraft controls %s", on ? "on" : "off");
	}

	bool focused()
	{
		DWORD pid = 0;
		GetWindowThreadProcessId(GetForegroundWindow(), &pid);
		return pid == GetCurrentProcessId();
	}

	void hotkeys()
	{
		static bool was[4] = {};
		const int keys[4] = {VK_F6, VK_F7, VK_F8, VK_F9};
		for (int i = 0; i < 4; ++i)
		{
			const bool down = focused() && (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
			if (down && !was[i])
			{
				if (i == 0)
				{
					g_forceMc = !g_forceMc;
					mc_text(g_forceMc ? "Controls swapped (F6): holstered = Fallout, drawn = Minecraft" : "Controls follow your weapon (F6)");
				}
				else if (i == 1)
				{
					g_enabled = !g_enabled;
					log("passthrough %s", g_enabled ? "on" : "off");
				}
				else if (i == 2)
					g_relevel = true;
				else
				{
					static bool before = true;
					before = !before;
					compositor::set_before_ui(before);
					log("Minecraft drawn %s", before ? "behind Fallout's UI" : "over everything (F9)");
				}
			}
			was[i] = down;
		}
	}


	using GetRawInputData_t = UINT(WINAPI *)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
	GetRawInputData_t g_getRawInputData = nullptr;

	/// Fallout reads mouse and keyboard through Raw Input. In Minecraft mode the buttons Minecraft needs are taken out
	/// of what Fallout sees and sent to Minecraft instead.
	UINT WINAPI hook_GetRawInputData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT headerSize)
	{
		const UINT result = g_getRawInputData(input, command, data, size, headerSize);
		if (command != RID_INPUT || data == nullptr || result == UINT(-1) || !g_ws.connected() || g_menuOpen)
			return result;
		if (!g_mcInput)
		{
			// Fallout controls: Fallout fires as usual; the trigger is also a shot into Minecraft (main thread)
			const RAWINPUT *const raw = static_cast<const RAWINPUT *>(data);
			if (raw->header.dwType == RIM_TYPEMOUSE)
			{
				const USHORT flags = raw->data.mouse.usButtonFlags;
				if (flags & RI_MOUSE_LEFT_BUTTON_DOWN)
				{
					g_triggerDown = true;
					++g_triggerPulls;
				}
				if (flags & RI_MOUSE_LEFT_BUTTON_UP)
					g_triggerDown = false;
			}
			return result;
		}
		RAWINPUT *const raw = static_cast<RAWINPUT *>(data);
		if (raw->header.dwType == RIM_TYPEMOUSE)
		{
			USHORT &flags = raw->data.mouse.usButtonFlags;
			struct
			{
				USHORT downFlag, upFlag;
				const char *key;
			} const buttons[] = {
				{RI_MOUSE_LEFT_BUTTON_DOWN, RI_MOUSE_LEFT_BUTTON_UP, "attack"},
				{RI_MOUSE_RIGHT_BUTTON_DOWN, RI_MOUSE_RIGHT_BUTTON_UP, "use"},
				{RI_MOUSE_MIDDLE_BUTTON_DOWN, RI_MOUSE_MIDDLE_BUTTON_UP, "pick"},
			};
			for (const auto &b : buttons)
			{
				if (flags & (b.downFlag | b.upFlag))
				{
					char msg[80];
					std::snprintf(msg, sizeof(msg), "{\"t\":\"key\",\"k\":\"%s\",\"down\":%s}", b.key, (flags & b.downFlag) ? "true" : "false");
					g_ws.send(msg);
				}
				flags &= USHORT(~(b.downFlag | b.upFlag));
			}
			if (flags & RI_MOUSE_WHEEL)
			{
				const short delta = short(raw->data.mouse.usButtonData);
				char msg[48];
				std::snprintf(msg, sizeof(msg), "{\"t\":\"scroll\",\"d\":%d}", delta > 0 ? 1 : -1);
				g_ws.send(msg);
				flags &= USHORT(~RI_MOUSE_WHEEL);
			}
		}
		else if (raw->header.dwType == RIM_TYPEKEYBOARD)
		{
			RAWKEYBOARD &kb = raw->data.keyboard;
			if (kb.VKey >= '1' && kb.VKey <= '9')
			{
				if (!(kb.Flags & RI_KEY_BREAK))
				{
					char msg[40];
					std::snprintf(msg, sizeof(msg), "{\"t\":\"slot\",\"n\":%d}", kb.VKey - '1');
					g_ws.send(msg);
				}
				kb.VKey = 0xFF;
				kb.MakeCode = 0;
			}
		}
		return result;
	}

	/// Points Fallout4.exe's import of USER32!GetRawInputData at our hook.
	bool hook_raw_input()
	{
		const auto base = reinterpret_cast<uint8_t *>(game::base());
		const auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
		const auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
		const IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
		for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp)
		{
			if (_stricmp(reinterpret_cast<const char *>(base + imp->Name), "USER32.dll") != 0)
				continue;
			auto names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->OriginalFirstThunk);
			auto slots = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->FirstThunk);
			for (; names->u1.AddressOfData; ++names, ++slots)
			{
				if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
					continue;
				const auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
				if (std::strcmp(reinterpret_cast<const char *>(byName->Name), "GetRawInputData") != 0)
					continue;
				DWORD old;
				VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old);
				g_getRawInputData = reinterpret_cast<GetRawInputData_t>(slots->u1.Function);
				slots->u1.Function = reinterpret_cast<ULONGLONG>(&hook_GetRawInputData);
				VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
				return true;
			}
		}
		return false;
	}

	// ---------------------------------------------------------------- health and shots (main thread)
	float g_lastHealthSent = -1.0f;
	double g_lastHealthTime = 0;
	std::mutex g_damageLock;
	std::vector<float> g_damage; // fractions of max health, from Minecraft: negative = Minecraft healed
	float g_prevFrac = -1.0f;    // last tick's health, to see Fallout's own healing

	using AvGet_t = float (*)(void *owner, void *info);
	using AvMod_t = void (*)(void *owner, void *info, float value);

	void *health_av()
	{
		return fn<LookupFormByID_t>(game::addr::LookupFormByID)(kHealthAV);
	}

	void *av_owner()
	{
		const uintptr_t p = player();
		return p ? reinterpret_cast<void *>(p + game::off::ActorValues) : nullptr;
	}

	template <typename T>
	T av_fn(void *owner, int index)
	{
		return reinterpret_cast<T>(game::ptr(game::ptr(uintptr_t(owner)) + 8 * index));
	}

	/// Both games show Fallout's health: Minecraft's hearts follow it, and Minecraft's damage lands here.
	void health_tick()
	{
		void *const owner = av_owner();
		void *const hp = health_av();
		if (!owner || !hp)
			return;
		const float max = av_fn<AvGet_t>(owner, 2)(owner, hp);
		if (max <= 0.0f)
			return;
		std::vector<float> damage;
		{
			std::lock_guard<std::mutex> lock(g_damageLock);
			damage.swap(g_damage);
		}
		float ours = 0.0f; // health changes Minecraft asked for (not Fallout's own healing)
		for (const float f : damage)
		{
			const float cur = av_fn<AvGet_t>(owner, 1)(owner, hp);
			// the damage modifier, like the game's own hits (it can kill); healing never goes past the maximum
			const float delta = f > 0.0f ? -f * max : std::min(-f * max, max - cur);
			if (delta == 0.0f)
				continue;
			if (f > 0.0f) // damage the game's way (it can kill); healing restores the damage modifier
				fn<DamageValue_t>(game::addr::DamageValueNative)(vm(), 0, reinterpret_cast<void *>(player()), hp, -delta);
			else
				av_fn<AvMod_t>(owner, 8)(owner, hp, delta);
			ours += delta / max;
			if (f > 0.0f)
				log("Minecraft hurt the player: %.0f%% of %.0f health", f * 100.0f, max);
		}
		const float frac = std::clamp(av_fn<AvGet_t>(owner, 1)(owner, hp) / max, 0.0f, 1.0f);
		// Fallout healed the player itself (stimpak, food, sleep): Steve eats as much
		if (g_prevFrac >= 0.0f)
		{
			const float healed = frac - g_prevFrac - ours;
			if (healed > 0.005f)
			{
				char msg[64];
				std::snprintf(msg, sizeof(msg), "{\"t\":\"feed\",\"f\":%.4f}", healed);
				g_ws.send(msg);
				log("Fallout healed the player %.0f%%: fed Steve", healed * 100.0f);
			}
		}
		g_prevFrac = frac;
		const double t = now_seconds();
		if (std::fabs(frac - g_lastHealthSent) > 0.004f || t - g_lastHealthTime > 2.0)
		{
			g_lastHealthSent = frac;
			g_lastHealthTime = t;
			char msg[64];
			std::snprintf(msg, sizeof(msg), "{\"t\":\"health\",\"f\":%.4f}", frac);
			g_ws.send(msg);
		}
	}

	int g_pullsHandled = 0;
	double g_triggerSince = 0, g_lastShot = 0;

	void send_shot()
	{
		Pose p;
		{
			std::lock_guard<std::mutex> lock(g_poseLock);
			p = g_lastPose;
		}
		constexpr double d2r = 3.14159265358979 / 180.0;
		const double cy = std::cos(p.yaw * d2r), sy = std::sin(p.yaw * d2r), cp = std::cos(p.pitch * d2r), sp = std::sin(p.pitch * d2r);
		char msg[200];
		std::snprintf(msg, sizeof(msg), "{\"t\":\"shot\",\"o\":[%.3f,%.3f,%.3f],\"d\":[%.4f,%.4f,%.4f],\"dmg\":%.1f}",
			p.pos.x, p.pos.y, p.pos.z, -sy * cp, -sp, cy * cp, kShotDamage);
		g_ws.send(msg);
	}

	/// Fallout's trigger also fires into Minecraft: one shot per pull, then automatic fire while it's held.
	void shots_tick()
	{
		const double t = now_seconds();
		const int pulls = g_triggerPulls;
		if (pulls != g_pullsHandled)
		{
			g_pullsHandled = pulls;
			g_triggerSince = g_lastShot = t;
			send_shot();
			return;
		}
		if (g_triggerDown && t - g_triggerSince > kAutoFireDelay && t - g_lastShot > kAutoFireInterval)
		{
			g_lastShot = t;
			send_shot();
		}
	}

	// ---------------------------------------------------------------- Fallout's people in Minecraft (main thread)
	std::mutex g_hitLock;
	std::vector<std::pair<uint32_t, float>> g_hits; // (form ID, Minecraft damage) from Minecraft weapons and mobs
	double g_lastPeople = 0;

	bool is_actor(uintptr_t ref)
	{
		const char *name = game::rtti_name(ref);
		return std::strstr(name, "?AVActor@@") || std::strstr(name, "?AVCharacter@@");
	}

	/// Actor::IsDead (what Papyrus Actor.IsDead calls: vfunc +0x600 with true). Bodies can still report health.
	bool is_dead(uintptr_t actor)
	{
		using IsDead_t = bool (*)(uintptr_t actor, bool);
		const uintptr_t f = game::ptr(game::ptr(actor) + 0x600);
		return f && reinterpret_cast<IsDead_t>(f)(actor, true);
	}

	/// The living actors in the player's cell go to Minecraft as stand-ins ("peds": [form ID, x, y, z] at their feet)
	/// that Minecraft's weapons and mobs can hit.
	void people_tick()
	{
		const double t = now_seconds();
		if (t - g_lastPeople < 0.25)
			return;
		g_lastPeople = t;
		const uintptr_t me = player();
		const uintptr_t cell = me ? game::ptr(me + game::off::ParentCell) : 0;
		void *const hp = health_av();
		game::Vec3 mine{};
		if (!cell || !hp || !player_pos(mine))
			return;
		const uintptr_t refs = game::ptr(cell + game::off::CellObjects);
		uint32_t count = 0;
		game::read(cell + game::off::CellObjects + 0x10, count);
		std::string msg = "{\"t\":\"peds\",\"p\":[";
		int n = 0;
		for (uint32_t i = 0; refs && i < std::min<uint32_t>(count, 20000); ++i)
		{
			const uintptr_t ref = game::ptr(refs + 8 * i);
			if (!ref || ref == me || !is_actor(ref))
				continue;
			game::Vec3 pos{};
			uint32_t id = 0;
			if (!game::read(ref + game::off::Pos, pos) || !game::read(ref + game::off::FormID, id))
				continue;
			const double dx = (pos.x - mine.x) / kUnitsPerBlock, dy = (pos.y - mine.y) / kUnitsPerBlock;
			if (dx * dx + dy * dy > kPeopleRange * kPeopleRange)
				continue;
			void *const owner = reinterpret_cast<void *>(ref + game::off::ActorValues);
			if (is_dead(ref) || av_fn<AvGet_t>(owner, 1)(owner, hp) <= 0.0f)
				continue; // dead (the cryo pods' bodies too)
			const V3 mc = to_mc(pos.x, pos.y, pos.z);
			char one[96];
			std::snprintf(one, sizeof(one), "%s[%d,%.3f,%.3f,%.3f]", n ? "," : "", int32_t(id), mc.x, mc.y, mc.z);
			msg += one;
			++n;
		}
		msg += "]}";
		g_ws.send(msg);
	}

	/// Minecraft hit one of Fallout's people (Steve's sword or arrows, a zombie, a skeleton): the same health damage
	/// the game's own hits do.
	void run_hits()
	{
		std::vector<std::pair<uint32_t, float>> hits;
		{
			std::lock_guard<std::mutex> lock(g_hitLock);
			hits.swap(g_hits);
		}
		void *const hp = health_av();
		for (const auto &[id, mcDamage] : hits)
		{
			void *const form = fn<LookupFormByID_t>(game::addr::LookupFormByID)(id);
			if (!form || !hp || !is_actor(uintptr_t(form)))
				continue;
			void *const owner = reinterpret_cast<void *>(uintptr_t(form) + game::off::ActorValues);
			const float damage = mcDamage * kMcToFalloutDamage;
			if (is_dead(uintptr_t(form)) || av_fn<AvGet_t>(owner, 1)(owner, hp) <= 0.0f)
				continue; // already dead
			// the game's own damage path: hit reactions, aggression, death
			fn<DamageValue_t>(game::addr::DamageValueNative)(vm(), 0, form, hp, damage);
			const float left = av_fn<AvGet_t>(owner, 1)(owner, hp);
			if (left <= 0.0f)
				fn<Kill_t>(game::addr::KillNative)(vm(), 0, form, reinterpret_cast<void *>(player()));
			log("Minecraft hit %08X for %.0f (left: %.0f)", id, damage, left);
		}
	}

	// ---------------------------------------------------------------- Minecraft's blocks as Fallout collision (main thread)
	// Every block placed in Minecraft gets an invisible solid stand-in in Fallout, so the player can stand on it: a cinder
	// block (40 units, origin at its base) scaled to one block, its meshes hidden, its collision kept.
	constexpr uint32_t kCinderBlock = 0x00001E00;
	constexpr float kCinderScale = 70.0f / 40.0f;
	constexpr size_t kMaxSolidBlocks = 400;
	std::mutex g_blockLock;
	std::vector<std::array<int, 3>> g_blocksSet, g_blocksClear;
	std::unordered_map<int64_t, void *> g_solid; // packed block position -> its stand-in reference
	std::vector<void *> g_unhidden;              // stand-ins whose meshes aren't hidden yet (3D still loading)
	bool g_solidFull = false;

	int64_t block_key(int x, int y, int z)
	{
		return (int64_t(x & 0x3FFFFF) << 42) | (int64_t(y & 0xFFFFF) << 22) | int64_t(z & 0x3FFFFF);
	}

	void solid_remove(void *ref)
	{
		void *const machine = vm();
		fn<Disable_t>(game::addr::DisableNative)(machine, 0, ref, false);
		fn<Delete_t>(game::addr::DeleteNative)(machine, 0, ref);
	}

	void solid_clear_all()
	{
		for (auto &[key, ref] : g_solid)
			solid_remove(ref);
		g_solid.clear();
		g_unhidden.clear();
	}

	void solid_tick()
	{
		if (g_solidReset.exchange(false))
		{
			// new heights (or a new place): the stand-ins go, and Minecraft lists the blocks around the player again
			solid_clear_all();
			g_solidFull = false;
			g_ws.send("{\"t\":\"blocksync\",\"r\":48}");
		}
		std::vector<std::array<int, 3>> set, clear;
		{
			std::lock_guard<std::mutex> lock(g_blockLock);
			set.swap(g_blocksSet);
			clear.swap(g_blocksClear);
		}
		for (const auto &b : clear)
		{
			const auto it = g_solid.find(block_key(b[0], b[1], b[2]));
			if (it == g_solid.end())
				continue;
			solid_remove(it->second);
			g_unhidden.erase(std::remove(g_unhidden.begin(), g_unhidden.end(), it->second), g_unhidden.end());
			g_solid.erase(it);
		}
		void *const cinder = set.empty() ? nullptr : fn<LookupFormByID_t>(game::addr::LookupFormByID)(kCinderBlock);
		void *pl = reinterpret_cast<void *>(player());
		for (const auto &b : set)
		{
			const int64_t key = block_key(b[0], b[1], b[2]);
			if (!cinder || !pl || g_solid.count(key))
				continue;
			if (g_solid.size() >= kMaxSolidBlocks)
			{
				if (!g_solidFull)
					log("solid blocks: at the limit (%zu), more aren't solid in Fallout", kMaxSolidBlocks);
				g_solidFull = true;
				break;
			}
			void *const machine = vm();
			void *const ref = fn<PlaceAtMe_t>(game::addr::PlaceAtMe)(machine, 0, &pl, cinder, 1, false, false, false);
			if (!ref)
				continue;
			fn<SetScale_t>(game::addr::SetScaleNative)(machine, 0, ref, kCinderScale);
			const V3 at = to_fo4(b[0] + 0.5, b[1], b[2] + 0.5); // the block's bottom centre
			struct
			{
				void *ref;
				void *other;
				uint64_t extra;
			} target{ref, nullptr, 0};
			fn<SetPosition_t>(game::addr::SetPositionNative)(machine, 0, reinterpret_cast<void **>(&target), float(at.x), float(at.y), float(at.z));
			g_solid[key] = ref;
			g_unhidden.push_back(ref);
		}
		// hide each stand-in once its 3D has loaded (its collision stays)
		for (auto it = g_unhidden.begin(); it != g_unhidden.end();)
		{
			const uintptr_t loaded = game::ptr(uintptr_t(*it) + game::off::Loaded3D);
			const uintptr_t root = loaded ? game::ptr(loaded + 0x08) : 0;
			if (!root)
			{
				++it;
				continue;
			}
			int budget = 64;
			each_geometry(root, 0, budget, [](uintptr_t g) { set_flag(g, true); });
			it = g_unhidden.erase(it);
		}
	}

	void parse_triples(const std::string &m, const char *key, std::vector<std::array<int, 3>> &out)
	{
		const std::string k = std::string("\"") + key + "\":[";
		size_t at = m.find(k);
		if (at == std::string::npos)
			return;
		const char *p = m.c_str() + at + k.size();
		while (*p && *p != ']')
		{
			int x, y, z, n = 0;
			if (std::sscanf(p, "%d,%d,%d%n", &x, &y, &z, &n) != 3)
				break;
			out.push_back({x, y, z});
			p += n;
			if (*p == ',')
				++p;
		}
	}

	// ---------------------------------------------------------------- the link (main thread)
	std::string json_string(const std::string &m, const char *key)
	{
		const std::string k = std::string("\"") + key + "\":\"";
		const size_t at = m.find(k);
		if (at == std::string::npos)
			return "";
		const size_t end = m.find('"', at + k.size());
		return m.substr(at + k.size(), end == std::string::npos ? std::string::npos : end - at - k.size());
	}

	double json_number(const std::string &m, const char *key, double fallback)
	{
		const std::string k = std::string("\"") + key + "\":";
		const size_t at = m.find(k);
		return at == std::string::npos ? fallback : std::atof(m.c_str() + at + k.size());
	}

	std::mutex g_teleportLock;
	bool g_teleportPending = false;
	V3 g_teleportTo;

	/// An ender pearl (or /tp) moved Steve: move Fallout's player to the same spot, facing the same way.
	void run_teleport()
	{
		V3 to;
		{
			std::lock_guard<std::mutex> lock(g_teleportLock);
			if (!g_teleportPending)
				return;
			g_teleportPending = false;
			to = g_teleportTo;
		}
		void *const pl = reinterpret_cast<void *>(player());
		if (!pl)
			return;
		const V3 at = to_fo4(to.x, to.y + 0.1, to.z); // a little up: the pearl lands on the barrier layer, Fallout's ground may be higher
		game::Vec3 pos{float(at.x), float(at.y), float(at.z)}, rot{};
		game::read(uintptr_t(pl) + game::off::Rot, rot);
		(void)rot;
		// SetPosition, not MoveRefrToPosition: moving the player with the latter is a MoveTo, with its fade and load screen
		// the native takes the reference as a small record, not a bare pointer: it reads +0x08 (another form, used for
		// its error text when set) and +0x10; zero them, or it reads our stack (Fallout4.exe+115DFBC)
		struct
		{
			void *ref;
			void *other;
			uint64_t extra;
		} target{pl, nullptr, 0};
		const bool ok = fn<SetPosition_t>(game::addr::SetPositionNative)(vm(), 0, reinterpret_cast<void **>(&target), pos.x, pos.y, pos.z);
		log("teleport (ender pearl) to mc %.1f %.1f %.1f: %s", to.x, to.y, to.z, ok ? "ok" : "refused");
	}

	void handle_message(const std::string &m)
	{
		if (json_string(m, "t") == "blocks")
		{
			std::lock_guard<std::mutex> lock(g_blockLock);
			parse_triples(m, "set", g_blocksSet);
			parse_triples(m, "clear", g_blocksClear);
			return;
		}
		if (json_string(m, "t") == "mobhit")
		{
			std::lock_guard<std::mutex> lock(g_hitLock);
			g_hits.emplace_back(uint32_t(int32_t(json_number(m, "h", 0.0))), float(json_number(m, "d", 0.0)));
			return;
		}
		if (json_string(m, "t") == "pteleport")
		{
			const size_t at = m.find("\"pos\":[");
			V3 to;
			if (at != std::string::npos && std::sscanf(m.c_str() + at + 7, "%lf,%lf,%lf", &to.x, &to.y, &to.z) == 3)
			{
				std::lock_guard<std::mutex> lock(g_teleportLock);
				g_teleportTo = to;
				g_teleportPending = true;
			}
			return;
		}
		if (json_string(m, "t") == "pdmg" || json_string(m, "t") == "pheal")
		{
			const float d = float(json_number(m, "d", 0.0));
			std::lock_guard<std::mutex> lock(g_damageLock);
			g_damage.push_back(json_string(m, "t") == "pheal" ? -d : d);
			return;
		}
		if (json_string(m, "t") == "explosion")
		{
			const size_t at = m.find("\"pos\":[");
			if (at == std::string::npos)
				return;
			Blast b;
			if (std::sscanf(m.c_str() + at + 7, "%lf,%lf,%lf", &b.mc.x, &b.mc.y, &b.mc.z) != 3)
				return;
			b.power = float(json_number(m, "r", 4.0));
			b.source = json_string(m, "src");
			std::lock_guard<std::mutex> lock(g_blastLock);
			g_blasts.push_back(b);
		}
	}

	int g_generation = -1;
	int g_viewW = 0, g_viewH = 0;

	void link_tick()
	{
		if (g_ws.generation() != g_generation)
		{
			// (re)connected: start the ground over and size Minecraft's window to Fallout's picture
			g_generation = g_ws.generation();
			g_haveOffset = false;
			g_viewW = g_viewH = 0;
			log("Minecraft link up (generation %d)", g_generation);
			g_ws.send("{\"t\":\"hud\",\"hidden\":false}");
			g_ws.send("{\"t\":\"key\",\"k\":\"escape\",\"down\":true}"); // a pause menu would freeze TNT
			g_ws.send(g_mcInput ? "{\"t\":\"hand\",\"show\":true}" : "{\"t\":\"hand\",\"show\":false}");
			g_lastHealthSent = -1.0f;
			g_prevFrac = -1.0f;
			g_menuOpen = false;
			mc_text("Fallout 4 connected: holster your weapon for Minecraft. F6 lock, F7 on/off, F8 re-level");
		}
		int w = 0, h = 0;
		compositor::backbuffer_size(w, h);
		if (w > 0 && h > 0 && (w != g_viewW || h != g_viewH))
		{
			g_viewW = w;
			g_viewH = h;
			char msg[64];
			std::snprintf(msg, sizeof(msg), "{\"t\":\"view\",\"w\":%d,\"h\":%d}", w, h);
			g_ws.send(msg);
			log("Fallout picture %dx%d", w, h);
		}
		std::string message;
		while (g_ws.poll(message))
			handle_message(message);
	}

	/// One step of the main tick, guarded on its own: a fault is logged with the step and the faulting address (relative
	/// to Fallout4.exe when it's in the game) and skips only that step. Repeats are logged once every 5 s.
	int step_filter(EXCEPTION_POINTERS *e, uintptr_t &where)
	{
		where = uintptr_t(e->ExceptionRecord->ExceptionAddress);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	void guarded(const char *name, void (*f)())
	{
		uintptr_t where = 0;
		__try
		{
			f();
		}
		__except (step_filter(GetExceptionInformation(), where))
		{
			static double last = 0;
			const double t = now_seconds();
			if (t - last > 5.0)
			{
				last = t;
				const uintptr_t b = game::base();
				const uintptr_t mine = uintptr_t(g_module);
				if (where >= b && where < b + 0x8000000)
					log("%s: fault at Fallout4.exe+%llX", name, (unsigned long long)(where - b));
				else if (where >= mine && where < mine + 0x100000)
					log("%s: fault at MCPassthrough.dll+%llX", name, (unsigned long long)(where - mine));
				else
					log("%s: fault at %p", name, (void *)where);
			}
		}
	}

	class MainTick : public ITaskDelegate
	{
	public:
		void Run() override
		{
			compositor::try_register(g_module);
			hotkeys();
			__try
			{
				player_visibility(g_enabled && g_ws.connected() && g_cameraOk, third_person(), g_mcInput);
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				g_body.nodes.clear();
				g_arms.nodes.clear();
			}
			if (!g_ws.connected())
				return;
			link_tick();
			if (!g_enabled)
				return;
			__try
			{
				controls_tick();
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
			}
			guarded("ground", ground_tick);
			guarded("explosions", run_blasts);
			guarded("teleport", run_teleport);
			guarded("people", people_tick);
			guarded("blocks", solid_tick);
			guarded("hits", run_hits);
			guarded("health", health_tick);
			if (!g_mcInput && g_cameraOk)
				guarded("shots", shots_tick);
		}
	};

	void open_log()
	{
		wchar_t path[MAX_PATH];
		GetModuleFileNameW(g_module, path, MAX_PATH);
		if (wchar_t *dot = wcsrchr(path, L'.'))
			wcscpy_s(dot, MAX_PATH - (dot - path), L".log");
		g_log = _wfopen(path, L"w");
	}
}

// ---------------------------------------------------------------- F4SE exports
extern "C"
{
	struct F4SEPluginVersionData
	{
		uint32_t dataVersion;
		uint32_t pluginVersion;
		char name[256];
		char author[256];
		uint32_t addressIndependence;
		uint32_t structureIndependence;
		uint32_t compatibleVersions[16];
		uint32_t seVersionRequired;
		uint32_t reservedNonBreaking;
		uint32_t reservedBreaking;
		uint8_t reserved[512];
	};

	__declspec(dllexport) F4SEPluginVersionData F4SEPlugin_Version = {
		1,                // kVersion
		1,                // plugin version
		"MCPassthrough",
		"",
		0,                // hard-coded addresses (F4SE 0.7.9's, for this runtime only)
		1 << 2,           // 1.11.137+ structure layout
		{0x010B0F00, 0},  // RUNTIME_VERSION_1_11_240
		0, 0, 0, {}};

	__declspec(dllexport) bool F4SEPlugin_Load(const F4SEInterface *f4se)
	{
		open_log();
		log("MCPassthrough loading: F4SE %08X, runtime %08X", f4se->f4seVersion, f4se->runtimeVersion);
		if (f4se->runtimeVersion != 0x010B0F00)
		{
			log("unsupported runtime: this build is for 1.11.240 only");
			return false;
		}
		g_tasks = static_cast<F4SETaskInterface *>(f4se->QueryInterface(kInterface_Task));
		if (g_tasks == nullptr || g_tasks->interfaceVersion < 2)
		{
			log("no F4SE task interface (v2)");
			return false;
		}
		compositor::set_frame_callback(on_frame);
		log("raw input hook: %s", hook_raw_input() ? "installed" : "NOT FOUND (Minecraft clicks won't reach Minecraft)");
		g_tasks->AddTaskPermanent(new MainTick());
		g_ws.start("127.0.0.1", kPort);
		log("waiting for Minecraft on 127.0.0.1:%d", kPort);
		return true;
	}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = module;
		DisableThreadLibraryCalls(module);
	}
	else if (reason == DLL_PROCESS_DETACH)
		compositor::unregister(module);
	return TRUE;
}
