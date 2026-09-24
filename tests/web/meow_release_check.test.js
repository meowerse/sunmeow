import fs from 'node:fs'
import path from 'node:path'

import { afterEach, describe, expect, it, vi } from 'vitest'

import {
  MEOW_RELEASES_REPO,
  fetchLatestPreRelease,
  fetchLatestRelease,
} from '../../src_assets/common/assets/web/meow_release_check.js'

/**
 * Stub fetch with a fixed response per URL suffix and record every URL requested.
 * @param {Record<string, {status: number, body: any} | Error>} routes
 */
function stubFetch(routes) {
  const requested = []
  vi.stubGlobal('fetch', vi.fn(async (url) => {
    requested.push(url)
    const key = Object.keys(routes).find((suffix) => url.endsWith(suffix))
    const route = routes[key]
    if (route instanceof Error) {
      throw route
    }
    return {
      ok: route.status >= 200 && route.status < 300,
      status: route.status,
      json: async () => route.body,
    }
  }))
  return requested
}

describe('meow_release_check', () => {
  afterEach(() => {
    vi.unstubAllGlobals()
    vi.restoreAllMocks()
  })

  it('queries this fork, never LizardByte/Sunshine', async () => {
    vi.spyOn(console, 'debug').mockImplementation(() => {})
    const requested = stubFetch({
      '/releases/latest': { status: 404, body: { message: 'Not Found' } },
      '/releases?per_page=100': { status: 200, body: [] },
    })

    await fetchLatestRelease()
    await fetchLatestPreRelease()

    expect(MEOW_RELEASES_REPO).toBe('meowerse/sunmeow')
    expect(requested).toHaveLength(2)
    for (const url of requested) {
      expect(url.startsWith('https://api.github.com/repos/meowerse/sunmeow/releases')).toBe(true)
      expect(url).not.toContain('LizardByte')
    }
  })

  it('returns null, not a bogus release, when nothing is published (404 and [])', async () => {
    vi.spyOn(console, 'debug').mockImplementation(() => {})
    stubFetch({
      '/releases/latest': { status: 404, body: { message: 'Not Found' } },
      '/releases?per_page=100': { status: 200, body: [] },
    })

    expect(await fetchLatestRelease()).toBeNull()
    expect(await fetchLatestPreRelease()).toBeNull()
  })

  it('survives a rate-limit body that is not an array (upstream threw on .find here)', async () => {
    vi.spyOn(console, 'debug').mockImplementation(() => {})
    stubFetch({
      '/releases/latest': { status: 403, body: { message: 'API rate limit exceeded' } },
      '/releases?per_page=100': { status: 403, body: { message: 'API rate limit exceeded' } },
    })

    expect(await fetchLatestRelease()).toBeNull()
    expect(await fetchLatestPreRelease()).toBeNull()
  })

  it('returns null when offline', async () => {
    vi.spyOn(console, 'debug').mockImplementation(() => {})
    stubFetch({
      '/releases/latest': new TypeError('Failed to fetch'),
      '/releases?per_page=100': new TypeError('Failed to fetch'),
    })

    expect(await fetchLatestRelease()).toBeNull()
    expect(await fetchLatestPreRelease()).toBeNull()
  })

  it('returns real releases, and only a pre-release from the list', async () => {
    const stable = { tag_name: 'v2026.10.1', name: 'stable', prerelease: false }
    const pre = { tag_name: 'v2026.11.0', name: 'beta', prerelease: true }
    stubFetch({
      '/releases/latest': { status: 200, body: stable },
      '/releases?per_page=100': { status: 200, body: [stable, pre] },
    })

    expect(await fetchLatestRelease()).toEqual(stable)
    expect(await fetchLatestPreRelease()).toEqual(pre)
  })
})

describe('web UI release checks', () => {
  // An upstream sync can bring the LizardByte release fetch back without any conflict -- the
  // 2026-09-24 SPA rewrite did exactly that in Home.vue. Fail on the source, not on a review.
  it('no page fetches LizardByte/Sunshine releases directly', () => {
    const webRoot = path.resolve(__dirname, '../../src_assets/common/assets/web')
    const offenders = []
    const walk = (dir) => {
      for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
        const full = path.join(dir, entry.name)
        if (entry.isDirectory()) {
          if (entry.name !== 'public') {
            walk(full)
          }
        } else if (/\.(vue|js|html)$/.test(entry.name) && entry.name !== 'meow_release_check.js') {
          if (fs.readFileSync(full, 'utf8').includes('repos/LizardByte/Sunshine/releases')) {
            offenders.push(path.relative(webRoot, full))
          }
        }
      }
    }
    walk(webRoot)
    expect(offenders).toEqual([])
  })
})
