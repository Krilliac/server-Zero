-- Cluster: authoritative per-character node assignment (Phase 4).
-- Apply to the CHARACTERS database. A dedicated table (NOT a characters column) so
-- it survives Player::SaveToDB, which rewrites the characters row via DELETE+INSERT
-- and would otherwise reset the assignment. node_id = the cluster node that owns the
-- character; absence of a row = unassigned (loads on any node).
--
-- Cluster anti-cheat Phase 4: the row also carries the departure snapshot
-- (last-known position + wall-clock departure time, written by the source node on
-- hand-off) so the destination node can semantically validate the migration seam:
-- distance(arrival, last-known) must be achievable in the elapsed time. The columns
-- are nullable; a NULL/absent snapshot (first migration / legacy row) makes node B
-- skip the seam check (back-compat, never false-positive on missing reference data).

CREATE TABLE IF NOT EXISTS `cluster_character_node` (
  `guid`              INT UNSIGNED NOT NULL COMMENT 'character low-guid',
  `node_id`           INT UNSIGNED NOT NULL COMMENT 'owning cluster node id',
  `last_map`          INT UNSIGNED NULL COMMENT 'AC Phase4: map at departure (source node)',
  `last_x`            FLOAT NULL COMMENT 'AC Phase4: x at departure',
  `last_y`            FLOAT NULL COMMENT 'AC Phase4: y at departure',
  `last_z`            FLOAT NULL COMMENT 'AC Phase4: z at departure',
  `departed_unixtime` INT UNSIGNED NULL COMMENT 'AC Phase4: shared-DB UNIX_TIMESTAMP() at departure',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='Cluster: per-character node assignment';

-- Backfill-safe migration for an existing table: add the AC Phase-4 departure
-- snapshot columns if they are not already present (MariaDB supports IF NOT EXISTS
-- on ADD COLUMN). All nullable/defaulted so an un-migrated DB keeps working.
ALTER TABLE `cluster_character_node`
  ADD COLUMN IF NOT EXISTS `last_map`          INT UNSIGNED NULL COMMENT 'AC Phase4: map at departure (source node)',
  ADD COLUMN IF NOT EXISTS `last_x`            FLOAT NULL COMMENT 'AC Phase4: x at departure',
  ADD COLUMN IF NOT EXISTS `last_y`            FLOAT NULL COMMENT 'AC Phase4: y at departure',
  ADD COLUMN IF NOT EXISTS `last_z`            FLOAT NULL COMMENT 'AC Phase4: z at departure',
  ADD COLUMN IF NOT EXISTS `departed_unixtime` INT UNSIGNED NULL COMMENT 'AC Phase4: shared-DB UNIX_TIMESTAMP() at departure';
