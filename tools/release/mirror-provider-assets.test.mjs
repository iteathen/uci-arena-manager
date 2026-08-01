import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";
import { fileURLToPath } from "node:url";

import { buildProviderMirrorPlan } from "./mirror-provider-assets.mjs";

function currentLock() {
  return JSON.parse(readFileSync(
    fileURLToPath(
      new URL("../../release/catalog.lock.json", import.meta.url),
    ),
    "utf8",
  ));
}

test("provider mirror plan contains every exact provider asset once", () => {
  const lock = currentLock();
  const plan = buildProviderMirrorPlan(lock);
  assert.equal(plan.repository, "iteathen/uci-arena-manager");
  assert.equal(plan.releaseTag, "provider-mirror/v0.3.6");
  assert.equal(plan.assets.length, lock.components.length * 3);
  assert.equal(
    new Set(plan.assets.map((asset) => asset.name)).size,
    plan.assets.length,
  );
  assert.deepEqual(
    [...new Set(plan.assets.map((asset) => asset.sourceRepository))].sort(),
    [
      "iteathen/UCI_Arena",
      "iteathen/uci-arena-book-forge",
      "iteathen/uci-arena-evidence-service",
    ].sort(),
  );
  assert.ok(plan.assets.every((asset) =>
    /^[0-9a-f]{64}$/u.test(asset.sha256)));
});

test("provider mirror plan rejects an asset-name collision", () => {
  const lock = currentLock();
  lock.components[1].provider_result_name =
    lock.components[0].provider_result_name;
  lock.components[1].provider_result_sha256 =
    lock.components[0].provider_result_sha256;
  assert.throws(
    () => buildProviderMirrorPlan(lock),
    /reuses provider mirror asset/u,
  );
});
