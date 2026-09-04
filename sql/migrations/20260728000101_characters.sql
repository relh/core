DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260728000101');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260728000101');

CREATE TABLE IF NOT EXISTS `coworld_territory_claim` (
  `site_id` VARCHAR(64) NOT NULL,
  `topology_revision` INT UNSIGNED NOT NULL,
  `revision` INT UNSIGNED NOT NULL DEFAULT 0,
  `state` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `owner_guild_id` INT UNSIGNED NULL,
  `vulnerability_midpoint_utc` SMALLINT UNSIGNED NULL,
  `challenger_guild_id` INT UNSIGNED NULL,
  `contest_at` BIGINT UNSIGNED NULL,
  `defender_score` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `challenger_score` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  `exclusive_guild_id` INT UNSIGNED NULL,
  `exclusive_until` BIGINT UNSIGNED NULL,
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`site_id`),
  KEY `idx_coworld_territory_owner` (`owner_guild_id`),
  KEY `idx_coworld_territory_challenger` (`challenger_guild_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `coworld_territory_node_state` (
  `site_id` VARCHAR(64) NOT NULL,
  `node_index` TINYINT UNSIGNED NOT NULL,
  `owner_guild_id` INT UNSIGNED NULL,
  `claim_revision` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`site_id`, `node_index`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `coworld_territory_event` (
  `event_id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `site_id` VARCHAR(64) NOT NULL,
  `claim_revision` INT UNSIGNED NOT NULL,
  `event_type` VARCHAR(32) NOT NULL,
  `actor_character_guid` INT UNSIGNED NULL,
  `actor_guild_id` INT UNSIGNED NULL,
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`event_id`),
  KEY `idx_coworld_territory_event_site` (`site_id`, `event_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
