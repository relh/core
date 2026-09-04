DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260728000103');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260728000103');

ALTER TABLE `coworld_territory_claim`
  MODIFY COLUMN `defender_score` INT UNSIGNED NOT NULL DEFAULT 0,
  MODIFY COLUMN `challenger_score` INT UNSIGNED NOT NULL DEFAULT 0,
  ADD COLUMN `contest_ends_at` BIGINT UNSIGNED NULL AFTER `contest_at`,
  ADD COLUMN `infrastructure_tier` TINYINT UNSIGNED NOT NULL DEFAULT 1 AFTER `challenger_score`,
  ADD COLUMN `treasury` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `infrastructure_tier`,
  ADD COLUMN `last_accrual_at` BIGINT UNSIGNED NULL AFTER `treasury`,
  ADD COLUMN `next_upkeep_at` BIGINT UNSIGNED NULL AFTER `last_accrual_at`;

ALTER TABLE `coworld_territory_node_state`
  ADD COLUMN `locked_until` BIGINT UNSIGNED NULL AFTER `owner_guild_id`;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
