import { Badge } from '@mantine/core'

export function StatusPill({ value }: { value: string }) {
  const normalized = value.toLowerCase()
  const color =
    normalized.includes('healthy') || normalized === 'running' || normalized === 'completed'
      ? 'teal'
      : normalized.includes('failed') || normalized.includes('unhealthy')
        ? 'red'
        : normalized.includes('degraded') || normalized.includes('interrupted')
          ? 'orange'
          : 'gray'
  return (
    <Badge color={color} variant="light" radius="sm">
      {value.replaceAll('_', ' ')}
    </Badge>
  )
}
