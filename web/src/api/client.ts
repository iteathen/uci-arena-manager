import type {
  ArenaEvent,
  CommandEnvelope,
  Resource,
  ResourceKind,
  ServiceDiscovery,
  ServiceProfile,
  SystemStatus,
} from './types'

const csrfKey = 'arena.csrf'

export class ApiError extends Error {
  constructor(
    message: string,
    public readonly status: number,
    public readonly code: string,
  ) {
    super(message)
  }
}

async function request<T>(path: string, init?: RequestInit): Promise<T> {
  const csrf = sessionStorage.getItem(csrfKey)
  const response = await fetch(path, {
    ...init,
    credentials: 'same-origin',
    headers: {
      Accept: 'application/json',
      ...(init?.body ? { 'Content-Type': 'application/json' } : {}),
      ...(csrf && init?.method && init.method !== 'GET' ? { 'X-Arena-CSRF': csrf } : {}),
      ...init?.headers,
    },
  })
  const body = (await response.json()) as Record<string, unknown>
  if (!response.ok) {
    throw new ApiError(
      String(body.detail ?? body.code ?? 'Request failed'),
      response.status,
      String(body.code ?? 'request_failed'),
    )
  }
  return body as T
}

export async function exchangePairingToken(): Promise<boolean> {
  const match = window.location.hash.match(/^#pair=([a-f0-9]{64})$/)
  if (!match) return false
  const paired = await request<{ csrf_token: string }>('/api/v1/auth/pair', {
    method: 'POST',
    body: JSON.stringify({ token: match[1] }),
  })
  sessionStorage.setItem(csrfKey, paired.csrf_token)
  history.replaceState(null, '', `${window.location.pathname}${window.location.search}`)
  return true
}

export const api = {
  status: () => request<SystemStatus>('/api/v1/system/status'),
  resources: <T extends Record<string, unknown>>(kind: ResourceKind) =>
    request<Array<Resource<T>>>(`/api/v1/resources/${kind}`),
  events: (cursor = 0) =>
    request<ArenaEvent[]>(`/api/v1/events/snapshot?cursor=${cursor}`),
  serviceProfiles: () => request<ServiceProfile[]>('/api/v1/service-profiles'),
  discoverService: (profileId: ServiceProfile['profile_id']) =>
    request<ServiceDiscovery>(`/api/v1/service-profiles/${profileId}/discovery`),
  command: <T = Resource>(envelope: CommandEnvelope) =>
    request<{ status: string; command_id: string; resource: T }>('/api/v1/commands', {
      method: 'POST',
      body: JSON.stringify(envelope),
    }),
}

export function makeCommand(
  action: string,
  kind: string,
  parameters: Record<string, unknown>,
  dryRun = false,
  targetId: string | null = null,
  expectedRevision: number | null = null,
): CommandEnvelope {
  const id = crypto.randomUUID()
  return {
    schema_version: 1,
    request_id: id,
    idempotency_key: `dashboard-${id}`,
    target: { kind, id: targetId },
    action,
    expected_revision: expectedRevision,
    dry_run: dryRun,
    parameters,
  }
}

export function connectEvents(
  onEvents: (events: ArenaEvent[]) => void,
  onState: (connected: boolean) => void,
): () => void {
  let socket: WebSocket | undefined
  let timer: number | undefined
  let closed = false
  let cursor = 0

  const connect = () => {
    if (closed) return
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:'
    socket = new WebSocket(`${protocol}//${window.location.host}/api/v1/events`)
    socket.onopen = () => onState(true)
    socket.onmessage = (message) => {
      const frame = JSON.parse(message.data) as { events?: ArenaEvent[] }
      if (frame.events?.length) {
        cursor = frame.events.at(-1)?.cursor ?? cursor
        onEvents(frame.events)
      }
    }
    socket.onclose = () => {
      onState(false)
      if (!closed) timer = window.setTimeout(connect, 1500)
    }
    socket.onerror = () => socket?.close()
    timer = window.setInterval(() => {
      if (socket?.readyState === WebSocket.OPEN) {
        socket.send(JSON.stringify({ cursor }))
      }
    }, 2000)
  }
  connect()
  return () => {
    closed = true
    if (timer) window.clearTimeout(timer)
    socket?.close()
  }
}
