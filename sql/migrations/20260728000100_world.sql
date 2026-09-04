DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260728000100');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260728000100');

CREATE TABLE IF NOT EXISTS `coworld_sovereignty_rules` (
  `singleton_id` TINYINT UNSIGNED NOT NULL,
  `topology_revision` INT UNSIGNED NOT NULL,
  `claim_rank` TINYINT UNSIGNED NOT NULL,
  `claim_participants` TINYINT UNSIGNED NOT NULL,
  `vulnerability_minutes` SMALLINT UNSIGNED NOT NULL,
  `reinforcement_warning_hours` SMALLINT UNSIGNED NOT NULL,
  `node_points` SMALLINT UNSIGNED NOT NULL,
  `victory_points` SMALLINT UNSIGNED NOT NULL,
  `exclusive_claim_minutes` SMALLINT UNSIGNED NOT NULL,
  `source_digest` CHAR(64) NOT NULL,
  PRIMARY KEY (`singleton_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `coworld_sovereignty_site` (
  `site_id` VARCHAR(64) NOT NULL,
  `topology_revision` INT UNSIGNED NOT NULL,
  `map_id` SMALLINT UNSIGNED NOT NULL,
  `zone_id` INT UNSIGNED NOT NULL,
  `name` VARCHAR(64) NOT NULL,
  `anchor_x` FLOAT NOT NULL,
  `anchor_y` FLOAT NOT NULL,
  `anchor_z` FLOAT NOT NULL,
  `anchor_o` FLOAT NOT NULL,
  `hub_gameobject_guid` INT UNSIGNED NOT NULL,
  `build_district_id` VARCHAR(64) NOT NULL,
  `world_state_base` SMALLINT UNSIGNED NOT NULL,
  `source_digest` CHAR(64) NOT NULL,
  PRIMARY KEY (`site_id`),
  UNIQUE KEY `uq_coworld_sovereignty_hub_guid` (`hub_gameobject_guid`),
  UNIQUE KEY `uq_coworld_sovereignty_build_district` (`build_district_id`),
  UNIQUE KEY `uq_coworld_sovereignty_world_state` (`world_state_base`),
  KEY `idx_coworld_sovereignty_zone` (`map_id`, `zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `coworld_sovereignty_polygon` (
  `site_id` VARCHAR(64) NOT NULL,
  `vertex_index` TINYINT UNSIGNED NOT NULL,
  `x` FLOAT NOT NULL,
  `y` FLOAT NOT NULL,
  PRIMARY KEY (`site_id`, `vertex_index`),
  CONSTRAINT `fk_coworld_sovereignty_polygon_site` FOREIGN KEY (`site_id`)
    REFERENCES `coworld_sovereignty_site` (`site_id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `coworld_sovereignty_node` (
  `site_id` VARCHAR(64) NOT NULL,
  `node_index` TINYINT UNSIGNED NOT NULL,
  `gameobject_guid` INT UNSIGNED NOT NULL,
  `x` FLOAT NOT NULL,
  `y` FLOAT NOT NULL,
  `z` FLOAT NOT NULL,
  `o` FLOAT NOT NULL,
  PRIMARY KEY (`site_id`, `node_index`),
  UNIQUE KEY `uq_coworld_sovereignty_node_guid` (`gameobject_guid`),
  CONSTRAINT `fk_coworld_sovereignty_node_site` FOREIGN KEY (`site_id`)
    REFERENCES `coworld_sovereignty_site` (`site_id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

CREATE TABLE IF NOT EXISTS `coworld_sovereignty_neighbor` (
  `site_id` VARCHAR(64) NOT NULL,
  `neighbor_site_id` VARCHAR(64) NOT NULL,
  PRIMARY KEY (`site_id`, `neighbor_site_id`),
  CONSTRAINT `fk_coworld_sovereignty_neighbor_site` FOREIGN KEY (`site_id`)
    REFERENCES `coworld_sovereignty_site` (`site_id`) ON DELETE CASCADE,
  CONSTRAINT `fk_coworld_sovereignty_neighbor_target` FOREIGN KEY (`neighbor_site_id`)
    REFERENCES `coworld_sovereignty_site` (`site_id`) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
