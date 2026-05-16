<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { auth } from '$lib/auth';
  import { randomQuote } from '$lib/quotes';

  let team = $state('');
  let mounted = $state(false);
  let quote = $state('');

  // Simulated live stats
  let engineTPS = $state(0);
  let p99Latency = $state(0);
  let activeTeams = $state(0);
  let queueDepth = $state(0);
  let totalSubmissions = $state(0);
  let uptime = $state('');

  // Recent activity feed
  interface Activity {
    id: string;
    team: string;
    action: string;
    time: string;
    status: 'scored' | 'compiling' | 'queued' | 'failed';
  }

  let activities = $state<Activity[]>([
    { id: '1', team: 'quantum_traders', action: 'Scored 0.924', time: '2m ago', status: 'scored' },
    { id: '2', team: 'zero_latency', action: 'Running benchmark', time: '5m ago', status: 'compiling' },
    { id: '3', team: 'cache_lords', action: 'Scored 0.853', time: '8m ago', status: 'scored' },
    { id: '4', team: 'lock_free_gang', action: 'Queued', time: '12m ago', status: 'queued' },
    { id: '5', team: 'simd_warriors', action: 'Build failed', time: '15m ago', status: 'failed' },
  ]);

  // Mini leaderboard
  interface MiniEntry {
    rank: number;
    team: string;
    score: number;
  }
  let topTeams = $state<MiniEntry[]>([
    { rank: 1, team: 'quantum_traders', score: 0.924 },
    { rank: 2, team: 'zero_latency', score: 0.881 },
    { rank: 3, team: 'cache_lords', score: 0.853 },
    { rank: 4, team: 'lock_free_gang', score: 0.791 },
    { rank: 5, team: 'simd_warriors', score: 0.714 },
  ]);

  onMount(() => {
    mounted = true;
    quote = randomQuote();
    const unsub = auth.subscribe(s => {
      team = s.team || '';
    });

    // Animate stats
    animateNumber(() => engineTPS, v => engineTPS = v, 13990000, 60);
    animateNumber(() => p99Latency, v => p99Latency = v, 810, 40);
    animateNumber(() => activeTeams, v => activeTeams = v, 47, 30);
    animateNumber(() => totalSubmissions, v => totalSubmissions = v, 312, 35);

    // Simulate queue depth changes
    const queueInterval = setInterval(() => {
      queueDepth = Math.floor(Math.random() * 3);
    }, 3000);

    // Uptime counter
    const start = Date.now();
    const uptimeInterval = setInterval(() => {
      const diff = Date.now() - start + 86400000; // pretend 24h+
      const h = Math.floor(diff / 3600000);
      const m = Math.floor((diff % 3600000) / 60000);
      const s = Math.floor((diff % 60000) / 1000);
      uptime = `${h}h ${m}m ${s}s`;
    }, 1000);

    const qi = setInterval(() => quote = randomQuote(), 15000);
    return () => { unsub(); clearInterval(qi); clearInterval(queueInterval); clearInterval(uptimeInterval); };
  });

  function animateNumber(getter: () => number, setter: (v: number) => void, target: number, frames: number) {
    let frame = 0;
    const interval = setInterval(() => {
      frame++;
      const progress = frame / frames;
      const eased = 1 - Math.pow(1 - progress, 3);
      setter(Math.round(target * eased));
      if (frame >= frames) clearInterval(interval);
    }, 30);
  }

  function formatTPS(n: number): string {
    if (n >= 1_000_000) return (n / 1_000_000).toFixed(2) + 'M';
    if (n >= 1_000) return (n / 1_000).toFixed(1) + 'K';
    return n.toString();
  }

  function statusBadge(s: string): string {
    if (s === 'scored') return 'badge-emerald';
    if (s === 'compiling') return 'badge-cyan';
    if (s === 'queued') return 'badge-amber';
    return 'badge-rose';
  }

  function medal(r: number): string {
    if (r === 1) return '🥇';
    if (r === 2) return '🥈';
    if (r === 3) return '🥉';
    return String(r);
  }
</script>

<svelte:head>
  <title>Dashboard — IICPC Arena</title>
</svelte:head>

{#if mounted}
<div class="max-w-6xl mx-auto fade-in">
  <!-- Greeting -->
  <div class="mb-10">
    <h1 class="text-3xl font-bold display tracking-tight mb-1">
      Welcome back<span class="text-[var(--text-ghost)]">,</span>
      <span class="text-gradient-accent mono">{team}</span>
    </h1>
    <p class="text-sm text-[var(--text-secondary)]">Here's your arena at a glance.</p>
  </div>

  <!-- Stats Grid -->
  <div class="grid grid-cols-2 lg:grid-cols-4 gap-4 mb-8 stagger">
    <!-- Engine TPS -->
    <div class="card p-5 group">
      <div class="flex items-center justify-between mb-3">
        <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">Engine TPS</span>
        <div class="w-7 h-7 rounded-lg flex items-center justify-center bg-[var(--accent)]/[0.08]">
          <svg class="w-3.5 h-3.5 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 13.5l10.5-11.25L12 10.5h8.25L9.75 21.75 12 13.5H3.75z"/>
          </svg>
        </div>
      </div>
      <div class="text-2xl font-bold mono text-[var(--text-primary)]">{formatTPS(engineTPS)}</div>
      <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">transactions/sec</div>
    </div>

    <!-- p99 Latency -->
    <div class="card p-5 group">
      <div class="flex items-center justify-between mb-3">
        <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">p99 Latency</span>
        <div class="w-7 h-7 rounded-lg flex items-center justify-center" style="background: var(--cyan-dim);">
          <svg class="w-3.5 h-3.5 text-[var(--cyan)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M12 6v6h4.5m4.5 0a9 9 0 11-18 0 9 9 0 0118 0z"/>
          </svg>
        </div>
      </div>
      <div class="text-2xl font-bold mono text-[var(--text-primary)]">{p99Latency}<span class="text-sm text-[var(--text-muted)] ml-1">ns</span></div>
      <div class="text-[10px] text-emerald-400 mt-1 mono">↓ 12% from baseline</div>
    </div>

    <!-- Active Teams -->
    <div class="card p-5 group">
      <div class="flex items-center justify-between mb-3">
        <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">Active Teams</span>
        <div class="w-7 h-7 rounded-lg flex items-center justify-center" style="background: var(--violet-dim);">
          <svg class="w-3.5 h-3.5 text-[var(--violet)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M15 19.128a9.38 9.38 0 002.625.372 9.337 9.337 0 004.121-.952 4.125 4.125 0 00-7.533-2.493M15 19.128v-.003c0-1.113-.285-2.16-.786-3.07M15 19.128v.106A12.318 12.318 0 018.624 21c-2.331 0-4.512-.645-6.374-1.766l-.001-.109a6.375 6.375 0 0111.964-3.07M12 6.375a3.375 3.375 0 11-6.75 0 3.375 3.375 0 016.75 0zm8.25 2.25a2.625 2.625 0 11-5.25 0 2.625 2.625 0 015.25 0z"/>
          </svg>
        </div>
      </div>
      <div class="text-2xl font-bold mono text-[var(--text-primary)]">{activeTeams}</div>
      <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">{totalSubmissions} total submissions</div>
    </div>

    <!-- Queue -->
    <div class="card p-5 group">
      <div class="flex items-center justify-between mb-3">
        <span class="text-[10px] text-[var(--text-muted)] uppercase tracking-[0.1em] font-bold">Queue</span>
        <div class="w-7 h-7 rounded-lg flex items-center justify-center" style="background: var(--amber-dim);">
          <svg class="w-3.5 h-3.5 text-[var(--amber)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 12h16.5m-16.5 3.75h16.5M3.75 19.5h16.5M5.625 4.5h12.75a1.875 1.875 0 010 3.75H5.625a1.875 1.875 0 010-3.75z"/>
          </svg>
        </div>
      </div>
      <div class="text-2xl font-bold mono text-[var(--text-primary)]">{queueDepth}</div>
      <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">pending jobs</div>
    </div>
  </div>

  <div class="grid lg:grid-cols-3 gap-6 mb-8">
    <!-- Quick Actions -->
    <div class="lg:col-span-2 grid sm:grid-cols-2 gap-4 stagger">
      <button
        class="card-action p-8 text-left group"
        style="--action-color: var(--accent);"
        onclick={() => goto('/dashboard/submit')}
      >
        <div class="absolute inset-0 bg-gradient-to-br from-[var(--accent)]/[0.03] to-transparent opacity-0 group-hover:opacity-100 transition-opacity rounded-3xl"></div>
        <div class="relative z-10">
          <div class="flex items-center justify-between mb-6">
            <div class="w-12 h-12 rounded-2xl flex items-center justify-center"
                 style="background: var(--accent-glow);">
              <svg class="w-5 h-5 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
              </svg>
            </div>
            <svg class="w-5 h-5 text-[var(--text-ghost)] group-hover:text-[var(--accent)] group-hover:translate-x-1 transition-all" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24"><path d="M9 5l7 7-7 7"/></svg>
          </div>
          <h3 class="text-lg font-semibold display mb-1.5">Submit Code</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Upload your C++ engine for automated compilation, sandboxed benchmarking, and scoring.</p>
        </div>
      </button>

      <button
        class="card-action p-8 text-left group"
        onclick={() => goto('/dashboard/leaderboard')}
      >
        <div class="absolute inset-0 bg-gradient-to-br from-amber-400/[0.03] to-transparent opacity-0 group-hover:opacity-100 transition-opacity rounded-3xl"></div>
        <div class="relative z-10">
          <div class="flex items-center justify-between mb-6">
            <div class="w-12 h-12 rounded-2xl flex items-center justify-center"
                 style="background: var(--amber-dim);">
              <svg class="w-5 h-5 text-[var(--amber)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M16.5 18.75h-9m9 0a3 3 0 013 3h-15a3 3 0 013-3m9 0v-3.375c0-.621-.503-1.125-1.125-1.125h-.871M7.5 18.75v-3.375c0-.621.504-1.125 1.125-1.125h.872m5.007 0H9.497m5.007 0a7.454 7.454 0 01-.982-3.172M9.497 14.25a7.454 7.454 0 00.981-3.172M5.25 4.236c-.982.143-1.954.317-2.916.52A6.003 6.003 0 007.73 9.728M5.25 4.236V4.5c0 2.108.966 3.99 2.48 5.228M5.25 4.236V2.721C7.456 2.41 9.71 2.25 12 2.25c2.291 0 4.545.16 6.75.47v1.516M18.75 4.236c.982.143 1.954.317 2.916.52A6.003 6.003 0 0016.27 9.728M18.75 4.236V4.5c0 2.108-.966 3.99-2.48 5.228m0 0a6.003 6.003 0 01-5.54 0"/>
              </svg>
            </div>
            <svg class="w-5 h-5 text-[var(--text-ghost)] group-hover:text-amber-400 group-hover:translate-x-1 transition-all" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24"><path d="M9 5l7 7-7 7"/></svg>
          </div>
          <h3 class="text-lg font-semibold display mb-1.5">Rankings</h3>
          <p class="text-xs text-[var(--text-muted)] leading-relaxed">Live leaderboard with scores, throughput, latency, and correctness across all teams.</p>
        </div>
      </button>
    </div>

    <!-- Mini Leaderboard -->
    <div class="card p-0 overflow-hidden">
      <div class="px-5 py-4 border-b border-[var(--border)] flex items-center justify-between">
        <span class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)]">Top 5</span>
        <button class="text-[10px] text-[var(--accent)] hover:underline" onclick={() => goto('/dashboard/leaderboard')}>View all →</button>
      </div>
      <div class="divide-y divide-[var(--border-subtle)]">
        {#each topTeams as entry}
          <div class="flex items-center justify-between px-5 py-3 hover:bg-white/[0.01] transition-colors
            {entry.team === team ? 'bg-[var(--accent)]/[0.03]' : ''}">
            <div class="flex items-center gap-3">
              <span class="text-sm {entry.rank <= 3 ? '' : 'mono text-[var(--text-muted)]'}">{medal(entry.rank)}</span>
              <span class="text-sm mono font-medium {entry.team === team ? 'text-[var(--accent)]' : ''}">{entry.team}</span>
            </div>
            <span class="text-sm mono font-bold
              {entry.score >= 0.9 ? 'text-emerald-400' :
               entry.score >= 0.7 ? 'text-cyan-400' :
               entry.score >= 0.5 ? 'text-amber-400' : 'text-rose-400'}">{entry.score.toFixed(3)}</span>
          </div>
        {/each}
      </div>
    </div>
  </div>

  <!-- Activity Feed -->
  <div class="card p-0 overflow-hidden mb-8 fade-up" style="animation-delay: 0.2s;">
    <div class="px-5 py-4 border-b border-[var(--border)] flex items-center justify-between">
      <span class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)]">Recent Activity</span>
      <div class="flex items-center gap-1.5">
        <div class="status-dot status-dot-online" style="width: 6px; height: 6px;"></div>
        <span class="text-[10px] text-[var(--text-muted)]">Live</span>
      </div>
    </div>
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
            <span class="text-[10px] text-[var(--text-ghost)] mono">{activity.time}</span>
          </div>
        </div>
      {/each}
    </div>
  </div>

  <!-- Bottom stats bar -->
  <div class="flex items-center justify-between text-[10px] text-[var(--text-ghost)] px-1 mono">
    <span>Uptime: {uptime}</span>
    <span>Engine v1.0.0 · {new Date().toLocaleDateString()}</span>
  </div>
</div>
{/if}
