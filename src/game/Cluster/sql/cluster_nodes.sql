-- Cluster: live world-node registry.
-- Apply to the LOGIN (realmd) database: realmd reads it to route logins to a
-- live node, and every world node upserts/heartbeats its own row here.

CREATE TABLE IF NOT EXISTS `cluster_nodes` (
  `node_id`        INT UNSIGNED NOT NULL                       COMMENT 'Cluster.NodeID of this world node',
  `host`           VARCHAR(64)  NOT NULL DEFAULT '127.0.0.1'   COMMENT 'address clients/peers reach this node on',
  `port`           INT UNSIGNED NOT NULL DEFAULT 0             COMMENT 'world port (advertised to clients)',
  `peer_port`      INT UNSIGNED NOT NULL DEFAULT 0             COMMENT 'inter-node message-bus port (Phase 2)',
  `capacity`       INT UNSIGNED NOT NULL DEFAULT 0             COMMENT 'max players this node accepts (0=unset)',
  `player_count`   INT UNSIGNED NOT NULL DEFAULT 0             COMMENT 'current active sessions (for load routing)',
  `last_heartbeat` INT UNSIGNED NOT NULL DEFAULT 0             COMMENT 'unix seconds of last heartbeat',
  `status`         ENUM('online','offline','draining') NOT NULL DEFAULT 'offline',
  PRIMARY KEY (`node_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8 COMMENT='Cluster: live world-node registry';
