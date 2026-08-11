import type { ManagedServiceData, Resource } from '../api/types'

export function selectPreferredService(
  services: Array<Resource<ManagedServiceData>>,
  profileId: string,
  componentId: string,
) {
  const candidates = services.filter(
    (service) => service.data.profile_id === profileId,
  )
  return (
    candidates.find(
      (service) => service.data.installed_component_id === componentId,
    ) ?? candidates[0]
  )
}

export function canEnqueueEvidenceCampaign(service: ManagedServiceData) {
  return evidenceCampaignBlocker(service) === null
}

export function evidenceCampaignBlocker(service: ManagedServiceData) {
  if (service.integration_state !== 'available' || !service.orchestration_projection) {
    return 'Evidence orchestration is not currently available.'
  }
  if (needsEvidenceTarget(service)) {
    return 'Start or register an exact compatible engine deployment first.'
  }
  if (service.orchestration_projection.jobs.length > 0) {
    return 'The first campaign already has jobs; use their job controls.'
  }
  return null
}

export function needsEvidenceTarget(service: ManagedServiceData) {
  return (
    service.status_projection?.collection?.state === 'waiting_for_target' &&
    (service.orchestration_projection?.targets.length ?? 0) === 0
  )
}

export function serviceCardStates(
  packageState: string | undefined,
  configured: boolean,
  observed: string | undefined,
  packageLoading: boolean,
  instancesLoading: boolean,
) {
  return {
    package: packageLoading && !packageState
      ? 'PACKAGE CHECKING'
      : `PACKAGE ${(packageState ?? 'unavailable').toUpperCase()}`,
    locator: instancesLoading
      ? 'LOCATOR CHECKING'
      : configured
        ? 'LOCATOR CONFIGURED'
        : 'LOCATOR NOT CONFIGURED',
    runtime: instancesLoading
      ? 'RUNTIME CHECKING'
      : configured
        ? `RUNTIME ${(observed ?? 'unknown').toUpperCase()}`
        : 'RUNTIME NOT CONFIGURED',
  }
}

export function serviceLocatorMode(service?: Resource<ManagedServiceData>) {
  if (!service) return 'configure' as const
  return service.data.installed_component_id ? 'view_installed' as const : 'edit_manual' as const
}

export type ServiceLifecycleAction = 'start' | 'stop' | 'pause' | 'resume'

export function serviceLifecycleActions(
  service: ManagedServiceData,
): ServiceLifecycleAction[] {
  const observed = service.observed_state
  const startupOwned =
    service.startup_control === 'provider_owned_per_user' ||
    service.startup_installed === true
  if (startupOwned) {
    if (observed === 'paused') return ['resume']
    if (observed === 'running' || observed === 'degraded') return ['pause']
    return []
  }
  if (observed === 'running' || observed === 'degraded') return ['pause', 'stop']
  if (observed === 'paused') return ['resume', 'stop']
  if (observed === 'starting') return ['stop']
  return service.integration_state === 'incompatible' ? [] : ['start']
}
