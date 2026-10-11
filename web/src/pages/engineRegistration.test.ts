import { describe, expect, it } from 'vitest'
import type { EngineCandidate, EngineData, Resource } from '../api/types'
import {
  activeEngineBookPath,
  activeEngineTimingIdentity,
  bookBindingState,
  candidateRegistration,
  candidateRepresentsOutput,
  candidateWithModelRoot,
  engineRegistrationParameters,
  timingBindingState,
} from './engineRegistration'

describe('selected service output activation', () => {
  it('requires explicit original control for a declared clock port and keeps baseline options intact', () => {
    const source = Object.assign(candidate(), {
      clock_option_bindings: { schema: 'arena_uci_clock_option_bindings_v1' as const, schema_version: 1 as const, bindings: { InitialBudgetMs: 'initial_ms' as const } },
    })
    source.uci_options.InitialBudgetMs = 0
    expect(() => engineRegistrationParameters(source)).toThrow()
    const parameters = engineRegistrationParameters(source, { initial_ms: 180000, increment_ms: 0 })
    expect(parameters).toMatchObject({ time_control: { initial_ms: 180000, increment_ms: 0 }, uci_options: { InitialBudgetMs: 0 } })
    expect(source.uci_options.InitialBudgetMs).toBe(0)
    expect(() => engineRegistrationParameters(source, { initial_ms: 0, increment_ms: 0 })).toThrow()
  })
  it('only declared clock values are exempt from immutable registration identity', () => {
    const source = Object.assign(candidate(), {
      clock_option_bindings: { schema: 'arena_uci_clock_option_bindings_v1' as const, schema_version: 1 as const, bindings: { InitialBudgetMs: 'initial_ms' as const } },
    })
    source.uci_options.InitialBudgetMs = 0
    const engine = registered(source)
    engine.data.clock_option_bindings = source.clock_option_bindings
    engine.data.option_overrides!.InitialBudgetMs = '180000'
    expect(candidateRegistration(source, [engine]).state).toBe('registered')
    engine.data.option_overrides!.Threads = '7'
    expect(candidateRegistration(source, [engine]).state).toBe('refresh_required')
  })
  it('refreshes only the selected resource when a component has duplicate registrations', () => {
    const source = candidate()
    const first = registered(source)
    const second = { ...registered(source), id: 'second-resource' }
    expect(candidateRegistration(source, [first, second], second.id).engine?.id).toBe(second.id)
    expect(candidateRegistration(source, [first, second], 'absent').state).toBe('unregistered')
  })
  it('requires the requested output binding rather than ordinary registration equality', () => {
    const source = candidate()
    expect(candidateRepresentsOutput(source, { kind: 'book', path: 'E:/published/book' })).toBe(false)
    expect(candidateRepresentsOutput(source, { kind: 'timing', identity: 'a'.repeat(64) })).toBe(false)
    source.uci_options = { ...source.uci_options, OwnBook: true, BookFile: 'E:/published/book/strong_rare_v1.bin' }
    expect(candidateRepresentsOutput(source, { kind: 'book', path: 'E:/published/book' })).toBe(true)
    source.uci_options.OwnBook = false
    expect(candidateRepresentsOutput(source, { kind: 'book', path: 'E:/published/book' })).toBe(false)
    source.launch_profile = { schema: 'arena_uci_engine_launch_profile_v1', state: 'qualified_timing', timing_profile: { available: true, identity_sha256: 'a'.repeat(64) }, expected_runtime: null }
    expect(candidateRepresentsOutput(source, { kind: 'timing', identity: 'a'.repeat(64) })).toBe(true)
    source.launch_profile.timing_profile.available = false
    expect(candidateRepresentsOutput(source, { kind: 'timing', identity: 'a'.repeat(64) })).toBe(false)
  })
})

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
      TimeProfileProviderReserveUs: 42000,
      TimeProfileBackupEmitReserveUs: 2500,
      TimeProfileIterationForecastScalePermille: 1350,
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
      ...engineRegistrationParameters(source, source.clock_option_bindings ? { initial_ms: 180000, increment_ms: 0 } : undefined),
      option_overrides: Object.fromEntries(
        Object.entries(source.uci_options).map(([name, value]) => [name, String(value)]),
      ),
      launch_profile: source.launch_profile,
      component_id: source.component_id,
      component_version: source.component_version,
      evidence_runtime: source.evidence_runtime,
      installed_artifact_sha256: source.installed_artifact_sha256,
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
  it('requires refresh for a same-version runtime binding or installed payload change', () => {
    const original = candidate()
    original.installed_artifact_sha256 = 'a'.repeat(64)
    original.evidence_runtime = { available: true, binding: {
      schema: 'uci_arena_evidence_runtime_binding_v2',
      component: { id: 'uci_arena.vector', version: '2.0.0', root: original.working_directory,
        manifest: 'arena-component.json', manifest_sha256: 'b'.repeat(64) },
      entrypoint: 'evidence_runtime_contract',
    } }
    const current = registered(original)
    expect(candidateRegistration(original, [current]).state).toBe('registered')
    const updated = structuredClone(original)
    if (updated.evidence_runtime?.available) updated.evidence_runtime.binding.component.manifest_sha256 = 'c'.repeat(64)
    expect(candidateRegistration(updated, [current]).state).toBe('refresh_required')
    updated.evidence_runtime = original.evidence_runtime
    updated.installed_artifact_sha256 = 'd'.repeat(64)
    expect(candidateRegistration(updated, [current]).state).toBe('refresh_required')
  })
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

  it.each(['uci_arena.vector', 'example.alternate-engine'])(
    'preserves the selected resource when %s moves to a new installed version',
    (componentId) => {
      const current = candidate()
      current.candidate_id = 'engcand-current-other-root'
      current.executable = 'C:\\engines\\other\\2.0.0\\node.exe'
      current.working_directory = 'C:\\engines\\other\\2.0.0'
      current.component_id = componentId
      current.recommended = false
      const prior = { ...current, candidate_id: 'engcand-prior-other-root',
        component_version: '1.9.0', executable: 'C:\\engines\\other\\1.9.0\\node.exe',
        working_directory: 'C:\\engines\\other\\1.9.0' }
      const previous = registered(prior)
      previous.id = 'selected-other-engine'
      const result = candidateRegistration(current, [registered(), previous])
      expect(result.state).toBe('refresh_required')
      expect(result.engine?.id).toBe('selected-other-engine')
    },
  )

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
    expect(selected.uci_options.TimeProfileProviderReserveUs).toBe(42000)
    expect(selected.uci_options.TimeProfileBackupEmitReserveUs).toBe(2500)
    expect(selected.uci_options.TimeProfileIterationForecastScalePermille).toBe(1350)
    expect(Object.keys(selected.uci_options)).toEqual(Object.keys(candidate().uci_options))
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

  it('distinguishes provider-current outputs from the active Engine bindings', () => {
    const engine = registered()
    engine.data.option_overrides = {
      ...engine.data.option_overrides,
      BookFile: 'C:\\UCI Arena Manager\\provider-data\\book-forge\\published\\integration\\strong_rare_v1.bin',
    }
    engine.data.launch_profile = {
      ...engine.data.launch_profile!,
      state: 'qualified_timing',
      timing_profile: { available: true, identity_sha256: 'a'.repeat(64) },
    }

    expect(activeEngineBookPath(engine)).toContain('strong_rare_v1.bin')
    expect(bookBindingState(
      engine,
      'c:/uci arena manager/provider-data/book-forge/published/integration',
    )).toBe('active')
    expect(bookBindingState(engine, 'C:\\other\\snapshot')).toBe('available')
    expect(bookBindingState(undefined, 'C:\\other\\snapshot')).toBe('engine_unregistered')
    engine.data.option_overrides!.OwnBook = 'false'
    expect(activeEngineBookPath(engine)).toBe('')
    expect(bookBindingState(
      engine,
      'c:/uci arena manager/provider-data/book-forge/published/integration',
    )).toBe('available')
    expect(activeEngineTimingIdentity(engine)).toBe('a'.repeat(64))
    expect(timingBindingState(engine, 'a'.repeat(64))).toBe('active')
    expect(timingBindingState(engine, 'b'.repeat(64))).toBe('available')
    expect(timingBindingState(undefined, 'a'.repeat(64))).toBe('engine_unregistered')

    delete engine.data.launch_profile
    engine.data.operational_timing_binding = {
      schema: 'arena_operational_timing_binding_v1',
      schema_version: 1,
      timing_profile: { available: true, identity_sha256: 'a'.repeat(64) },
      operational_distribution: 'retained',
      marginal_move_quality: 'unqualified',
    }
    expect(activeEngineTimingIdentity(engine)).toBe('a'.repeat(64))
    expect(timingBindingState(engine, 'a'.repeat(64))).toBe('active')
  })
})
