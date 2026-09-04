DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260712160000');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260712160000');

-- Permanent quest-history conditions for both faction variants.
DELETE FROM `conditions`
WHERE `condition_entry` IN (19020, 19021, 19022, 19023, 19024, 19025, 19026, 19027, 19028);
INSERT INTO `conditions`
    (`condition_entry`, `type`, `value1`, `value2`, `value3`, `value4`, `flags`)
VALUES
    (19020, 8, 5283, 0, 0, 0, 0),
    (19021, 8, 5301, 0, 0, 0, 0),
    (19022, -2, 19020, 19021, 0, 0, 0),
    (19023, 8, 5284, 0, 0, 0, 0),
    (19024, 8, 5302, 0, 0, 0, 0),
    (19025, -2, 19023, 19024, 0, 0, 0),
    (19026, -1, 178, 368, 1356, 4018, 0),
    (19027, -1, 19026, 19022, 0, 0, 0),
    (19028, -1, 19026, 19025, 0, 0, 0);

-- The veteran interaction restores the earned focus directly. It does not
-- reopen a quest or modify the first-time hidden-reputation selection.
DELETE FROM `gossip_scripts` WHERE `id` IN (318203, 318204);
INSERT INTO `gossip_scripts`
    (`id`, `delay`, `priority`, `command`, `datalong`, `datalong2`, `datalong3`, `datalong4`,
     `target_param1`, `target_param2`, `target_type`, `data_flags`,
     `dataint`, `dataint2`, `dataint3`, `dataint4`,
     `x`, `y`, `z`, `o`, `condition_id`, `comments`)
VALUES
    (318203, 0, 0, 15, 9790, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19027,
     'Retake the Hammer Once More - Teach Artisan Armorsmith'),
    (318204, 0, 0, 15, 9789, 6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 19028,
     'Retake the Hammer Once More - Teach Artisan Weaponsmith');

DELETE FROM `gossip_menu_option`
WHERE `menu_id` IN (3182, 3187) AND `id` IN (2, 3);
INSERT INTO `gossip_menu_option`
    (`menu_id`, `id`, `option_icon`, `option_text`, `option_broadcast_text`,
     `option_id`, `npc_option_npcflag`, `action_menu_id`, `action_poi_id`, `action_script_id`,
     `box_coded`, `box_money`, `box_text`, `box_broadcast_text`, `condition_id`)
VALUES
    (3182, 2, 0,
     'Myolor, I was once an armorsmith and wish to retake the hammer once more! Teach me the way of the armorsmith.',
     8892, 1, 3, -1, 0, 318203, 0, 0, '', 0, 19027),
    (3182, 3, 0,
     'Myolor, I was once a weaponsmith and wish to retake the hammer once more! Teach me the way of the weaponsmith!',
     8893, 1, 3, -1, 0, 318204, 0, 0, '', 0, 19028),
    (3187, 2, 0,
     'Krathok, I was once an armorsmith and wish to retake the hammer once more! Teach me the way of the armorsmith.',
     8894, 1, 3, -1, 0, 318203, 0, 0, '', 0, 19027),
    (3187, 3, 0,
     'Krathok, I was once a weaponsmith and wish to retake the hammer once more! Teach me the way of the weaponsmith.',
     8895, 1, 3, -1, 0, 318204, 0, 0, '', 0, 19028);

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
