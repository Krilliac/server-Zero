-- Phase 7b: cross-node battleground queue (shared-DB source of truth).
-- Apply to the LOGIN/realmd database (same DB as cluster_nodes).
-- Each node publishes its own queued BG players here; the coordinator node
-- reads the union, forms matches, and converges players to a host node via
-- the existing Phase 4 migration (Player::MigrateToNode).
DROP TABLE IF EXISTS `cluster_bg_queue`;
CREATE TABLE `cluster_bg_queue` (
  `player_guid`  INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'character guid (low)',
  `node_id`      INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'node the player is currently on',
  `player_name`  VARCHAR(12)   NOT NULL DEFAULT '',
  `team`         INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'ALLIANCE/HORDE Team value',
  `bg_type_id`   INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'BattleGroundTypeId',
  `bracket_id`   INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'BattleGroundBracketId',
  `as_group`     TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `group_id`     INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'persistent Group::GetId(), 0 if solo',
  `level`        INT UNSIGNED  NOT NULL DEFAULT 0,
  `host_node`    INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'chosen host node once matched',
  `enqueued_at`  INT UNSIGNED  NOT NULL DEFAULT 0 COMMENT 'UNIX_TIMESTAMP at enqueue',
  `status`       ENUM('queued','matched') NOT NULL DEFAULT 'queued',
  PRIMARY KEY (`player_guid`),
  INDEX `idx_match` (`bg_type_id`,`bracket_id`,`status`),
  INDEX `idx_node` (`node_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='Phase 7b cross-node BG queue';
