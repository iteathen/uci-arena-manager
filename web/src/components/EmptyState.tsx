import { IconCirclePlus } from '@tabler/icons-react'
import { Button, Center, Stack, Text, ThemeIcon, Title } from '@mantine/core'

export function EmptyState({
  title,
  detail,
  action,
  onAction,
}: {
  title: string
  detail: string
  action?: string
  onAction?: () => void
}) {
  return (
    <Center className="empty-state">
      <Stack align="center" gap="xs">
        <ThemeIcon variant="light" size={44} radius="xl">
          <IconCirclePlus size={22} />
        </ThemeIcon>
        <Title order={3}>{title}</Title>
        <Text c="dimmed" ta="center" maw={460}>
          {detail}
        </Text>
        {action && <Button onClick={onAction}>{action}</Button>}
      </Stack>
    </Center>
  )
}
