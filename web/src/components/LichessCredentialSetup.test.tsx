import { MantineProvider } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import { LichessCredentialSetup } from './LichessCredentialSetup'

beforeEach(() => {
  sessionStorage.setItem('arena.picker.port', '47832')
  sessionStorage.setItem('arena.picker.token', 'a'.repeat(64))
  sessionStorage.setItem('arena.setup.capability', 'enabled')
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
  sessionStorage.clear()
  vi.restoreAllMocks()
})

function renderSetup() {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false }, mutations: { retry: false } },
  })
  return render(
    <MantineProvider>
      <QueryClientProvider client={client}>
        <LichessCredentialSetup />
      </QueryClientProvider>
    </MantineProvider>,
  )
}

describe('Lichess credential setup', () => {
  it('defaults to direct masked entry and hides the existing-file locator', async () => {
    renderSetup()
    expect((screen.getByLabelText('Lichess API key') as HTMLInputElement).type).toBe('password')
    expect(screen.queryByLabelText('Existing Lichess token file')).toBeNull()

    fireEvent.click(screen.getByRole('button', { name: 'Use an existing key file instead' }))
    expect(await screen.findByLabelText('Existing Lichess token file')).toBeTruthy()
  })

  it('reviews without a key body, then sends the key only as octet-stream', async () => {
    const fixture = 'synthetic-component-entry-value'
    const fetchMock = vi
      .spyOn(globalThis, 'fetch')
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            schema: 'arena_ui_credential_plan_v1',
            credential: 'lichess_bot_token',
            storage: 'private_operator_external_file',
            provider_startup_must_be_disabled: true,
            actions: [
              { action: 'materialize_private_operator_file' },
              { action: 'commit_installer_locator' },
            ],
          }),
          { status: 200 },
        ),
      )
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            schema: 'arena_ui_credential_result_v1',
            state: 'committed',
            credential: 'lichess_bot_token',
            locator_committed: true,
            consumer_configuration_regenerated: true,
          }),
          { status: 200 },
        ),
      )

    renderSetup()
    const input = screen.getByLabelText('Lichess API key') as HTMLInputElement
    fireEvent.change(input, { target: { value: fixture } })
    fireEvent.click(screen.getByRole('button', { name: 'Review and save API key' }))
    fireEvent.click(await screen.findByRole('button', { name: 'Confirm and save' }))

    await waitFor(() => expect(input.value).toBe(''))
    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      'http://127.0.0.1:47832/v1/bot-credential-plan',
    )
    expect(String(fetchMock.mock.calls[0]?.[1]?.body)).not.toContain(fixture)
    expect(fetchMock.mock.calls[1]?.[0]).toBe(
      'http://127.0.0.1:47832/v1/bot-credential-apply',
    )
    expect(fetchMock.mock.calls[1]?.[1]?.body).toBe(fixture)
    expect(fetchMock.mock.calls[1]?.[1]?.headers).toMatchObject({
      'Content-Type': 'application/octet-stream',
    })
  })
})
