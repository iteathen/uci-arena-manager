import { IconCalendarEvent, IconSwords } from '@tabler/icons-react'
import {
  Alert,
  Button,
  Grid,
  Group,
  MultiSelect,
  NumberInput,
  Paper,
  SegmentedControl,
  Select,
  Stack,
  Text,
  TextInput,
  Title,
} from '@mantine/core'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { useMemo, useState } from 'react'
import { api, makeCommand } from '../api/client'
import type { EngineData, MatchPlanData } from '../api/types'
import { PageHeader } from '../components/PageHeader'

export function MatchPlannerPage() {
  const queryClient = useQueryClient()
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const plans = useQuery({
    queryKey: ['resources', 'match_plan'],
    queryFn: () => api.resources<MatchPlanData>('match_plan'),
  })
  const [name, setName] = useState('Local engine round robin')
  const [selected, setSelected] = useState<string[]>([])
  const [format, setFormat] = useState('round_robin')
  const [variant, setVariant] = useState('standard')
  const [games, setGames] = useState<number | string>(2)
  const [concurrency, setConcurrency] = useState<number | string>(1)
  const [initial, setInitial] = useState<number | string>(60)
  const [increment, setIncrement] = useState<number | string>(0.6)
  const [seed, setSeed] = useState<number | string>(1)
  const options = useMemo(
    () =>
      engines.data?.map((engine) => ({
        value: engine.id,
        label: engine.data.display_name,
      })) ?? [],
    [engines.data],
  )
  const selectedPairs =
    format === 'head_to_head' || format === 'sprt'
      ? selected.length === 2
        ? 1
        : 0
      : (selected.length * (selected.length - 1)) / 2
  const scheduledGames =
    format === 'gauntlet'
      ? Math.max(0, selected.length - 1) * Number(games)
      : format === 'swiss'
        ? Math.floor(selected.length / 2)
        : selectedPairs * Number(games)

  const create = useMutation({
    mutationFn: (dryRun: boolean) =>
      api.command(
        makeCommand(
          'match.plan',
          'match_plan',
          {
            display_name: name,
            engine_ids: selected,
            format,
            variant,
            games_per_pair: Number(games),
            rounds: 1,
            concurrency: Number(concurrency),
            seed: Number(seed),
            time_control: {
              initial_ms: Math.round(Number(initial) * 1000),
              increment_ms: Math.round(Number(increment) * 1000),
            },
          },
          dryRun,
        ),
      ),
    onSuccess: async (_, dryRun) => {
      if (!dryRun) await queryClient.invalidateQueries({ queryKey: ['resources', 'match_plan'] })
    },
  })
  const validCount =
    selected.length >= 2 &&
    (!(format === 'head_to_head' || format === 'sprt') || selected.length === 2)

  return (
    <>
      <PageHeader
        title="Local match planner"
        description="Build deterministic schedules from registered UCI engines and immutable settings."
      />
      <Grid>
        <Grid.Col span={{ base: 12, lg: 8 }}>
          <Paper withBorder className="panel">
            <Stack gap="lg">
              <div>
                <Text className="form-step">01 · FORMAT</Text>
                <SegmentedControl
                  visibleFrom="sm"
                  fullWidth
                  value={format}
                  onChange={setFormat}
                  data={[
                    { label: 'Head-to-head', value: 'head_to_head' },
                    { label: 'Round robin', value: 'round_robin' },
                    { label: 'Gauntlet', value: 'gauntlet' },
                    { label: 'Swiss', value: 'swiss' },
                    { label: 'SPRT', value: 'sprt' },
                  ]}
                />
                <Select
                  hiddenFrom="sm"
                  aria-label="Match format"
                  value={format}
                  onChange={(value) => setFormat(value ?? 'round_robin')}
                  data={[
                    { label: 'Head-to-head', value: 'head_to_head' },
                    { label: 'Round robin', value: 'round_robin' },
                    { label: 'Gauntlet', value: 'gauntlet' },
                    { label: 'Swiss', value: 'swiss' },
                    { label: 'SPRT', value: 'sprt' },
                  ]}
                />
              </div>
              <div>
                <Text className="form-step">02 · ENGINES</Text>
                <MultiSelect
                  searchable
                  clearable
                  label="Participants"
                  placeholder="Choose registered UCI engines…"
                  data={options}
                  value={selected}
                  onChange={setSelected}
                  nothingFoundMessage="Register engines first"
                />
                {(format === 'head_to_head' || format === 'sprt') && selected.length > 2 && (
                  <Alert color="orange" mt="sm">
                    This format requires exactly two engines.
                  </Alert>
                )}
              </div>
              <div>
                <Text className="form-step">03 · RULES</Text>
                <Grid>
                  <Grid.Col span={{ base: 12, sm: 6 }}>
                    <TextInput label="Plan name" value={name} onChange={(e) => setName(e.currentTarget.value)} />
                  </Grid.Col>
                  <Grid.Col span={{ base: 6, sm: 3 }}>
                    <Select
                      label="Variant"
                      value={variant}
                      onChange={(value) => setVariant(value ?? 'standard')}
                      data={[
                        { value: 'standard', label: 'Standard' },
                        { value: 'chess960', label: 'Chess960' },
                      ]}
                    />
                  </Grid.Col>
                  <Grid.Col span={{ base: 6, sm: 3 }}>
                    <NumberInput label="Games / pair" min={1} value={games} onChange={setGames} />
                  </Grid.Col>
                  <Grid.Col span={{ base: 6, sm: 3 }}>
                    <NumberInput label="Base seconds" min={1} value={initial} onChange={setInitial} />
                  </Grid.Col>
                  <Grid.Col span={{ base: 6, sm: 3 }}>
                    <NumberInput
                      label="Increment"
                      min={0}
                      decimalScale={2}
                      value={increment}
                      onChange={setIncrement}
                    />
                  </Grid.Col>
                  <Grid.Col span={{ base: 6, sm: 3 }}>
                    <NumberInput label="Concurrency" min={1} value={concurrency} onChange={setConcurrency} />
                  </Grid.Col>
                  <Grid.Col span={{ base: 6, sm: 3 }}>
                    <NumberInput label="Seed" min={0} value={seed} onChange={setSeed} />
                  </Grid.Col>
                </Grid>
              </div>
              {create.error && <Alert color="red">{create.error.message}</Alert>}
              <Group justify="flex-end">
                <Button
                  variant="default"
                  disabled={!validCount}
                  loading={create.isPending}
                  onClick={() => create.mutate(true)}
                >
                  Dry run
                </Button>
                <Button
                  leftSection={<IconCalendarEvent size={17} />}
                  disabled={!validCount || !name}
                  loading={create.isPending}
                  onClick={() => create.mutate(false)}
                >
                  Save plan
                </Button>
              </Group>
            </Stack>
          </Paper>
        </Grid.Col>
        <Grid.Col span={{ base: 12, lg: 4 }}>
          <Paper withBorder className="panel planner-summary">
            <IconSwords size={28} />
            <Text className="eyebrow" mt="md">
              SCHEDULE PREVIEW
            </Text>
            <Title order={2} mt={5}>
              {scheduledGames} games
            </Title>
            <Stack gap="xs" mt="lg">
              <Group justify="space-between">
                <Text c="dimmed">Participants</Text>
                <Text fw={650}>{selected.length}</Text>
              </Group>
              <Group justify="space-between">
                <Text c="dimmed">Color pairs</Text>
                <Text fw={650}>{Math.ceil(scheduledGames / 2)}</Text>
              </Group>
              <Group justify="space-between">
                <Text c="dimmed">Parallel games</Text>
                <Text fw={650}>{concurrency}</Text>
              </Group>
              <Group justify="space-between">
                <Text c="dimmed">Time control</Text>
                <Text fw={650}>
                  {initial}+{increment}
                </Text>
              </Group>
            </Stack>
          </Paper>
          <Paper withBorder className="panel" mt="lg">
            <Text fw={700}>Saved plans</Text>
            <Text size="sm" c="dimmed" mt={4}>
              {plans.data?.length ?? 0} deterministic plans ready for execution.
            </Text>
          </Paper>
        </Grid.Col>
      </Grid>
    </>
  )
}
