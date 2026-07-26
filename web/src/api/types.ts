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
  capabilities: string[]
  required_configuration: string[]
}

export interface ManagedServiceData extends Record<string, unknown> {
  profile_id: string
  display_name: string
  integration_state: string
  desired_state: string
  observed_state: string
  capabilities: string[]
  health: { state: string; reason?: string }
}
