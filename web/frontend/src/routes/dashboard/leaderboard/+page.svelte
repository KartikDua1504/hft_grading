<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { auth } from '$lib/auth';
  import { connectWS, onWSEvent, type WSEvent } from '$lib/ws';

  interface Entry {
    rank: number;
    team: string;
    score: number;
    throughput: number;
    correctness: number;
    p99: number;
    runs: number;
    delta?: number;
  }

  let entries = $state<Entry[]>([]);
  let team = $state('');
  let mounted = $state(false);
  let search = $state('');
  let expandedTeam = $state<string | null>(null);
  let lastUpdate = $state('');
  let loading = $state(true);
  let systestState = $state('idle');
  let systestNotif = $state('');

  onMount(() => {
    mounted = true;
    const unsub = auth.subscribe(s => {
      if (!s.authenticated) goto('/auth');
      team = s.team || '';
    });
    loadLeaderboard();
    lastUpdate = new Date().toLocaleTimeString('en-US', { hour12: false });

    // WebSocket for live updates
    connectWS();
    const unsubWS = onWSEvent('leaderboard', (evt: WSEvent) => {
      if (evt.data && Array.isArray(evt.data)) {
        entries = evt.data.map((e: any, i: number) => ({
          rank: i + 1, team: e.team_name, score: e.score,
          throughput: e.throughput, correctness: e.correctness,
          p99: e.p99_latency_ns, runs: e.submissions, delta: 0,
        }));
        lastUpdate = new Date().toLocaleTimeString('en-US', { hour12: false });
      }
    });

    // System test events
    const unsubSystest = onWSEvent('system_test', (evt: WSEvent) => {
      if (evt.data) {
        systestState = evt.data.state || systestState;
        if (evt.data.state === 'completed') {
          systestNotif = 'System tests complete — rankings updated';
          loadLeaderboard();
          setTimeout(() => { systestNotif = ''; }, 8000);
        } else if (evt.data.state === 'running' && evt.data.current_team) {
          systestNotif = `Testing: ${evt.data.current_team} (${evt.data.progress})`;
        }
      }
    });

    // Auto refresh every 30s as fallback
    const refresh = setInterval(() => {
      loadLeaderboard();
    }, 30000);

    // Poll systest status
    async function pollSystest() {
      try {
        const res = await fetch('/api/system-test/status');
        if (res.ok) {
          const d = await res.json();
          systestState = d.state;
        }
      } catch {}
    }
    pollSystest();

    return () => { unsub(); unsubWS(); unsubSystest(); clearInterval(refresh); };
  });

  async function loadLeaderboard() {
    try {
      const res = await fetch('/api/leaderboard');
      if (res.ok) {
        const data = await res.json();
        entries = data.map((e: any, i: number) => ({
          rank: i + 1, team: e.team_name, score: e.score,
          throughput: e.throughput, correctness: e.correctness,
          p99: e.p99_latency_ns, runs: e.submissions, delta: 0,
        }));
        lastUpdate = new Date().toLocaleTimeString('en-US', { hour12: false });
      }
    } catch {}
    loading = false;
  }

  function filteredEntries(): Entry[] {
    let result = entries;
    if (search) {
      result = result.filter(e => e.team.toLowerCase().includes(search.toLowerCase()));
    }
    return result;
  }

  function fmt(n: number): string {
    if (n >= 1_000_000) return (n / 1_000_000).toFixed(2) + 'M';
    if (n >= 1_000) return (n / 1_000).toFixed(1) + 'K';
    return n.toString();
  }

  function latFmt(ns: number): string {
    if (ns >= 1_000_000) return (ns / 1_000_000).toFixed(1) + 'ms';
    if (ns >= 1_000) return (ns / 1_000).toFixed(1) + 'µs';
    return ns + 'ns';
  }

  function scoreColor(s: number): string {
    if (s >= 0.9) return 'text-emerald-400';
    if (s >= 0.7) return 'text-cyan-400';
    if (s >= 0.5) return 'text-amber-400';
    return 'text-rose-400';
  }

  function scoreBarColor(s: number): string {
    if (s >= 0.9) return 'bg-emerald-500';
    if (s >= 0.7) return 'bg-cyan-500';
    if (s >= 0.5) return 'bg-amber-500';
    return 'bg-rose-500';
  }

  function podiumGradient(r: number): string {
    if (r === 1) return 'from-amber-500/10 to-amber-500/[0.02]';
    if (r === 2) return 'from-slate-400/10 to-slate-400/[0.02]';
    if (r === 3) return 'from-amber-700/10 to-amber-700/[0.02]';
    return '';
  }

  function deltaIcon(d: number | undefined): string {
    if (!d || d === 0) return '—';
    if (d > 0) return `↑${d}`;
    return `↓${Math.abs(d)}`;
  }

  function deltaColor(d: number | undefined): string {
    if (!d || d === 0) return 'text-[var(--text-ghost)]';
    if (d > 0) return 'text-emerald-400';
    return 'text-rose-400';
  }
</script>

<svelte:head>
  <title>Rankings — IICPC Arena</title>
</svelte:head>

{#if mounted}
<div class="w-full xl:max-w-[1400px] mx-auto px-6 fade-in">
  <!-- Header -->
  <div class="flex items-end justify-between mb-8 border-b border-[var(--border)] pb-4">
    <div>
      <div class="flex items-center gap-2 mb-2">
        <div class="w-2 h-2 rounded-full bg-[var(--accent)] flicker"></div>
        <span class="text-[10px] mono uppercase tracking-widest text-[var(--accent)]">
          {systestState === 'completed' ? 'Final Standings' : 'Live Rankings'}
        </span>
        {#if systestState === 'completed'}
          <span class="text-[9px] px-2 py-0.5 rounded-full bg-violet-500/10 text-violet-300 mono font-bold">SYSTEM TESTED</span>
        {/if}
      </div>
      <h1 class="text-3xl font-bold display tracking-tight mb-1 glitch-hover">Rankings</h1>
      <p class="text-xs text-[var(--text-secondary)] mono">{entries.length} teams ranked · SYNC: {lastUpdate}</p>
      {#if systestNotif}
        <div class="mt-2 text-[11px] mono text-violet-300/80 animate-pulse">{systestNotif}</div>
      {/if}
    </div>
    <button class="btn btn-ghost text-xs gap-2 mono" onclick={() => goto('/dashboard/submit')}>
      <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
      </svg>
      Deploy Engine
    </button>
  </div>

  <!-- Podium — Top 3 -->
  {#if entries.length >= 3}
    <div class="grid grid-cols-3 gap-4 mb-8 stagger">
      <!-- 2nd Place -->
      <div class="card p-6 text-center bg-gradient-to-b {podiumGradient(2)} order-1 self-end">
        <div class="w-12 h-12 mx-auto rounded-2xl flex items-center justify-center text-sm font-bold mono mb-3"
             style="background: rgba(148,163,184,0.1); color: rgb(148,163,184);">
          {entries[1].team.slice(0, 2).toUpperCase()}
        </div>
        <div class="text-[10px] text-[var(--text-ghost)] mono tracking-widest uppercase mb-1">Rank 02</div>
        <div class="text-sm font-semibold mono truncate">{entries[1].team}</div>
        <div class="text-2xl font-bold mono mt-2 {scoreColor(entries[1].score)}">{entries[1].score.toFixed(3)}</div>
        <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">{fmt(entries[1].throughput)} ops · {latFmt(entries[1].p99)}</div>
      </div>

      <!-- 1st Place -->
      <div class="card p-8 text-center bg-gradient-to-b {podiumGradient(1)} order-2 relative card-glow">
        <div class="absolute inset-0 pointer-events-none rounded-[var(--radius-lg)]"
             style="background: radial-gradient(circle at 50% 0%, rgba(251,191,36,0.06), transparent 60%);"></div>
        <div class="relative z-10">
          <div class="w-16 h-16 mx-auto rounded-2xl flex items-center justify-center text-lg font-bold mono mb-3"
               style="background: rgba(251,191,36,0.1); color: rgb(251,191,36);">
            {entries[0].team.slice(0, 2).toUpperCase()}
          </div>
          <div class="text-[10px] text-[var(--accent)] mono tracking-widest uppercase mb-1">Rank 01</div>
          <div class="text-base font-bold mono truncate">{entries[0].team}</div>
          <div class="text-3xl font-black mono mt-2 {scoreColor(entries[0].score)} glitch-hover">{entries[0].score.toFixed(3)}</div>
          <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">{fmt(entries[0].throughput)} ops · {latFmt(entries[0].p99)}</div>
          <div class="badge badge-emerald mt-3 mx-auto mono">{entries[0].runs} EXECS</div>
        </div>
      </div>

      <!-- 3rd Place -->
      <div class="card p-6 text-center bg-gradient-to-b {podiumGradient(3)} order-3 self-end">
        <div class="w-12 h-12 mx-auto rounded-2xl flex items-center justify-center text-sm font-bold mono mb-3"
             style="background: rgba(180,83,9,0.1); color: rgb(180,83,9);">
          {entries[2].team.slice(0, 2).toUpperCase()}
        </div>
        <div class="text-[10px] text-[var(--text-ghost)] mono tracking-widest uppercase mb-1">Rank 03</div>
        <div class="text-sm font-semibold mono truncate">{entries[2].team}</div>
        <div class="text-2xl font-bold mono mt-2 {scoreColor(entries[2].score)}">{entries[2].score.toFixed(3)}</div>
        <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">{fmt(entries[2].throughput)} ops · {latFmt(entries[2].p99)}</div>
      </div>
    </div>
  {/if}

  <!-- Search Bar -->
  <div class="flex items-center gap-3 mb-5">
    <div class="flex-1 relative">
      <svg class="absolute left-3.5 top-1/2 -translate-y-1/2 w-4 h-4 text-[var(--text-ghost)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M21 21l-5.197-5.197m0 0A7.5 7.5 0 105.196 5.196a7.5 7.5 0 0010.607 10.607z"/>
      </svg>
      <input
        class="input pl-10 mono"
        placeholder="Search teams..."
        bind:value={search}
      />
    </div>
  </div>

  <!-- Table -->
  <div class="card p-0 overflow-hidden mb-8">
    <table class="leaderboard-table">
      <thead>
        <tr>
          <th class="w-12">#</th>
          <th class="w-12"></th>
          <th>Team</th>
          <th class="text-right">Score</th>
          <th class="text-right">Throughput</th>
          <th class="text-right">Accuracy</th>
          <th class="text-right">p99</th>
          <th class="text-right w-16">Runs</th>
          <th class="text-right w-12">Δ</th>
        </tr>
      </thead>
      <tbody class="stagger">
        {#each filteredEntries() as entry, i (entry.team)}
          <tr
            class="{entry.team === team ? 'highlight' : ''} cursor-pointer"
            onclick={() => expandedTeam = expandedTeam === entry.team ? null : entry.team}
          >
            <td>
              <span class="{entry.rank <= 3 ? 'text-lg font-bold' : 'mono text-[var(--text-muted)] text-sm'}">
                {entry.rank <= 3 ? '0' + entry.rank : entry.rank}
              </span>
            </td>
            <td>
              <div class="w-7 h-7 rounded-md flex items-center justify-center text-[9px] font-bold mono"
                   style="background: rgba(255,255,255,0.03); color: var(--text-muted);">
                {entry.team.slice(0, 2).toUpperCase()}
              </div>
            </td>
            <td>
              <span class="font-medium mono text-sm {entry.team === team ? 'text-[var(--accent)]' : ''}">{entry.team}</span>
            </td>
            <td class="text-right">
              <div class="flex items-center justify-end gap-2">
                <div class="w-16 h-1.5 rounded-full bg-[var(--bg-elevated)] overflow-hidden hidden sm:block">
                  <div class="h-full rounded-full {scoreBarColor(entry.score)} transition-all" style="width: {entry.score * 100}%"></div>
                </div>
                <span class="mono font-bold text-sm {scoreColor(entry.score)}">{entry.score.toFixed(3)}</span>
              </div>
            </td>
            <td class="text-right mono text-sm text-[var(--text-secondary)]">{fmt(entry.throughput)}<span class="text-[var(--text-ghost)] text-[10px] ml-0.5">ops</span></td>
            <td class="text-right mono text-sm {entry.correctness >= 0.99 ? 'text-emerald-400' : entry.correctness >= 0.95 ? 'text-cyan-400' : 'text-amber-400'}">{(entry.correctness * 100).toFixed(1)}%</td>
            <td class="text-right mono text-sm text-[var(--text-secondary)]">{latFmt(entry.p99)}</td>
            <td class="text-right mono text-sm text-[var(--text-muted)]">{entry.runs}</td>
            <td class="text-right">
              <span class="mono text-sm {deltaColor(entry.delta)}">{deltaIcon(entry.delta)}</span>
            </td>
          </tr>
          <!-- Expanded detail -->
          {#if expandedTeam === entry.team}
            <tr class="expanded-row">
              <td colspan="9">
                <div class="p-6 grid grid-cols-4 gap-6 border-t border-[var(--border-subtle)]">
                  <div>
                    <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Score</div>
                    <div class="text-lg font-bold mono {scoreColor(entry.score)}">{entry.score.toFixed(4)}</div>
                  </div>
                  <div>
                    <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Throughput</div>
                    <div class="text-lg font-bold mono">{fmt(entry.throughput)} ops/s</div>
                  </div>
                  <div>
                    <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Tail Latency</div>
                    <div class="text-lg font-bold mono">{latFmt(entry.p99)}</div>
                    <div class="text-[10px] text-[var(--text-ghost)] mono">p99 round-trip</div>
                  </div>
                  <div>
                    <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Submissions</div>
                    <div class="text-lg font-bold mono">{entry.runs}</div>
                    <div class="text-[10px] text-[var(--text-ghost)] mono">total runs</div>
                  </div>
                </div>
              </td>
            </tr>
          {/if}
        {/each}
      </tbody>
    </table>

    {#if loading}
      <div class="px-6 py-16 text-center">
        <div class="text-[var(--text-muted)]">Loading rankings...</div>
      </div>
    {:else if filteredEntries().length === 0}
      <div class="px-6 py-16 text-center">
        {#if search}
          <div class="text-[var(--text-muted)] mb-2">No teams matching "{search}"</div>
          <button class="text-xs text-[var(--accent)] hover:underline" onclick={() => search = ''}>Clear search</button>
        {:else}
          <div class="text-[var(--text-muted)] mb-4">No submissions yet.</div>
          <button class="btn btn-primary text-sm" onclick={() => goto('/dashboard/submit')}>Be the first</button>
        {/if}
      </div>
    {/if}
  </div>
</div>
{/if}
