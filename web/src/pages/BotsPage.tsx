import { IconRefresh, IconRobot } from '@tabler/icons-react'
import { Alert, Badge, Button, Code, Group, Paper, SimpleGrid, Stack, Text, ThemeIcon, Title } from '@mantine/core'
import { useMutation, useQuery } from '@tanstack/react-query'
import { useState } from 'react'
import { api, probeLocalBot } from '../api/client'
import type { BotCandidate } from '../api/types'
import { PageHeader } from '../components/PageHeader'
import { PathPickerInput } from '../components/PathPickerInput'

export function BotsPage() {
  const profiles = useQuery({ queryKey: ['bot-profiles'], queryFn: api.botProfiles })
  const candidates = useQuery({ queryKey: ['bot-candidates'], queryFn: api.botCandidates })
  const [tokenPath, setTokenPath] = useState('')
  const saveToken = useMutation({
    mutationFn: api.saveBotTokenReference,
    onSuccess: async () => {
      setTokenPath('')
      await candidates.refetch()
    },
  })
  const probe = useMutation({ mutationFn: probeLocalBot })
  const candidate = candidates.data?.candidates[0]
  const readiness: BotCandidate['readiness'] = {
    ...(candidate?.readiness ?? {}),
    ...(probe.data ? { bot_account_probe: 'verified' as const } : {}),
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

  return (
    <>
      <PageHeader
        title="Bots"
        description="Install, detect, verify, and operate typed bot adapters without exposing account credentials."
        actions={
          <Button
            variant="default"
            leftSection={<IconRefresh size={16} />}
            onClick={() => candidates.refetch()}
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
              description="Only the path is saved. Manager never opens, copies, hashes, logs, or returns the token."
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
              <Code>{candidate?.startup_state ?? 'checking'}</Code>
            </Group>
            {candidate?.issues.map((issue) => (
              <Alert key={issue.code} color="orange" title={issue.code}>
                {issue.detail}
              </Alert>
            ))}
            {candidates.error && <Alert color="red">{candidates.error.message}</Alert>}
            {saveToken.error && <Alert color="red">{saveToken.error.message}</Alert>}
            {saveToken.data && (
              <Alert color="green">
                Token-file reference saved without reading its contents.
              </Alert>
            )}
            {probe.error && <Alert color="red">{probe.error.message}</Alert>}
            {probe.data && (
              <Alert color="green" title="BOT account verified">
                {probe.data.account_name} · {probe.data.games} games
              </Alert>
            )}
            <Group justify="flex-end">
              <Button
                loading={probe.isPending}
                disabled={candidate?.state !== 'ready_to_probe'}
                onClick={() => probe.mutate()}
              >
                Verify BOT account
              </Button>
            </Group>
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
          <Code block mt="md">
            {JSON.stringify(candidate?.options ?? {}, null, 2)}
          </Code>
          <Text size="xs" c="dimmed" mt="md">
            {profiles.data?.[0]?.capabilities.length ?? 0} declared adapter capabilities.
            Token contents never enter the Manager API or dashboard.
          </Text>
        </Paper>
      </SimpleGrid>
    </>
  )
}
