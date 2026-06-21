-- Cluster: per-character node assignment.
-- Apply to the CHARACTERS database. Records which cluster node a character is
-- assigned to so realmd/the cluster can route the player to the right node
-- after a transfer. 0 = unassigned (single-node behaviour).

ALTER TABLE `characters`
  ADD COLUMN `cluster_node` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'assigned cluster node id (0=unassigned)';
