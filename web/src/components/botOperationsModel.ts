import type { BotOptions, BotOptionsUpdate } from '../api/types'

export function needsRatedConfirmation(
  current: BotOptions,
  proposed: BotOptionsUpdate,
) {
  const enablesRatedInbound =
    !current.inbound_challenges.modes.includes('rated') &&
    proposed.inbound_challenges.modes.includes('rated')
  const enablesRatedOutbound =
    current.outbound_challenge.rated !== true &&
    proposed.outbound_challenge.rated === true
  return enablesRatedInbound || enablesRatedOutbound
}

export function needsOutboundConfirmation(
  current: BotOptions,
  proposed: BotOptionsUpdate,
) {
  const previouslyDisabled =
    current.scheduler.enabled !== true || current.capacity.bot.target_active === 0
  const nowEnabled =
    proposed.scheduler.enabled === true && proposed.capacity.bot.target_active > 0
  return previouslyDisabled && nowEnabled
}
