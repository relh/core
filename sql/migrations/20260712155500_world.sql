DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260712155500');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260712155500');

-- First-time Armor/Weaponsmith selection is available at level 40 with 200
-- Blacksmithing only while neither hidden specialization faction is Friendly.
DELETE FROM `conditions` WHERE `condition_entry` IN (19010, 19011, 19012);
INSERT INTO `conditions`
    (`condition_entry`, `type`, `value1`, `value2`, `value3`, `value4`, `flags`)
VALUES
    (19010, 5, 46, 4, 0, 0, 1),
    (19011, 5, 289, 4, 0, 0, 1),
    (19012, -1, 178, 368, 19010, 19011, 0);

-- The selector reputation unlocks the appropriate quest. The quest reward
-- remains the only first-time source of the final specialization spell.
UPDATE `quest_template`
SET `RequiredMinRepFaction`=46, `RequiredMinRepValue`=6000
WHERE `entry` IN (5283, 5301);

UPDATE `quest_template`
SET `RequiredMinRepFaction`=289, `RequiredMinRepValue`=6000
WHERE `entry` IN (5284, 5302);

-- Separate the Alliance and Horde scripts so the selector cast and referral
-- dialogue both match the NPC offering the path.
DELETE FROM `gossip_scripts` WHERE `id` IN (318201, 318202, 318205, 318206);
INSERT INTO `gossip_scripts`
    (`id`, `delay`, `priority`, `command`, `datalong`, `datalong2`, `datalong3`, `datalong4`,
     `target_param1`, `target_param2`, `target_type`, `data_flags`,
     `dataint`, `dataint2`, `dataint3`, `dataint4`,
     `x`, `y`, `z`, `o`, `condition_id`, `comments`)
VALUES
    (318201, 0, 0, 15, 17451, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19012,
     'Krathok Moltenfist - Select Armorsmithing reputation'),
    (318201, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6491, 0, 0, 0, 0, 0, 0, 0, 0,
     'Krathok Moltenfist - Refer player to Okothos'),
    (318202, 0, 0, 15, 17452, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19012,
     'Krathok Moltenfist - Select Weaponsmithing reputation'),
    (318202, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6490, 0, 0, 0, 0, 0, 0, 0, 0,
     'Krathok Moltenfist - Refer player to Borgosh'),
    (318205, 0, 0, 15, 17451, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19012,
     'Myolor Sunderfury - Select Armorsmithing reputation'),
    (318205, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6487, 0, 0, 0, 0, 0, 0, 0, 0,
     'Myolor Sunderfury - Refer player to Grumnus'),
    (318206, 0, 0, 15, 17452, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19012,
     'Myolor Sunderfury - Select Weaponsmithing reputation'),
    (318206, 3, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6486, 0, 0, 0, 0, 0, 0, 0, 0,
     'Myolor Sunderfury - Refer player to Ironus');

UPDATE `gossip_menu`
SET `condition_id`=19012
WHERE (`entry`=3182 AND `text_id`=3938)
   OR (`entry`=3187 AND `text_id`=3953);

UPDATE `gossip_menu_option`
SET `condition_id`=19012,
    `action_script_id`=CASE
        WHEN `menu_id`=3182 AND `id`=0 THEN 318205
        WHEN `menu_id`=3182 AND `id`=1 THEN 318206
        WHEN `menu_id`=3187 AND `id`=0 THEN 318201
        WHEN `menu_id`=3187 AND `id`=1 THEN 318202
    END
WHERE `menu_id` IN (3182, 3187) AND `id` IN (0, 1);

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
