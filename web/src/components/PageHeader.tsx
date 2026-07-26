import { Group, Stack, Text, Title } from '@mantine/core'
import type { ReactNode } from 'react'

export function PageHeader({
  title,
  description,
  actions,
}: {
  title: string
  description: string
  actions?: ReactNode
}) {
  return (
    <Group justify="space-between" align="flex-start" mb="xl" wrap="wrap">
      <Stack gap={3}>
        <Title order={1}>{title}</Title>
        <Text c="dimmed">{description}</Text>
      </Stack>
      {actions}
    </Group>
  )
}
