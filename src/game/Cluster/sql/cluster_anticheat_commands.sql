-- Cluster / gateway anti-cheat GM command-table rows (World DB).
--
-- Apply to the WORLD database (mangos0). These are the GM commands added by the
-- cluster/gateway anti-cheat suite so that non-console GMs (i.e. logged-in players
-- with the right security) can run them; without a `command` row a subcommand is
-- console-only.
--
-- Kept as an IN-REPO cluster migration and INTENTIONALLY NOT mirrored to the
-- upstream database repo PR: per project rule, cluster-/gateway-specific SQL is
-- fork-only and excluded from the upstream mirror (the upstream command rows live
-- in database Rel22_04_040, which deliberately omits .gateway*/.cluster* and
-- cluster anti-cheat commands). Security values map to this fork's AccountTypes
-- (SEC_GAMEMASTER=2, SEC_ADMINISTRATOR=3).
--
-- Idempotent: removes any prior rows for these exact subcommands first, so a
-- re-run cannot duplicate them regardless of the auto_increment id.

DELETE FROM `command` WHERE `command_text` IN
  ('anticheat gwevent','anticheat migtest','anticheat autoban','anticheat evasion');

INSERT INTO `command` (`id`, `command_text`, `security`, `help_text`) VALUES
(833,'anticheat gwevent',3,'Syntax: .anticheat gwevent $type $severity\r\n\r\nInjects a synthetic gateway anti-cheat event (as if reported by the connection gateway) into your own session, exercising the GW_AC_EVENT channel and the node-side scoring pipeline end to end. $type is an AntiCheatViolationType, $severity a 0-255 weight hint. Administrator-only.'),
(834,'anticheat migtest',3,'Syntax: .anticheat migtest\r\n\r\nSimulates a node-migration arrival on your own character to exercise the Phase 4 migration-seam teleport/state validation without a real cross-node hand-off. Administrator-only.'),
(835,'anticheat autoban',2,'Syntax: .anticheat autoban [reset] [$playername]\r\n\r\nShows the cluster-wide accumulated autoban (kick) score for a player''s account, or resets it with the ''reset'' keyword. The score is read through the shared realm DB so it aggregates kicks across every cluster node.'),
(836,'anticheat evasion',2,'Syntax: .anticheat evasion $playername\r\n\r\nLists accounts sharing the named player''s last-known IP as a best-effort ban-evasion report. Flag-only: it never bans automatically and skips GM accounts.');
