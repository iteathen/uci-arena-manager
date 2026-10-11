import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import test from 'node:test';

test('native CLI publishes all fixed installer root arguments without starting a session', {
  skip: !process.env.ARENA_ROOTS_TEST_CLI,
}, () => {
  const help = execFileSync(process.env.ARENA_ROOTS_TEST_CLI, ['--help'], { encoding: 'utf8', windowsHide: true });
  for (const flag of ['--data-dir', '--install-root', '--cache-root', '--workspace-root', '--knowledge-root']) {
    assert.ok(help.includes(flag), `native CLI must accept ${flag} from the installed desktop`);
  }
});
