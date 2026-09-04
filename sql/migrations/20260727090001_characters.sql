DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260727090001');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260727090001');

ALTER TABLE `auction`
  ADD COLUMN `market_stall_guid` int(10) unsigned NOT NULL DEFAULT 0 AFTER `deposit`,
  ADD KEY `idx_market_stall_guid` (`market_stall_guid`);

CREATE TABLE IF NOT EXISTS `market_stall` (
  `stall_guid` int(10) unsigned NOT NULL,
  `owner_account_id` int(10) unsigned NOT NULL,
  `owner_guid` int(10) unsigned NOT NULL,
  `lease_expires_at` bigint(20) unsigned NOT NULL,
  `last_active_at` bigint(20) unsigned NOT NULL,
  `state` enum('active','closing') NOT NULL DEFAULT 'active',
  PRIMARY KEY (`stall_guid`),
  UNIQUE KEY `uq_market_stall_owner_account` (`owner_account_id`),
  KEY `idx_market_stall_owner_guid` (`owner_guid`),
  KEY `idx_market_stall_lease` (`state`,`lease_expires_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `market_stall_service` (
  `stall_guid` int(10) unsigned NOT NULL,
  `offer_id` int(10) unsigned NOT NULL,
  `seller_guid` int(10) unsigned NOT NULL,
  `spell_id` int(10) unsigned NOT NULL,
  `price_copper` int(10) unsigned NOT NULL,
  `created_at` bigint(20) unsigned NOT NULL,
  PRIMARY KEY (`stall_guid`,`offer_id`),
  UNIQUE KEY `uq_market_stall_service_spell` (`stall_guid`,`seller_guid`,`spell_id`),
  KEY `idx_market_stall_service_seller` (`seller_guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
