import {
  IconBookDownload,
  IconCopy,
  IconDatabaseImport,
  IconDownload,
  IconShieldLock,
} from '@tabler/icons-react'
import { Alert, Button, Code, Group, Paper, SimpleGrid, Stack, Text } from '@mantine/core'
import { useState } from 'react'
import { PageHeader } from '../components/PageHeader'
import { PathPickerInput } from '../components/PathPickerInput'

export function ConfigurationPage() {
  const [bookPath, setBookPath] = useState('')
  const [bookFolderPath, setBookFolderPath] = useState('')
  const [syzygyPath, setSyzygyPath] = useState('')
  const [lichessTokenPath, setLichessTokenPath] = useState('')
  const [copied, setCopied] = useState('')
  const copy = async (label: string, command: string) => {
    await navigator.clipboard.writeText(command)
    setCopied(label)
  }
  const windowsShell = navigator.userAgent.toLowerCase().includes('windows')
  const quote = (path: string) =>
    windowsShell
      ? `'${path.replaceAll("'", "''")}'`
      : `'${path.replaceAll("'", `'"'"'`)}'`
  const importCommand = (kind: string, path: string) =>
    `arena-setup import --kind ${kind} --path ${quote(path)} --json`

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
          <Group justify="space-between" align="flex-start" mb="md">
            <div>
              <Text fw={700}>Existing books, tablebases, and bot credentials</Text>
              <Text size="sm" c="dimmed">
                Browse locally, then review and run the generated arena-setup command in a
                terminal. Manager never executes installers or reads credential contents.
              </Text>
            </div>
            <IconDatabaseImport size={22} />
          </Group>
          <SimpleGrid cols={{ base: 1, lg: 2 }}>
            <Stack>
              <PathPickerInput
                label="Opening book file"
                description="Select a Polyglot .bin or .book file."
                placeholder="D:\Chess\books\strong_rare_v1.bin"
                value={bookPath}
                onChange={setBookPath}
                mode="file"
                filters={[{ name: 'Opening books', patterns: ['*.bin', '*.book'] }]}
              />
              <Button
                variant="default"
                leftSection={<IconCopy size={16} />}
                disabled={!bookPath}
                onClick={() =>
                  void copy('book', importCommand('opening_book', bookPath))
                }
              >
                {copied === 'book' ? 'Import command copied' : 'Copy import command'}
              </Button>
            </Stack>
            <Stack>
              <PathPickerInput
                label="Opening book snapshot folder"
                description="Select a flat Book Forge snapshot directory containing the mutually validated book files."
                placeholder="D:\Chess\books\integration"
                value={bookFolderPath}
                onChange={setBookFolderPath}
                mode="directory"
              />
              <Button
                variant="default"
                leftSection={<IconCopy size={16} />}
                disabled={!bookFolderPath}
                onClick={() =>
                  void copy(
                    'book-folder',
                    importCommand('opening_book', bookFolderPath),
                  )
                }
              >
                {copied === 'book-folder'
                  ? 'Import command copied'
                  : 'Copy import command'}
              </Button>
            </Stack>
            <Stack>
              <PathPickerInput
                label="Syzygy tablebase directory"
                description="Select the existing folder containing .rtbw and optional .rtbz files; files remain in place."
                placeholder="E:\Syzygy\3-4-5"
                value={syzygyPath}
                onChange={setSyzygyPath}
                mode="directory"
              />
              <Button
                variant="default"
                leftSection={<IconCopy size={16} />}
                disabled={!syzygyPath}
                onClick={() =>
                  void copy('syzygy', importCommand('syzygy', syzygyPath))
                }
              >
                {copied === 'syzygy' ? 'Import command copied' : 'Copy import command'}
              </Button>
            </Stack>
            <Stack>
              <PathPickerInput
                label="Lichess token file reference"
                description="Select only the credential file path. Its contents are never opened or returned."
                placeholder="C:\Users\you\.config\uci-arena\lichess-token.txt"
                value={lichessTokenPath}
                onChange={setLichessTokenPath}
                mode="file"
                filters={[{ name: 'Credential text files', patterns: ['*.txt', '*.token'] }]}
              />
              <Button
                variant="default"
                leftSection={<IconCopy size={16} />}
                disabled={!lichessTokenPath}
                onClick={() =>
                  void copy(
                    'lichess',
                    importCommand('lichess_token_path', lichessTokenPath),
                  )
                }
              >
                {copied === 'lichess' ? 'Reference command copied' : 'Copy reference command'}
              </Button>
            </Stack>
          </SimpleGrid>
          <Group mt="xl">
            <Button
              variant="light"
              leftSection={<IconBookDownload size={16} />}
              onClick={() =>
                void copy(
                  'download-book',
                  'arena-setup apply --preset manager_only --download-missing-books --yes --json',
                )
              }
            >
              {copied === 'download-book'
                ? 'Book download command copied'
                : 'Download missing signed book'}
            </Button>
            <Button
              variant="light"
              leftSection={<IconDownload size={16} />}
              onClick={() =>
                void copy(
                  'download-tables',
                  'arena-setup apply --preset manager_only --download-missing-tablebases --yes --json',
                )
              }
            >
              {copied === 'download-tables'
                ? 'Tablebase download command copied'
                : 'Download bounded 3–5 piece WDL'}
            </Button>
          </Group>
          <Text size="xs" c="dimmed" mt="md">
            Six- and seven-piece Syzygy sets are import-only because their size is not suitable
            for an automatic installer download.
          </Text>
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
