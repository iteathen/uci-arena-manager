import type { LocalSetupResult } from '../api/client'

export function managerEndpointLabel(location: Pick<Location, 'host'>) {
  return location.host || 'unavailable'
}

export function setupCommitNotice(plan: LocalSetupResult) {
  const authority = plan.elevation_required
    ? 'Administrator confirmation is required for this all-users installation.'
    : 'This personal installation commits as the current user without UAC.'
  return plan.manager_service_restart
    ? `${authority} Manager will restart and reconnect after the verified transaction.`
    : `${authority} Manager will refresh after the verified transaction.`
}
