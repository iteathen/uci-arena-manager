import type { LocalSetupRequest } from '../api/client'

type ImportRequest = Extract<LocalSetupRequest, { operation: 'import' }>

export function localSetupImportReviewText(assetKind: ImportRequest['asset_kind']) {
  if (assetKind === 'lichess_token_path') {
    return 'The adjacent Installer validates this external credential reference using filesystem metadata only and updates every affected consumer as one transaction. Token contents are never opened or copied.'
  }
  return "The adjacent Installer validates this asset reference and updates every affected consumer as one transaction. The reviewed import policy preserves the asset's declared ownership."
}
