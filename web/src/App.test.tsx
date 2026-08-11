import { afterEach, describe, expect, it } from 'vitest'
import { locationHasPairingToken, pageFromLocation } from './components/pageNavigation'
import { canEnableBotStartup, canProbeBot } from './pages/botAdmission'
import type { BotCandidate } from './api/types'

afterEach(() => {
  history.replaceState(null, '', window.location.pathname)
})

describe('dashboard location model', () => {
  it('resolves durable page hashes and rejects unknown pages', () => {
    history.replaceState(null, '', '#page=tournaments')
    expect(pageFromLocation()).toBe('tournaments')

    history.replaceState(null, '', '#page=unknown')
    expect(pageFromLocation()).toBe('overview')
  })

  it('distinguishes one-use pairing hashes from ordinary navigation', () => {
    history.replaceState(null, '', '#page=reports')
    expect(locationHasPairingToken()).toBe(false)

    history.replaceState(null, '', `#pair=${'a'.repeat(64)}`)
    expect(locationHasPairingToken()).toBe(true)
  })
})

describe('Lichess BOT admission projections', () => {
  const candidate = {
    state: 'ready_to_probe',
    startup_eligibility: 'blocked',
  } as BotCandidate

  it('keeps account verification independent from live-play readiness', () => {
    expect(canProbeBot(candidate)).toBe(true)
    expect(canEnableBotStartup(candidate, true, false)).toBe(false)
  })

  it('enables startup only after the backend projects live-play readiness', () => {
    expect(
      canEnableBotStartup(
        { ...candidate, startup_eligibility: 'ready' },
        true,
        false,
      ),
    ).toBe(true)
    expect(
      canEnableBotStartup(
        { ...candidate, startup_eligibility: 'ready' },
        false,
        false,
      ),
    ).toBe(false)
  })
})
