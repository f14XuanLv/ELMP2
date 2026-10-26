-- LilyEnemy: ELMP SDK example (third-party Lua mod, slot 2001). An enemy-team Lily that approaches P1 and
-- attacks when close; PvP damage / knockback come from ELMP's hit rules (elmp set pvpDamage / pvpKnockback).
-- Prints ELMP events (deaths, respawns, map switches) polled through ELMP_PollEvents.

local SLOT = 2001
local ATTACK = 1 << 3 -- ECommandInputTypes ATTACK bit
local spawned, tick = false, 0

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
        if ELMP_Acquire(SLOT) ~= 0 or ELMP_Spawn(SLOT, 'default', 2) ~= 0 or ELMP_SetTeam(SLOT, 1) ~= 0 then
            ELMP_Log(1, 'spawn failed: ' .. tostring(ELMP_LastError()))
            return
        end
        spawned = true
        ELMP_Log(0, 'enemy lily spawned in slot ' .. SLOT)
    end
    tick = tick + 1
    local st = ELMP_State(SLOT)
    local y = st and st:match('"pos":%[[-%d.]+,([-%d.]+),')
    if not y then return end
    local dy = p1.Y - tonumber(y)
    local x = 0
    if dy > 120 then x = 1 elseif dy < -120 then x = -1 end
    ELMP_SetInput(SLOT, x, 0, (math.abs(dy) < 220 and tick % 4 == 0) and ATTACK or 0)
    if tick % 10 == 0 then
        local evs = ELMP_PollEvents()
        if evs and evs ~= '[]' then ELMP_Log(0, 'events ' .. evs) end
    end
end

LoopAsync(100, function()
    ExecuteInGameThread(function()
        local ok, err = pcall(step)
        if not ok then ELMP_Log(2, tostring(err)) end
    end)
    return false
end)
