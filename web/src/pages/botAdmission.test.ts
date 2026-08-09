import { describe, expect, it } from 'vitest'
import {
  BOT_STATUS_REFRESH_MILLISECONDS,
  botStatusRefreshInterval,
} from './botAdmission'

describe('Lichess provider status freshness', () => {
  it('polls only while the verified adapter is installed', () => {
    expect(botStatusRefreshInterval(true)).toBe(BOT_STATUS_REFRESH_MILLISECONDS)
    expect(BOT_STATUS_REFRESH_MILLISECONDS).toBe(5_000)
    expect(botStatusRefreshInterval(false)).toBe(false)
  })
})
