import { afterEach, describe, expect, it, vi } from 'vitest'
import { exchangePairingToken, makeCommand, pickLocalPath } from './client'

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
