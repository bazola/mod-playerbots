-- Who wrote each anchor, so writers can share playerbots_bot_anchor without erasing each other's rows:
-- 'meeting' (AiPlayerbot.PersistentProgression.AnchorOnMeeting), a realm-side writer's own name, or
-- 'manual' for anything else. The module reads every row the same way; only the writers care.
ALTER TABLE `playerbots_bot_anchor`
  ADD COLUMN `source` VARCHAR(16) NOT NULL DEFAULT 'manual' COMMENT 'who wrote the row' AFTER `anchor`;
