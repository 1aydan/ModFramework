-- Brutal Combat — example mod script.
--
-- Everything a script can reach lives on the global `mod` table. That is deliberate and total:
-- there is no `io`, no `os.execute`, no `require`, no `loadfile`, and no way to reach the engine
-- directly. The runtime binds one object — the mod's UModContext — and nothing else.
--
-- That restraint is the point. Blueprint mods run in-process with full engine access, so the
-- permission system can only *declare* intent. A script sees only what was bound, so
-- "this mod may not touch the filesystem" is actually true here.
--
-- Scripts load after the mod's context exists but before its Blueprint entry point runs, so state
-- set up here is visible to Blueprint.

local BrutalCombat = {}

-- ---------------------------------------------------------------------------------------------
-- Configuration
--
-- config_get takes a default, and its Lua type decides how the value is read. Values come from the
-- mod's Config/ folder, overlaid with anything the player changed — so shipping a new default in a
-- later version never clobbers a player's edit.
-- ---------------------------------------------------------------------------------------------

local DamageMultiplier = mod.config_get("DamageMultiplier", 1.5)
local MaxEnemies       = mod.config_get("MaxExtraEnemies", 4)
local Verbose          = mod.config_get("VerboseLogging", false)

local function trace(message)
    if Verbose then
        mod.log(message)
    end
end

-- ---------------------------------------------------------------------------------------------
-- Requesting a game API
--
-- APIs are addressed by stable id and a version RANGE, not an exact version. Pinning "^1.0.0" means
-- this mod keeps working across every compatible game patch. If the range cannot be satisfied the
-- call returns nil plus a reason — handle it rather than assuming success, because a mod that
-- errors on load is a mod the player uninstalls.
-- ---------------------------------------------------------------------------------------------

local Combat, CombatError = mod.request_api("game.combat", "^1.0.0")
if not Combat then
    mod.warn("Combat API unavailable (" .. tostring(CombatError) .. "); damage scaling is disabled.")
end

-- ---------------------------------------------------------------------------------------------
-- Permissions
--
-- Requesting a permission in mod.json is not the same as being granted it. Check before relying on
-- one; a denied permission is a normal outcome, not an error.
-- ---------------------------------------------------------------------------------------------

local CanModifyGameplay = mod.has_permission("gameplay.modify")
if not CanModifyGameplay then
    mod.warn("gameplay.modify was not granted; running in observe-only mode.")
end

-- ---------------------------------------------------------------------------------------------
-- Events
--
-- Subscriptions return a handle. Hold onto it: unsubscribing is how a mod stops reacting when it is
-- deactivated. The framework also drops every subscription a mod owns when it unloads, so a leaked
-- handle is untidy rather than fatal.
-- ---------------------------------------------------------------------------------------------

local EnemiesKilled = 0

BrutalCombat.OnEnemyKilled = mod.subscribe("Game.EnemyKilled", function()
    EnemiesKilled = EnemiesKilled + 1
    trace("Enemies killed this session: " .. EnemiesKilled)

    -- Persist through the mod's own save namespace. Removing this mod later never disturbs the
    -- base game's save or any other mod's data.
    mod.save(string.format('{"enemiesKilled":%d}', EnemiesKilled))
end)

-- ---------------------------------------------------------------------------------------------
-- Lifecycle hooks
--
-- The runtime calls these by name if they exist. None are required.
-- ---------------------------------------------------------------------------------------------

function OnModActivated()
    mod.log(string.format(
        "Brutal Combat active — damage x%.2f, up to %d extra enemies.",
        DamageMultiplier, MaxEnemies))

    local Saved = mod.load()
    if Saved and #Saved > 0 then
        -- A real mod would parse this properly; kept trivial so the example stays readable.
        trace("Restored save data: " .. Saved)
    end
end

function OnModDeactivated()
    if BrutalCombat.OnEnemyKilled then
        mod.unsubscribe(BrutalCombat.OnEnemyKilled)
        BrutalCombat.OnEnemyKilled = nil
    end

    -- Config changes are in memory until saved. Saving on deactivate means a player who changed a
    -- setting mid-session keeps it, rather than losing it because the mod was unloaded instead of
    -- the game being closed.
    mod.config_save()
    mod.log("Brutal Combat deactivated after " .. EnemiesKilled .. " kills.")
end

return BrutalCombat
