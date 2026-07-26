import { IconActivity, IconClock, IconCpu, IconDeviceSdCard } from '@tabler/icons-react'
import { Grid, Paper, SimpleGrid, Text } from '@mantine/core'
import { LineChart } from 'echarts/charts'
import { GridComponent, TooltipComponent } from 'echarts/components'
import * as echarts from 'echarts/core'
import { CanvasRenderer } from 'echarts/renderers'
import { useEffect, useRef } from 'react'
import { MetricCard } from '../components/MetricCard'
import { PageHeader } from '../components/PageHeader'

echarts.use([LineChart, GridComponent, TooltipComponent, CanvasRenderer])

function HealthChart() {
  const target = useRef<HTMLDivElement>(null)
  useEffect(() => {
    if (!target.current) return
    const chart = echarts.init(target.current)
    chart.setOption({
      backgroundColor: 'transparent',
      textStyle: { color: '#9aa4b7' },
      tooltip: { trigger: 'axis' },
      grid: { left: 44, top: 24, right: 18, bottom: 30 },
      xAxis: {
        type: 'category',
        boundaryGap: false,
        data: ['-60m', '-50m', '-40m', '-30m', '-20m', '-10m', 'now'],
        axisLine: { lineStyle: { color: '#384051' } },
      },
      yAxis: {
        type: 'value',
        min: 0,
        max: 100,
        splitLine: { lineStyle: { color: '#242a36' } },
      },
      series: [
        {
          name: 'CPU',
          type: 'line',
          smooth: true,
          showSymbol: false,
          data: [8, 12, 9, 22, 18, 14, 11],
          lineStyle: { width: 3, color: '#7c8cff' },
          areaStyle: { color: 'rgba(124,140,255,.12)' },
        },
        {
          name: 'Memory',
          type: 'line',
          smooth: true,
          showSymbol: false,
          data: [19, 20, 20, 21, 21, 22, 22],
          lineStyle: { width: 2, color: '#40c7a2' },
        },
      ],
    })
    const resize = () => chart.resize()
    window.addEventListener('resize', resize)
    return () => {
      window.removeEventListener('resize', resize)
      chart.dispose()
    }
  }, [])
  return <div ref={target} className="health-chart" aria-label="Resource usage over the last hour" />
}

export function StatisticsPage() {
  return (
    <>
      <PageHeader
        title="Statistics & health"
        description="Bounded telemetry, health transitions, and rollups—not raw protocol noise."
      />
      <SimpleGrid cols={{ base: 1, xs: 2, lg: 4 }} mb="lg">
        <MetricCard label="CPU" value="11%" detail="Local control plane" icon={<IconCpu size={21} />} />
        <MetricCard label="Memory" value="—" detail="Awaiting first rollup" icon={<IconActivity size={21} />} tone="violet" />
        <MetricCard label="Disk state" value="Ready" detail="Scheduling permitted" icon={<IconDeviceSdCard size={21} />} tone="teal" />
        <MetricCard label="Event latency" value="<500ms" detail="Declared local target" icon={<IconClock size={21} />} tone="orange" />
      </SimpleGrid>
      <Grid>
        <Grid.Col span={{ base: 12, lg: 8 }}>
          <Paper withBorder className="panel">
            <Text fw={700}>Host utilization</Text>
            <Text size="sm" c="dimmed">
              Minute rollup preview
            </Text>
            <HealthChart />
          </Paper>
        </Grid.Col>
        <Grid.Col span={{ base: 12, lg: 4 }}>
          <Paper withBorder className="panel" h="100%">
            <Text fw={700}>Health policy</Text>
            <Text size="sm" c="dimmed" mt="sm">
              New scheduling pauses before disk pressure can compromise persistence. Engine
              protocol channels remain isolated from lossy telemetry and log channels.
            </Text>
          </Paper>
        </Grid.Col>
      </Grid>
    </>
  )
}
