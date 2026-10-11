import { Group, NumberInput, Stack, Text } from '@mantine/core'
import { useState } from 'react'
import type { ProbeTimeControl } from '../api/types'

export function EngineProbeClockInputs({ onChange }: { onChange: (clock?: ProbeTimeControl) => void }) {
  const [initial, setInitial] = useState<number | string>('')
  const [increment, setIncrement] = useState<number | string>(0)
  const publish = (initial_ms: number | string, increment_ms: number | string) => {
    onChange(typeof initial_ms === 'number' && Number.isSafeInteger(initial_ms) && initial_ms > 0 &&
      typeof increment_ms === 'number' && Number.isSafeInteger(increment_ms) && increment_ms >= 0
      ? { initial_ms, increment_ms } : undefined)
  }
  return <Stack gap="xs" mb="md">
    <Text size="sm">Engine refresh test clock, required for engines that bind original clocks. This local diagnostic stops after at most 500 ms.</Text>
    <Group grow>
      <NumberInput label="Test initial time (ms)" min={1} max={Number.MAX_SAFE_INTEGER} allowDecimal={false} value={initial}
        onChange={value => { setInitial(value); publish(value, increment) }} />
      <NumberInput label="Test increment (ms)" min={0} max={Number.MAX_SAFE_INTEGER} allowDecimal={false} value={increment}
        onChange={value => { setIncrement(value); publish(initial, value) }} />
    </Group>
  </Stack>
}
