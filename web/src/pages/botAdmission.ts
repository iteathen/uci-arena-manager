import type { BotCandidate } from '../api/types'

export const BOT_STATUS_REFRESH_MILLISECONDS = 5_000

export function botStatusRefreshInterval(installed: boolean) {
  return installed ? BOT_STATUS_REFRESH_MILLISECONDS : false
}

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
