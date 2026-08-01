export function managerEndpointLabel(location: Pick<Location, 'host'>) {
  return location.host || 'unavailable'
}
