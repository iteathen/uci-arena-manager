import { MantineProvider } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { EnginesPage } from './EnginesPage'

beforeEach(() => {
  class ResizeObserverMock {
    observe() {}
    unobserve() {}
    disconnect() {}
  }
  vi.stubGlobal('ResizeObserver', ResizeObserverMock)
  Object.defineProperty(window, 'matchMedia', {
    configurable: true,
    value: vi.fn().mockImplementation(() => ({
      matches: false,
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
    })),
  })
  vi.spyOn(globalThis, 'fetch').mockImplementation(
    () => new Promise<Response>(() => undefined),
  )
})

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

function renderPage() {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false }, mutations: { retry: false } },
  })
  return render(
    <MantineProvider>
      <QueryClientProvider client={client}>
        <EnginesPage navigate={vi.fn()} />
      </QueryClientProvider>
    </MantineProvider>,
  )
}

describe('Engine inventory loading state', () => {
  it('requires operator clock selection and sends it without rewriting profile options', async () => {
    const submitted: Array<Record<string, unknown>> = []
    vi.mocked(globalThis.fetch).mockImplementation(async (input, init) => {
      const url = String(input)
      if (init?.method === 'POST') submitted.push(JSON.parse(String(init.body)))
      const body = url.includes('/engine-candidates') ? {
        candidates: [{ schema: 'arena_engine_candidate_v1', schema_version: 1,
          candidate_id: 'engcand-clock', source: 'install_receipt', state: 'ready_to_probe',
          display_name: 'Clock engine', executable: 'E:/engine/actor.exe', working_directory: 'E:/engine', arguments: [],
          uci_options: { OriginalClockMs: 0, RequirePolicy: true }, issues: [],
          clock_option_bindings: { schema: 'arena_uci_clock_option_bindings_v1', schema_version: 1, bindings: { OriginalClockMs: 'initial_ms' } },
        }],
      } : url.includes('/components') ? { components: [] } : url.includes('/engine-probes') ? { probe_id: 'probe-clock' } : []
      return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } })
    })
    renderPage()
    const initial = await screen.findByLabelText('Test initial time (ms)')
    const test = screen.getByRole('button', { name: 'Test only' }) as HTMLButtonElement
    const register = screen.getByRole('button', { name: 'Register engine' }) as HTMLButtonElement
    expect((initial as HTMLInputElement).value).toBe('')
    expect(test.disabled).toBe(true)
    expect(register.disabled).toBe(true)
    fireEvent.change(initial, { target: { value: '180000' } })
    expect(test.disabled).toBe(false)
    fireEvent.click(test)
    await waitFor(() => expect(submitted).toHaveLength(1))
    expect(submitted[0]).toMatchObject({ candidate_id: 'engcand-clock', time_control: { initial_ms: 180000, increment_ms: 0 }, uci_options: { OriginalClockMs: 0, RequirePolicy: true } })
    await screen.findByText('Test passed')
    fireEvent.click(register)
    await waitFor(() => expect(submitted).toHaveLength(2))
    expect(submitted[1]).toMatchObject({ action: 'engine.register', parameters: { time_control: { initial_ms: 180000, increment_ms: 0 }, uci_options: { OriginalClockMs: 0, RequirePolicy: true } } })
  })
  it('does not present pending queries as an empty inventory', () => {
    renderPage()

    expect(screen.getByText('Checking')).toBeTruthy()
    expect(
      screen.getByText('Checking installed engine receipts and saved locators…'),
    ).toBeTruthy()
    expect(screen.getByText('Checking registered engines…')).toBeTruthy()
    expect(screen.queryByText('No engines registered')).toBeNull()
    expect(screen.queryByText(/0 candidates/i)).toBeNull()
  })

  it('offers ModelRoot selection separately from the packaged default', async () => {
    vi.mocked(globalThis.fetch).mockImplementation(async (input) => {
      const url = typeof input === 'string'
        ? input
        : input instanceof URL
          ? input.toString()
          : input.url
      const body = url.includes('/api/v1/engine-candidates')
        ? {
            schema: 'arena_engine_candidates_v1',
            schema_version: 1,
            precedence: ['install_receipt'],
            candidates: [{
              schema: 'arena_engine_candidate_v1',
              schema_version: 1,
              candidate_id: 'engcand-managed',
              source: 'install_receipt',
              state: 'ready_to_probe',
              display_name: 'UCI Arena Engine',
              executable: 'C:\\UCI Arena\\uci_arena_engine.exe',
              working_directory: 'C:\\UCI Arena',
              arguments: [],
              uci_options: { ModelRoot: 'C:\\UCI Arena\\models\\default' },
              component_id: 'uci_arena.engine',
              component_version: '1.0.0',
              recommended: true,
              default_model: {
                model_id: 'latticeknight',
                display_name: 'LatticeKnight',
                input_adapter_id: 'dense_planes_v1',
                root: 'models/default',
              },
              launch_profile: {
                schema: 'arena_uci_engine_launch_profile_v1',
                state: 'qualified_timing',
                timing_profile: { available: true, identity_sha256: 'a'.repeat(64) },
                expected_runtime: null,
              },
              issues: [],
            }],
          }
        : url.includes('/api/v1/components')
          ? { schema_version: 1, components: [] }
          : []
      return new Response(JSON.stringify(body), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      })
    })

    renderPage()

    const modelRoot = await screen.findByLabelText('Model root')
    expect((modelRoot as HTMLInputElement).value).toBe(
      'C:\\UCI Arena\\models\\default',
    )
    expect(screen.getByText(/Packaged default: LatticeKnight/)).toBeTruthy()

    fireEvent.change(modelRoot, { target: { value: 'C:\\models\\custom' } })
    expect(screen.getByText('Timing evidence retained')).toBeTruthy()
    expect(screen.getByText(/selected timing profile remains active/)).toBeTruthy()
    expect(screen.getByText(/marginal estimate of how additional search time improves move quality/)).toBeTruthy()
    expect(screen.getByText('Reset to packaged default')).toBeTruthy()
  })
})
