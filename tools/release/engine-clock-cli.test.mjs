import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import test from 'node:test';

test('native engine client exposes explicit local clock and verified candidate selection', {
  skip: !process.env.ARENA_CLOCK_TEST_CLI,
}, () => {
  for (const command of ['probe', 'add', 'set-model']) {
    const help = execFileSync(process.env.ARENA_CLOCK_TEST_CLI, ['engine', command, '--help'], {
      encoding: 'utf8', windowsHide: true,
    });
    assert.ok(help.includes('--initial-ms'), `${command} needs an explicit original initial clock`);
    assert.ok(help.includes('--increment-ms'), `${command} needs an explicit original increment`);
    if (command !== 'set-model') assert.ok(help.includes('--candidate-id'));
  }
});
