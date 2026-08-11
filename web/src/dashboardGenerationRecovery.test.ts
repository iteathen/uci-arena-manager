import { afterEach, describe, expect, it, vi } from 'vitest'
import { recoverDashboardGeneration } from './dashboardGenerationRecovery'

afterEach(() => {
  sessionStorage.clear()
})

function preloadError(message: string) {
  const event = new Event('vite:preloadError', { cancelable: true })
  Object.defineProperty(event, 'payload', { value: new Error(message) })
  return event
}

describe('dashboard generation recovery', () => {
  it('reloads once when an installed upgrade replaces a lazy chunk', () => {
    const event = preloadError('Failed to fetch StatisticsPage-old.js')
    const reload = vi.fn()

    expect(recoverDashboardGeneration(event, sessionStorage, reload)).toBe('reloading')
    expect(event.defaultPrevented).toBe(true)
    expect(reload).toHaveBeenCalledOnce()

    const repeated = preloadError('Failed to fetch StatisticsPage-old.js')
    expect(recoverDashboardGeneration(repeated, sessionStorage, reload)).toBe('already_retried')
    expect(repeated.defaultPrevented).toBe(false)
    expect(reload).toHaveBeenCalledOnce()
  })

  it('permits one recovery for a later distinct installed generation', () => {
    const reload = vi.fn()
    recoverDashboardGeneration(preloadError('Page-old.js'), sessionStorage, reload)

    expect(
      recoverDashboardGeneration(preloadError('Page-new.js'), sessionStorage, reload),
    ).toBe('reloading')
    expect(reload).toHaveBeenCalledTimes(2)
  })

  it('does not risk a reload loop when session storage is unavailable', () => {
    const event = preloadError('Page-missing.js')
    const reload = vi.fn()
    const unavailable = {
      getItem: () => {
        throw new Error('storage disabled')
      },
      setItem: () => {
        throw new Error('storage disabled')
      },
    }

    expect(recoverDashboardGeneration(event, unavailable, reload)).toBe('storage_unavailable')
    expect(event.defaultPrevented).toBe(false)
    expect(reload).not.toHaveBeenCalled()
  })
})
