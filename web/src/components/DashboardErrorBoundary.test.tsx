import '@testing-library/jest-dom/vitest'
import { MantineProvider } from '@mantine/core'
import { cleanup, fireEvent, render, screen } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { DashboardErrorBoundary } from './DashboardErrorBoundary'

beforeEach(() => {
  Object.defineProperty(window, 'matchMedia', {
    configurable: true,
    value: vi.fn().mockImplementation(() => ({
      matches: false,
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
    })),
  })
})

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
})

function BrokenPage(): never {
  throw new Error('lazy page unavailable')
}

describe('DashboardErrorBoundary', () => {
  it('renders an actionable recovery surface instead of a blank window', () => {
    const reload = vi.fn()
    const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined)

    render(
      <MantineProvider>
        <DashboardErrorBoundary reload={reload}>
          <BrokenPage />
        </DashboardErrorBoundary>
      </MantineProvider>,
    )

    expect(screen.getByRole('heading', { name: 'Dashboard update did not load' })).toBeVisible()
    expect(screen.getByText('Manager is still running')).toBeVisible()
    fireEvent.click(screen.getByRole('button', { name: 'Reload dashboard' }))
    expect(reload).toHaveBeenCalledOnce()

    consoleError.mockRestore()
  })
})
