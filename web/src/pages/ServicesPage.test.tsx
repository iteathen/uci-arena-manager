import { describe, expect, it } from 'vitest'
import type { ManagedServiceData, Resource } from '../api/types'
import {
  canEnqueueEvidenceCampaign,
  evidenceCampaignBlocker,
  needsEvidenceTarget,
  selectPreferredService,
  serviceCardStates,
  serviceLocatorMode,
} from './serviceSelection'

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

  it('blocks a known-impossible first Evidence campaign until a target exists', () => {
    const evidence = service('evidence', 'uci_arena_evidence')
    evidence.data.status_projection = {
      collection: { state: 'waiting_for_target' },
    }
    evidence.data.orchestration_projection = {
      schema: 'uci_arena_evidence_orchestration_v1',
      targets: [],
      jobs: [],
    }

    expect(canEnqueueEvidenceCampaign(evidence.data)).toBe(false)
    expect(needsEvidenceTarget(evidence.data)).toBe(true)
    expect(evidenceCampaignBlocker(evidence.data)).toContain('register')
    evidence.data.orchestration_projection.targets.push({
      id: '01900000-0000-7000-8000-000000000001',
      label: 'active deployment',
      model_id: 'model',
      model_fingerprint: 'fingerprint',
      engine_sha256: 'sha256:fixture',
      provider: 'DirectML',
      created_at: '2026-01-01T00:00:00.000Z',
    })
    expect(canEnqueueEvidenceCampaign(evidence.data)).toBe(true)
    expect(needsEvidenceTarget(evidence.data)).toBe(false)
    expect(evidenceCampaignBlocker(evidence.data)).toBeNull()
    evidence.data.orchestration_projection.jobs.push({
      id: '01900000-0000-7000-8000-000000000002',
      adapter: 'timing_profile',
      state: 'queued',
      priority: 100,
      target_id: evidence.data.orchestration_projection.targets[0]!.id,
      shard_id: 'deadline-provider-qualification',
      dataset_id: '01900000-0000-7000-8000-000000000003',
      attempts: 0,
      wait_reason: '',
      last_error: '',
      created_at: '2026-01-01T00:00:00.000Z',
      updated_at: '2026-01-01T00:00:00.000Z',
    })
    expect(canEnqueueEvidenceCampaign(evidence.data)).toBe(false)
    expect(evidenceCampaignBlocker(evidence.data)).toContain('already')
  })

  it('keeps package locator and runtime truth separate during loading', () => {
    expect(serviceCardStates(undefined, false, undefined, true, true)).toEqual({
      package: 'PACKAGE CHECKING',
      locator: 'LOCATOR CHECKING',
      runtime: 'RUNTIME CHECKING',
    })
    expect(serviceCardStates('installed', true, 'running', false, false)).toEqual({
      package: 'PACKAGE INSTALLED',
      locator: 'LOCATOR CONFIGURED',
      runtime: 'RUNTIME RUNNING',
    })
  })

  it('keeps receipt-owned locators read-only and manual locators editable', () => {
    expect(serviceLocatorMode()).toBe('configure')
    expect(serviceLocatorMode(service('manual', 'book_forge'))).toBe('edit_manual')
    expect(serviceLocatorMode(
      service('installed', 'book_forge', 'book_forge.service'),
    )).toBe('view_installed')
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
