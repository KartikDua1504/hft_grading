<script lang="ts">
  import { onMount, onDestroy } from 'svelte';

  let adminKey = $state('');
  let authenticated = $state(false);
  let password = $state('');
  let loginError = $state('');

  // Competition
  let compState = $state('stopped');
  let compEndTime = $state('');
  let compRemaining = $state(0);
  let durationMins = $state(120);
  let extendMins = $state(30);

  // Teams & Jobs
  let teams = $state<any[]>([]);
  let jobs = $state<any[]>([]);
  let activeTab = $state<'competition' | 'teams' | 'jobs'>('competition');

  let refreshTimer: ReturnType<typeof setInterval> | null = null;

  onMount(() => {
    const saved = localStorage.getItem('iicpc_admin_key');
    if (saved) {
      adminKey = saved;
      authenticated = true;
      refreshAll();
    }
    refreshTimer = setInterval(() => {
      if (authenticated) refreshCompStatus();
    }, 1000);
  });

  onDestroy(() => {
    if (refreshTimer) clearInterval(refreshTimer);
  });

  async function login() {
    loginError = '';
    try {
      const res = await fetch('/api/admin/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ password }),
      });
      if (res.ok) {
        adminKey = password;
        authenticated = true;
        localStorage.setItem('iicpc_admin_key', adminKey);
        refreshAll();
      } else {
        loginError = 'Invalid admin password';
      }
    } catch {
      loginError = 'Backend unreachable';
    }
  }

  function logout() {
    authenticated = false;
    adminKey = '';
    localStorage.removeItem('iicpc_admin_key');
  }

  async function adminFetch(url: string, opts: any = {}) {
    const headers = { ...opts.headers, 'x-admin-key': adminKey };
    return fetch(url, { ...opts, headers });
  }

  async function refreshAll() {
    await Promise.all([refreshCompStatus(), refreshTeams(), refreshJobs()]);
  }

  async function refreshCompStatus() {
    try {
      const res = await fetch('/api/competition/status');
      if (res.ok) {
        const d = await res.json();
        compState = d.state;
        compEndTime = d.end_time || '';
        compRemaining = d.remaining_secs;
      }
    } catch {}
  }

  async function refreshTeams() {
    try {
      const res = await adminFetch('/api/admin/teams');
      if (res.ok) teams = await res.json();
    } catch {}
  }

  async function refreshJobs() {
    try {
      const res = await adminFetch('/api/admin/jobs');
      if (res.ok) jobs = await res.json();
    } catch {}
  }

  async function startCompetition() {
    await adminFetch('/api/admin/competition/start', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ duration_mins: durationMins }),
    });
    refreshCompStatus();
  }

  async function stopCompetition() {
    await adminFetch('/api/admin/competition/stop', { method: 'POST' });
    refreshCompStatus();
  }

  async function extendCompetition() {
    await adminFetch('/api/admin/competition/extend', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ minutes: extendMins }),
    });
    refreshCompStatus();
  }

  async function resetLeaderboard() {
    if (!confirm('Are you sure? This will clear ALL scores permanently.')) return;
    await adminFetch('/api/admin/leaderboard/reset', { method: 'POST' });
    refreshTeams();
  }

  function formatRemaining(secs: number): string {
    const h = Math.floor(secs / 3600);
    const m = Math.floor((secs % 3600) / 60);
    const s = secs % 60;
    if (h > 0) return `${h}h ${String(m).padStart(2, '0')}m ${String(s).padStart(2, '0')}s`;
    return `${m}m ${String(s).padStart(2, '0')}s`;
  }

  function timeAgo(iso: string): string {
    if (!iso) return '';
    const diff = Date.now() - new Date(iso).getTime();
    const mins = Math.floor(diff / 60000);
    if (mins < 1) return 'just now';
    if (mins < 60) return `${mins}m ago`;
    return `${Math.floor(mins / 60)}h ago`;
  }
</script>

<svelte:head>
  <title>Admin Panel — IICPC Arena</title>
</svelte:head>

<div class="min-h-screen bg-[var(--bg-void)] text-[var(--text-primary)]">
  {#if !authenticated}
    <!-- Login -->
    <div class="flex items-center justify-center min-h-screen">
      <div class="card p-10 w-full max-w-md">
        <div class="flex items-center gap-3 mb-8">
          <div class="w-10 h-10 rounded-xl flex items-center justify-center bg-rose-500/10">
            <svg class="w-5 h-5 text-rose-400" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M9 12.75L11.25 15 15 9.75m-3-7.036A11.959 11.959 0 013.598 6 11.99 11.99 0 003 9.749c0 5.592 3.824 10.29 9 11.623 5.176-1.332 9-6.03 9-11.622 0-1.31-.21-2.571-.598-3.751h-.152c-3.196 0-6.1-1.248-8.25-3.285z"/>
            </svg>
          </div>
          <div>
            <h1 class="text-xl font-bold display">Admin Panel</h1>
            <p class="text-xs text-[var(--text-muted)]">IICPC Arena Control</p>
          </div>
        </div>

        <div class="space-y-4">
          <input
            type="password"
            class="input w-full"
            placeholder="Admin password"
            bind:value={password}
            onkeydown={(e) => e.key === 'Enter' && login()}
          />
          {#if loginError}
            <div class="text-xs text-rose-400">{loginError}</div>
          {/if}
          <button class="btn btn-primary btn-pill w-full py-3" onclick={login}>
            <span class="mono uppercase tracking-wider font-bold text-sm">Authenticate</span>
          </button>
        </div>
      </div>
    </div>
  {:else}
    <!-- Admin Dashboard -->
    <div class="max-w-6xl mx-auto px-6 py-8">
      <!-- Header -->
      <div class="flex items-center justify-between mb-8 border-b border-[var(--border)] pb-4">
        <div>
          <div class="flex items-center gap-2 mb-1">
            <div class="w-2 h-2 rounded-full bg-rose-400 flicker"></div>
            <span class="text-[10px] mono uppercase tracking-widest text-rose-400">Admin Control</span>
          </div>
          <h1 class="text-3xl font-black display tracking-tight">Command Center</h1>
        </div>
        <div class="flex items-center gap-3">
          <button class="text-xs text-[var(--text-muted)] hover:text-[var(--text-secondary)] mono" onclick={refreshAll}>↻ Refresh</button>
          <button class="text-xs text-rose-400 hover:text-rose-300 mono" onclick={logout}>Logout</button>
        </div>
      </div>

      <!-- Competition Status Banner -->
      <div class="card p-6 mb-6" style="border-color: {compState === 'running' ? 'var(--emerald)' : compState === 'finished' ? 'var(--amber)' : 'var(--border)'};">
        <div class="flex items-center justify-between">
          <div class="flex items-center gap-4">
            <div class="w-14 h-14 rounded-2xl flex items-center justify-center text-2xl"
                 style="background: {compState === 'running' ? 'rgba(16,185,129,0.1)' : compState === 'finished' ? 'rgba(245,158,11,0.1)' : 'rgba(100,100,100,0.1)'};">
              {#if compState === 'running'}⚡{:else if compState === 'finished'}🏁{:else}⏸{/if}
            </div>
            <div>
              <div class="text-xs uppercase tracking-widest text-[var(--text-muted)] mb-1">Competition Status</div>
              <div class="text-2xl font-black mono uppercase {compState === 'running' ? 'text-[var(--emerald)]' : compState === 'finished' ? 'text-[var(--amber)]' : 'text-[var(--text-muted)]'}">
                {compState}
              </div>
            </div>
          </div>
          {#if compState === 'running' && compRemaining > 0}
            <div class="text-right">
              <div class="text-xs text-[var(--text-muted)] uppercase tracking-wider mb-1">Time Remaining</div>
              <div class="text-3xl font-black mono text-[var(--accent)]">{formatRemaining(compRemaining)}</div>
            </div>
          {/if}
        </div>
      </div>

      <!-- Tab Nav -->
      <div class="flex gap-1 mb-6">
        {#each ['competition', 'teams', 'jobs'] as tab}
          <button
            class="px-5 py-2.5 rounded-xl text-xs font-bold mono uppercase tracking-wider transition-all {activeTab === tab ? 'bg-[var(--accent)] text-white' : 'bg-[var(--bg-surface)] text-[var(--text-muted)] hover:text-[var(--text-secondary)]'}"
            onclick={() => { activeTab = tab as any; if (tab === 'teams') refreshTeams(); if (tab === 'jobs') refreshJobs(); }}
          >
            {tab}
          </button>
        {/each}
      </div>

      <!-- Tab Content -->
      {#if activeTab === 'competition'}
        <div class="grid md:grid-cols-2 gap-6">
          <!-- Start Competition -->
          <div class="card p-6">
            <h3 class="text-sm font-bold display mb-4">Start Competition</h3>
            <div class="space-y-3">
              <label class="block text-xs text-[var(--text-muted)]">Duration (minutes)</label>
              <input type="number" class="input w-full" bind:value={durationMins} min="1" max="720" />
              <div class="text-xs text-[var(--text-ghost)] mono">= {Math.floor(durationMins / 60)}h {durationMins % 60}m</div>
              <button class="btn btn-primary btn-pill w-full py-3 text-sm" onclick={startCompetition}
                      disabled={compState === 'running'}>
                <span class="mono uppercase tracking-wider font-bold">
                  {compState === 'running' ? 'Already Running' : 'Start Competition'}
                </span>
              </button>
            </div>
          </div>

          <!-- Controls -->
          <div class="card p-6">
            <h3 class="text-sm font-bold display mb-4">Controls</h3>
            <div class="space-y-3">
              <!-- Extend -->
              <div class="flex gap-2">
                <input type="number" class="input flex-1" bind:value={extendMins} min="1" max="120" />
                <button class="btn btn-pill px-5 py-2 text-xs" onclick={extendCompetition}
                        style="background: var(--bg-elevated); border: 1px solid var(--border);"
                        disabled={compState !== 'running'}>
                  <span class="mono">+ Extend</span>
                </button>
              </div>

              <!-- Stop -->
              <button class="btn btn-pill w-full py-3 text-sm" onclick={stopCompetition}
                      style="background: rgba(244,63,94,0.1); border: 1px solid rgba(244,63,94,0.3); color: #fb7185;"
                      disabled={compState !== 'running'}>
                <span class="mono uppercase tracking-wider font-bold">Stop Competition</span>
              </button>

              <!-- Reset Leaderboard -->
              <button class="btn btn-pill w-full py-3 text-sm" onclick={resetLeaderboard}
                      style="background: rgba(245,158,11,0.1); border: 1px solid rgba(245,158,11,0.3); color: #fbbf24;">
                <span class="mono uppercase tracking-wider font-bold">Reset Leaderboard</span>
              </button>
            </div>
          </div>

          <!-- Stats -->
          <div class="card p-6 md:col-span-2">
            <h3 class="text-sm font-bold display mb-4">Quick Stats</h3>
            <div class="grid grid-cols-4 gap-4">
              <div class="bg-[var(--bg-surface)] rounded-xl p-4 text-center">
                <div class="text-2xl font-black mono text-[var(--accent)]">{teams.length}</div>
                <div class="text-[10px] uppercase tracking-wider text-[var(--text-muted)]">Teams</div>
              </div>
              <div class="bg-[var(--bg-surface)] rounded-xl p-4 text-center">
                <div class="text-2xl font-black mono text-[var(--cyan)]">{jobs.length}</div>
                <div class="text-[10px] uppercase tracking-wider text-[var(--text-muted)]">Total Jobs</div>
              </div>
              <div class="bg-[var(--bg-surface)] rounded-xl p-4 text-center">
                <div class="text-2xl font-black mono text-[var(--emerald)]">{jobs.filter(j => j.status === 'scored').length}</div>
                <div class="text-[10px] uppercase tracking-wider text-[var(--text-muted)]">Scored</div>
              </div>
              <div class="bg-[var(--bg-surface)] rounded-xl p-4 text-center">
                <div class="text-2xl font-black mono text-rose-400">{jobs.filter(j => j.status === 'failed').length}</div>
                <div class="text-[10px] uppercase tracking-wider text-[var(--text-muted)]">Failed</div>
              </div>
            </div>
          </div>
        </div>
      {:else if activeTab === 'teams'}
        <div class="card overflow-hidden">
          <table class="w-full text-sm">
            <thead>
              <tr class="border-b border-[var(--border)] text-[var(--text-muted)] text-xs uppercase tracking-wider">
                <th class="text-left py-3 px-5">#</th>
                <th class="text-left py-3 px-5">Team</th>
                <th class="text-right py-3 px-5">Best Score</th>
                <th class="text-right py-3 px-5">Submissions</th>
              </tr>
            </thead>
            <tbody>
              {#each teams as team, i}
                <tr class="border-b border-[var(--border-subtle)] hover:bg-[var(--bg-surface)]/50">
                  <td class="py-3 px-5 mono text-[var(--text-muted)]">{i + 1}</td>
                  <td class="py-3 px-5 mono font-bold">{team.team_name}</td>
                  <td class="py-3 px-5 mono text-right text-[var(--accent)]">{team.best_score?.toFixed(4) ?? '—'}</td>
                  <td class="py-3 px-5 mono text-right text-[var(--text-secondary)]">{team.submissions}</td>
                </tr>
              {:else}
                <tr><td colspan="4" class="py-8 text-center text-[var(--text-muted)]">No teams registered</td></tr>
              {/each}
            </tbody>
          </table>
        </div>
      {:else if activeTab === 'jobs'}
        <div class="card overflow-hidden">
          <table class="w-full text-sm">
            <thead>
              <tr class="border-b border-[var(--border)] text-[var(--text-muted)] text-xs uppercase tracking-wider">
                <th class="text-left py-3 px-5">Job ID</th>
                <th class="text-left py-3 px-5">Team</th>
                <th class="text-left py-3 px-5">File</th>
                <th class="text-left py-3 px-5">Status</th>
                <th class="text-right py-3 px-5">Score</th>
                <th class="text-right py-3 px-5">When</th>
              </tr>
            </thead>
            <tbody>
              {#each jobs.slice(0, 50) as job}
                <tr class="border-b border-[var(--border-subtle)] hover:bg-[var(--bg-surface)]/50">
                  <td class="py-3 px-5 mono text-xs text-[var(--text-muted)]">{job.job_id?.slice(0, 8)}</td>
                  <td class="py-3 px-5 mono font-bold text-sm">{job.team_name}</td>
                  <td class="py-3 px-5 mono text-xs text-[var(--text-secondary)]">{job.filename}</td>
                  <td class="py-3 px-5">
                    <span class="badge {job.status === 'scored' ? 'badge-emerald' : job.status === 'failed' ? 'badge-rose' : 'badge-amber'}">
                      {job.status}
                    </span>
                  </td>
                  <td class="py-3 px-5 mono text-right text-[var(--accent)]">{job.score?.toFixed(4) ?? '—'}</td>
                  <td class="py-3 px-5 mono text-right text-xs text-[var(--text-muted)]">{timeAgo(job.submitted_at)}</td>
                </tr>
              {:else}
                <tr><td colspan="6" class="py-8 text-center text-[var(--text-muted)]">No jobs recorded</td></tr>
              {/each}
            </tbody>
          </table>
        </div>
      {/if}
    </div>
  {/if}
</div>
