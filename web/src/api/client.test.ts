import { afterEach, describe, expect, it, vi } from 'vitest'
import {
  controlLocalBot,
  exchangePairingToken,
  makeCommand,
  pickLocalPath,
  probeLocalBot,
} from './client'

afterEach(() => {
  vi.restoreAllMocks()
  sessionStorage.clear()
  history.replaceState(null, '', '/')
})

describe('command envelope', () => {
  it('uses one typed envelope for dashboard mutations', () => {
    const command = makeCommand('match.plan', 'match_plan', { engine_ids: ['a', 'b'] }, true)
    expect(command.schema_version).toBe(1)
    expect(command.request_id).toMatch(/^[0-9a-f-]{36}$/)
    expect(command.idempotency_key).toContain(command.request_id)
    expect(command.target).toEqual({ kind: 'match_plan', id: null })
    expect(command.dry_run).toBe(true)
  })
})

describe('interactive-user path picker', () => {
  it('retains a valid picker session from the pairing fragment', async () => {
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValueOnce(
      new Response(JSON.stringify({ csrf_token: 'csrf' }), {
        status: 200,
        headers: { 'Content-Type': 'application/json' },
      }),
    )
    const pair = 'a'.repeat(64)
    const picker = 'b'.repeat(64)
    window.location.hash = `pair=${pair}&picker_port=47832&picker_token=${picker}`

    await expect(exchangePairingToken()).resolves.toBe(true)
    expect(fetchMock).toHaveBeenCalledWith(
      '/api/v1/auth/pair',
      expect.objectContaining({ body: JSON.stringify({ token: pair }) }),
    )
    expect(sessionStorage.getItem('arena.picker.port')).toBe('47832')
    expect(sessionStorage.getItem('arena.picker.token')).toBe(picker)
    expect(window.location.hash).toBe('')
  })

  it('uses the loopback helper and returns only an explicitly selected path', async () => {
    sessionStorage.setItem('arena.picker.port', '47832')
    sessionStorage.setItem('arena.picker.token', 'c'.repeat(64))
    const fetchMock = vi.spyOn(globalThis, 'fetch').mockResolvedValueOnce(
      new Response(
        JSON.stringify({
          schema: 'arena_ui_picker_result_v1',
          status: 'selected',
          path: 'C:\\engines\\stockfish.exe',
        }),
        { status: 200, headers: { 'Content-Type': 'application/json' } },
      ),
    )

    await expect(
      pickLocalPath({
        mode: 'file',
        title: 'Select an engine',
        filters: [{ name: 'Engine', patterns: ['*.exe'] }],
      }),
    ).resolves.toBe('C:\\engines\\stockfish.exe')
    expect(fetchMock).toHaveBeenCalledWith(
      'http://127.0.0.1:47832/v1/pick',
      expect.objectContaining({
        method: 'POST',
        headers: expect.objectContaining({ Authorization: `Bearer ${'c'.repeat(64)}` }),
      }),
    )
  })
})

describe('interactive-user BOT account control', () => {
  it('probes and controls startup only through the bearer-scoped helper', async () => {
    sessionStorage.setItem('arena.picker.port', '47832')
    sessionStorage.setItem('arena.picker.token', 'd'.repeat(64))
    const fetchMock = vi.spyOn(globalThis, 'fetch')
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            schema: 'arena_bot_probe_result_v1',
            schema_version: 1,
            state: 'passed',
            account_name: 'ArenaBot',
            is_bot: true,
            games: 12,
          }),
          { status: 200, headers: { 'Content-Type': 'application/json' } },
        ),
      )
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            schema: 'arena_bot_control_result_v1',
            schema_version: 1,
            state: 'active',
            installed: true,
            active: true,
            account_name: 'ArenaBot',
          }),
          { status: 200, headers: { 'Content-Type': 'application/json' } },
        ),
      )

    await expect(probeLocalBot()).resolves.toMatchObject({ account_name: 'ArenaBot' })
    await expect(controlLocalBot('enable_startup')).resolves.toMatchObject({
      state: 'active',
      installed: true,
    })
    expect(fetchMock).toHaveBeenNthCalledWith(
      1,
      'http://127.0.0.1:47832/v1/bot-probe',
      expect.objectContaining({
        headers: expect.objectContaining({ Authorization: `Bearer ${'d'.repeat(64)}` }),
      }),
    )
    expect(fetchMock).toHaveBeenNthCalledWith(
      2,
      'http://127.0.0.1:47832/v1/bot-control',
      expect.objectContaining({
        body: JSON.stringify({
          schema: 'arena_ui_bot_control_request_v1',
          action: 'enable_startup',
        }),
      }),
    )
  })
})

describe('native desktop typed setup', () => {
  it('retains setup capability only for a native-host pairing and sends typed fields', async () => {
    history.replaceState(
      null,
      '',
      '/#pair=' +
        'a'.repeat(64) +
        '&picker_port=47832&picker_token=' +
        'b'.repeat(64) +
        '&setup=1',
    )
    const fetchMock = vi
      .spyOn(globalThis, 'fetch')
      .mockResolvedValueOnce(
        new Response(JSON.stringify({ csrf_token: 'csrf' }), {
          status: 200,
          headers: { 'Content-Type': 'application/json' },
        }),
      )
      .mockResolvedValueOnce(
        new Response(
          JSON.stringify({
            schema: 'arena_ui_setup_plan_v1',
            operation: 'component',
            actions: [],
          }),
          {
            status: 200,
            headers: { 'Content-Type': 'application/json' },
          },
        ),
      )

    await exchangePairingToken()
    const { planLocalSetup } = await import('./client')
    await planLocalSetup({
      operation: 'component',
      component_id: 'opening_book.standard',
    })

    expect(fetchMock.mock.calls[1]?.[0]).toBe('http://127.0.0.1:47832/v1/setup-plan')
    expect(JSON.parse(String(fetchMock.mock.calls[1]?.[1]?.body))).toEqual({
      schema: 'arena_ui_setup_request_v1',
      operation: 'component',
      component_id: 'opening_book.standard',
    })
    fetchMock.mockRestore()
  })
})
