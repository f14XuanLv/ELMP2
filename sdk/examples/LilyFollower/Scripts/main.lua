-- LilyFollower: ELMP SDK example (third-party Lua mod, slot 1001). Uses only the C functions ELMP injects
-- into mods that ship an elmp.json (ELMP_*); ELMP itself runs no Lua. Spawns a friendly Lily that walks
-- after P1 and jumps when P1 is clearly above it.

local SLOT = 1001
local JUMP = 1 << 1 -- ECommandInputTypes JUMP bit (see elmp_sdk.h setInput)
local spawned = false

local function p1_location()
    local gs = StaticFindObject('/Script/Engine.Default__GameplayStatics')
    local p1 = gs:GetPlayerPawn(FindFirstOf('GameInstance'), 0)
    if not p1 or not p1:IsValid() then return nil end
    return p1:K2_GetActorLocation()
end

local function step()
    local p1 = p1_location()
    if not p1 then spawned = false return end
    if not spawned then
        if ELMP_Acquire(SLOT) ~= 0 or ELMP_Spawn(SLOT, 'full', 1) ~= 0 then
            ELMP_Log(1, 'spawn failed: ' .. tostring(ELMP_LastError()))
            return
        end
        spawned = true
        ELMP_Log(0, 'follower spawned in slot ' .. SLOT)
    end
    local st = ELMP_State(SLOT)
    if not st then return end
    local y, z = st:match('"pos":%[[-%d.]+,([-%d.]+),([-%d.]+)%]') -- (`st and st:match()` would keep one value)
    if not y then return end
    local dy, dz = p1.Y - tonumber(y), p1.Z - tonumber(z)
    local x = 0
    if dy > 150 then x = 1 elseif dy < -150 then x = -1 end
    ELMP_SetInput(SLOT, x, 0, dz > 200 and JUMP or 0)
end

LoopAsync(100, function()
    ExecuteInGameThread(function()
        local ok, err = pcall(step)
        if not ok then ELMP_Log(2, tostring(err)) end
    end)
    return false
end)
