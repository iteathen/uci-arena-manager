export function isNamedModelTestFixture(relativePath) {
  return /(?:^|[\\/])[^\\/]+\.(?:test|spec)\.(?:[cm]?[jt]sx?|c(?:pp)?|h(?:pp)?)$/iu.test(relativePath)
}
