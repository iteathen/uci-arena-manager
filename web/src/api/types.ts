export type ResourceKind =
  | 'engine'
  | 'bot'
  | 'managed_service'
  | 'health_probe'
  | 'match_plan'
  | 'tournament'
  | 'game'
  | 'operation'
  | 'report'
  | 'artifact'
  | 'health_snapshot'
  | 'audit'

export interface Resource<T extends Record<string, unknown> = Record<string, unknown>> {
  id: string
  host_id: 'local'
  kind: ResourceKind
  revision: number
  created_at: string
  updated_at: string
  labels: Record<string, string>
  data: T
}

export interface SystemStatus {
  schema_version: number
  service: string
  version: string
  host_id: 'local'
  started_at: string
  database: string
  counts: Partial<Record<ResourceKind, number>>
}

export interface ArenaEvent {
  schema_version: number
  cursor: number
  event_id: string
  timestamp: string
  stream: string
  kind: string
  resource: { kind: ResourceKind; id: string; revision: number } | null
  correlation_id: string | null
  payload: Record<string, unknown>
}

export interface CommandEnvelope {
  schema_version: 1
  request_id: string
  idempotency_key: string
  target: { kind: string; id: string | null }
  action: string
  expected_revision: number | null
  dry_run: boolean
  parameters: Record<string, unknown>
}

export interface EngineData extends Record<string, unknown> {
  display_name: string
  executable: string
  engine_name: string
  engine_author: string
  fingerprint: string
  observed_state: string
  discovered_options: Array<{ name: string; type: string; default: string }>
  component_id?: string
  component_version?: string
  onboarding_source?: EngineCandidate['source']
  candidate_id?: string
  recommended?: boolean
  default_model?: {
    model_id: string
    display_name: string
    input_adapter_id: string
  }
}

export interface EngineCandidate {
  schema: 'arena_engine_candidate_v1'
  schema_version: 1
  candidate_id: string
  source: 'install_receipt' | 'saved_locator' | 'provider_manifest' | 'discovered'
  state: 'ready_to_probe' | 'unavailable'
  display_name: string
  executable: string
  working_directory: string
  arguments: string[]
  uci_options: Record<string, unknown>
  component_id?: string
  component_version?: string
  recommended?: boolean
  default_model?: {
    model_id: string
    display_name: string
    input_adapter_id: string
  }
  fingerprint?: string
  issues: Array<{ code: string; detail: string }>
}

export interface EngineCandidates {
  schema: 'arena_engine_candidates_v1'
  schema_version: 1
  precedence: string[]
  candidates: EngineCandidate[]
}

export interface EngineProbeResult {
  schema: 'arena_engine_probe_result_v1'
  schema_version: 1
  probe_id: string
  candidate_id: string
  state: 'passed' | 'failed'
  registration_parameters: Record<string, unknown>
  preview?: Record<string, unknown>
  diagnostic?: Record<string, unknown>
}

export interface MatchPlanData extends Record<string, unknown> {
  display_name: string
  format: string
  variant: string
  engine_ids: string[]
  games_per_pair: number
  concurrency: number
  lifecycle: string
  pairings: Array<{
    sequence: number
    white_engine_id: string
    black_engine_id: string
    round: number
  }>
}

export interface ServiceProfile {
  profile_id: 'book_forge' | 'uci_arena_evidence'
  display_name: string
  implementation_state:
    | 'implemented_external'
    | 'in_development_external'
    | 'planned_external'
  description: string
  optional: true
  capabilities: string[]
  required_configuration: string[]
  discovery: {
    supported: boolean
    manual_fallback: true
    sources: string[]
  }
}

export interface ServiceDiscovery {
  schema_version: 1
  profile_id: 'book_forge' | 'uci_arena_evidence'
  optional: true
  candidates: Array<{
    source: string
    complete: boolean
    configuration: Record<string, string>
    missing: string[]
  }>
  manual_configuration: string[]
}

export interface ManagedServiceData extends Record<string, unknown> {
  profile_id: string
  display_name: string
  integration_state: string
  desired_state: string
  observed_state: string
  capabilities: string[]
  health: { state: string; reason?: string }
  node_executable?: string
  cli_entrypoint?: string
  workspace?: string
  startup_installed?: boolean
  startup_control?: string
  installed_component_id?: string
  output_integration?: ServiceOutputIntegration
}

export interface BookSnapshotProjection {
  snapshot_id: string
  channel: string
  manifest_sha256: string
  artifact_files: Array<{ name: string; sha256: string }>
  record_count: number
  qualified: boolean
  includes_fixture: boolean
  current: boolean
  published_at: string
}

export interface EvidenceDatasetProjection {
  schema: 'uci_arena_evidence_dataset_v1'
  id: string
  campaign: string
  state: string
  target_identity: Record<string, unknown>
  authority: Record<string, unknown>
  observation_count: number
  created_at: string
  completed_at: string | null
}

export interface TimingProfileProjection {
  schema: 'uci_arena_evidence_profile_v1'
  source: string
  path: string
  sha256: string
  profile_schema: string
  qualified: true
  published_at: string
  identity_sha256: string
  target_identity: Record<string, unknown>
  permissions: Record<string, unknown>
}

export interface ServiceOutputIntegration {
  authority: 'book_forge' | 'uci_arena_evidence_service'
  snapshots?: BookSnapshotProjection[]
  dataset_count?: number
  datasets?: EvidenceDatasetProjection[]
  timing_profile_count?: number
  timing_profiles?: TimingProfileProjection[]
  selected_timing_profile?: TimingProfileProjection | null
}

export interface ComponentInventory {
  schema: 'arena_component_inventory_v1'
  schema_version: 1
  retained_catalog_available: boolean
  components: Array<{
    component_id: string
    display_name: string
    optional: boolean
    package_state: 'installed' | 'missing' | 'broken'
    downloadable: boolean
    component_version?: string
    startup_installed?: boolean
    issue?: string
    default_model?: {
      model_id: string
      display_name: string
      input_adapter_id: string
    }
  }>
}

export interface BotProfile {
  profile_id: 'lichess_bot'
  display_name: string
  adapter_component_id: 'lichess.adapter'
  optional: true
  capabilities: string[]
}

export interface BotCandidate {
  candidate_id: string
  profile_id: 'lichess_bot'
  display_name: string
  adapter_component_id: 'lichess.adapter'
  package_state: 'installed' | 'missing'
  configuration_state: 'configured' | 'missing'
  token_reference_available: boolean
  startup_state: 'installed' | 'disabled'
  state: 'ready_to_probe' | 'blocked'
  options: Record<string, unknown>
  default_model?: {
    model_id: string
    display_name: string
    input_adapter_id: string
  }
  readiness: Record<
    string,
    'ready' | 'missing' | 'not_verified' | 'verified' | 'disabled' | 'enabled'
  >
  issues: Array<{ code: string; detail: string }>
}

export interface BotCandidates {
  schema: 'arena_bot_candidates_v1'
  schema_version: 1
  candidates: BotCandidate[]
}

export interface BotProbeResult {
  schema: 'arena_bot_probe_result_v1'
  schema_version: 1
  state: 'passed'
  account_name: string
  is_bot: true
  games: number
}

export type BotControlAction = 'status' | 'enable_startup' | 'disable_startup'

export interface BotControlResult {
  schema: 'arena_bot_control_result_v1'
  schema_version: 1
  state: 'disabled' | 'enabled' | 'active'
  installed: boolean
  active: boolean
  account_name?: string
}

export interface BotTokenReferenceResult {
  schema: 'arena_bot_token_reference_result_v1'
  schema_version: 1
  state: 'saved'
  validation: 'path_reference_only'
}
