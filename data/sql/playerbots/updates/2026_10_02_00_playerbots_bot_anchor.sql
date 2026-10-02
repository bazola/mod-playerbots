-- AiPlayerbot.PersistentProgression = 2: the random bots whose progression is kept, and the character
-- each one is anchored to (its level is what AiPlayerbot.PersistentProgression.FollowGap follows).
-- The module only reads this table; filling it is up to the realm.
CREATE TABLE IF NOT EXISTS `playerbots_bot_anchor` (
  `bot` INT UNSIGNED NOT NULL COMMENT 'random bot character guid',
  `anchor` INT UNSIGNED NOT NULL COMMENT 'character guid the bot is anchored to',
  `updated_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (`bot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
