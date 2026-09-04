DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260728120000');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260728120000');

CREATE TABLE `character_class_deck_draft` (
  `guid` int unsigned NOT NULL,
  `draft_index` tinyint unsigned NOT NULL,
  `earned_level` tinyint unsigned NOT NULL,
  `catalog_version` char(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
  `offer1` int unsigned NOT NULL DEFAULT 0,
  `offer2` int unsigned NOT NULL DEFAULT 0,
  `offer3` int unsigned NOT NULL DEFAULT 0,
  `selected_service` int unsigned NOT NULL DEFAULT 0,
  `state` tinyint unsigned NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `draft_index`),
  KEY `idx_class_deck_state` (`state`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
