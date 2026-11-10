const js = require('@eslint/js');
const globals = require('globals');

module.exports = [
  { ignores: ['build/', 'tests/wasm/data.js'] },
  js.configs.recommended,
  {
    rules: {
      quotes: ['error', 'single', { avoidEscape: true }],
      'no-unused-vars': ['error', { caughtErrors: 'none' }],
    },
  },
  {
    files: ['eslint.config.js', 'tests/wasm/gen.js'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'commonjs', globals: globals.node },
  },
  {
    // Runs after the generated data.js, which defines the tables and W; the host provides gc and __result
    files: ['tests/wasm/script.js'],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'script',
      globals: {
        ...globals.node,
        RECORDING: 'readonly',
        E: 'writable',
        H: 'writable',
        W: 'readonly',
        NUM_OPS: 'readonly',
        NUM_IN: 'readonly',
        SIMD_OPS: 'readonly',
        F32S: 'readonly',
        F64S: 'readonly',
        hex: 'readonly',
        gc: 'readonly',
        __result: 'writable',
      },
    },
  },
  {
    // Run on the page and in a worker alike, see tests/bindings_worker_test.c
    files: ['tests/parity_probe.js'],
    languageOptions: { ecmaVersion: 2022, sourceType: 'script', globals: { ...globals.browser, ...globals.es2021 } },
  },
];
