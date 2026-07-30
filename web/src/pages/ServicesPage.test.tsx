import { describe, expect, it } from 'vitest'
import type { ManagedServiceData, Resource } from '../api/types'
import { selectPreferredService } from './serviceSelection'

function service(
  id: string,
  profileId: string,
  installedComponentId?: string,
): Resource<ManagedServiceData> {
  return {
    id,
    host_id: 'local',
    kind: 'managed_service',
    revision: 1,
    created_at: '2026-01-01T00:00:00.000Z',
    updated_at: '2026-01-01T00:00:00.000Z',
    labels: {},
    data: {
      profile_id: profileId,
      display_name: profileId,
      integration_state: 'available',
      desired_state: 'running',
      observed_state: 'running',
      capabilities: [],
      health: { state: 'healthy' },
      installed_component_id: installedComponentId,
    },
  }
}

describe('managed-service catalog selection', () => {
  it('prefers the exact receipt-owned service over a legacy manual duplicate', () => {
    const manual = service('manual', 'book_forge')
    const installed = service(
      'installed',
      'book_forge',
      'book_forge.service',
    )

    expect(
      selectPreferredService(
        [installed, manual],
        'book_forge',
        'book_forge.service',
      )?.id,
    ).toBe('installed')
  })

  it('keeps the configured manual service when no receipt-owned service exists', () => {
    const manual = service('manual', 'uci_arena_evidence')

    expect(
      selectPreferredService(
        [manual],
        'uci_arena_evidence',
        'uci_arena_evidence.service',
      )?.id,
    ).toBe('manual')
  })
})
