import {
  Alert,
  Button,
  Group,
  Modal,
  Paper,
  PasswordInput,
  Stack,
  Text,
} from '@mantine/core'
import { IconChevronDown, IconChevronUp, IconCopy, IconKey } from '@tabler/icons-react'
import { useMutation } from '@tanstack/react-query'
import { useState } from 'react'
import {
  applyLocalLichessCredential,
  localSetupAvailable,
  planLocalLichessCredential,
  type LocalCredentialPlan,
} from '../api/client'
import { LocalSetupImportButton } from './LocalSetupImportButton'
import { PathPickerInput } from './PathPickerInput'

function shellQuote(path: string) {
  const windows = navigator.userAgent.toLowerCase().includes('windows')
  return windows
    ? `'${path.replaceAll("'", "''")}'`
    : `'${path.replaceAll("'", `'"'"'`)}'`
}

export function LichessCredentialSetup({
  onCompleted,
}: {
  onCompleted?: () => void | Promise<void>
}) {
  const nativeSetup = localSetupAvailable()
  const [credential, setCredential] = useState('')
  const [existingPath, setExistingPath] = useState('')
  const [advanced, setAdvanced] = useState(false)
  const [plan, setPlan] = useState<LocalCredentialPlan | null>(null)
  const [message, setMessage] = useState('')
  const [copied, setCopied] = useState(false)
  const prepare = useMutation({
    mutationFn: planLocalLichessCredential,
    onSuccess: setPlan,
  })
  const apply = useMutation({
    mutationFn: () => applyLocalLichessCredential(credential),
    onSuccess: async () => {
      setCredential('')
      setPlan(null)
      setMessage('API key saved as a private external credential and the locator committed.')
      await onCompleted?.()
    },
  })

  const copyImportCommand = async () => {
    await navigator.clipboard.writeText(
      `arena-setup --json import --kind lichess_token_path --path ${shellQuote(existingPath)}`,
    )
    setCopied(true)
  }

  return (
    <Stack gap="sm">
      <PasswordInput
        label="Lichess API key"
        description="The key is written once to a private file owned by your account. Manager service storage, logs, and setup results receive only a redacted locator result."
        placeholder="Paste the Lichess API key"
        value={credential}
        autoComplete="off"
        disabled={!nativeSetup}
        onChange={(event) => {
          setCredential(event.currentTarget.value)
          setMessage('')
          apply.reset()
        }}
      />
      <Group justify="space-between" align="flex-start">
        <Button
          leftSection={<IconKey size={16} />}
          loading={prepare.isPending}
          disabled={!nativeSetup || credential.length === 0 || credential.length > 512}
          onClick={() => prepare.mutate()}
        >
          Review and save API key
        </Button>
        <Button
          variant="subtle"
          rightSection={advanced ? <IconChevronUp size={15} /> : <IconChevronDown size={15} />}
          onClick={() => setAdvanced((value) => !value)}
        >
          {advanced ? 'Hide existing key file option' : 'Use an existing key file instead'}
        </Button>
      </Group>
      {!nativeSetup && (
        <Alert color="blue" title="Desktop application required">
          Reopen the installed UCI Arena Manager desktop application to enter an API key.
          The advanced file option can still produce a command for terminal use.
        </Alert>
      )}
      {message && <Alert color="green">{message}</Alert>}
      {prepare.error && <Alert color="red">{prepare.error.message}</Alert>}
      {apply.error && <Alert color="red">{apply.error.message}</Alert>}
      {advanced && (
        <Paper withBorder p="md">
          <Stack gap="sm">
            <PathPickerInput
              label="Existing Lichess token file"
              description="Advanced: select an operator-owned file outside Manager data and installed component roots. Its contents are never opened by Manager or Installer."
              placeholder="Select the existing token file"
              value={existingPath}
              onChange={(value) => {
                setExistingPath(value)
                setCopied(false)
              }}
              mode="file"
              filters={[{ name: 'Token reference files', patterns: ['*.txt', '*.token', '*'] }]}
            />
            <Group justify="flex-end">
              {nativeSetup ? (
                <LocalSetupImportButton
                  assetKind="lichess_token_path"
                  path={existingPath}
                  label="Review token-file reference"
                  onCompleted={async () => {
                    setExistingPath('')
                    setMessage('Existing token-file locator and consumer configuration committed.')
                    await onCompleted?.()
                  }}
                />
              ) : (
                <Button
                  variant="default"
                  leftSection={<IconCopy size={16} />}
                  disabled={!existingPath}
                  onClick={() => void copyImportCommand()}
                >
                  {copied ? 'Command copied' : 'Copy import command'}
                </Button>
              )}
            </Group>
          </Stack>
        </Paper>
      )}
      <Modal
        opened={plan !== null}
        onClose={() => !apply.isPending && setPlan(null)}
        title="Review Lichess API-key setup"
        size="lg"
      >
        <Stack>
          <Alert color="blue">
            The key will be atomically stored in an access-restricted file outside installed
            software. Only its path is committed through Suite Setup. Disable bot startup before
            replacing an existing key.
          </Alert>
          {plan?.actions.map((action) => (
            <Paper withBorder p="sm" key={action.action}>
              <Text fw={650}>{action.action.replaceAll('_', ' ')}</Text>
            </Paper>
          ))}
          <Group justify="flex-end">
            <Button variant="default" disabled={apply.isPending} onClick={() => setPlan(null)}>
              Cancel
            </Button>
            <Button loading={apply.isPending} onClick={() => apply.mutate()}>
              Confirm and save
            </Button>
          </Group>
        </Stack>
      </Modal>
    </Stack>
  )
}
