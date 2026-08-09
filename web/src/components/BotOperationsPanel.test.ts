import { describe, expect, it } from 'vitest'
import type { BotOptions, BotOptionsUpdate } from '../api/types'
import {
  changeOutboundBehavior,
  needsOutboundConfirmation,
  needsRatedConfirmation,
  outboundBehavior,
  schedulerAuthorityState,
} from './botOperationsModel'

function fixture(): BotOptions {
  return {
    schema: 'uci_arena_lichess_manager_options_v1',
    capacity: {
      total_slots: 2,
      startup_parallelism: 2,
      human: { slots: 1, allow_borrowing: false, accept_public: true },
      bot: { slots: 1, target_active: 0 },
      evidence: { status: 'qualified', qualified: true, maximum_concurrency: 2 },
    },
    inbound_challenges: {
      accept_bots: true,
      min_base: 600,
      max_base: 1800,
      min_increment: 0,
      max_increment: 10,
      speeds: ['rapid'],
      modes: ['casual'],
    },
    outbound_challenge: {
      rated: false,
      color: 'random',
      clock_limit: 600,
      clock_increment: 2,
    },
    scheduler: {
      enabled: false,
      discovery_interval_seconds: 60,
      challenge_interval_seconds: 5,
      reservation_retry_seconds: 30,
      challenge_timeout_seconds: 25,
    },
    opponent_selection: {
      max_games_per_opponent_per_day: 1,
      timeout_cooldown_minutes: 30,
      decline_cooldown_minutes: 360,
      rating_target: 2200,
      rating_spread: 600,
    },
  }
}

function editable(options: BotOptions): BotOptionsUpdate {
  return {
    capacity: {
      total_slots: options.capacity.total_slots,
      startup_parallelism: options.capacity.startup_parallelism,
      human: { ...options.capacity.human },
      bot: { ...options.capacity.bot },
    },
    inbound_challenges: {
      ...options.inbound_challenges,
      speeds: [...options.inbound_challenges.speeds],
      modes: [...options.inbound_challenges.modes],
    },
    outbound_challenge: { ...options.outbound_challenge },
    scheduler: { ...options.scheduler },
    opponent_selection: { ...options.opponent_selection },
  }
}

describe('Lichess option confirmation policy', () => {
  it('requires rated confirmation for either newly rated inbound or outbound play', () => {
    const current = fixture()
    const inbound = editable(current)
    inbound.inbound_challenges.modes.push('rated')
    expect(needsRatedConfirmation(current, inbound)).toBe(true)

    const outbound = editable(current)
    outbound.outbound_challenge.rated = true
    expect(needsRatedConfirmation(current, outbound)).toBe(true)

    const alreadyRated = fixture()
    alreadyRated.inbound_challenges.modes.push('rated')
    expect(needsRatedConfirmation(alreadyRated, editable(alreadyRated))).toBe(false)
  })

  it('requires outbound confirmation only when scheduling gains active bot capacity', () => {
    const current = fixture()
    const proposed = editable(current)
    proposed.scheduler.enabled = true
    expect(needsOutboundConfirmation(current, proposed)).toBe(false)

    proposed.capacity.bot.target_active = 1
    expect(needsOutboundConfirmation(current, proposed)).toBe(true)

    const alreadyEnabled = fixture()
    alreadyEnabled.scheduler.enabled = true
    alreadyEnabled.capacity.bot.target_active = 1
    expect(needsOutboundConfirmation(alreadyEnabled, editable(alreadyEnabled))).toBe(false)
  })

  it('projects active play only when the scheduler has a positive outbound target', () => {
    const proposed = editable(fixture())
    proposed.scheduler.enabled = true
    proposed.capacity.bot.target_active = 0
    expect(outboundBehavior(proposed)).toBe('wait')

    const active = changeOutboundBehavior(proposed, 'active')
    expect(active.scheduler.enabled).toBe(true)
    expect(active.capacity.bot.target_active).toBe(1)
    expect(outboundBehavior(active)).toBe('active')

    const waiting = changeOutboundBehavior(active, 'wait')
    expect(waiting.scheduler.enabled).toBe(false)
    expect(outboundBehavior(waiting)).toBe('wait')
  })

  it('cannot project active play without a bot slot', () => {
    const proposed = editable(fixture())
    proposed.capacity.bot.slots = 0
    proposed.capacity.bot.target_active = 0
    const result = changeOutboundBehavior(proposed, 'active')
    expect(result.scheduler.enabled).toBe(false)
    expect(result.capacity.bot.target_active).toBe(0)
    expect(outboundBehavior(result)).toBe('wait')
  })

  it('distinguishes configured scheduler authority from its durable pause bit', () => {
    expect(schedulerAuthorityState(undefined, false)).toBe('checking')
    expect(schedulerAuthorityState(false, false)).toBe('disabled')
    expect(schedulerAuthorityState(true, true)).toBe('paused')
    expect(schedulerAuthorityState(true, false)).toBe('active')
  })
})
