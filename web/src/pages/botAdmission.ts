import type { BotCandidate } from '../api/types'

export function canProbeBot(candidate: BotCandidate | undefined) {
  return candidate?.state === 'ready_to_probe'
}

export function canEnableBotStartup(
  candidate: BotCandidate | undefined,
  startupObserved: boolean,
  startupInstalled: boolean,
) {
  return startupObserved && candidate?.startup_eligibility === 'ready' && !startupInstalled
}
