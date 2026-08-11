const preloadRecoveryKey = 'arena.dashboard.preload-reload'

type PreloadErrorEvent = Event & { payload?: unknown }

export type PreloadRecoveryResult =
  | 'reloading'
  | 'already_retried'
  | 'storage_unavailable'

function failureIdentity(payload: unknown) {
  const detail = payload instanceof Error
    ? payload.message
    : typeof payload === 'string'
      ? payload
      : 'unknown preload failure'
  return detail.slice(0, 1024)
}

export function recoverDashboardGeneration(
  event: Event,
  storage: Pick<Storage, 'getItem' | 'setItem'>,
  reload: () => void,
): PreloadRecoveryResult {
  const identity = failureIdentity((event as PreloadErrorEvent).payload)
  try {
    if (storage.getItem(preloadRecoveryKey) === identity) return 'already_retried'
    storage.setItem(preloadRecoveryKey, identity)
  } catch {
    return 'storage_unavailable'
  }

  event.preventDefault()
  reload()
  return 'reloading'
}

