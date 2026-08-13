import { MantineProvider } from '@mantine/core'
import { QueryClient, QueryClientProvider } from '@tanstack/react-query'
import { cleanup, render, screen } from '@testing-library/react'
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest'
import type { BotAccountAction, BotOptions } from '../api/types'
import { BotOperationsPanel } from './BotOperationsPanel'

const options: BotOptions = {
  schema: 'uci_arena_lichess_manager_options_v1',
  capacity: {
    total_slots: 2,
    startup_parallelism: 2,
    human: { slots: 1, allow_borrowing: false, accept_public: true },
    bot: { slots: 1, target_active: 0 },
    evidence: { status: 'qualified', qualified: true, maximum_concurrency: 2 },
  },
  inbound_challenges: {
    accept_bots: true,
    min_base: 600,
    max_base: 1800,
    min_increment: 0,
    max_increment: 10,
    speeds: ['rapid'],
    modes: ['casual'],
  },
  outbound_challenge: {
    rated: false,
    color: 'random',
    clock_limit: 600,
    clock_increment: 2,
  },
  scheduler: {
    enabled: false,
    discovery_interval_seconds: 60,
    challenge_interval_seconds: 5,
    reservation_retry_seconds: 30,
    challenge_timeout_seconds: 25,
    first_move_timeout_seconds: 60,
  },
  opponent_selection: {
    max_games_per_opponent_per_day: 1,
    timeout_cooldown_minutes: 30,
    decline_cooldown_minutes: 360,
    rating_target: 2200,
    rating_spread: 600,
  },
}

beforeEach(() => {
  class ResizeObserverMock {
    observe() {}
    unobserve() {}
    disconnect() {}
  }
  vi.stubGlobal('ResizeObserver', ResizeObserverMock)
  sessionStorage.setItem('arena.picker.port', '47832')
  sessionStorage.setItem('arena.picker.token', 'a'.repeat(64))
  Object.defineProperty(window, 'matchMedia', {
    configurable: true,
    value: vi.fn().mockImplementation(() => ({
      matches: false,
      addEventListener: vi.fn(),
      removeEventListener: vi.fn(),
    })),
  })
  vi.spyOn(globalThis, 'fetch').mockImplementation(async (_input, init) => {
    const body = JSON.parse(String(init?.body ?? '{}')) as {
      operation?: string
      action?: BotAccountAction
    }
    if (body.operation === 'get') {
      return new Response(JSON.stringify(options), { status: 200 })
    }
    const base = {
      schema: 'arena_bot_local_account_result_v1',
      schema_version: 1,
      action: body.action,
    }
    if (body.action === 'owner_list') {
      return new Response(JSON.stringify({ ...base, owners: [] }), { status: 200 })
    }
    if (body.action === 'reservation_status') {
      return new Response(JSON.stringify({ ...base, reservation: null }), { status: 200 })
    }
    if (body.action === 'scheduler_status') {
      return new Response(JSON.stringify({ ...base, scheduler: { paused: false } }), { status: 200 })
    }
    return new Response(JSON.stringify({ code: 'unexpected_test_action' }), { status: 400 })
  })
})

afterEach(() => {
  cleanup()
  sessionStorage.clear()
  vi.restoreAllMocks()
  vi.unstubAllGlobals()
})

function renderPanel() {
  const client = new QueryClient({
    defaultOptions: { queries: { retry: false }, mutations: { retry: false } },
  })
  return render(
    <MantineProvider>
      <QueryClientProvider client={client}>
        <BotOperationsPanel installed providerActive={false} navigate={vi.fn()} />
      </QueryClientProvider>
    </MantineProvider>,
  )
}

describe('Lichess provider control surface', () => {
  it('renders every declared policy group and does not call disabled scheduling active', async () => {
    renderPanel()

    expect(await screen.findByText('DISABLED')).toBeTruthy()
    expect(
      (screen.getByRole('button', { name: 'Pause scheduler' }) as HTMLButtonElement).disabled,
    ).toBe(true)
    for (const label of [
      'Parallel engine startups',
      'Human slots',
      'Bot slots',
      'Target active bot games',
      'Human slots may borrow unused bot capacity',
      'Accept public human challenges',
      'Accept challenges from bots',
      'Minimum base time (seconds)',
      'Maximum base time (seconds)',
      'Minimum increment (seconds)',
      'Maximum increment (seconds)',
      'Accepted speeds',
      'Accepted modes',
      'Rated games',
      'Play behavior',
      'Preferred color',
      'Clock base (seconds)',
      'Clock increment (seconds)',
      'Discovery interval (seconds)',
      'Challenge interval (seconds)',
      'Reservation retry (seconds)',
      'Challenge timeout (seconds)',
      'First-move timeout (seconds)',
      'Games per opponent / day',
      'Timeout cooldown (minutes)',
      'Decline cooldown (minutes)',
      'Rating target',
      'Rating spread',
    ]) {
      expect(screen.getAllByLabelText(label).length).toBeGreaterThan(0)
    }
  })
})
