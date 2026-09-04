DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260728000104');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260728000104');

CREATE TABLE IF NOT EXISTS `coworld_sovereignty_structure` (
  `site_id` VARCHAR(64) NOT NULL,
  `slot_id` VARCHAR(64) NOT NULL,
  `tier_min` TINYINT UNSIGNED NOT NULL,
  `tier_max` TINYINT UNSIGNED NOT NULL,
  `role` VARCHAR(24) NOT NULL,
  `gameobject_guid` INT UNSIGNED NOT NULL,
  `gameobject_entry` INT UNSIGNED NOT NULL,
  `pitch` FLOAT NOT NULL DEFAULT 0,
  PRIMARY KEY (`site_id`, `slot_id`),
  UNIQUE KEY `uq_coworld_sovereignty_structure_guid` (`gameobject_guid`),
  CONSTRAINT `fk_coworld_sovereignty_structure_site` FOREIGN KEY (`site_id`)
    REFERENCES `coworld_sovereignty_site` (`site_id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
