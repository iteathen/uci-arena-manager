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
