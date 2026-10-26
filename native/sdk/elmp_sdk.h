/* ELMP SDK (C ABI, versioned function table; ARCHITECTURE-V2 §12, requirements C20).
 *
 * A C++ UE4SS mod obtains the table with
 *     auto get = (ElmpGetApiFn)GetProcAddress(GetModuleHandleW(L"main.dll" of ELMP), "ELMP_GetApi");
 *     const ElmpApi_v1* api = (const ElmpApi_v1*)get(ELMP_API_VERSION_1, "MyMod");
 * The caller's identity is its mod name; the mod must ship `Mods/<name>/elmp.json`:
 *     { "name": "MyMod", "elmpApi": 1, "slots": [1001, 1099] }
 * Slots outside the declared range, or ranges overlapping another mod, are refused. A Lua mod with an
 * elmp.json gets the same table as global C functions `ELMP_*` (no Lua source is injected).
 * Every call must happen on the game thread (UE4SS hooks / tick callbacks). Strings returned by the API
 * stay valid until the next call from the same mod. Third-party objects are not replicated online (state()
 * reports "online": "unsupported" for slots that belong to a mod while a session runs).
 */
#ifndef ELMP_SDK_H
#define ELMP_SDK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ELMP_API_VERSION_1 1u

/* result codes: 0 = ok; otherwise see the text from lastError() */
typedef int32_t ElmpResult;

typedef struct ElmpEvent {
    uint64_t seq;
    uint16_t type;  /* elmp events::Type (Death, Respawn, MapSwitchStart, MapTransitDone, ProgressGrant, …) */
    int32_t slot;
    int64_t a, b;
} ElmpEvent;

typedef void (*ElmpEventFn)(const ElmpEvent* ev, void* user);

typedef struct ElmpApi_v1 {
    uint32_t version; /* ELMP_API_VERSION_1 */
    uint32_t size;    /* sizeof(ElmpApi_v1) */
    ElmpResult (*acquire)(int64_t slot);
    ElmpResult (*release)(int64_t slot);
    ElmpResult (*spawn)(int64_t slot, const char* profile_json_or_preset, int32_t skin);
    ElmpResult (*despawn)(int64_t slot, int32_t keep_equipment);
    ElmpResult (*respawn)(int64_t slot);
    ElmpResult (*profileApply)(int64_t slot, const char* profile_json, int32_t exact);
    const char* (*profileRead)(int64_t slot);           /* JSON or NULL */
    ElmpResult (*setInput)(int64_t slot, float x, float y, uint32_t held_commands); /* bit n = ECommandInputTypes n */
    const char* (*state)(int64_t slot);                 /* JSON: alive, hp, mp, pos, anim, online */
    ElmpResult (*setSettings)(const char* key, const char* value);
    ElmpResult (*setPermission)(int64_t slot, const char* gate, int32_t allowed);
    ElmpResult (*setTeam)(int64_t slot, int32_t enemy);
    ElmpResult (*hitRule)(float pvp_damage, int32_t pvp_knockback);
    int32_t (*subscribe)(ElmpEventFn fn, void* user);   /* id > 0; dispatched at the end of the frame pump */
    ElmpResult (*unsubscribe)(int32_t id);
    const char* (*storagePath)(void);                   /* per-mod writable directory (created) */
    void (*log)(int32_t level, const char* text);       /* 0 info, 1 warn, 2 error */
    const char* (*lastError)(void);
} ElmpApi_v1;

typedef const void* (*ElmpGetApiFn)(uint32_t version, const char* mod_name);

#ifdef __cplusplus
}
#endif

#endif /* ELMP_SDK_H */
