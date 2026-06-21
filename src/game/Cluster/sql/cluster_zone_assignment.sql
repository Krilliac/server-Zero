-- Cluster: zone -> node assignment for auto-migration (Phase 5).
-- Apply to the LOGIN (realmd) database. Maps a zone id to the node that owns it;
-- when Cluster.AutoMigrate (and Cluster.EnableMigration) are on, entering a mapped
-- zone migrates the player to that node. Unmapped zones stay on the current node.
-- Example: INSERT INTO cluster_zone_assignment VALUES (1537, 2); -- Ironforge -> node 2

CREATE TABLE IF NOT EXISTS `cluster_zone_assignment` (
  `zone_id` INT UNSIGNED NOT NULL COMMENT 'area/zone id (Player zone)',
  `node_id` INT UNSIGNED NOT NULL COMMENT 'owning cluster node id',
  PRIMARY KEY (`zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='Cluster: zone->node assignment for auto-migration';
