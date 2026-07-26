import { Group, Paper, Stack, Text, ThemeIcon } from '@mantine/core'
import type { ReactNode } from 'react'

export function MetricCard({
  label,
  value,
  detail,
  icon,
  tone = 'indigo',
}: {
  label: string
  value: string | number
  detail: string
  icon: ReactNode
  tone?: string
}) {
  return (
    <Paper className="metric-card" withBorder>
      <Group justify="space-between" align="flex-start" wrap="nowrap">
        <Stack gap={2}>
          <Text size="sm" c="dimmed" fw={600}>
            {label}
          </Text>
          <Text className="metric-value">{value}</Text>
          <Text size="xs" c="dimmed">
            {detail}
          </Text>
        </Stack>
        <ThemeIcon size={42} radius="md" variant="light" color={tone}>
          {icon}
        </ThemeIcon>
      </Group>
    </Paper>
  )
}
