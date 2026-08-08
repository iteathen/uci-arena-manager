import type {
  ArenaEvent,
  BotCandidates,
  BotAccountAction,
  BotAccountResult,
  BotControlAction,
  BotControlResult,
  BotProbeResult,
  BotOptions,
  BotOptionsUpdate,
  BotProfile,
  CommandEnvelope,
  ComponentInventory,
  EngineCandidates,
  EngineProbeResult,
  Resource,
  ResourceKind,
  ServiceDiscovery,
  ServiceProfile,
  SystemStatus,
} from './types'

const csrfKey = 'arena.csrf'
const pickerPortKey = 'arena.picker.port'
const pickerTokenKey = 'arena.picker.token'
const setupCapabilityKey = 'arena.setup.capability'

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
  const parameters = new URLSearchParams(window.location.hash.replace(/^#/, ''))
  const pairToken = parameters.get('pair')
  const paired = pairToken?.match(/^[a-f0-9]{64}$/)
    ? await request<{ csrf_token: string }>('/api/v1/auth/pair', {
        method: 'POST',
        body: JSON.stringify({ token: pairToken }),
      })
    : await request<{ csrf_token: string }>('/api/v1/auth/session')
  sessionStorage.setItem(csrfKey, paired.csrf_token)
  if (!pairToken) return false
  const pickerPort = parameters.get('picker_port')
  const pickerToken = parameters.get('picker_token')
  if (
    pickerPort?.match(/^\d{4,5}$/) &&
    Number(pickerPort) >= 1024 &&
    Number(pickerPort) <= 65535 &&
    pickerToken?.match(/^[a-f0-9]{64}$/)
  ) {
    sessionStorage.setItem(pickerPortKey, pickerPort)
    sessionStorage.setItem(pickerTokenKey, pickerToken)
    if (parameters.get('setup') === '1') {
      sessionStorage.setItem(setupCapabilityKey, 'enabled')
    } else {
      sessionStorage.removeItem(setupCapabilityKey)
    }
  } else {
    sessionStorage.removeItem(pickerPortKey)
    sessionStorage.removeItem(pickerTokenKey)
    sessionStorage.removeItem(setupCapabilityKey)
  }
  history.replaceState(null, '', `${window.location.pathname}${window.location.search}`)
  return true
}

export type LocalSetupRequest =
  | {
      operation: 'discover'
      roots?: string[]
    }
  | {
      operation: 'import'
      asset_kind:
        | 'opening_book'
        | 'syzygy'
        | 'uci_engine'
        | 'book_forge'
        | 'evidence_service'
        | 'timing_profile'
        | 'lichess_single_game_profile'
        | 'lichess_capacity_profile'
        | 'lichess_transport_profile'
        | 'lichess_token_path'
      path: string
      managed_copy?: boolean
    }
  | {
      operation: 'component'
      component_id:
        | 'uci_arena.engine'
        | 'stockfish.engine'
        | 'node_runtime.private'
        | 'book_forge.service'
        | 'opening_book.standard'
        | 'uci_arena_evidence.runtime'
        | 'uci_arena_evidence.service'
        | 'lichess.adapter'
        | 'syzygy.wdl.3-4-5'
        | 'syzygy.dtz.3-4-5'
    }

export interface LocalSetupResult {
  schema: string
  state?: string
  operation?: string
  elevation_required?: boolean
  manager_service_restart?: boolean
  disk_impact_bytes?: number | null
  actions?: Array<Record<string, unknown>>
  discovery?: Record<string, LocalAssetDiscovery>
  [key: string]: unknown
}

export interface LocalAssetCandidate {
  source: string
  state: string
  path: string
  details?: Record<string, unknown>
}

export interface LocalIndexedSearch {
  attempted?: boolean
  adapter?: string
  state?: string
  detail_code?: string
  maximum_results?: number
  returned_candidates?: number
  total_returned_candidates?: number
  accepted_candidates?: number
  rejected_candidates?: number
  elapsed_ms?: number
  truncated?: boolean
}

export interface LocalAssetDiscovery {
  selection?: LocalAssetCandidate | null
  candidates?: LocalAssetCandidate[]
  indexed_search?: LocalIndexedSearch
}

export function localSetupAvailable() {
  return sessionStorage.getItem(setupCapabilityKey) === 'enabled'
}

async function localHelperRequest<T>(path: string, body: Record<string, unknown>): Promise<T> {
  const port = sessionStorage.getItem(pickerPortKey)
  const token = sessionStorage.getItem(pickerTokenKey)
  if (!port || !token) {
    throw new ApiError(
      'Open UCI Arena Manager from its desktop launcher and keep it open.',
      503,
      'interactive_helper_unavailable',
    )
  }
  let response: Response
  try {
    response = await fetch(`http://127.0.0.1:${port}${path}`, {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(body),
    })
  } catch {
    throw new ApiError(
      'The interactive-user helper is no longer running. Reopen UCI Arena Manager and try again.',
      503,
      'interactive_helper_unavailable',
    )
  }
  const result = (await response.json()) as T & { code?: string; detail?: string }
  if (!response.ok) {
    throw new ApiError(
      result.detail ?? 'The local setup operation failed',
      response.status,
      result.code ?? 'setup_failed',
    )
  }
  return result
}

export function planLocalSetup(request: LocalSetupRequest) {
  if (!localSetupAvailable()) {
    throw new ApiError(
      'Typed setup is available only in the native desktop application.',
      503,
      'setup_capability_unavailable',
    )
  }
  return localHelperRequest<LocalSetupResult>('/v1/setup-plan', {
    schema: 'arena_ui_setup_request_v1',
    ...request,
  })
}

export function discoverLocalAssets(roots: string[] = []) {
  return planLocalSetup(
    roots.length > 0 ? { operation: 'discover', roots } : { operation: 'discover' },
  )
}

export interface LocalCredentialPlan {
  schema: 'arena_ui_credential_plan_v1'
  credential: 'lichess_bot_token'
  storage: 'private_operator_external_file'
  provider_startup_must_be_disabled: true
  actions: Array<{ action: string }>
}

export interface LocalCredentialResult {
  schema: 'arena_ui_credential_result_v1'
  state: 'committed'
  credential: 'lichess_bot_token'
  locator_committed: true
  consumer_configuration_regenerated: true
}

export function applyLocalSetup(request: LocalSetupRequest) {
  if (!localSetupAvailable()) {
    throw new ApiError(
      'Typed setup is available only in the native desktop application.',
      503,
      'setup_capability_unavailable',
    )
  }
  return localHelperRequest<LocalSetupResult>('/v1/setup-apply', {
    schema: 'arena_ui_setup_request_v1',
    ...request,
  })
}

export function planLocalLichessCredential(): Promise<LocalCredentialPlan> {
  if (!localSetupAvailable()) {
    throw new ApiError(
      'API-key setup is available only in the native desktop application.',
      503,
      'credential_capability_unavailable',
    )
  }
  return localHelperRequest<LocalCredentialPlan>('/v1/bot-credential-plan', {
    schema: 'arena_ui_credential_plan_request_v1',
  })
}

export async function applyLocalLichessCredential(
  credential: string,
): Promise<LocalCredentialResult> {
  const port = sessionStorage.getItem(pickerPortKey)
  const token = sessionStorage.getItem(pickerTokenKey)
  if (!localSetupAvailable() || !port || !token) {
    throw new ApiError(
      'Open UCI Arena Manager from its desktop launcher and keep it open to save the API key.',
      503,
      'credential_capability_unavailable',
    )
  }
  let response: Response
  try {
    response = await fetch(`http://127.0.0.1:${port}/v1/bot-credential-apply`, {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/octet-stream',
      },
      body: credential,
    })
  } catch {
    throw new ApiError(
      'The interactive-user helper is no longer running. Reopen UCI Arena Manager and try again.',
      503,
      'interactive_helper_unavailable',
    )
  }
  const result = (await response.json()) as LocalCredentialResult & {
    code?: string
    detail?: string
  }
  if (!response.ok) {
    throw new ApiError(
      result.detail ?? 'The API key could not be saved',
      response.status,
      result.code ?? 'credential_setup_failed',
    )
  }
  return result
}

export interface LocalPathFilter {
  name: string
  patterns: string[]
}

export async function pickLocalPath(options: {
  mode: 'file' | 'directory'
  title: string
  filters?: LocalPathFilter[]
}): Promise<string | null> {
  const port = sessionStorage.getItem(pickerPortKey)
  const token = sessionStorage.getItem(pickerTokenKey)
  if (!port || !token) {
    throw new ApiError(
      'Run arena ui and keep it open to use native browse dialogs, or enter the path manually.',
      503,
      'picker_unavailable',
    )
  }
  let response: Response
  try {
    response = await fetch(`http://127.0.0.1:${port}/v1/pick`, {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        schema: 'arena_ui_picker_request_v1',
        ...options,
        filters: options.filters ?? [],
      }),
    })
  } catch {
    throw new ApiError(
      'The arena ui browse helper is no longer running. Restart arena ui or enter the path manually.',
      503,
      'picker_unavailable',
    )
  }
  const body = (await response.json()) as {
    status?: string
    path?: string | null
    code?: string
    detail?: string
  }
  if (!response.ok || body.status === 'error') {
    throw new ApiError(
      body.detail ?? 'Native browse dialog failed',
      response.status,
      body.code ?? 'picker_failed',
    )
  }
  return body.status === 'selected' && body.path ? body.path : null
}

export async function probeLocalBot(): Promise<BotProbeResult> {
  const port = sessionStorage.getItem(pickerPortKey)
  const token = sessionStorage.getItem(pickerTokenKey)
  if (!port || !token) {
    throw new ApiError(
      'Open UCI Arena Manager from its desktop launcher and keep it open to verify the BOT account.',
      503,
      'interactive_helper_unavailable',
    )
  }
  let response: Response
  try {
    response = await fetch(`http://127.0.0.1:${port}/v1/bot-probe`, {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({ schema: 'arena_ui_bot_probe_request_v1' }),
    })
  } catch {
    throw new ApiError(
      'The interactive-user helper is no longer running. Reopen UCI Arena Manager and try again.',
      503,
      'interactive_helper_unavailable',
    )
  }
  const body = (await response.json()) as BotProbeResult & {
    code?: string
    detail?: string
  }
  if (!response.ok) {
    throw new ApiError(
      body.detail ?? 'BOT account verification failed',
      response.status,
      body.code ?? 'bot_probe_failed',
    )
  }
  return body
}

export async function controlLocalBot(action: BotControlAction): Promise<BotControlResult> {
  const port = sessionStorage.getItem(pickerPortKey)
  const token = sessionStorage.getItem(pickerTokenKey)
  if (!port || !token) {
    throw new ApiError(
      'Open UCI Arena Manager from its desktop launcher and keep it open to control BOT startup.',
      503,
      'interactive_helper_unavailable',
    )
  }
  let response: Response
  try {
    response = await fetch(`http://127.0.0.1:${port}/v1/bot-control`, {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        schema: 'arena_ui_bot_control_request_v1',
        action,
      }),
    })
  } catch {
    throw new ApiError(
      'The interactive-user helper is no longer running. Reopen UCI Arena Manager and try again.',
      503,
      'interactive_helper_unavailable',
    )
  }
  const body = (await response.json()) as BotControlResult & {
    code?: string
    detail?: string
  }
  if (!response.ok) {
    throw new ApiError(
      body.detail ?? 'BOT startup control failed',
      response.status,
      body.code ?? 'bot_control_failed',
    )
  }
  return body
}

async function interactiveBotRequest<T>(
  path: string,
  body: Record<string, unknown>,
  failureMessage: string,
): Promise<T> {
  const port = sessionStorage.getItem(pickerPortKey)
  const token = sessionStorage.getItem(pickerTokenKey)
  if (!port || !token) {
    throw new ApiError(
      'Open UCI Arena Manager from its desktop launcher and keep it open to manage the BOT account.',
      503,
      'interactive_helper_unavailable',
    )
  }
  let response: Response
  try {
    response = await fetch(`http://127.0.0.1:${port}${path}`, {
      method: 'POST',
      headers: {
        Accept: 'application/json',
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify(body),
    })
  } catch {
    throw new ApiError(
      'The interactive-user helper is no longer running. Reopen UCI Arena Manager and try again.',
      503,
      'interactive_helper_unavailable',
    )
  }
  const result = (await response.json()) as T & { code?: string; detail?: string }
  if (!response.ok) {
    throw new ApiError(
      result.detail ?? failureMessage,
      response.status,
      result.code ?? 'bot_control_failed',
    )
  }
  return result
}

export function getLocalBotOptions(): Promise<BotOptions> {
  return interactiveBotRequest<BotOptions>(
    '/v1/bot-options',
    {
      schema: 'arena_ui_bot_options_request_v1',
      operation: 'get',
    },
    'BOT options could not be loaded',
  )
}

export function updateLocalBotOptions(
  settings: BotOptionsUpdate,
  confirmRated: boolean,
  confirmOutbound: boolean,
): Promise<BotOptions> {
  return interactiveBotRequest<BotOptions>(
    '/v1/bot-options',
    {
      schema: 'arena_ui_bot_options_request_v1',
      operation: 'update',
      settings,
      confirm_rated: confirmRated,
      confirm_outbound: confirmOutbound,
    },
    'BOT options could not be saved',
  )
}

export function controlLocalBotAccount(
  action: BotAccountAction,
  parameters: Record<string, unknown> = {},
): Promise<BotAccountResult> {
  return interactiveBotRequest<BotAccountResult>(
    '/v1/bot-account',
    {
      schema: 'arena_ui_bot_account_request_v1',
      action,
      parameters,
    },
    'BOT account control failed',
  )
}

export const api = {
  status: () => request<SystemStatus>('/api/v1/system/status'),
  resources: <T extends Record<string, unknown>>(kind: ResourceKind) =>
    request<Array<Resource<T>>>(`/api/v1/resources/${kind}`),
  events: (cursor = 0) =>
    request<ArenaEvent[]>(`/api/v1/events/snapshot?cursor=${cursor}`),
  serviceProfiles: () => request<ServiceProfile[]>('/api/v1/service-profiles'),
  components: () => request<ComponentInventory>('/api/v1/components'),
  botProfiles: () => request<BotProfile[]>('/api/v1/bot-profiles'),
  botCandidates: () => request<BotCandidates>('/api/v1/bot-candidates'),
  discoverService: (profileId: ServiceProfile['profile_id']) =>
    request<ServiceDiscovery>(`/api/v1/service-profiles/${profileId}/discovery`),
  engineCandidates: (root = '') =>
    request<EngineCandidates>(
      `/api/v1/engine-candidates${root ? `?root=${encodeURIComponent(root)}` : ''}`,
    ),
  probeEngine: (parameters: Record<string, unknown>) =>
    request<EngineProbeResult>('/api/v1/engine-probes', {
      method: 'POST',
      body: JSON.stringify(parameters),
    }),
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
