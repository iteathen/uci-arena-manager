import type { BotOptions, BotOptionsUpdate } from '../api/types'

export type OutboundBehavior = 'wait' | 'active'
export type SchedulerAuthorityState = 'checking' | 'disabled' | 'paused' | 'active'

export function schedulerAuthorityState(
  enabled: boolean | undefined,
  paused: boolean | undefined,
): SchedulerAuthorityState {
  if (enabled === undefined || paused === undefined) return 'checking'
  if (!enabled) return 'disabled'
  return paused ? 'paused' : 'active'
}

export function outboundBehavior(
  options: Pick<BotOptionsUpdate, 'capacity' | 'scheduler'>,
): OutboundBehavior {
  return options.scheduler.enabled && options.capacity.bot.target_active > 0
    ? 'active'
    : 'wait'
}

export function changeOutboundBehavior(
  options: BotOptionsUpdate,
  behavior: OutboundBehavior,
): BotOptionsUpdate {
  const active = behavior === 'active' && options.capacity.bot.slots > 0
  return {
    ...options,
    capacity: {
      ...options.capacity,
      bot: {
        ...options.capacity.bot,
        target_active: active
          ? Math.max(1, options.capacity.bot.target_active)
          : options.capacity.bot.target_active,
      },
    },
    scheduler: {
      ...options.scheduler,
      enabled: active,
    },
  }
}

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
