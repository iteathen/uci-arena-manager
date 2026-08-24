import type {
  ProviderIncident,
  ProviderIncidentPageProjection,
  ProviderStatusProjection,
} from '../api/types'

export interface ProviderStatusAlert {
  key: string
  color: 'red' | 'orange' | 'blue'
  title: string
  detail: string
}

function retryDetail(milliseconds: number | null): string {
  if (milliseconds === null) return 'The provider owns the retry deadline.'
  return `Provider retry eligibility is expected in ${Math.ceil(milliseconds / 1000)} seconds.`
}

function displayCode(value: string): string {
  return value.replaceAll('_', ' ')
}

function incidentTimestamp(value: number): string {
  const timestamp = new Date(value)
  return Number.isNaN(timestamp.getTime())
    ? `${value} ms since epoch`
    : timestamp.toLocaleString()
}

function incidentDetail(incident: ProviderIncident): string {
  const context = [
    `${displayCode(incident.code)} · ${displayCode(incident.source)} / ${displayCode(incident.operation)}`,
    `${incident.occurrence_count} occurrence${incident.occurrence_count === 1 ? '' : 's'}`,
    `last ${incidentTimestamp(incident.last_occurrence)}`,
  ]
  if (incident.http_status !== null) context.push(`HTTP ${incident.http_status}`)
  if (incident.retry_deadline !== null) {
    context.push(`provider retry ${incidentTimestamp(incident.retry_deadline)}`)
  }
  if (incident.game_id) context.push(`game ${incident.game_id}`)
  context.push(incident.detail)
  return context.join(' · ')
}

export function deduplicateProviderIncidents(
  incidents: ProviderIncident[],
): ProviderIncident[] {
  const byCursor = new Map<number, ProviderIncident>()
  for (const incident of incidents) byCursor.set(incident.cursor, incident)
  return [...byCursor.values()].sort((left, right) => {
    const severity = { info: 0, warning: 1, error: 2, critical: 3 }
    return severity[right.severity] - severity[left.severity]
      || right.last_occurrence - left.last_occurrence
      || right.cursor - left.cursor
  })
}

export function providerIncidentAlerts(
  projection: ProviderIncidentPageProjection | undefined,
  includeResolved = false,
): ProviderStatusAlert[] {
  if (projection?.availability !== 'available' || !projection.page) return []
  return deduplicateProviderIncidents(projection.page.incidents)
    .filter((incident) => includeResolved || incident.state === 'active')
    .map((incident) => ({
      key: `incident-${incident.cursor}`,
      color:
        incident.state === 'resolved'
          ? 'blue'
          : incident.severity === 'critical' || incident.severity === 'error'
            ? 'red'
            : incident.severity === 'warning'
              ? 'orange'
              : 'blue',
      title:
        incident.state === 'resolved'
          ? `Recovered Provider incident · ${displayCode(incident.code)}`
          : `${incident.severity === 'critical' ? 'Critical' : 'Active'} Provider incident · ${displayCode(incident.code)}`,
      detail: incidentDetail(incident),
    }))
}

export function providerStatusAlerts(
  projection: ProviderStatusProjection | undefined,
): ProviderStatusAlert[] {
  if (!projection) return []
  const status = projection.status
  const alerts: ProviderStatusAlert[] = []

  if (status.scheduler.fault) {
    const httpStatus = status.scheduler.fault.status
    alerts.push({
      key: 'scheduler-fault',
      color: 'red',
      title: 'Current Lichess scheduler fault',
      detail: `${displayCode(status.scheduler.fault.kind)}${httpStatus === null ? '' : ` · HTTP ${httpStatus}`}. This is a live provider observation${projection.incident_availability === 'unavailable' ? `; durable incident history is unavailable on contract v${projection.provider_contract_version}` : '; durable details are projected separately'}.`,
    })
  } else if (status.scheduler.paused) {
    alerts.push({
      key: 'scheduler-paused',
      color: 'blue',
      title: 'Lichess scheduler paused',
      detail: 'The provider currently reports outbound scheduling paused.',
    })
  }

  if (status.rate_limit.state === 'cooldown') {
    alerts.push({
      key: 'api-cooldown',
      color: 'orange',
      title: 'Lichess API cooldown active',
      detail: retryDetail(status.rate_limit.cooldown_remaining_ms),
    })
  } else if (status.rate_limit.state === 'challenge_quarantined') {
    alerts.push({
      key: 'challenge-quarantined',
      color: 'orange',
      title: 'Outbound challenges quarantined',
      detail: retryDetail(status.rate_limit.challenge_remaining_ms),
    })
  } else if (status.rate_limit.state === 'challenge_half_open') {
    alerts.push({
      key: 'challenge-half-open',
      color: 'blue',
      title: 'Outbound challenge recovery probe',
      detail: 'The provider owns the half-open recovery attempt and cooldown authority.',
    })
  }

  if (status.active && !status.stream_connected) {
    alerts.push({
      key: 'stream-disconnected',
      color: 'orange',
      title: 'Lichess event stream disconnected',
      detail: `Current provider stream state: ${displayCode(status.stream_state)}.`,
    })
  }

  if ('supervision' in status) {
    if (status.supervision.restart_pending) {
      alerts.push({
        key: 'provider-restart-pending',
        color: 'red',
        title: 'Provider restart pending',
        detail: 'The Provider owns restart eligibility and is waiting to recover its existing startup registration.',
      })
    }
    if (status.supervision.last_exit?.disposition === 'unexpected_exit') {
      alerts.push({
        key: 'provider-unexpected-exit',
        color: status.supervision.restart_pending ? 'red' : 'orange',
        title: 'Provider exited unexpectedly',
        detail: `Exit ${status.supervision.last_exit.child_exit_code} at ${status.supervision.last_exit.exited_at}. The Provider remains recovery authority.`,
      })
    }
    if (status.supervision.exit_receipt_state === 'failed') {
      alerts.push({
        key: 'provider-exit-receipt-failed',
        color: 'red',
        title: 'Provider exit evidence unavailable',
        detail: 'The Provider reported that its bounded exit receipt could not be established.',
      })
    }
    if (status.incidents.truncated) {
      alerts.push({
        key: 'provider-incidents-truncated',
        color: 'orange',
        title: 'Provider incident retention truncated',
        detail: `${status.incidents.truncated_rows ?? 0} Provider-owned incident rows are no longer retained.`,
      })
    }
  }

  for (const blocker of status.blockers.slice(0, 8)) {
    alerts.push({
      key: `blocker-${blocker}`,
      color: 'blue',
      title: 'Current provider blocker',
      detail: displayCode(blocker),
    })
  }
  if (status.blockers.length > 8) {
    alerts.push({
      key: 'additional-blockers',
      color: 'blue',
      title: 'Additional provider blockers',
      detail: `${status.blockers.length - 8} more bounded blocker codes are present in current status.`,
    })
  }
  return alerts
}
