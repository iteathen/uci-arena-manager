import { Button, Group } from '@mantine/core'
import { CompetitionList } from '../components/CompetitionList'
import { PageHeader } from '../components/PageHeader'

export function TournamentPage({ navigate }: { navigate: (page: string) => void }) {
  return (
    <>
      <Group justify="space-between" align="flex-start" mb="lg">
        <PageHeader
          title="Tournaments & matches"
          description="Backend-authoritative schedules, lifecycle controls, and durable game progress."
        />
        <Button component="a" href="#page=planner" onClick={() => navigate('planner')}>
          Create plan
        </Button>
      </Group>
      <CompetitionList
        navigate={navigate}
        emptyAction={{ label: 'Create a plan', onClick: () => navigate('planner') }}
      />
    </>
  )
}
