DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260728140001');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260728140001');

ALTER TABLE `market_stall`
  ADD COLUMN `category` tinyint(3) unsigned NOT NULL DEFAULT 0 AFTER `owner_guid`,
  ADD COLUMN `claimed_at` bigint(20) unsigned NOT NULL DEFAULT 0 AFTER `category`;

UPDATE `market_stall` SET `claimed_at`=`last_active_at` WHERE `claimed_at`=0;
UPDATE `market_stall` SET `state`='closing' WHERE `stall_guid` BETWEEN 990001 AND 990006;

ALTER TABLE `market_stall_service`
  DROP KEY `uq_market_stall_service_spell`,
  ADD COLUMN `service_kind` enum('enchant','craft_advert') NOT NULL DEFAULT 'enchant' AFTER `offer_id`,
  ADD UNIQUE KEY `uq_market_stall_service_spell`
    (`stall_guid`,`service_kind`,`seller_guid`,`spell_id`);

CREATE TABLE IF NOT EXISTS `market_stall_claim_cooldown` (
  `owner_account_id` int(10) unsigned NOT NULL,
  `eligible_at` bigint(20) unsigned NOT NULL,
  `reason` varchar(32) NOT NULL,
  PRIMARY KEY (`owner_account_id`),
  KEY `idx_market_stall_claim_eligible` (`eligible_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
