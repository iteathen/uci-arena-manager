import assert from "node:assert/strict";
import test from "node:test";

import { isNamedModelTestFixture } from "../model-product-naming-policy.mjs";

test("named model metadata is exempt only inside automated test fixtures", () => {
  assert.equal(isNamedModelTestFixture("web/src/pages/EnginesPage.render.test.tsx"), true);
  assert.equal(isNamedModelTestFixture("tests/engine_resource.spec.cpp"), true);
  assert.equal(isNamedModelTestFixture("web/src/pages/EnginesPage.tsx"), false);
  assert.equal(isNamedModelTestFixture("specs/product_contract_v1.yaml"), false);
  assert.equal(isNamedModelTestFixture("docs/latticeknight.test-plan.md"), false);
});
