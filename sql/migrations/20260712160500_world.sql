DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260712160500');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260712160500');

-- Engineering: one prior specialization quest, 200 skill, no active focus,
-- and patch 1.10 or later.
-- Leatherworking: one prior specialization quest, 225 skill, no active focus,
-- and patch 1.10 or later.
DELETE FROM `conditions` WHERE `condition_entry` BETWEEN 19100 AND 19124;
INSERT INTO `conditions`
    (`condition_entry`, `type`, `value1`, `value2`, `value3`, `value4`, `flags`)
VALUES
    (19100, 8, 3639, 0, 0, 0, 0),
    (19101, 8, 3641, 0, 0, 0, 0),
    (19102, 8, 3643, 0, 0, 0, 0),
    (19103, -2, 19100, 19101, 19102, 0, 0),
    (19104, 17, 20219, 1, 0, 0, 0),
    (19105, 17, 20222, 1, 0, 0, 0),
    (19106, -1, 19104, 19105, 0, 0, 0),
    (19107, -1, 19103, 393, 19106, 4018, 0),

    (19110, 8, 5141, 0, 0, 0, 0),
    (19111, 8, 5145, 0, 0, 0, 0),
    (19112, 8, 5144, 0, 0, 0, 0),
    (19113, 8, 5146, 0, 0, 0, 0),
    (19114, 8, 5143, 0, 0, 0, 0),
    (19115, 8, 5148, 0, 0, 0, 0),
    (19116, -2, 19110, 19111, 19112, 19113, 0),
    (19117, -2, 19114, 19115, 0, 0, 0),
    (19118, -2, 19116, 19117, 0, 0, 0),
    (19119, 17, 10656, 1, 0, 0, 0),
    (19120, 17, 10658, 1, 0, 0, 0),
    (19121, 17, 10660, 1, 0, 0, 0),
    (19122, -1, 19119, 19120, 19121, 0, 0),
    (19123, 7, 165, 225, 0, 0, 0),
    (19124, -1, 19118, 19123, 19122, 4018, 0);

-- Enable the book's gossip interaction from patch 1.10 onward.
UPDATE `gameobject_template`
SET `flags`=0, `data0`=0
WHERE `entry`=177226;

UPDATE `gossip_menu`
SET `condition_id`=4018
WHERE `entry`=7058 AND `text_id`=8321;

-- These wrappers teach the actual specialization spells recorded in 5875
-- Spell.dbc. They never reopen or reward a profession quest.
DELETE FROM `gossip_scripts` WHERE `id` IN (2861, 2862, 2863, 2864, 2865);
INSERT INTO `gossip_scripts`
    (`id`, `delay`, `priority`, `command`, `datalong`, `datalong2`, `datalong3`, `datalong4`,
     `target_param1`, `target_param2`, `target_type`, `data_flags`,
     `dataint`, `dataint2`, `dataint3`, `dataint4`,
     `x`, `y`, `z`, `o`, `condition_id`, `comments`)
VALUES
    (2861, 0, 0, 15, 20221, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 19107,
     'Soothsaying for Dummies - Teach Goblin Engineering'),
    (2862, 0, 0, 15, 20220, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 19107,
     'Soothsaying for Dummies - Teach Gnomish Engineering'),
    (2863, 0, 0, 15, 10657, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 19124,
     'Soothsaying for Dummies - Teach Dragonscale Leatherworking'),
    (2864, 0, 0, 15, 10659, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 19124,
     'Soothsaying for Dummies - Teach Elemental Leatherworking'),
    (2865, 0, 0, 15, 10661, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 19124,
     'Soothsaying for Dummies - Teach Tribal Leatherworking');

DELETE FROM `gossip_menu_option`
WHERE `menu_id`=7058 AND `id` BETWEEN 1 AND 5;
INSERT INTO `gossip_menu_option`
    (`menu_id`, `id`, `option_icon`, `option_text`, `option_broadcast_text`,
     `option_id`, `npc_option_npcflag`, `action_menu_id`, `action_poi_id`, `action_script_id`,
     `box_coded`, `box_money`, `box_text`, `box_broadcast_text`, `condition_id`)
VALUES
    (7058, 1, 0, 'I am 100% confident that I wish to learn in the ways of goblin engineering.',
     11876, 1, 1, -1, 0, 2861, 0, 0, '', 0, 19107),
    (7058, 2, 0, 'I am 100% confident that I wish to learn in the ways of gnomish engineering.',
     11878, 1, 1, -1, 0, 2862, 0, 0, '', 0, 19107),
    (7058, 3, 0, 'I am absolutely certain that I want to learn dragonscale leatherworking.',
     11889, 1, 1, -1, 0, 2863, 0, 0, '', 0, 19124),
    (7058, 4, 0, 'I am absolutely certain that I want to learn elemental leatherworking.',
     11890, 1, 1, -1, 0, 2864, 0, 0, '', 0, 19124),
    (7058, 5, 0, 'I am absolutely certain that I want to learn tribal leatherworking.',
     11891, 1, 1, -1, 0, 2865, 0, 0, '', 0, 19124);

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
