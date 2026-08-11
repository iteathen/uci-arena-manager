import { MantineProvider } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { cleanup, fireEvent, render, screen } from '@testing-library/react'
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
                state: 'conservative',
                timing_profile: { available: false },
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
    expect(screen.getByText('Custom model selected')).toBeTruthy()
    expect(screen.getByText('Reset to packaged default')).toBeTruthy()
  })
})
