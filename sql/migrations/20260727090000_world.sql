DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260727090000');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260727090000');

CREATE TABLE IF NOT EXISTS `market_stall_pad` (
  `stall_guid` int(10) unsigned NOT NULL,
  `map` int(10) unsigned NOT NULL,
  `position_x` float NOT NULL,
  `position_y` float NOT NULL,
  `position_z` float NOT NULL,
  `interaction_radius` float NOT NULL DEFAULT 10,
  PRIMARY KEY (`stall_guid`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

DELETE FROM `creature_template` WHERE `entry`=190100;
INSERT INTO `creature_template`
  (`entry`,`patch`,`name`,`subname`,`level_min`,`level_max`,`faction`,`npc_flags`,
   `display_id1`,`unit_class`,`health_multiplier`,`inhabit_type`,`static_flags1`,`flags_extra`)
VALUES
(190100,0,'Market Clerk','Player Market',50,50,29,4097,10754,1,1.25,1,138412038,2),
(190100,1,'Market Clerk','Player Market',50,50,29,4097,10754,1,3,3,138936326,2),
(190100,7,'Market Clerk','Player Market',50,50,120,4097,10754,1,3,3,138936326,2);

DELETE FROM `creature` WHERE `guid` BETWEEN 990001 AND 990006;
INSERT INTO `creature` (`guid`,`id`,`id2`,`id3`,`id4`,`id5`,`map`,`position_x`,`position_y`,`position_z`,`orientation`,`spawntimesecsmin`,`spawntimesecsmax`,`wander_distance`,`health_percent`,`mana_percent`,`movement_type`,`spawn_flags`,`visibility_mod`,`patch_min`,`patch_max`) VALUES
(990001,190100,0,0,0,0,1,1673.0,-4453.0,20.1,0.0,300,300,0,100,100,0,0,0,0,10),
(990002,190100,0,0,0,0,1,1677.0,-4453.0,20.1,0.0,300,300,0,100,100,0,0,0,0,10),
(990003,190100,0,0,0,0,1,1681.0,-4453.0,20.1,0.0,300,300,0,100,100,0,0,0,0,10),
(990004,190100,0,0,0,0,1,1673.0,-4449.0,20.1,3.14,300,300,0,100,100,0,0,0,0,10),
(990005,190100,0,0,0,0,1,1677.0,-4449.0,20.1,3.14,300,300,0,100,100,0,0,0,0,10),
(990006,190100,0,0,0,0,1,1681.0,-4449.0,20.1,3.14,300,300,0,100,100,0,0,0,0,10);

DELETE FROM `market_stall_pad` WHERE `stall_guid` BETWEEN 990001 AND 990006;
INSERT INTO `market_stall_pad` (`stall_guid`,`map`,`position_x`,`position_y`,`position_z`,`interaction_radius`) VALUES
(990001,1,1673.0,-4453.0,20.1,10.0),(990002,1,1677.0,-4453.0,20.1,10.0),
(990003,1,1681.0,-4453.0,20.1,10.0),(990004,1,1673.0,-4449.0,20.1,10.0),
(990005,1,1677.0,-4449.0,20.1,10.0),(990006,1,1681.0,-4449.0,20.1,10.0);

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
