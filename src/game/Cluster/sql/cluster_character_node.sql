-- Cluster: authoritative per-character node assignment (Phase 4).
-- Apply to the CHARACTERS database. A dedicated table (NOT a characters column) so
-- it survives Player::SaveToDB, which rewrites the characters row via DELETE+INSERT
-- and would otherwise reset the assignment. node_id = the cluster node that owns the
-- character; absence of a row = unassigned (loads on any node).

CREATE TABLE IF NOT EXISTS `cluster_character_node` (
  `guid`    INT UNSIGNED NOT NULL COMMENT 'character low-guid',
  `node_id` INT UNSIGNED NOT NULL COMMENT 'owning cluster node id',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='Cluster: per-character node assignment';
