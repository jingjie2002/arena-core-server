RULE_VERSION = "match-v1"

function allowed_mmr_delta(waited_seconds)
  if waited_seconds < 0 then
    waited_seconds = 0
  end
  local value = 100 + waited_seconds * 25
  if value > 1000 then
    value = 1000
  end
  return value
end
