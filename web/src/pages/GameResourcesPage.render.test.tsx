import '@testing-library/jest-dom/vitest'
import { MantineProvider } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { cleanup, fireEvent, render, screen } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { ProviderGameSummary } from '../api/types'
import { GameResourcesPage } from './GameResourcesPage'

const initialFen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
const afterFen = 'rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1'

function providerGame(
  archive_completeness: ProviderGameSummary['archive_completeness'],
): ProviderGameSummary {
  const complete = archive_completeness === 'complete'
  return {
    cursor: 646,
    game_id: complete ? 'complete-game' : 'summary-game',
    state: 'history',
    opponent: 'FixtureBot',
    color: 'white',
    rated: false,
    speed: 'blitz',
    variant: 'standard',
    clock_limit: 300,
    clock_increment: 3,
    status: 'mate',
    result: 'win',
    winner: 'white',
    plies: complete ? 1 : 0,
    started_at: 1_777_000_000_000,
    finished_at: 1_777_000_010_000,
    archive_completeness,
    incomplete_reason: complete ? null : 'historical_moves_unavailable',
    archive_sha256: complete ? `sha256:${'a'.repeat(64)}` : null,
    provider_component: {
      component_id: 'lichess.adapter',
      version: '1.0.24',
      manifest_sha256: `sha256:${'b'.repeat(64)}`,
    },
    engine_component: null,
  }
}

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
})

afterEach(() => {
  cleanup()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

function renderPage(game: ProviderGameSummary) {
  vi.spyOn(globalThis, 'fetch').mockImplementation(async (input) => {
    const url = typeof input === 'string'
      ? input
      : input instanceof URL
        ? input.toString()
        : input.url
    if (url.includes(`/games/${game.game_id}`)) {
      return new Response(JSON.stringify({
        schema: 'arena_provider_game_projection_v1',
        schema_version: 1,
        profile_id: 'lichess_bot',
        availability: 'available',
        provider_contract_version: '1.1',
        game: {
          schema: 'uci_arena_provider_game_v1',
          ...game,
          initial_fen: initialFen,
          runtime_provenance: {
            sha256: `sha256:${'c'.repeat(64)}`,
            details_available: false,
          },
          replay: [{
            ply: 0,
            uci_move: 'e2e4',
            before_fen: initialFen,
            after_fen: afterFen,
            actor_color: 'white',
            clock_ms: 300_000,
          }],
        },
      }), { status: 200 })
    }
    if (url.includes('/api/v1/providers/lichess_bot/games?')) {
      return new Response(JSON.stringify({
        schema: 'arena_provider_game_page_projection_v1',
        schema_version: 1,
        profile_id: 'lichess_bot',
        availability: 'available',
        provider_contract_version: '1.1',
        page: {
          schema: 'uci_arena_provider_game_page_v1',
          scope: 'history',
          before_cursor: 0,
          next_cursor: game.cursor,
          has_more: false,
          games: [game],
        },
      }), { status: 200 })
    }
    return new Response(JSON.stringify([]), { status: 200 })
  })
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false }, mutations: { retry: false } },
  })
  return render(
    <MantineProvider>
      <QueryClientProvider client={client}>
        <GameResourcesPage mode="history" navigate={vi.fn()} providerEnabled />
      </QueryClientProvider>
    </MantineProvider>,
  )
}

describe('Provider game projections', () => {
  it('keeps summary-only history visible without fabricating replay', async () => {
    renderPage(providerGame('summary_only'))

    expect(await screen.findByText('Lichess vs FixtureBot')).toBeTruthy()
    expect(screen.getByText('Replay unavailable')).toBeTruthy()
    expect(screen.getByText(/No position, move, or clock data is inferred/)).toBeTruthy()
    expect(screen.queryByRole('button', { name: 'Load Provider replay' })).toBeNull()
    expect(screen.getByRole('link', { name: 'Open on Lichess' })).toHaveAttribute(
      'href',
      'https://lichess.org/summary-game',
    )
  })

  it('loads complete replay only after an explicit request', async () => {
    renderPage(providerGame('complete'))

    const load = await screen.findByRole('button', { name: 'Load Provider replay' })
    expect(screen.queryByText('Provider-authored archive')).toBeNull()
    fireEvent.click(load)

    expect(await screen.findByText('Provider-authored archive')).toBeTruthy()
    expect(screen.getByText('Ply 1/1')).toBeTruthy()
    expect(screen.getByText(/does not reconstruct chess state/)).toBeTruthy()
  })
})
