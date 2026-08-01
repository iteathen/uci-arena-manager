import { describe, expect, it } from 'vitest'
import type { ResourceKind } from '../api/types'
import { resourcePageForKind } from './resourceNavigation'

describe('resourcePageForKind', () => {
  it('maps every public resource kind to its owning surface', () => {
    const expected: Record<ResourceKind, string> = {
      host: 'statistics',
      engine: 'engines',
      bot: 'bots',
      managed_service: 'services',
      health_probe: 'statistics',
      match_plan: 'tournaments',
      tournament: 'tournaments',
      game: 'history',
      operation: 'logs',
      report: 'reports',
      artifact: 'reports',
      secret_reference: 'configuration',
      log_source: 'logs',
      health_snapshot: 'statistics',
    }

    for (const [kind, page] of Object.entries(expected)) {
      expect(resourcePageForKind(kind as ResourceKind)).toBe(page)
    }
  })
})
