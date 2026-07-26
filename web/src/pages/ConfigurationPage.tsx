import { IconDownload, IconShieldLock } from '@tabler/icons-react'
import { Alert, Button, Code, Group, Paper, Stack, Text } from '@mantine/core'
import { PageHeader } from '../components/PageHeader'

export function ConfigurationPage() {
  return (
    <>
      <PageHeader
        title="Configuration"
        description="Validate, review, and apply explicit local control-plane settings."
        actions={
          <Button variant="default" leftSection={<IconDownload size={16} />}>
            Export redacted config
          </Button>
        }
      />
      <Stack>
        <Alert icon={<IconShieldLock size={18} />} color="teal" title="Secret boundary active">
          Secret values are never returned by the API. External services retain their own
          credentials unless a manager-owned SecretReference is explicitly required.
        </Alert>
        <Paper withBorder className="panel">
          <Text fw={700}>Network</Text>
          <Group justify="space-between" mt="md">
            <Text c="dimmed">Listen endpoint</Text>
            <Code>127.0.0.1:47831</Code>
          </Group>
          <Group justify="space-between" mt="sm">
            <Text c="dimmed">Remote binding</Text>
            <Text fw={650}>Rejected in v1</Text>
          </Group>
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
    </>
  )
}
