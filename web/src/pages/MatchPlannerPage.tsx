import { IconCalendarEvent, IconSwords } from '@tabler/icons-react'
import {
  Accordion,
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
import type { EngineData, MatchPlanData, Resource } from '../api/types'
import { CompetitionList } from '../components/CompetitionList'
import { competitionPlanTarget, competitionScheduleSize } from '../components/competitionModel'
import { PageHeader } from '../components/PageHeader'

export function MatchPlannerPage({ navigate }: { navigate: (page: string) => void }) {
  const queryClient = useQueryClient()
  const engines = useQuery({
    queryKey: ['resources', 'engine'],
    queryFn: () => api.resources<EngineData>('engine'),
  })
  const [name, setName] = useState('Local engine round robin')
  const [selected, setSelected] = useState<string[]>([])
  const [format, setFormat] = useState('round_robin')
  const [variant, setVariant] = useState('standard')
  const [games, setGames] = useState<number | string>(2)
  const [rounds, setRounds] = useState<number | string>(3)
  const [concurrency, setConcurrency] = useState<number | string>(1)
  const [initial, setInitial] = useState<number | string>(60)
  const [increment, setIncrement] = useState<number | string>(0.6)
  const [movesToGo, setMovesToGo] = useState<number | string>('')
  const [seed, setSeed] = useState<number | string>(1)
  const [openingSuite, setOpeningSuite] = useState('')
  const [maxPlies, setMaxPlies] = useState<number | string>(512)
  const [drawCp, setDrawCp] = useState<number | string>('')
  const [drawConsecutive, setDrawConsecutive] = useState<number | string>(8)
  const [drawMinPlies, setDrawMinPlies] = useState<number | string>(80)
  const [winCp, setWinCp] = useState<number | string>('')
  const [winConsecutive, setWinConsecutive] = useState<number | string>(6)
  const [elo0, setElo0] = useState<number | string>(0)
  const [elo1, setElo1] = useState<number | string>(5)
  const [alpha, setAlpha] = useState<number | string>(0.05)
  const [beta, setBeta] = useState<number | string>(0.05)
  const [engineOverrides, setEngineOverrides] = useState<Record<string, Record<string, string>>>({})
  const [dryRunPreview, setDryRunPreview] = useState<MatchPlanData | null>(null)
  const options = useMemo(
    () =>
      [...(engines.data ?? [])]
        .sort((left, right) => {
          const leftPreferred =
            left.data.component_id === 'uci_arena.engine' && left.data.recommended === true
          const rightPreferred =
            right.data.component_id === 'uci_arena.engine' && right.data.recommended === true
          if (leftPreferred !== rightPreferred) return leftPreferred ? -1 : 1
          return left.data.display_name.localeCompare(right.data.display_name)
        })
        .map((engine) => ({
          value: engine.id,
          label:
            engine.data.component_id === 'uci_arena.engine' &&
            engine.data.recommended === true
              ? `${engine.data.display_name} · preferred`
              : engine.data.display_name,
        })),
    [engines.data],
  )
  const scheduledGames = competitionScheduleSize(
    format,
    selected.length,
    Number(games),
    Number(rounds),
  )
  const selectedEngines = selected
    .map((id) => engines.data?.find((engine) => engine.id === id))
    .filter((engine): engine is Resource<EngineData> => engine !== undefined)

  const updateEngineOverride = (engineId: string, name: string, value: string) => {
    setEngineOverrides((current) => ({
      ...current,
      [engineId]: { ...current[engineId], [name]: value },
    }))
  }

  const create = useMutation({
    mutationFn: (dryRun: boolean) => {
      const target = competitionPlanTarget(format)
      const overrides = Object.fromEntries(
        selected.map((engineId) => [
          engineId,
          Object.fromEntries(
            Object.entries(engineOverrides[engineId] ?? {}).filter(([, value]) => value !== ''),
          ),
        ]).filter(([, values]) => Object.keys(values as Record<string, string>).length > 0),
      )
      const parameters: Record<string, unknown> = {
        display_name: name,
        engine_ids: selected,
        format,
        variant,
        games_per_pair: format === 'swiss' ? 1 : Number(games),
        rounds: format === 'swiss' ? Number(rounds) : 1,
        concurrency: Number(concurrency),
        seed: Number(seed),
        time_control: {
          initial_ms: Math.round(Number(initial) * 1000),
          increment_ms: Math.round(Number(increment) * 1000),
          ...(movesToGo === '' ? {} : { moves_to_go: Number(movesToGo) }),
        },
        adjudication: {
          max_plies: Number(maxPlies),
          draw_consecutive_plies: Number(drawConsecutive),
          draw_min_plies: Number(drawMinPlies),
          win_consecutive_plies: Number(winConsecutive),
          ...(drawCp === '' ? {} : { draw_cp: Number(drawCp) }),
          ...(winCp === '' ? {} : { win_cp: Number(winCp) }),
        },
        ...(openingSuite.trim() ? { opening_suite: openingSuite.trim() } : {}),
        ...(Object.keys(overrides).length ? { engine_option_overrides: overrides } : {}),
        ...(format === 'sprt'
          ? {
              sprt: {
                elo0: Number(elo0),
                elo1: Number(elo1),
                alpha: Number(alpha),
                beta: Number(beta),
              },
            }
          : {}),
      }
      return api.command<Resource<MatchPlanData>>(
        makeCommand(target.action, target.kind, parameters, dryRun),
      )
    },
    onSuccess: async (result, dryRun) => {
      if (dryRun) {
        setDryRunPreview(result.resource.data)
      } else {
        setDryRunPreview(null)
        await queryClient.invalidateQueries({ queryKey: ['resources'] })
      }
    },
  })
  const validCount =
    selected.length >= 2 &&
    (!(format === 'head_to_head' || format === 'sprt') || selected.length === 2) &&
    (format !== 'sprt' ||
      (Number(games) >= 2 &&
        Number(games) % 2 === 0 &&
        Number(elo1) > Number(elo0) &&
        Number(alpha) > 0 && Number(alpha) < 0.5 &&
        Number(beta) > 0 && Number(beta) < 0.5))

  return (
    <>
      <Group justify="space-between" align="flex-start" mb="lg">
        <PageHeader
          title="Local match planner"
          description="Build deterministic schedules from registered UCI engines and immutable settings."
        />
        <Group gap="xs">
          <Button variant="subtle" component="a" href="#page=engines" onClick={() => navigate('engines')}>Engines</Button>
          <Button variant="subtle" component="a" href="#page=services" onClick={() => navigate('services')}>Opening services</Button>
          <Button variant="subtle" component="a" href="#page=reports" onClick={() => navigate('reports')}>Reports</Button>
        </Group>
      </Group>
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
                    {format === 'swiss' ? (
                      <NumberInput label="Rounds" min={1} value={rounds} onChange={setRounds} />
                    ) : (
                      <NumberInput
                        label={format === 'sprt' ? 'Maximum games (even)' : 'Games / pair'}
                        min={format === 'sprt' ? 2 : 1}
                        step={format === 'sprt' ? 2 : 1}
                        value={games}
                        onChange={setGames}
                      />
                    )}
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
                  <Grid.Col span={{ base: 6, sm: 3 }}>
                    <NumberInput
                      label="Moves to go"
                      description="Optional UCI clock horizon"
                      min={1}
                      value={movesToGo}
                      onChange={setMovesToGo}
                    />
                  </Grid.Col>
                </Grid>
                {format === 'sprt' && (
                  <>
                    <Text className="form-step" mt="lg">04 · SPRT DECISION</Text>
                    <Grid>
                      <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Elo H0" min={-1000} max={1000} value={elo0} onChange={setElo0} /></Grid.Col>
                      <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Elo H1" min={-1000} max={1000} value={elo1} onChange={setElo1} /></Grid.Col>
                      <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Alpha" min={0.001} max={0.499} step={0.01} decimalScale={3} value={alpha} onChange={setAlpha} /></Grid.Col>
                      <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Beta" min={0.001} max={0.499} step={0.01} decimalScale={3} value={beta} onChange={setBeta} /></Grid.Col>
                    </Grid>
                    {!validCount && selected.length === 2 && (
                      <Alert color="orange" mt="sm">SPRT requires an even maximum game count, H1 above H0, and alpha/beta strictly between 0 and 0.5.</Alert>
                    )}
                  </>
                )}
                <Accordion variant="contained" mt="lg">
                  <Accordion.Item value="execution">
                    <Accordion.Control>Opening and adjudication controls</Accordion.Control>
                    <Accordion.Panel>
                      <Grid>
                        <Grid.Col span={12}>
                          <TextInput
                            label="Opening suite JSON path"
                            description="Optional bounded arena_opening_suite_v1 file. Book Forge and service controls are linked above."
                            placeholder="C:\\path\\to\\opening-suite.json"
                            value={openingSuite}
                            onChange={(event) => setOpeningSuite(event.currentTarget.value)}
                          />
                        </Grid.Col>
                        <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Maximum plies" min={2} max={2048} value={maxPlies} onChange={setMaxPlies} /></Grid.Col>
                        <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Draw threshold (cp)" min={0} max={1000} value={drawCp} onChange={setDrawCp} /></Grid.Col>
                        <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Draw consecutive plies" min={1} max={100} value={drawConsecutive} onChange={setDrawConsecutive} /></Grid.Col>
                        <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Draw minimum plies" min={0} max={1000} value={drawMinPlies} onChange={setDrawMinPlies} /></Grid.Col>
                        <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Win threshold (cp)" min={1} max={100000} value={winCp} onChange={setWinCp} /></Grid.Col>
                        <Grid.Col span={{ base: 6, sm: 3 }}><NumberInput label="Win consecutive plies" min={1} max={100} value={winConsecutive} onChange={setWinConsecutive} /></Grid.Col>
                      </Grid>
                    </Accordion.Panel>
                  </Accordion.Item>
                  {selectedEngines.map((engine) => {
                    const options = engine.data.discovered_options.filter((option) => option.type !== 'button')
                    if (!options.length) return null
                    return (
                      <Accordion.Item value={`engine:${engine.id}`} key={engine.id}>
                        <Accordion.Control>{engine.data.display_name} option overrides</Accordion.Control>
                        <Accordion.Panel>
                          <Grid>
                            {options.map((option) => (
                              <Grid.Col span={{ base: 12, sm: 6 }} key={option.name}>
                                {option.type === 'check' || option.type === 'combo' ? (
                                  <Select
                                    clearable
                                    searchable={option.type === 'combo'}
                                    label={option.name}
                                    placeholder={`Default: ${option.default}`}
                                    data={option.type === 'check' ? ['true', 'false'] : (option.variants ?? [])}
                                    value={engineOverrides[engine.id]?.[option.name] || null}
                                    onChange={(value) => updateEngineOverride(engine.id, option.name, value ?? '')}
                                  />
                                ) : option.type === 'spin' ? (
                                  <NumberInput
                                    label={option.name}
                                    placeholder={`Default: ${option.default}`}
                                    min={option.minimum ?? undefined}
                                    max={option.maximum ?? undefined}
                                    value={engineOverrides[engine.id]?.[option.name] ?? ''}
                                    onChange={(value) => updateEngineOverride(engine.id, option.name, String(value))}
                                  />
                                ) : (
                                  <TextInput
                                    label={option.name}
                                    placeholder={`Default: ${option.default}`}
                                    value={engineOverrides[engine.id]?.[option.name] ?? ''}
                                    onChange={(event) => updateEngineOverride(engine.id, option.name, event.currentTarget.value)}
                                  />
                                )}
                              </Grid.Col>
                            ))}
                          </Grid>
                        </Accordion.Panel>
                      </Accordion.Item>
                    )
                  })}
                </Accordion>
              </div>
              {create.error && <Alert color="red">{create.error.message}</Alert>}
              {dryRunPreview && (
                <Alert color="teal" title="Dry run passed">
                  The backend normalized {dryRunPreview.pairings.length} deterministic
                  pairing(s). Nothing was saved.
                </Alert>
              )}
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
        </Grid.Col>
      </Grid>
      <Title order={2} mt="xl" mb="md">Saved competitions</Title>
      <CompetitionList navigate={navigate} />
    </>
  )
}
