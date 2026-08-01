import { IconRefresh, IconRobot } from '@tabler/icons-react'
import { Alert, Badge, Button, Code, Group, Paper, SimpleGrid, Stack, Text, ThemeIcon, Title } from '@mantine/core'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useState } from 'react'
import { api, controlLocalBot, probeLocalBot } from '../api/client'
import type { BotCandidate } from '../api/types'
import { PageHeader } from '../components/PageHeader'
import { PathPickerInput } from '../components/PathPickerInput'
import { ComponentSetupButton } from '../components/ComponentSetupButton'
import { BotOperationsPanel } from '../components/BotOperationsPanel'

export function BotsPage({ navigate }: { navigate: (page: string) => void }) {
  const queryClient = useQueryClient()
  const profiles = useQuery({ queryKey: ['bot-profiles'], queryFn: api.botProfiles })
  const candidates = useQuery({ queryKey: ['bot-candidates'], queryFn: api.botCandidates })
  const components = useQuery({ queryKey: ['components'], queryFn: api.components })
  const adapterComponent = components.data?.components.find(
    (component) => component.component_id === 'lichess.adapter',
  )
  const [tokenPath, setTokenPath] = useState('')
  const [detectionMessage, setDetectionMessage] = useState('')
  const saveToken = useMutation({
    mutationFn: api.saveBotTokenReference,
    onSuccess: async () => {
      setTokenPath('')
      await candidates.refetch()
    },
  })
  const probe = useMutation({ mutationFn: probeLocalBot })
  const candidate = candidates.data?.candidates[0]
  const startup = useQuery({
    queryKey: ['bot-startup'],
    queryFn: () => controlLocalBot('status'),
    enabled: candidate?.package_state === 'installed',
    retry: false,
  })
  const startupControl = useMutation({
    mutationFn: controlLocalBot,
    onSuccess: (result) => {
      queryClient.setQueryData(['bot-startup'], result)
    },
  })
  const observedStartup = startup.data
  const startupInstalled = observedStartup?.installed === true
  const readiness: BotCandidate['readiness'] = {
    ...(candidate?.readiness ?? {}),
    ...(probe.data ? { bot_account_probe: 'verified' as const } : {}),
    ...(observedStartup
      ? { startup: observedStartup.installed ? 'enabled' as const : 'disabled' as const }
      : {}),
  }
  const readinessLabels: Record<string, string> = {
    verified_adapter: 'Verified adapter',
    adapter_runtime: 'Private adapter runtime',
    verified_uci_arena_engine: 'UCI Arena Engine',
    active_model: 'Active default model',
    opening_book: 'Opening book',
    syzygy: 'Syzygy',
    timing_profile: 'Timing profile',
    single_game_profile: 'Single-game profile',
    capacity_profile: 'Capacity profile',
    transport_profile: 'Transport profile',
    token_reference: 'Token-file reference',
    bot_account_probe: 'BOT account probe',
    startup: 'Provider startup',
  }
  const issueDestination = (code: string) => {
    if (code.includes('engine') || code.includes('model')) {
      return { page: 'engines', label: 'Open Engines' }
    }
    if (code.includes('timing_profile')) {
      return { page: 'reports', label: 'Review evidence' }
    }
    if (code.includes('adapter')) {
      return { page: 'bots', label: 'Review adapter' }
    }
    return { page: 'configuration', label: 'Open Configuration' }
  }
  const detect = async () => {
    setDetectionMessage('')
    const result = await candidates.refetch()
    if (result.error) return
    const detected = result.data?.candidates[0]
    setDetectionMessage(
      detected
        ? `Auto-detect completed: adapter ${detected.package_state}, configuration ${detected.configuration_state}, ${detected.issues.length} blocker${detected.issues.length === 1 ? '' : 's'}.`
        : 'Auto-detect completed: no verified Lichess adapter candidate was found.',
    )
  }

  return (
    <>
      <PageHeader
        title="Bots"
        description="Install, detect, verify, and operate typed bot adapters without exposing account credentials."
        actions={
          <Button
            variant="default"
            leftSection={<IconRefresh size={16} />}
            loading={candidates.isFetching}
            onClick={() => void detect()}
          >
            Auto-detect
          </Button>
        }
      />
      <SimpleGrid cols={{ base: 1, lg: 2 }}>
        <Paper withBorder className="panel" p="lg">
          <Group justify="space-between" align="flex-start">
            <Group>
              <ThemeIcon size={44} radius="md" variant="light">
                <IconRobot size={24} />
              </ThemeIcon>
              <div>
                <Title order={2}>Lichess Bot</Title>
                <Text size="sm" c="dimmed">
                  One local BOT account using a token-file path reference.
                </Text>
              </div>
            </Group>
            <Badge color={candidate?.state === 'ready_to_probe' ? 'teal' : 'gray'}>
              {candidate?.state === 'ready_to_probe' ? 'READY TO VERIFY' : 'SETUP REQUIRED'}
            </Badge>
          </Group>
          <Stack mt="lg" gap="sm">
            <Group justify="space-between">
              <Text size="sm">Adapter package</Text>
              <Code>{candidate?.package_state ?? 'checking'}</Code>
            </Group>
            {candidate?.package_state === 'missing' && adapterComponent && (
                <Group justify="flex-end">
                  <ComponentSetupButton
                    componentId="lichess.adapter"
                    label="Lichess Bot adapter"
                    available={adapterComponent.downloadable}
                    onCompleted={async () => {
                      await new Promise((resolve) => window.setTimeout(resolve, 1000))
                      await Promise.all([components.refetch(), candidates.refetch()])
                    }}
                  />
                </Group>
              )}
            <Group justify="space-between">
              <Text size="sm">Configuration</Text>
              <Code>{candidate?.configuration_state ?? 'checking'}</Code>
            </Group>
            <Group justify="space-between">
              <Text size="sm">Token-file reference</Text>
              <Code>{candidate?.token_reference_available ? 'available' : 'missing'}</Code>
            </Group>
            <PathPickerInput
              label="Lichess token file"
              description="Select an operator-owned file outside Manager data and the installed adapter. Only its path is saved."
              placeholder="Select the existing token file"
              value={tokenPath}
              onChange={setTokenPath}
              mode="file"
              filters={[{ name: 'Token reference files', patterns: ['*.txt', '*.token', '*'] }]}
            />
            <Group justify="flex-end">
              <Button
                variant="default"
                loading={saveToken.isPending}
                disabled={!tokenPath}
                onClick={() => saveToken.mutate(tokenPath)}
              >
                Save token-file reference
              </Button>
            </Group>
            <Group justify="space-between">
              <Text size="sm">Startup</Text>
              <Code>
                {observedStartup?.state ??
                  (startup.isError
                    ? 'helper unavailable'
                    : candidate
                      ? `receipt hint: ${candidate.startup_state}`
                      : 'checking')}
              </Code>
            </Group>
            {candidate?.issues.map((issue) => (
              <Alert key={issue.code} color="orange" title={issue.code}>
                <Group justify="space-between" align="flex-end">
                  <Text size="sm">{issue.detail}</Text>
                  <Button
                    size="xs"
                    variant="subtle"
                    onClick={() => navigate(issueDestination(issue.code).page)}
                  >
                    {issueDestination(issue.code).label}
                  </Button>
                </Group>
              </Alert>
            ))}
            {candidates.error && <Alert color="red">{candidates.error.message}</Alert>}
            {detectionMessage && <Alert color="blue">{detectionMessage}</Alert>}
            {saveToken.error && <Alert color="red">{saveToken.error.message}</Alert>}
            {saveToken.data && (
              <Alert color="green">
                Token-file reference saved without reading its contents.
              </Alert>
            )}
            {probe.error && <Alert color="red">{probe.error.message}</Alert>}
            {startup.error && (
              <Alert color="blue">
                Live BOT startup status requires the desktop launcher helper.
              </Alert>
            )}
            {startupControl.error && (
              <Alert color="red">{startupControl.error.message}</Alert>
            )}
            {probe.data && (
              <Alert color="green" title="BOT account verified">
                {probe.data.account_name} · {probe.data.games} games
              </Alert>
            )}
            {startupControl.data?.account_name && (
              <Alert color="green" title="BOT startup enabled">
                {startupControl.data.account_name}
              </Alert>
            )}
            <Group justify="space-between">
              <Button
                variant="default"
                loading={startup.isFetching}
                disabled={candidate?.package_state !== 'installed'}
                onClick={() => {
                  startupControl.reset()
                  void startup.refetch()
                }}
              >
                Refresh startup status
              </Button>
              <Button
                loading={probe.isPending}
                disabled={candidate?.state !== 'ready_to_probe'}
                onClick={() => probe.mutate()}
              >
                Verify BOT account
              </Button>
            </Group>
            <Group justify="flex-end">
              <Button
                variant="default"
                loading={startupControl.isPending}
                disabled={!observedStartup || !startupInstalled}
                onClick={() => startupControl.mutate('disable_startup')}
              >
                Disable startup
              </Button>
              <Button
                loading={startupControl.isPending}
                disabled={
                  !observedStartup ||
                  candidate?.state !== 'ready_to_probe' ||
                  startupInstalled
                }
                onClick={() => startupControl.mutate('enable_startup')}
              >
                Enable startup
              </Button>
            </Group>
            <Text size="xs" c="dimmed">
              Startup is owned by the verified provider under your desktop session.
              Closing this Manager window removes control authority but does not stop an
              enabled bot.
            </Text>
          </Stack>
        </Paper>
        <Paper withBorder className="panel" p="lg">
          <Title order={3}>Readiness and options</Title>
          <Text size="sm" c="dimmed" mt="xs">
            Package, configuration, timing evidence, engine identity, knowledge,
            capacity, transport, credential reference, and startup must all be ready
            before activation.
          </Text>
          <SimpleGrid cols={{ base: 1, sm: 2 }} mt="md">
            {Object.entries(readinessLabels).map(([key, label]) => {
              const state = readiness[key] ?? 'missing'
              const ready = state === 'ready' || state === 'verified' || state === 'enabled'
              return (
                <Paper withBorder p="sm" key={key}>
                  <Text size="xs" c="dimmed">{label}</Text>
                  <Badge color={ready ? 'teal' : state === 'not_verified' ? 'blue' : 'orange'}>
                    {state.replaceAll('_', ' ')}
                  </Badge>
                </Paper>
              )
            })}
          </SimpleGrid>
          {candidate?.default_model && (
            <Text size="sm" mt="md">
              Engine default model: {candidate.default_model.display_name} ·{' '}
              {candidate.default_model.input_adapter_id}
            </Text>
          )}
          <Text size="xs" c="dimmed" mt="md">
            {profiles.data?.[0]?.capabilities.length ?? 0} declared adapter capabilities.
            Token contents never enter the Manager API or dashboard.
          </Text>
          <Stack mt="lg">
            <BotOperationsPanel
              installed={candidate?.package_state === 'installed'}
              navigate={navigate}
            />
          </Stack>
        </Paper>
      </SimpleGrid>
    </>
  )
}
