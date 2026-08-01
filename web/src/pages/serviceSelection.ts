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
  if (service.integration_state !== 'available' || !service.orchestration_projection) {
    return false
  }
  return !(
    service.status_projection?.collection?.state === 'waiting_for_target' &&
    service.orchestration_projection.targets.length === 0
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
