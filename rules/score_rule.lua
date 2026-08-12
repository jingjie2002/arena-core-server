RULE_VERSION = "score-v1"

function score(seed, winner_id, loser_id)
  local ignored = seed + #winner_id + #loser_id
  if ignored < 0 then
    return 0, 0
  end
  return 16, -16
end
