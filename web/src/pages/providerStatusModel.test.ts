import { describe, expect, it } from 'vitest'
import type { ProviderStatusProjection } from '../api/types'
import { providerStatusAlerts } from './providerStatusModel'

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
})
