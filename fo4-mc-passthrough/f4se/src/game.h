// What the plugin touches inside Fallout4.exe 1.11.240. Every address is the one F4SE 0.7.9 uses for this runtime
// (ianpatt/f4se, f4se/*.cpp); offsets are relative to the exe's base. Structure offsets come from F4SE's headers.
// Game memory is read through read<T>(), which survives a bad pointer (SEH) instead of crashing the game.
#pragma once
#include <windows.h>
#include <cstdint>
#include <cstring>

namespace game
{
	namespace addr
	{
		constexpr uintptr_t PlayerCamera = 0x030E6E58;       // RelocPtr<PlayerCamera *> g_playerCamera
		constexpr uintptr_t Player = 0x032DD370;             // RelocPtr<PlayerCharacter *> g_player
		constexpr uintptr_t GameVM = 0x030EB308;             // RelocPtr<GameVM *> g_gameVM
		constexpr uintptr_t InvalidRefHandle = 0x030E5280;   // RelocPtr<UInt32> g_invalidRefHandle
		constexpr uintptr_t LookupFormByID = 0x00311B80;     // TESForm *(UInt32 formID)
		constexpr uintptr_t PlaceAtMe = 0x01159E80;          // PlaceAtMe_Native
		constexpr uintptr_t MoveRefrToPosition = 0x01181020; // MoveRefrToPosition
		constexpr uintptr_t GetWorldspace = 0x005172E0;      // TESObjectREFR::GetWorldspace
		constexpr uintptr_t UI = 0x030E8930;                 // RelocPtr<UI *> g_ui
		// Not in F4SE: Papyrus ObjectReference.SetPosition's native (vm, stackId, TESObjectREFR **, x, y, z), found
		// where Fallout4.exe registers it (tools/find_native.py SetPosition). Moves in place, no load screen.
		constexpr uintptr_t SetPositionNative = 0x0115DED0;
		// ObjectReference.DamageValue's native (vm, stackId, TESObjectREFR *, ActorValueInfo *, float): the game's own
		// damage path (hit reactions, death). Actor.Kill's native (vm, stackId, Actor *, Actor *killer).
		constexpr uintptr_t DamageValueNative = 0x011543B0;
		constexpr uintptr_t KillNative = 0x010F9E90;
		// ObjectReference.SetScale (vm, stackId, ref, float), Disable (vm, stackId, ref, bool fade), Delete (vm, stackId, ref)
		constexpr uintptr_t SetScaleNative = 0x0115D4D0;
		constexpr uintptr_t DisableNative = 0x01154870;
		constexpr uintptr_t DeleteNative = 0x011546B0;
	}

	namespace off
	{
		// TESCamera / PlayerCamera
		constexpr uintptr_t CameraNode = 0x20;    // NiNode *
		constexpr uintptr_t CameraState = 0x28;   // TESCameraState *
		constexpr uintptr_t CameraStates = 0xE0;  // TESCameraState *[13]; [0] = first person
		constexpr uintptr_t DefaultWorldFov = 0x168;
		// NiAVObject
		constexpr uintptr_t WorldRot = 0x70;      // NiMatrix43 (3 rows of 4 floats)
		constexpr uintptr_t WorldPos = 0xA0;      // NiPoint3
		constexpr uintptr_t AvFlags = 0x108;      // UInt64; bit 0 = hidden
		// NiNode
		constexpr uintptr_t Children = 0x128;     // NiAVObject ** (NiTArray m_data)
		constexpr uintptr_t ChildCount = 0x132;   // UInt16 m_emptyRunStart
		// TESObjectREFR
		constexpr uintptr_t FormID = 0x14;        // TESForm
		constexpr uintptr_t ParentCell = 0xB8;
		constexpr uintptr_t Rot = 0xC0;           // NiPoint3 radians: x pitch, z heading (0 = +Y, clockwise)
		constexpr uintptr_t Pos = 0xD0;           // NiPoint3
		constexpr uintptr_t Loaded3D = 0xF0;      // LoadedData *: +0x08 is the third-person root node
		constexpr uintptr_t ActorValues = 0x58;   // ActorValueOwner (vtable: 1 GetValue, 2 GetMaximum, 8 damage-modify)
		// PlayerCharacter
		constexpr uintptr_t FirstPersonSkeleton = 0xB78; // NiNode *
		// Actor
		constexpr uintptr_t ActorStateFlags = 0x128 + 0x0C; // ActorState::flags: weapon state = (flags >> 1) & 7, 3+ = drawn
		// UI
		constexpr uintptr_t NumPauseGame = 0x1E0;  // menus that pause the game (Pip-Boy, pause menu, ...)
		// TESObjectCELL
		constexpr uintptr_t CellObjects = 0x70;   // tArray<TESObjectREFR *>: entries +0x00, count +0x10
		// GameVM
		constexpr uintptr_t VirtualMachine = 0xB0;
	}

	inline uintptr_t base()
	{
		static const uintptr_t b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
		return b;
	}

	inline bool read_raw(uintptr_t at, void *out, size_t n)
	{
		if (at < 0x10000)
			return false;
		__try
		{
			std::memcpy(out, reinterpret_cast<const void *>(at), n);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
	}

	template <typename T>
	bool read(uintptr_t at, T &out)
	{
		return read_raw(at, &out, sizeof(T));
	}

	inline uintptr_t ptr(uintptr_t at)
	{
		uintptr_t p = 0;
		return read(at, p) ? p : 0;
	}

	/// The MSVC RTTI class name of a polymorphic object (".?AVNiCamera@@"), or "" if it can't be read.
	inline const char *rtti_name(uintptr_t object)
	{
		const uintptr_t vtbl = ptr(object);
		const uintptr_t col = vtbl ? ptr(vtbl - 8) : 0;
		int32_t typeRva = 0;
		if (!col || !read(col + 12, typeRva) || typeRva <= 0)
			return "";
		static char name[96];
		if (!read_raw(base() + typeRva + 0x10, name, sizeof(name) - 1))
			return "";
		name[sizeof(name) - 1] = 0;
		return name;
	}

	struct Vec3
	{
		float x, y, z;
	};
}
