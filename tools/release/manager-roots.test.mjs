import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import test from 'node:test';
import { readFileSync } from 'node:fs';
import path from 'node:path';

test('Windows layout admission leaves the prior Linux CLI and helper setup gate intact', () => {
  const root = path.resolve(import.meta.dirname, '../..');
  const cli = readFileSync(path.join(root, 'apps/cli/main.cpp'), 'utf8');
  const helper = readFileSync(path.join(root, 'apps/cli/ui_picker.cpp'), 'utf8');
  assert.match(cli, /#else\s+const bool setup_authorized = desktop_host;\s+#endif/u);
  assert.match(cli, /#ifdef _WIN32\s+const bool complete_roots_requested(?:(?!#else|#endif)[\s\S])*query_manager_package_layout(?:(?!#else|#endif)[\s\S])*#else\s+const bool setup_authorized = desktop_host;/u);
  assert.match(helper, /#else\s+static_cast<void>\(setup_executable\);\s+static_cast<void>\(roots\);\s+#endif/u);
  assert.match(helper, /manager_setup_root_arguments\(fixed_setup_roots, manager_data_root\)/u);
  assert.match(helper, /#ifdef _WIN32\s+if \(allow_setup &&/u);
  assert.match(helper, /#ifdef _WIN32\s+if \(!roots\)/u);
});

test('native CLI publishes all fixed installer root arguments without starting a session', {
  skip: !process.env.ARENA_ROOTS_TEST_CLI,
}, () => {
  const help = execFileSync(process.env.ARENA_ROOTS_TEST_CLI, ['--help'], { encoding: 'utf8', windowsHide: true });
  for (const flag of ['--data-dir', '--install-root', '--cache-root', '--workspace-root', '--knowledge-root']) {
    assert.ok(help.includes(flag), `native CLI must accept ${flag} from the installed desktop`);
  }
});
