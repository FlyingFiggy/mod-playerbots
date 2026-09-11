USE acore_world;

-- Restore Mirelow (14424) rare-spawn pool membership from the existing creature spawns.
-- Current AzerothCore uses creature.id for the primary creature template entry.
-- Safe to re-run; it does not create or delete creature spawns.
INSERT INTO pool_creature (`guid`, `pool_entry`, `chance`, `description`)
SELECT c.`guid`, 1072, 0, 'Mirelow (14424)'
FROM creature c
WHERE c.`id` = 14424
ON DUPLICATE KEY UPDATE
    `pool_entry` = VALUES(`pool_entry`),
    `chance` = VALUES(`chance`),
    `description` = VALUES(`description`);

-- Current AzerothCore registers this script for spell 28819 but the base
-- spell_script_names table does not bind it, which causes a startup warning.
INSERT IGNORE INTO spell_script_names (`spell_id`, `ScriptName`)
VALUES (28819, 'spell_gen_submerge_visual');

-- Verification
SELECT COUNT(*) AS MirelowPoolMembers FROM pool_creature WHERE pool_entry = 1072;
SELECT * FROM spell_script_names WHERE spell_id = 28819;
