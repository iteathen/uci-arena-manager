import { MantineProvider } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { cleanup, render, screen } from '@testing-library/react'
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
})
