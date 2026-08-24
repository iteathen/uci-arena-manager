import type { ProviderStatusProjection } from '../api/types'

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
      detail: `${displayCode(status.scheduler.fault.kind)}${httpStatus === null ? '' : ` · HTTP ${httpStatus}`}. This is a live provider observation; durable incident history is unavailable on contract v${projection.provider_contract_version}.`,
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
