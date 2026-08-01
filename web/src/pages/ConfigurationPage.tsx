import {
  IconCopy,
  IconDatabaseImport,
  IconDownload,
  IconShieldLock,
} from '@tabler/icons-react'
import {
  Alert,
  Button,
  Code,
  Group,
  Modal,
  Paper,
  SimpleGrid,
  Stack,
  Text,
} from '@mantine/core'
import { useMutation } from '@tanstack/react-query'
import { useState } from 'react'
import {
  applyLocalSetup,
  localSetupAvailable,
  planLocalSetup,
  type LocalSetupRequest,
  type LocalSetupResult,
} from '../api/client'
import { managerEndpointLabel } from '../components/configurationModel'
import { PageHeader } from '../components/PageHeader'
import { PathPickerInput } from '../components/PathPickerInput'

export function ConfigurationPage() {
  const [bookPath, setBookPath] = useState('')
  const [bookFolderPath, setBookFolderPath] = useState('')
  const [syzygyPath, setSyzygyPath] = useState('')
  const [timingProfilePath, setTimingProfilePath] = useState('')
  const [singleGameProfilePath, setSingleGameProfilePath] = useState('')
  const [capacityProfilePath, setCapacityProfilePath] = useState('')
  const [transportProfilePath, setTransportProfilePath] = useState('')
  const [lichessTokenPath, setLichessTokenPath] = useState('')
  const [copied, setCopied] = useState('')
  const [pendingSetup, setPendingSetup] = useState<{
    key: string
    label: string
    request: LocalSetupRequest
  } | null>(null)
  const [setupPlan, setSetupPlan] = useState<LocalSetupResult | null>(null)
  const [setupResult, setSetupResult] = useState('')
  const nativeSetup = localSetupAvailable()
  const planSetup = useMutation({
    mutationFn: (pending: NonNullable<typeof pendingSetup>) =>
      planLocalSetup(pending.request),
    onSuccess: (result, pending) => {
      setPendingSetup(pending)
      setSetupPlan(result)
    },
  })
  const applySetup = useMutation({
    mutationFn: (pending: NonNullable<typeof pendingSetup>) =>
      applyLocalSetup(pending.request),
    onSuccess: (result, pending) => {
      setSetupPlan(null)
      setPendingSetup(null)
      setSetupResult(
        `${pending.label} completed: ${String(result.state ?? 'validated and committed')}.`,
      )
    },
  })
  const copy = async (label: string, command: string) => {
    await navigator.clipboard.writeText(command)
    setCopied(label)
  }
  const windowsShell = navigator.userAgent.toLowerCase().includes('windows')
  const quote = (path: string) =>
    windowsShell
      ? `'${path.replaceAll("'", "''")}'`
      : `'${path.replaceAll("'", `'"'"'`)}'`
  const importCommand = (kind: string, path: string) =>
    `arena-setup --json import --kind ${kind} --path ${quote(path)}`
  const reviewImport = (
    key: string,
    label: string,
    assetKind: Extract<LocalSetupRequest, { operation: 'import' }>['asset_kind'],
    path: string,
  ) => (
    <Button
      variant="default"
      leftSection={nativeSetup ? <IconDatabaseImport size={16} /> : <IconCopy size={16} />}
      disabled={!path}
      loading={planSetup.isPending && planSetup.variables?.key === key}
      onClick={() => {
        if (nativeSetup) {
          planSetup.mutate({
            key,
            label,
            request: { operation: 'import', asset_kind: assetKind, path },
          })
        } else {
          void copy(key, importCommand(assetKind, path))
        }
      }}
    >
      {nativeSetup
        ? `Review ${label.toLowerCase()}`
        : copied === key
          ? 'Command copied'
          : 'Copy import command'}
    </Button>
  )
  const reviewComponent = (
    key: string,
    label: string,
    componentId: Extract<LocalSetupRequest, { operation: 'component' }>['component_id'],
    command: string,
  ) => (
    <Button
      variant="light"
      leftSection={<IconDownload size={16} />}
      loading={planSetup.isPending && planSetup.variables?.key === key}
      onClick={() => {
        if (nativeSetup) {
          planSetup.mutate({
            key,
            label,
            request: { operation: 'component', component_id: componentId },
          })
        } else {
          void copy(key, command)
        }
      }}
    >
      {nativeSetup
        ? `Review ${label.toLowerCase()}`
        : copied === key
          ? 'Command copied'
          : 'Copy download command'}
    </Button>
  )
  const setupActions = setupPlan?.actions ?? []
  const blockers = Array.isArray(setupPlan?.blockers) ? setupPlan.blockers : []
  const space =
    setupPlan?.space && typeof setupPlan.space === 'object'
      ? (setupPlan.space as Record<string, unknown>)
      : {}

  return (
    <>
      <PageHeader
        title="Configuration"
        description="Validate, review, and apply explicit local control-plane settings."
      />
      <Stack>
        <Alert icon={<IconShieldLock size={18} />} color="teal" title="Secret boundary active">
          Secret values are never returned by the API. External services retain their own
          credentials unless a manager-owned SecretReference is explicitly required.
        </Alert>
        {setupResult && <Alert color="green">{setupResult}</Alert>}
        {planSetup.error && <Alert color="red">{planSetup.error.message}</Alert>}
        {applySetup.error && <Alert color="red">{applySetup.error.message}</Alert>}
        <Paper withBorder className="panel">
          <Text fw={700}>Network</Text>
          <Group justify="space-between" mt="md">
            <Text c="dimmed">Listen endpoint</Text>
            <Code>{managerEndpointLabel(window.location)}</Code>
          </Group>
          <Group justify="space-between" mt="sm">
            <Text c="dimmed">Remote binding</Text>
            <Text fw={650}>Rejected in v1</Text>
          </Group>
        </Paper>
        <Paper withBorder className="panel">
          <Group justify="space-between" align="flex-start" mb="md">
            <div>
              <Text fw={700}>Existing books, tablebases, and bot credentials</Text>
              <Text size="sm" c="dimmed">
                {nativeSetup
                  ? 'Browse locally, review the typed setup plan, then confirm it in this desktop application.'
                  : 'Browse locally, then copy the generated arena-setup command. Package changes require the native desktop application.'}
                {' '}Manager never reads credential contents.
              </Text>
            </div>
            <IconDatabaseImport size={22} />
          </Group>
          <SimpleGrid cols={{ base: 1, lg: 2 }}>
            <Stack>
              <PathPickerInput
                label="Opening book file"
                description="Select a Polyglot .bin or .book file."
                placeholder="D:\Chess\books\strong_rare_v1.bin"
                value={bookPath}
                onChange={setBookPath}
                mode="file"
                filters={[{ name: 'Opening books', patterns: ['*.bin', '*.book'] }]}
              />
              {reviewImport('book', 'Import opening book', 'opening_book', bookPath)}
            </Stack>
            <Stack>
              <PathPickerInput
                label="Opening book snapshot folder"
                description="Select a flat Book Forge snapshot directory containing the mutually validated book files."
                placeholder="D:\Chess\books\integration"
                value={bookFolderPath}
                onChange={setBookFolderPath}
                mode="directory"
              />
              {reviewImport(
                'book-folder',
                'Import book snapshot',
                'opening_book',
                bookFolderPath,
              )}
            </Stack>
            <Stack>
              <PathPickerInput
                label="Syzygy tablebase directory"
                description="Select the existing folder containing .rtbw and optional .rtbz files; files remain in place."
                placeholder="E:\Syzygy\3-4-5"
                value={syzygyPath}
                onChange={setSyzygyPath}
                mode="directory"
              />
              {reviewImport('syzygy', 'Import Syzygy', 'syzygy', syzygyPath)}
            </Stack>
            <Stack>
              <PathPickerInput
                label="Lichess token file reference"
                description="Select only the credential file path. Its contents are never opened or returned."
                placeholder="C:\Users\you\.config\uci-arena\lichess-token.txt"
                value={lichessTokenPath}
                onChange={setLichessTokenPath}
                mode="file"
                filters={[{ name: 'Credential text files', patterns: ['*.txt', '*.token'] }]}
              />
              {reviewImport(
                'lichess',
                'Save token-file reference',
                'lichess_token_path',
                lichessTokenPath,
              )}
            </Stack>
            <Stack>
              <PathPickerInput
                label="Qualified UCI timing profile"
                description="Select the exact provider-safety-qualified v3 timing profile for this engine, model, provider, and hardware."
                placeholder="D:\Chess\profiles\uci_time_profile_v3.json"
                value={timingProfilePath}
                onChange={setTimingProfilePath}
                mode="file"
                filters={[{ name: 'UCI timing profiles', patterns: ['uci_time_profile_v3.json', '*.json'] }]}
              />
              {reviewImport(
                'timing-profile',
                'Import timing profile',
                'timing_profile',
                timingProfilePath,
              )}
            </Stack>
            <Stack>
              <PathPickerInput
                label="Lichess single-game profile"
                description="Select the qualified single-game safety profile that matches the installed engine and model."
                placeholder="D:\Chess\profiles\uci_arena_single_game_profile_v1.json"
                value={singleGameProfilePath}
                onChange={setSingleGameProfilePath}
                mode="file"
                filters={[{ name: 'Single-game profiles', patterns: ['uci_arena_single_game_profile_v1.json', '*.json'] }]}
              />
              {reviewImport(
                'single-game-profile',
                'Import single-game profile',
                'lichess_single_game_profile',
                singleGameProfilePath,
              )}
            </Stack>
            <Stack>
              <PathPickerInput
                label="Lichess capacity profile"
                description="Select the qualified concurrency profile bound to the same engine, model, and single-game profile."
                placeholder="D:\Chess\profiles\uci_arena_lichess_capacity_profile_v1.json"
                value={capacityProfilePath}
                onChange={setCapacityProfilePath}
                mode="file"
                filters={[{ name: 'Lichess capacity profiles', patterns: ['uci_arena_lichess_capacity_profile_v1.json', '*.json'] }]}
              />
              {reviewImport(
                'capacity-profile',
                'Import capacity profile',
                'lichess_capacity_profile',
                capacityProfilePath,
              )}
            </Stack>
            <Stack>
              <PathPickerInput
                label="Lichess transport profile"
                description="Select the qualified v2 network transport-reserve profile measured for this host."
                placeholder="D:\Chess\profiles\transport_profile_v2.json"
                value={transportProfilePath}
                onChange={setTransportProfilePath}
                mode="file"
                filters={[{ name: 'Lichess transport profiles', patterns: ['transport_profile_v2.json', '*.json'] }]}
              />
              {reviewImport(
                'transport-profile',
                'Import transport profile',
                'lichess_transport_profile',
                transportProfilePath,
              )}
            </Stack>
          </SimpleGrid>
          <Group mt="xl">
            {reviewComponent(
              'download-book',
              'Download and install signed book',
              'opening_book.standard',
              'arena-setup --json apply --preset custom --component opening_book.standard --yes',
            )}
            {reviewComponent(
              'download-tables',
              'Download and install bounded 3–5 piece WDL',
              'syzygy.wdl.3-4-5',
              'arena-setup --json apply --preset custom --component syzygy.wdl.3-4-5 --yes',
            )}
          </Group>
          <Text size="xs" c="dimmed" mt="md">
            Six- and seven-piece Syzygy sets are import-only because their size is not suitable
            for an automatic installer download.
          </Text>
        </Paper>
        <Paper withBorder className="panel">
          <Text fw={700}>Persistence</Text>
          <Group justify="space-between" mt="md">
            <Text c="dimmed">Database mode</Text>
            <Code>SQLite · WAL · synchronous FULL</Code>
          </Group>
          <Group justify="space-between" mt="sm">
            <Text c="dimmed">Host identity</Text>
            <Code>local</Code>
          </Group>
        </Paper>
      </Stack>
      <Modal
        opened={Boolean(setupPlan && pendingSetup)}
        onClose={() => {
          if (!applySetup.isPending) {
            setSetupPlan(null)
            setPendingSetup(null)
          }
        }}
        title={pendingSetup?.label ?? 'Review setup'}
        size="lg"
      >
        <Stack>
          <Alert color="blue">
            This plan came from the adjacent signed setup tool. No executable, shell
            command, catalog, or install root was supplied by the dashboard.
          </Alert>
          <SimpleGrid cols={{ base: 1, sm: 3 }}>
            <Paper withBorder p="sm">
              <Text size="xs" c="dimmed">Actions</Text>
              <Text fw={700}>{setupActions.length}</Text>
            </Paper>
            <Paper withBorder p="sm">
              <Text size="xs" c="dimmed">Peak disk impact</Text>
              <Text fw={700}>
                {Number(space.peak_required_bytes ?? setupPlan?.disk_impact_bytes ?? 0)
                  .toLocaleString()}{' '}
                bytes
              </Text>
            </Paper>
            <Paper withBorder p="sm">
              <Text size="xs" c="dimmed">Commit authority</Text>
              <Text fw={700}>
                {setupPlan?.elevation_required ? 'Administrator confirmation' : 'Current user'}
              </Text>
            </Paper>
          </SimpleGrid>
          {setupActions.map((action, index) => (
            <Paper withBorder p="sm" key={`${String(action.component_id ?? action.asset_kind)}-${index}`}>
              <Text fw={650}>
                {String(action.action ?? 'apply')} ·{' '}
                {String(action.component_id ?? action.asset_kind ?? 'local asset')}
              </Text>
              {Array.isArray(action.dependencies) && action.dependencies.length > 0 && (
                <Text size="xs" c="dimmed">
                  Dependencies: {action.dependencies.map(String).join(', ')}
                </Text>
              )}
            </Paper>
          ))}
          {blockers.map((blocker, index) => (
            <Alert color="red" key={index}>
              {JSON.stringify(blocker)}
            </Alert>
          ))}
          {setupPlan?.manager_service_restart && (
            <Alert color="orange">
              Manager service will restart after the transaction. This window will reconnect.
            </Alert>
          )}
          <Group justify="flex-end">
            <Button
              variant="default"
              disabled={applySetup.isPending}
              onClick={() => {
                setSetupPlan(null)
                setPendingSetup(null)
              }}
            >
              Cancel
            </Button>
            <Button
              loading={applySetup.isPending}
              disabled={!pendingSetup || blockers.length > 0}
              onClick={() => pendingSetup && applySetup.mutate(pendingSetup)}
            >
              Confirm and apply
            </Button>
          </Group>
        </Stack>
      </Modal>
    </>
  )
}
