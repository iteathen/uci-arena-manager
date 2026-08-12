import { describe, expect, it } from 'vitest'
import { localSetupImportReviewText } from './localSetupImportModel'

describe('local setup import review copy', () => {
  it('reserves credential language for the token-file reference', () => {
    const token = localSetupImportReviewText('lichess_token_path')
    const openingBook = localSetupImportReviewText('opening_book')
    const timing = localSetupImportReviewText('timing_profile')

    expect(token).toContain('credential reference')
    expect(token).toContain('Token contents are never opened or copied')
    expect(openingBook).not.toMatch(/credential|token/i)
    expect(timing).not.toMatch(/credential|token/i)
  })

  it('states the transactional ownership contract for non-secret assets', () => {
    expect(localSetupImportReviewText('opening_book')).toContain(
      "preserves the asset's declared ownership",
    )
    expect(localSetupImportReviewText('timing_profile')).toContain(
      'updates every affected consumer as one transaction',
    )
  })
})
