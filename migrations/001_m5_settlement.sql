CREATE TABLE IF NOT EXISTS schema_migrations (
  version VARCHAR(64) PRIMARY KEY,
  applied_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS players (
  player_id VARCHAR(64) PRIMARY KEY,
  score INT NOT NULL DEFAULT 1000,
  version BIGINT UNSIGNED NOT NULL DEFAULT 0,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  CONSTRAINT chk_players_score CHECK (score BETWEEN -1000000000 AND 1000000000)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS matches (
  match_id VARCHAR(64) PRIMARY KEY,
  room_id VARCHAR(64) NOT NULL UNIQUE,
  mode VARCHAR(32) NOT NULL,
  rule_version VARCHAR(32) NOT NULL,
  status VARCHAR(16) NOT NULL,
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  started_at TIMESTAMP(6) NULL,
  ended_at TIMESTAMP(6) NULL,
  INDEX idx_matches_status_created (status, created_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS match_tickets (
  ticket_id VARCHAR(64) PRIMARY KEY,
  player_id VARCHAR(64) NOT NULL,
  mode VARCHAR(32) NOT NULL,
  mmr INT NOT NULL,
  status VARCHAR(16) NOT NULL,
  enqueued_at TIMESTAMP(6) NOT NULL,
  expires_at TIMESTAMP(6) NOT NULL,
  matched_at TIMESTAMP(6) NULL,
  match_id VARCHAR(64) NULL,
  CONSTRAINT fk_match_tickets_player FOREIGN KEY (player_id) REFERENCES players(player_id),
  CONSTRAINT fk_match_tickets_match FOREIGN KEY (match_id) REFERENCES matches(match_id),
  INDEX idx_match_tickets_player_status (player_id, status),
  INDEX idx_match_tickets_mode_status_enqueued (mode, status, enqueued_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS settlements (
  settlement_id VARCHAR(64) PRIMARY KEY,
  match_id VARCHAR(64) NOT NULL UNIQUE,
  fingerprint CHAR(40) NOT NULL,
  rule_version VARCHAR(32) NOT NULL,
  result_json JSON NOT NULL,
  committed_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  CONSTRAINT fk_settlements_match FOREIGN KEY (match_id) REFERENCES matches(match_id),
  INDEX idx_settlements_committed (committed_at)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS match_players (
  match_id VARCHAR(64) NOT NULL,
  player_id VARCHAR(64) NOT NULL,
  seat TINYINT UNSIGNED NOT NULL,
  before_score INT NOT NULL,
  after_score INT NOT NULL,
  outcome VARCHAR(8) NOT NULL,
  PRIMARY KEY (match_id, player_id),
  UNIQUE KEY uq_match_players_seat (match_id, seat),
  CONSTRAINT fk_match_players_match FOREIGN KEY (match_id) REFERENCES matches(match_id),
  CONSTRAINT fk_match_players_player FOREIGN KEY (player_id) REFERENCES players(player_id),
  INDEX idx_match_players_player_match (player_id, match_id)
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS rank_cache_dirty (
  leaderboard VARCHAR(64) NOT NULL,
  player_id VARCHAR(64) NOT NULL,
  target_score INT NOT NULL,
  settlement_id VARCHAR(64) NOT NULL,
  status VARCHAR(16) NOT NULL DEFAULT 'pending',
  attempts INT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(255) NOT NULL DEFAULT '',
  next_retry_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  created_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
  updated_at TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6),
  PRIMARY KEY (leaderboard, player_id),
  CONSTRAINT fk_rank_dirty_player FOREIGN KEY (player_id) REFERENCES players(player_id),
  CONSTRAINT fk_rank_dirty_settlement FOREIGN KEY (settlement_id) REFERENCES settlements(settlement_id),
  INDEX idx_rank_dirty_retry (status, next_retry_at, leaderboard, player_id)
) ENGINE=InnoDB;

INSERT IGNORE INTO schema_migrations(version) VALUES ('001_m5_settlement');
