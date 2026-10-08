import { MantineProvider } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { cleanup, fireEvent, render, screen, waitFor, within } from '@testing-library/react'
import { afterEach, beforeEach, expect, it, vi } from 'vitest'
import { ServicesPage } from './ServicesPage'

beforeEach(() => {
  Object.defineProperty(window.HTMLElement.prototype, 'scrollIntoView', { configurable: true, value: vi.fn() })
  vi.stubGlobal('ResizeObserver', class { observe() {} unobserve() {} disconnect() {} })
  Object.defineProperty(window, 'matchMedia', { configurable: true, value: vi.fn(() => ({
    matches: false, addEventListener: vi.fn(), removeEventListener: vi.fn(),
  })) })
})
afterEach(() => { cleanup(); vi.restoreAllMocks(); vi.unstubAllGlobals() })

it('can add another engine after a target exists and retains mixed-profile campaign access', async () => {
  const commands: Array<Record<string, unknown>> = []
  const service = { id: 'service', revision: 1, kind: 'managed_service', labels: {}, data: {
    profile_id: 'uci_arena_evidence', display_name: 'Evidence', integration_state: 'available',
    observed_state: 'stopped', capabilities: [], health: { state: 'healthy' },
    orchestration_projection: { schema: 'uci_arena_evidence_orchestration_v1', jobs: [], targets: [
      { id: 'current-target', label: 'Current GPU engine', target_schema: 'uci_arena_evidence_target_v2',
        campaigns: [{ name: 'engine.diagnostic', parameters_schema: { type: 'object', required: [] } }] },
      { id: 'retained-target', label: 'Retained deployment' },
    ] },
  } }
  const engines = ['GPU engine', 'Alternative engine'].map((name, index) => ({ id: `engine-${index}`, data: {
    display_name: name, component_id: `supplier-${index}`, onboarding_source: 'install_receipt',
    evidence_runtime: { available: true },
  } }))
  vi.spyOn(globalThis, 'fetch').mockImplementation(async (input, init) => {
    const url = typeof input === 'string' ? input : input instanceof URL ? input.toString() : input.url
    let body: unknown = []
    if (url.endsWith('/api/v1/resources/managed_service')) body = [service]
    else if (url.endsWith('/api/v1/resources/engine')) body = engines
    else if (url.endsWith('/api/v1/components')) body = { components: [], verification_state: 'verified' }
    else if (init?.method === 'POST') { commands.push(JSON.parse(String(init.body))); body = { status: 'completed', resource: service } }
    return new Response(JSON.stringify(body), { status: 200, headers: { 'Content-Type': 'application/json' } })
  })
  const client = new QueryClient({ defaultOptions: { queries: { retry: false }, mutations: { retry: false } } })
  render(<MantineProvider><QueryClientProvider client={client}><ServicesPage navigate={vi.fn()} /></QueryClientProvider></MantineProvider>)
  const selectOption = async (label: string, option: string) => {
    const input = await screen.findByLabelText(label, { selector: 'input' })
    fireEvent.click(input)
    const listbox = document.querySelector(`[role="listbox"][aria-labelledby="${input.id}-label"]`)
    expect(listbox).not.toBeNull()
    fireEvent.click(within(listbox as HTMLElement).getByText(option))
  }
  await selectOption('Engine for Evidence', 'Alternative engine')
  fireEvent.click(screen.getByRole('button', { name: 'Use selected engine' }))
  await waitFor(() => expect(commands.some(command => command.action === 'service.evidence_target_register')).toBe(true))
  expect(commands.find(command => command.action === 'service.evidence_target_register')?.parameters).toEqual({ engine_id: 'engine-1' })
  await selectOption('Evidence target', 'Retained deployment')
  fireEvent.click(screen.getByRole('button', { name: 'Run first evidence campaign' }))
  await waitFor(() => expect(commands.some(command => command.action === 'service.evidence_campaign_enqueue')).toBe(true))
  expect(commands.find(command => command.action === 'service.evidence_campaign_enqueue')?.parameters).toEqual({ target_id: 'retained-target' })
})
