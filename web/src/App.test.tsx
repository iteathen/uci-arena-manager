import { afterEach, describe, expect, it } from 'vitest'
import { locationHasPairingToken, pageFromLocation } from './components/pageNavigation'

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
