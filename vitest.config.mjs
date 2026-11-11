import { resolve } from 'node:path'
import { defineConfig, configDefaults } from 'vitest/config'
import { name } from './package.json'

const r = (p) => resolve(__dirname, p)

export default defineConfig({
  resolve: {
    alias: {
      [name]: r('./index.js'),
    },
  },
  test: {
    // *.test.mjs files run on Node's built-in test runner (node --test)
    exclude: [...configDefaults.exclude, '**/*.test.mjs'],
  },
})
