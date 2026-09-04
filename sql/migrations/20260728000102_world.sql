DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260728000102');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260728000102');

ALTER TABLE `coworld_sovereignty_rules`
  ADD COLUMN `claim_channel_seconds` SMALLINT UNSIGNED NOT NULL DEFAULT 30 AFTER `claim_participants`,
  ADD COLUMN `challenge_channel_seconds` SMALLINT UNSIGNED NOT NULL DEFAULT 30 AFTER `claim_channel_seconds`,
  ADD COLUMN `contest_minutes` SMALLINT UNSIGNED NOT NULL DEFAULT 60 AFTER `exclusive_claim_minutes`,
  ADD COLUMN `capture_channel_seconds` SMALLINT UNSIGNED NOT NULL DEFAULT 10 AFTER `contest_minutes`,
  ADD COLUMN `recapture_lock_seconds` SMALLINT UNSIGNED NOT NULL DEFAULT 20 AFTER `capture_channel_seconds`,
  ADD COLUMN `control_tick_seconds` SMALLINT UNSIGNED NOT NULL DEFAULT 5 AFTER `contest_minutes`,
  ADD COLUMN `majority_points` SMALLINT UNSIGNED NOT NULL DEFAULT 5 AFTER `control_tick_seconds`,
  ADD COLUMN `supermajority_points` SMALLINT UNSIGNED NOT NULL DEFAULT 7 AFTER `majority_points`,
  ADD COLUMN `total_control_points` SMALLINT UNSIGNED NOT NULL DEFAULT 10 AFTER `supermajority_points`,
  ADD COLUMN `supply_per_hour` SMALLINT UNSIGNED NOT NULL DEFAULT 2 AFTER `victory_points`,
  ADD COLUMN `supply_cap` INT UNSIGNED NOT NULL DEFAULT 336 AFTER `supply_per_hour`,
  ADD COLUMN `outpost_cost` INT UNSIGNED NOT NULL DEFAULT 96 AFTER `supply_cap`,
  ADD COLUMN `stronghold_cost` INT UNSIGNED NOT NULL DEFAULT 240 AFTER `outpost_cost`,
  ADD COLUMN `outpost_daily_upkeep` INT UNSIGNED NOT NULL DEFAULT 12 AFTER `stronghold_cost`,
  ADD COLUMN `stronghold_daily_upkeep` INT UNSIGNED NOT NULL DEFAULT 36 AFTER `outpost_daily_upkeep`;

ALTER TABLE `coworld_sovereignty_site`
  ADD COLUMN `strategic_role` VARCHAR(16) NOT NULL DEFAULT 'frontier' AFTER `build_district_id`,
  ADD COLUMN `placement_rationale` VARCHAR(320) NOT NULL DEFAULT '' AFTER `strategic_role`;

ALTER TABLE `coworld_sovereignty_polygon`
  DROP PRIMARY KEY,
  ADD COLUMN `polygon_kind` ENUM('territory','build') NOT NULL DEFAULT 'territory' AFTER `site_id`,
  ADD PRIMARY KEY (`site_id`, `polygon_kind`, `vertex_index`);

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
