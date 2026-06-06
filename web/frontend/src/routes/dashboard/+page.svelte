<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { auth } from '$lib/auth';
  import { randomQuote } from '$lib/quotes';
  import { connectWS, onWSEvent, wsState, type WSEvent } from '$lib/ws';

  let token = $state('');
  let team = $state('');
  let mounted = $state(false);
  let quote = $state({ text: '', author: '' });
  let backendOnline = $state(false);

  // Real stats from /api/metrics
  let totalSubmissions = $state(0);
  let queueDepth = $state(0);
  let uptimeSeconds = $state(0);
  let connectedWS = $state(0);

  // Real leaderboard from /api/leaderboard
  interface MiniEntry { rank: number; team: string; score: number; }
  let topTeams = $state<MiniEntry[]>([]);

  // Real activity from /api/activity
  interface Activity {
    id: string;
    team: string;
    action: string;
    status: string;
    submitted_at: string;
  }
  let activities = $state<Activity[]>([]);

  // System status
  let systemStatus = $state<any>(null);

  onMount(() => {
    mounted = true;
    const q = randomQuote();
    quote = typeof q === 'string' ? { text: q, author: '' } : q;
    const unsub = auth.subscribe(s => {
      token = s.token || '';
      team = s.team || '';
    });

    // Load real data
    loadMetrics();
    loadLeaderboard();
    loadActivity();
    loadSystemStatus();

    // Connect WebSocket for live updates
    connectWS();
    const unsubLeaderboard = onWSEvent('leaderboard', (evt: WSEvent) => {
      if (evt.data && Array.isArray(evt.data)) {
        topTeams = evt.data.slice(0, 5).map((e: any, i: number) => ({
          rank: i + 1, team: e.team_name, score: e.score,
        }));
      }
    });
    const unsubActivity = onWSEvent('*', (evt: WSEvent) => {
      if (['submission', 'scored', 'failed'].includes(evt.type)) {
        loadActivity(); // Refresh activity on events
      }
    });

    // Refresh metrics every 10s
    const metricsInterval = setInterval(loadMetrics, 10000);
    const quoteInterval = setInterval(() => {
      const q = randomQuote();
      quote = typeof q === 'string' ? { text: q, author: '' } : q;
    }, 15000);

    return () => {
      unsub();
      unsubLeaderboard();
      unsubActivity();
      clearInterval(metricsInterval);
      clearInterval(quoteInterval);
    };
  });

  async function loadMetrics() {
    try {
      const res = await fetch('/api/metrics');
      if (res.ok) {
        const data = await res.json();
        totalSubmissions = data.total_submissions || 0;
        queueDepth = data.queue_depth || 0;
        uptimeSeconds = data.uptime_seconds || 0;
        connectedWS = data.connected_websockets || 0;
        backendOnline = true;
      } else {
        backendOnline = false;
      }
    } catch {
      backendOnline = false;
    }
  }

  async function loadLeaderboard() {
    try {
      const res = await fetch('/api/leaderboard');
      if (res.ok) {
        const data = await res.json();
        topTeams = data.slice(0, 5).map((e: any, i: number) => ({
          rank: i + 1, team: e.team_name, score: e.score,
        }));
      }
    } catch {}
  }

  async function loadActivity() {
    try {
      const res = await fetch('/api/activity');
      if (res.ok) {
        const data = await res.json();
        activities = data.map((a: any) => ({
          id: a.id,
          team: a.team,
          action: a.action,
          status: a.status,
          submitted_at: a.submitted_at,
        }));
      }
    } catch {}
  }

  async function loadSystemStatus() {
    try {
      const res = await fetch('/api/system/status');
      if (res.ok) {
        systemStatus = await res.json();
      }
    } catch {}
  }

  function formatUptime(s: number): string {
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const sec = Math.floor(s % 60);
    return `${h}h ${m}m ${sec}s`;
  }

  function timeAgo(iso: string): string {
    if (!iso) return '';
    const diff = Date.now() - new Date(iso).getTime();
    const mins = Math.floor(diff / 60000);
    if (mins < 1) return 'just now';
    if (mins < 60) return `${mins}m ago`;
    const hrs = Math.floor(mins / 60);
    if (hrs < 24) return `${hrs}h ago`;
    return `${Math.floor(hrs / 24)}d ago`;
  }

  function statusBadge(s: string): string {
    if (s === 'scored') return 'badge-emerald';
    if (s === 'compiling' || s === 'running') return 'badge-cyan';
    if (s === 'queued') return 'badge-amber';
    return 'badge-rose';
  }

  function medal(r: number): string {
    return r < 10 ? '0' + r : String(r);
  }
</script>

<svelte:head>
  <title>Dashboard — IICPC Arena</title>
</svelte:head>

{#if mounted}
<div class="w-full xl:max-w-[1400px] mx-auto px-6 fade-in">
  <!-- Greeting -->
  <div class="mb-10">
    <h1 class="text-4xl font-black display tracking-tight mb-2">
      Welcome back<span class="text-[var(--text-ghost)]">,</span>
      <span class="text-gradient-accent mono">{team}</span>
    </h1>
    <p class="text-sm text-[var(--text-secondary)] max-w-lg">Your competition command center. All data is live from the backend.</p>
  </div>

  <!-- Backend Status Banner -->
  {#if !backendOnline}
    <div class="card p-5 border-rose-500/20 mb-8 flex items-center gap-4 fade-in">
      <div class="w-10 h-10 rounded-xl flex items-center justify-center bg-rose-500/10">
        <svg class="w-5 h-5 text-rose-400" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
          <path stroke-linecap="round" stroke-linejoin="round" d="M12 9v3.75m-9.303 3.376c-.866 1.5.217 3.374 1.948 3.374h14.71c1.73 0 2.813-1.874 1.948-3.374L13.949 3.378c-.866-1.5-3.032-1.5-3.898 0L2.697 16.126zM12 15.75h.007v.008H12v-.008z"/>
        </svg>
      </div>
      <div>
        <div class="font-semibold text-sm text-rose-400">Backend Offline</div>
        <div class="text-xs text-[var(--text-muted)] mt-0.5">Run <code class="mono bg-[var(--bg-elevated)] px-1.5 py-0.5 rounded">bash start.sh</code> from the project root to launch the full stack.</div>
      </div>
    </div>
  {/if}

  <!-- Stats Grid -->
  <div class="grid grid-cols-2 lg:grid-cols-4 gap-4 mb-8 stagger">
    <!-- Total Submissions -->
    <div class="card p-5 group relative overflow-hidden">
      <div class="absolute inset-0 bg-gradient-to-br from-[var(--accent)]/5 to-transparent opacity-0 group-hover:opacity-100 transition-opacity"></div>
      <div class="relative z-10">
        <div class="flex items-center justify-between mb-3">
          <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">Submissions</span>
          <div class="w-7 h-7 rounded-lg flex items-center justify-center bg-[var(--accent)]/[0.08]">
            <svg class="w-3.5 h-3.5 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
            </svg>
          </div>
        </div>
        <div class="text-3xl font-black mono text-[var(--text-primary)]">{totalSubmissions}</div>
        <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">total uploads</div>
      </div>
    </div>

    <!-- Queue -->
    <div class="card p-5 group relative overflow-hidden">
      <div class="absolute inset-0 bg-gradient-to-br from-[var(--cyan)]/5 to-transparent opacity-0 group-hover:opacity-100 transition-opacity"></div>
      <div class="relative z-10">
        <div class="flex items-center justify-between mb-3">
          <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">Queue</span>
          <div class="w-7 h-7 rounded-lg flex items-center justify-center" style="background: var(--cyan-dim);">
            <svg class="w-3.5 h-3.5 text-[var(--cyan)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 12h16.5m-16.5 3.75h16.5M3.75 19.5h16.5M5.625 4.5h12.75a1.875 1.875 0 010 3.75H5.625a1.875 1.875 0 010-3.75z"/>
            </svg>
          </div>
        </div>
        <div class="text-3xl font-black mono text-[var(--text-primary)]">{queueDepth}</div>
        <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">pending jobs</div>
      </div>
    </div>

    <!-- Uptime -->
    <div class="card p-5 group relative overflow-hidden">
      <div class="absolute inset-0 bg-gradient-to-br from-[var(--emerald)]/5 to-transparent opacity-0 group-hover:opacity-100 transition-opacity"></div>
      <div class="relative z-10">
        <div class="flex items-center justify-between mb-3">
          <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">Uptime</span>
          <div class="w-7 h-7 rounded-lg flex items-center justify-center" style="background: var(--emerald-dim);">
            <svg class="w-3.5 h-3.5 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M12 6v6h4.5m4.5 0a9 9 0 11-18 0 9 9 0 0118 0z"/>
            </svg>
          </div>
        </div>
        <div class="text-3xl font-black mono text-[var(--text-primary)]">{formatUptime(uptimeSeconds)}</div>
        <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">engine runtime</div>
      </div>
    </div>

    <!-- WebSocket -->
    <div class="card p-5 group relative overflow-hidden">
      <div class="absolute inset-0 bg-gradient-to-br from-[var(--violet)]/5 to-transparent opacity-0 group-hover:opacity-100 transition-opacity"></div>
      <div class="relative z-10">
        <div class="flex items-center justify-between mb-3">
          <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">Live Clients</span>
          <div class="w-7 h-7 rounded-lg flex items-center justify-center" style="background: var(--violet-dim);">
            <svg class="w-3.5 h-3.5 text-[var(--violet)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M8.288 15.038a5.25 5.25 0 017.424 0M5.106 11.856c3.807-3.808 9.98-3.808 13.788 0M1.924 8.674c5.565-5.565 14.587-5.565 20.152 0"/>
            </svg>
          </div>
        </div>
        <div class="text-3xl font-black mono text-[var(--text-primary)]">{connectedWS}</div>
        <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">websocket connections</div>
      </div>
    </div>
  </div>

  <div class="grid lg:grid-cols-3 gap-6 mb-8">
    <!-- Quick Actions -->
    <div class="lg:col-span-2 grid sm:grid-cols-2 gap-4 stagger">
      <button
        class="card p-8 text-left group relative overflow-hidden transition-all duration-300 hover:border-[var(--accent)]/30 hover:shadow-[0_0_40px_rgba(99,91,255,0.06)]"
        onclick={() => goto('/dashboard/submit')}
      >
        <div class="absolute inset-0 bg-gradient-to-br from-[var(--accent)]/[0.04] to-transparent opacity-0 group-hover:opacity-100 transition-opacity"></div>
        <div class="relative z-10">
          <div class="flex items-center justify-between mb-6">
            <div class="w-14 h-14 rounded-2xl flex items-center justify-center"
                 style="background: linear-gradient(135deg, var(--accent-glow), rgba(139,92,246,0.05));">
              <svg class="w-6 h-6 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
              </svg>
            </div>
            <svg class="w-5 h-5 text-[var(--text-ghost)] group-hover:text-[var(--accent)] group-hover:translate-x-1 transition-all" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24"><path d="M9 5l7 7-7 7"/></svg>
          </div>
          <h3 class="text-xl font-bold display mb-2">Submit Code</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Upload your C++ engine for automated compilation, sandboxed benchmarking, and scoring.</p>
        </div>
      </button>

      <button
        class="card p-8 text-left group relative overflow-hidden transition-all duration-300 hover:border-amber-500/30 hover:shadow-[0_0_40px_rgba(251,191,36,0.06)]"
        onclick={() => goto('/dashboard/leaderboard')}
      >
        <div class="absolute inset-0 bg-gradient-to-br from-amber-400/[0.04] to-transparent opacity-0 group-hover:opacity-100 transition-opacity"></div>
        <div class="relative z-10">
          <div class="flex items-center justify-between mb-6">
            <div class="w-14 h-14 rounded-2xl flex items-center justify-center"
                 style="background: linear-gradient(135deg, var(--amber-dim), rgba(251,191,36,0.05));">
              <svg class="w-6 h-6 text-[var(--amber)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M16.5 18.75h-9m9 0a3 3 0 013 3h-15a3 3 0 013-3m9 0v-3.375c0-.621-.503-1.125-1.125-1.125h-.871M7.5 18.75v-3.375c0-.621.504-1.125 1.125-1.125h.872m5.007 0H9.497m5.007 0a7.454 7.454 0 01-.982-3.172M9.497 14.25a7.454 7.454 0 00.981-3.172M5.25 4.236c-.982.143-1.954.317-2.916.52A6.003 6.003 0 007.73 9.728M5.25 4.236V4.5c0 2.108.966 3.99 2.48 5.228M5.25 4.236V2.721C7.456 2.41 9.71 2.25 12 2.25c2.291 0 4.545.16 6.75.47v1.516M18.75 4.236c.982.143 1.954.317 2.916.52A6.003 6.003 0 0016.27 9.728M18.75 4.236V4.5c0 2.108-.966 3.99-2.48 5.228m0 0a6.003 6.003 0 01-5.54 0"/>
              </svg>
            </div>
            <svg class="w-5 h-5 text-[var(--text-ghost)] group-hover:text-amber-400 group-hover:translate-x-1 transition-all" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24"><path d="M9 5l7 7-7 7"/></svg>
          </div>
          <h3 class="text-xl font-bold display mb-2">Rankings</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Live leaderboard with scores, throughput, and latency across all teams.</p>
        </div>
      </button>
    </div>

    <!-- Mini Leaderboard -->
    <div class="card p-0 overflow-hidden">
      <div class="px-5 py-4 border-b border-[var(--border)] flex items-center justify-between">
        <span class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)]">Top 5</span>
        <button class="text-[10px] text-[var(--accent)] hover:underline" onclick={() => goto('/dashboard/leaderboard')}>View all →</button>
      </div>
      {#if topTeams.length > 0}
        <div class="divide-y divide-[var(--border-subtle)]">
          {#each topTeams as entry}
            <div class="flex items-center justify-between px-5 py-3 hover:bg-white/[0.01] transition-colors
              {entry.team === team ? 'bg-[var(--accent)]/[0.03]' : ''}">
              <div class="flex items-center gap-3">
                <span class="text-sm {entry.rank <= 3 ? 'font-bold' : 'mono text-[var(--text-muted)]'}">{medal(entry.rank)}</span>
                <div class="w-6 h-6 rounded-md flex items-center justify-center text-[8px] font-bold mono"
                     style="background: rgba(255,255,255,0.03); color: var(--text-muted);">
                  {entry.team.slice(0, 2).toUpperCase()}
                </div>
                <span class="text-sm mono font-medium {entry.team === team ? 'text-[var(--accent)]' : ''}">{entry.team}</span>
              </div>
              <span class="text-sm mono font-bold
                {entry.score >= 0.9 ? 'text-emerald-400' :
                 entry.score >= 0.7 ? 'text-cyan-400' :
                 entry.score >= 0.5 ? 'text-amber-400' : 'text-rose-400'}">{entry.score.toFixed(3)}</span>
            </div>
          {/each}
        </div>
      {:else}
        <div class="px-5 py-12 text-center">
          <div class="text-sm text-[var(--text-muted)]">No teams ranked yet</div>
          <div class="text-[10px] text-[var(--text-ghost)] mt-1">Be the first to submit!</div>
        </div>
      {/if}
    </div>
  </div>

  <!-- Activity Feed -->
  <div class="card p-0 overflow-hidden mb-8 fade-up" style="animation-delay: 0.2s;">
    <div class="px-5 py-4 border-b border-[var(--border)] flex items-center justify-between">
      <span class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)]">Recent Activity</span>
      <div class="flex items-center gap-2">
        {#if backendOnline}
          <div class="flex items-center gap-1.5">
            <div class="status-dot status-dot-online" style="width: 6px; height: 6px;"></div>
            <span class="text-[10px] text-[var(--text-muted)]">Live</span>
          </div>
        {:else}
          <div class="flex items-center gap-1.5">
            <div class="status-dot status-dot-offline" style="width: 6px; height: 6px;"></div>
            <span class="text-[10px] text-[var(--text-muted)]">Offline</span>
          </div>
        {/if}
      </div>
    </div>
    {#if activities.length > 0}
      <div class="divide-y divide-[var(--border-subtle)] stagger">
        {#each activities as activity (activity.id)}
          <div class="flex items-center justify-between px-5 py-3.5 hover:bg-white/[0.01] transition-colors">
            <div class="flex items-center gap-3">
              <div class="w-7 h-7 rounded-md flex items-center justify-center text-[10px] font-bold mono"
                   style="background: rgba(255,255,255,0.03); color: var(--text-muted);">
                {activity.team.slice(0, 2).toUpperCase()}
              </div>
              <div>
                <span class="text-sm font-medium mono">{activity.team}</span>
                <span class="text-xs text-[var(--text-muted)] ml-2">{activity.action}</span>
              </div>
            </div>
            <div class="flex items-center gap-3">
              <span class="badge {statusBadge(activity.status)}">{activity.status}</span>
              <span class="text-[10px] text-[var(--text-ghost)] mono">{timeAgo(activity.submitted_at)}</span>
            </div>
          </div>
        {/each}
      </div>
    {:else}
      <div class="px-5 py-12 text-center">
        <div class="text-sm text-[var(--text-muted)]">No activity yet</div>
        <div class="text-[10px] text-[var(--text-ghost)] mt-1">Submit your first engine to get started</div>
      </div>
    {/if}
  </div>

  <!-- System Status -->
  {#if systemStatus}
    <div class="card p-0 overflow-hidden mb-8 fade-up" style="animation-delay: 0.3s;">
      <div class="px-5 py-4 border-b border-[var(--border)]">
        <span class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)]">System Status</span>
      </div>
      <div class="grid grid-cols-2 sm:grid-cols-4 gap-px bg-[var(--border-subtle)]">
        <div class="bg-[var(--bg-surface)] px-5 py-4">
          <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Isolation</div>
          <div class="text-sm font-medium mono">{systemStatus.isolation_mode}</div>
        </div>
        <div class="bg-[var(--bg-surface)] px-5 py-4">
          <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Redis</div>
          <div class="text-sm font-medium mono {systemStatus.redis_connected ? 'text-emerald-400' : 'text-rose-400'}">{systemStatus.redis_connected ? 'Connected' : 'Down'}</div>
        </div>
        <div class="bg-[var(--bg-surface)] px-5 py-4">
          <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">KVM</div>
          <div class="text-sm font-medium mono">{systemStatus.kvm_available ? 'Available' : 'N/A'}</div>
        </div>
        <div class="bg-[var(--bg-surface)] px-5 py-4">
          <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Ready</div>
          <div class="text-sm font-medium mono {systemStatus.ready ? 'text-emerald-400' : 'text-amber-400'}">{systemStatus.ready ? 'Yes' : 'Partial'}</div>
        </div>
      </div>
    </div>
  {/if}

  <!-- Bottom stats bar -->
  <div class="flex items-center justify-between text-[10px] text-[var(--text-ghost)] px-1 mono">
    <span>Uptime: {formatUptime(uptimeSeconds)}</span>
    <span>Engine v1.0.0 · {new Date().toLocaleDateString()}</span>
  </div>
</div>
{/if}
