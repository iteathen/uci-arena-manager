export type ResourceKind =
  | 'host'
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
  | 'secret_reference'
  | 'log_source'
  | 'health_snapshot'

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
  discovered_options: Array<{
    name: string
    type: 'check' | 'spin' | 'combo' | 'string' | 'button' | string
    default: string
    minimum?: number | null
    maximum?: number | null
    variants?: string[]
  }>
  component_id?: string
  component_version?: string
  onboarding_source?: EngineCandidate['source']
  candidate_id?: string
  recommended?: boolean
  default_model?: {
    model_id: string
    display_name: string
    input_adapter_id: string
    root: string
  }
  runtime_model_identity?: {
    model_fingerprint: string
    provider: string
    provider_numeric_fingerprint: string
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
    root: string
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
  rounds: number
  concurrency: number
  planned_total_games: number
  opening_suite: string | null
  time_control: {
    initial_ms: number
    increment_ms: number
    moves_to_go: number | null
  }
  adjudication: {
    max_plies: number
    draw_cp: number | null
    draw_consecutive_plies: number
    draw_min_plies: number
    win_cp: number | null
    win_consecutive_plies: number
  }
  sprt: {
    elo0: number
    elo1: number
    alpha: number
    beta: number
    lower_threshold: number
    upper_threshold: number
    llr?: number
    games: number
    score: number
    verdict: 'accept_h0' | 'accept_h1' | null
  } | null
  swiss_byes: Array<{ round: number; engine_id: string; score: number }>
  swiss_standings: Array<{ engine_id: string; score: number; color_balance: number }>
  lifecycle: string
  progress: {
    total_games: number
    queued_games: number
    active_games: number
    completed_games: number
    cancelled_games: number
    failed_games: number
    unplayed_games: number
    percentage: number
    current_round: number | null
    updated_at: string
  }
  pairings: Array<{
    sequence: number
    white_engine_id: string
    black_engine_id: string
    round: number
    opening_index?: number
  }>
}

export interface GameData extends Record<string, unknown> {
  display_name: string
  competition_kind: 'match_plan' | 'tournament'
  competition_id: string
  sequence: number
  round: number
  board: number
  starting_fen: string
  variant: 'standard' | 'chess960'
  uci_moves: string[]
  san_moves: string[]
  per_ply_fen: string[]
  evaluations_cp: Array<number | null>
  principal_variations: string[][]
  clocks: { white_ms: number; black_ms: number }
  result: string
  termination: string
  lifecycle: 'running' | 'completed' | 'cancelled' | 'failed' | 'interrupted'
  white_engine: { id: string; fingerprint: string; options: Record<string, unknown> }
  black_engine: { id: string; fingerprint: string; options: Record<string, unknown> }
  failure_detail?: string
}

export interface ReportData extends Record<string, unknown> {
  display_name: string
  schema: 'arena_report_document_v1'
  schema_version: 1
  scope: 'current_state' | 'competition'
  filters: {
    scope: 'current_state' | 'competition'
    competition_id: string | null
  }
  generation_parameters: {
    formats: Array<'html' | 'pdf' | 'csv' | 'json' | 'pgn'>
    maximum_games: number
  }
  data_cutoff: string
  content_hashes: { source_projection_sha256: string }
  provenance: {
    producer: 'uci-arena-manager'
    producer_version: string
    host_id: 'local'
    service_outputs: 'bounded_public_projections'
  }
  artifacts: Array<{
    format: 'html' | 'pdf' | 'csv' | 'json' | 'pgn'
    filename: string
    media_type: string
    size_bytes: number
    sha256: string
    download_url: string
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
  last_successful_status_at?: string
  output_projection_stale?: boolean
  status_projection?: {
    collection?: { state?: string; reason?: string }
    jobs?: Record<string, number>
    targets?: number
    datasets?: number
  }
  orchestration_projection?: EvidenceOrchestrationProjection
}

export interface EvidenceOrchestrationProjection {
  schema: 'uci_arena_evidence_orchestration_v1'
  targets: Array<{
    id: string
    label: string
    model_id: string
    model_fingerprint: string
    engine_sha256: string
    provider: string
    created_at: string
  }>
  jobs: Array<{
    id: string
    adapter: string
    state: string
    priority: number
    target_id: string | null
    shard_id: string
    dataset_id: string
    attempts: number
    wait_reason: string
    last_error: string
    created_at: string
    updated_at: string
  }>
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
  package_state: 'installed' | 'missing' | 'broken'
  integration_state: 'not_configured' | 'compatible' | 'incompatible'
  configuration_state: 'configured' | 'missing'
  token_reference_available: boolean
  startup_state: 'installed' | 'disabled'
  state: 'ready_to_probe' | 'blocked'
  options_state: 'interactive_helper_required'
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
  provider_state: 'inactive' | 'ready' | 'degraded' | 'unavailable'
  provider_active: boolean
  provider_ready: boolean
  stream_connected: boolean
  blockers: string[]
  account_name?: string
}

export interface BotTokenReferenceResult {
  schema: 'arena_bot_token_reference_result_v1'
  schema_version: 1
  state: 'saved'
  validation: 'path_reference_only'
}

export interface BotOptions {
  schema: 'uci_arena_lichess_manager_options_v1'
  capacity: {
    total_slots: number
    startup_parallelism: number
    human: {
      slots: number
      allow_borrowing: boolean
      accept_public: boolean
    }
    bot: {
      slots: number
      target_active: number
    }
    evidence: {
      status: string
      qualified: boolean
      maximum_concurrency: number | null
    }
  }
  inbound_challenges: {
    accept_bots: boolean
    min_base: number
    max_base: number
    min_increment: number
    max_increment: number
    speeds: string[]
    modes: string[]
  }
  outbound_challenge: {
    rated: boolean
    color: 'random' | 'white' | 'black'
    clock_limit: number
    clock_increment: number
  }
  scheduler: {
    enabled: boolean
    discovery_interval_seconds: number
    challenge_interval_seconds: number
    reservation_retry_seconds: number
    challenge_timeout_seconds: number
  }
  opponent_selection: {
    max_games_per_opponent_per_day: number
    timeout_cooldown_minutes: number
    decline_cooldown_minutes: number
    rating_target: number
    rating_spread: number
  }
}

export interface BotOptionsUpdate {
  capacity: Omit<BotOptions['capacity'], 'evidence'>
  inbound_challenges: BotOptions['inbound_challenges']
  outbound_challenge: BotOptions['outbound_challenge']
  scheduler: BotOptions['scheduler']
  opponent_selection: BotOptions['opponent_selection']
}

export type BotAccountAction =
  | 'owner_list'
  | 'owner_add'
  | 'owner_remove'
  | 'reservation_status'
  | 'reservation_create'
  | 'reservation_cancel'
  | 'scheduler_status'
  | 'scheduler_pause'
  | 'scheduler_resume'

export interface BotAccountResult {
  schema: 'arena_bot_local_account_result_v1'
  schema_version: 1
  action: BotAccountAction
  owners?: Array<{ username: string; created_at: number }>
  username?: string
  added?: boolean
  removed?: boolean
  reservation?: {
    opponent: string
    games_remaining: number
    expires_at: number
    created_at: number
  } | null
  canceled?: boolean
  scheduler?: { paused: boolean }
}
