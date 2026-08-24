import { describe, expect, it } from 'vitest'
import type {
  ProviderIncidentPageProjection,
  ProviderStatusProjection,
} from '../api/types'
import {
  deduplicateProviderIncidents,
  providerIncidentAlerts,
  providerStatusAlerts,
} from './providerStatusModel'

function fixture(): ProviderStatusProjection {
  return {
    schema: 'arena_provider_status_projection_v1',
    schema_version: 1,
    profile_id: 'lichess_bot',
    availability: 'available',
    provider_contract_version: '1.0',
    provider_status_schema: 'uci_arena_lichess_status_v3',
    supervision_availability: 'unavailable',
    incident_availability: 'unavailable',
    status: {
      schema: 'uci_arena_lichess_status_v3',
      installed: true,
      registered: true,
      active: true,
      ready: false,
      stream_connected: false,
      stream_state: 'waiting_for_api_cooldown',
      runtime_updated_at: '2026-08-23T18:45:00.000Z',
      account: { username: 'ArenaBot', bot: true },
      capacity: {
        configured: 2,
        qualified: 2,
        slots: {
          empty: 1,
          preparing: 0,
          pending: 0,
          cancelling: 0,
          active: 1,
          draining: 0,
        },
      },
      games: { active: 1, preparing: 0, pending: 0, cancelling: 0, draining: 0 },
      scheduler: {
        enabled: true,
        paused: false,
        target_active_bots: 1,
        fault: { kind: 'api_request_failed', status: 429 },
      },
      rate_limit: {
        state: 'cooldown',
        cooldown_until: 1_777_000_000_000,
        cooldown_remaining_ms: 45_000,
        strike_count: 2,
        challenge_state: 'quarantined',
        challenge_blocked_until: 1_777_000_000_000,
        challenge_remaining_ms: 45_000,
        challenge_strike_count: 2,
        queue_depth: 1,
        in_flight_class: null,
        client_lease: {
          state: 'authoritative',
          active_clients: 1,
          heartbeat_at: 1_776_999_950_000,
          expires_at: 1_777_000_010_000,
        },
      },
      blockers: ['rate_limit_cooldown'],
    },
  }
}

function currentFixture(): ProviderStatusProjection {
  const legacy = fixture()
  return {
    ...legacy,
    provider_contract_version: '1.1',
    provider_status_schema: 'uci_arena_lichess_status_v4',
    supervision_availability: 'available',
    incident_availability: 'available',
    status: {
      ...legacy.status,
      schema: 'uci_arena_lichess_status_v4',
      supervision: {
        state: 'restart_pending',
        restart_pending: true,
        exit_receipt_state: 'ready',
        last_exit: {
          disposition: 'unexpected_exit',
          exited_at: '2026-08-23T18:44:30.000Z',
          child_exit_code: 3_221_225_481,
        },
      },
      incidents: {
        available: true,
        active_count: 2,
        highest_severity: 'critical',
        latest_cursor: 8,
        truncated: false,
        truncated_rows: 0,
      },
    },
  }
}

function incidentFixture(): ProviderIncidentPageProjection {
  return {
    schema: 'arena_provider_incident_page_projection_v1',
    schema_version: 1,
    profile_id: 'lichess_bot',
    availability: 'available',
    provider_contract_version: '1.1',
    page: {
      schema: 'uci_arena_provider_incident_page_v1',
      filter: 'active',
      after_cursor: 0,
      next_cursor: 8,
      has_more: false,
      incidents: [
        {
          cursor: 3,
          code: 'api_request_failed',
          severity: 'error',
          source: 'api',
          operation: 'online_bots',
          first_occurrence: 1_777_000_000_000,
          last_occurrence: 1_777_000_010_000,
          occurrence_count: 2,
          state: 'active',
          resolved_at: null,
          http_status: 429,
          retry_deadline: 1_777_000_060_000,
          game_id: null,
          detail: 'Lichess request failed with HTTP 429',
        },
        {
          cursor: 8,
          code: 'stream_disconnected',
          severity: 'critical',
          source: 'stream',
          operation: 'event_stream',
          first_occurrence: 1_777_000_020_000,
          last_occurrence: 1_777_000_030_000,
          occurrence_count: 1,
          state: 'active',
          resolved_at: null,
          http_status: null,
          retry_deadline: 1_777_000_060_000,
          game_id: 'fixture-game',
          detail: 'Provider event stream disconnected',
        },
      ],
      retention: {
        maximum_rows: 2048,
        resolved_retention_ms: 7_776_000_000,
        last_truncated_cursor: null,
        truncated_rows: 0,
        truncated_active_rows: 0,
      },
    },
  }
}

describe('providerStatusAlerts', () => {
  it('projects current scheduler, cooldown, stream, and blocker state', () => {
    const alerts = providerStatusAlerts(fixture())
    expect(alerts.map((alert) => alert.key)).toEqual([
      'scheduler-fault',
      'api-cooldown',
      'stream-disconnected',
      'blocker-rate_limit_cooldown',
    ])
    expect(alerts[0].detail).toContain('durable incident history is unavailable')
    expect(alerts[1].detail).toContain('45 seconds')
  })

  it('does not fabricate alerts when the current provider reports ready', () => {
    const status = fixture()
    status.status.ready = true
    status.status.stream_connected = true
    status.status.stream_state = 'connected'
    status.status.scheduler.fault = null
    status.status.rate_limit.state = 'ready'
    status.status.blockers = []
    expect(providerStatusAlerts(status)).toEqual([])
  })

  it('projects provider-owned unexpected exit and restart state', () => {
    const alerts = providerStatusAlerts(currentFixture())
    expect(alerts.map((alert) => alert.key)).toContain('provider-restart-pending')
    expect(alerts.map((alert) => alert.key)).toContain('provider-unexpected-exit')
    expect(alerts[0].detail).toContain('durable details are projected separately')
  })
})

describe('providerIncidentAlerts', () => {
  it('orders active incidents by severity and preserves provider context', () => {
    const alerts = providerIncidentAlerts(incidentFixture())
    expect(alerts.map((alert) => alert.key)).toEqual(['incident-8', 'incident-3'])
    expect(alerts[0].title).toContain('Critical')
    expect(alerts[0].detail).toContain('game fixture-game')
    expect(alerts[1].detail).toContain('HTTP 429')
  })

  it('deduplicates one provider cursor while retaining the newest projection', () => {
    const projection = incidentFixture()
    const duplicate = {
      ...projection.page!.incidents[0],
      occurrence_count: 4,
      last_occurrence: projection.page!.incidents[0].last_occurrence + 1,
    }
    const deduplicated = deduplicateProviderIncidents([
      projection.page!.incidents[0],
      duplicate,
    ])
    expect(deduplicated).toHaveLength(1)
    expect(deduplicated[0].occurrence_count).toBe(4)
  })

  it('does not turn operation unavailability into an empty healthy page', () => {
    const unavailable: ProviderIncidentPageProjection = {
      ...incidentFixture(),
      availability: 'unavailable',
      page: null,
    }
    expect(providerIncidentAlerts(unavailable)).toEqual([])
  })
})
