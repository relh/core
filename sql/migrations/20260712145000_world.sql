DROP PROCEDURE IF EXISTS add_migration;
delimiter ??
CREATE PROCEDURE `add_migration`()
BEGIN
DECLARE v INT DEFAULT 1;
SET v = (SELECT COUNT(*) FROM `migrations` WHERE `id`='20260712145000');
IF v=0 THEN
INSERT INTO `migrations` VALUES ('20260712145000');

-- Consume Shaman Clearcasting when the next damage spell finishes casting,
-- not when the projectile that granted it later lands.
UPDATE `spell_proc_event`
SET `procEx` = `procEx` | 0x0080000
WHERE `entry`=16246;

END IF;
END??
delimiter ;
CALL add_migration();
DROP PROCEDURE IF EXISTS add_migration;
