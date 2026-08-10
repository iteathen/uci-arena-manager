import { describe, expect, it } from 'vitest'
import type { EngineCandidate, EngineData, Resource } from '../api/types'
import {
  candidateRegistration,
  candidateWithModelRoot,
  engineRegistrationParameters,
} from './engineRegistration'

function candidate(): EngineCandidate {
  return {
    schema: 'arena_engine_candidate_v1',
    schema_version: 1,
    candidate_id: 'engcand-managed',
    source: 'install_receipt',
    state: 'ready_to_probe',
    display_name: 'UCI Arena Engine',
    executable: 'C:\\Program Files\\UCI Arena\\engine.exe',
    working_directory: 'C:\\Program Files\\UCI Arena',
    arguments: [],
    uci_options: {
      ModelRoot: 'C:\\Program Files\\UCI Arena\\models\\default',
      SearchCore: 'mcgs', OpeningBook: 'book.bin', Threads: 3, Ponder: false,
    },
    component_id: 'uci_arena.engine',
    component_version: '2.0.0',
    recommended: true,
    launch_profile: {
      schema: 'arena_uci_engine_launch_profile_v1',
      state: 'conservative',
      timing_profile: { available: false },
      expected_runtime: null,
    },
    issues: [],
  }
}

function registered(source = candidate()): Resource<EngineData> {
  return {
    id: 'engine-id',
    host_id: 'local',
    kind: 'engine',
    revision: 3,
    created_at: '2026-01-01T00:00:00Z',
    updated_at: '2026-01-01T00:00:00Z',
    labels: {},
    data: {
      ...engineRegistrationParameters(source),
      option_overrides: Object.fromEntries(
        Object.entries(source.uci_options).map(([name, value]) => [name, String(value)]),
      ),
      launch_profile: source.launch_profile,
      component_id: source.component_id,
      component_version: source.component_version,
      onboarding_source: 'install_receipt',
      recommended: true,
      engine_name: 'Mock',
      engine_author: 'Test',
      fingerprint: 'sha256:fixture',
      observed_state: 'stopped',
      discovered_options: [],
    },
  }
}

describe('managed Engine registration state', () => {
  it('recognizes an exact managed launch profile', () => {
    expect(candidateRegistration(candidate(), [registered()]).state).toBe('registered')
  })

  it('requires refresh when Installer regenerates options or profile identity', () => {
    const updated = candidate()
    updated.uci_options = { ...updated.uci_options, TimingProfile: 'qualified.json' }
    updated.launch_profile = {
      ...updated.launch_profile!,
      state: 'qualified_timing',
      timing_profile: { available: true, identity_sha256: 'a'.repeat(64) },
      expected_runtime: {
        timing_contract_version: 'timing-v1', model_id: 'model', model_fingerprint: '1',
        adapter_id: 'adapter', provider_requested: 'DirectML', provider_effective: 'DirectML',
        provider_numeric_fingerprint: '2', hardware_driver_fingerprint: '3',
        onnx_runtime_version: '1.0', search_core: 'mcgs',
      },
    }
    expect(candidateRegistration(updated, [registered()]).state).toBe('refresh_required')
  })

  it('requires one refresh for a retained non-canonical option map', () => {
    const legacy = registered()
    legacy.data.option_overrides = candidate().uci_options
    expect(candidateRegistration(candidate(), [legacy]).state).toBe('refresh_required')
  })

  it('refreshes the same managed component across immutable version roots', () => {
    const prior = candidate()
    prior.candidate_id = 'engcand-prior-root'
    prior.component_version = '1.9.0'
    prior.executable = 'C:\\Program Files\\UCI Arena\\components\\engine\\1.9.0\\engine.exe'
    prior.working_directory = 'C:\\Program Files\\UCI Arena\\components\\engine\\1.9.0'
    expect(candidateRegistration(candidate(), [registered(prior)]).state).toBe('refresh_required')
  })

  it('does not let a manual registration hide a receipt-managed candidate', () => {
    const manual = registered()
    manual.data.candidate_id = undefined
    manual.data.onboarding_source = 'discovered'
    manual.data.component_id = undefined
    expect(candidateRegistration(candidate(), [manual]).state).toBe('unregistered')
  })

  it('treats a selected ModelRoot as the only operator-owned profile override', () => {
    const selected = candidateWithModelRoot(
      candidate(),
      'C:\\models\\operator-selected',
    )
    expect(selected.uci_options.ModelRoot).toBe('C:\\models\\operator-selected')
    expect(selected.launch_profile).toBeUndefined()
    expect(candidateRegistration(selected, [registered(selected)]).state).toBe('registered')
    expect(candidateRegistration(candidate(), [registered(selected)]).state).toBe(
      'refresh_required',
    )
  })

  it('preserves the packaged launch profile when ModelRoot is unchanged', () => {
    const selected = candidateWithModelRoot(
      candidate(),
      'c:/program files/uci arena/models/default',
    )
    expect(selected.launch_profile).toEqual(candidate().launch_profile)
    expect(selected.uci_options.ModelRoot).toBe(
      'C:\\Program Files\\UCI Arena\\models\\default',
    )
  })
})
