-- DebugVis labeled-marker pool (Slice 13).
--
-- Reserves a block of generic gameobject_template entries the debug visualizer
-- ring-allocates at runtime. Each labeled marker gets its own entry so the
-- client (which caches GO name/type per entry) shows that marker's verbose
-- tooltip. The model/colour is overridden per-instance via GAMEOBJECT_DISPLAYID,
-- so the template displayId here is only a harmless default.
--
-- Range 305000..305511 sits just above the current MAX(entry) (~300153), so
-- sGOStorage's index array grows only marginally. type=5 = GAMEOBJECT_TYPE_GENERIC
-- (non-interactive). Safe to re-run.

DELETE FROM `gameobject_template` WHERE `entry` BETWEEN 305000 AND 305511;

INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `faction`, `flags`, `size`)
WITH RECURSIVE seq(n) AS (
    SELECT 305000
    UNION ALL
    SELECT n + 1 FROM seq WHERE n < 305511
)
SELECT n, 5, 263, 'DebugVis Marker', 0, 0, 1 FROM seq;
