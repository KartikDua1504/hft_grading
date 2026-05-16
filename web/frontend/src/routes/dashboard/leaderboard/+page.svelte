<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { auth } from '$lib/auth';

  interface Entry {
    rank: number;
    team: string;
    score: number;
    throughput: number;
    correctness: number;
    p99: number;
    runs: number;
    delta?: number; // rank change
  }

  let entries = $state<Entry[]>([]);
  let team = $state('');
  let mounted = $state(false);
  let search = $state('');
  let sortBy = $state<'score' | 'throughput' | 'correctness' | 'p99'>('score');
  let expandedTeam = $state<string | null>(null);
  let lastUpdate = $state('');

  const DEMO: Entry[] = [
    { rank: 1, team: 'quantum_traders', score: 0.924, throughput: 847293, correctness: 0.9847, p99: 1250, runs: 14, delta: 0 },
    { rank: 2, team: 'zero_latency', score: 0.881, throughput: 723451, correctness: 0.9912, p99: 1890, runs: 9, delta: 1 },
    { rank: 3, team: 'cache_lords', score: 0.853, throughput: 651002, correctness: 0.9734, p99: 2100, runs: 22, delta: -1 },
    { rank: 4, team: 'lock_free_gang', score: 0.791, throughput: 534210, correctness: 0.9501, p99: 3400, runs: 7, delta: 2 },
    { rank: 5, team: 'simd_warriors', score: 0.714, throughput: 412830, correctness: 0.9223, p99: 5600, runs: 31, delta: 0 },
    { rank: 6, team: 'branch_predict', score: 0.682, throughput: 389100, correctness: 0.9102, p99: 6200, runs: 4, delta: -2 },
    { rank: 7, team: 'page_fault_gang', score: 0.641, throughput: 342000, correctness: 0.8891, p99: 8400, runs: 18, delta: 0 },
    { rank: 8, team: 'naive_alloc', score: 0.523, throughput: 214500, correctness: 0.8634, p99: 12300, runs: 2, delta: 1 },
    { rank: 9, team: 'malloc_monkeys', score: 0.487, throughput: 189200, correctness: 0.8412, p99: 15000, runs: 6, delta: -1 },
    { rank: 10, team: 'heap_overflow', score: 0.441, throughput: 156800, correctness: 0.8201, p99: 18500, runs: 3, delta: 0 },
  ];

  onMount(() => {
    mounted = true;
    const unsub = auth.subscribe(s => {
      if (!s.authenticated) goto('/auth');
      team = s.team || '';
    });
    loadLeaderboard();
    lastUpdate = new Date().toLocaleTimeString('en-US', { hour12: false });

    // Auto refresh
    const refresh = setInterval(() => {
      lastUpdate = new Date().toLocaleTimeString('en-US', { hour12: false });
    }, 30000);

    return () => { unsub(); clearInterval(refresh); };
  });

  async function loadLeaderboard() {
    try {
      const res = await fetch('/api/leaderboard');
      if (res.ok) {
        const data = await res.json();
        if (data.length > 0) {
          entries = data.map((e: any, i: number) => ({
            rank: i + 1, team: e.team_name, score: e.score,
            throughput: e.throughput, correctness: e.correctness,
            p99: e.p99_latency_ns, runs: e.submissions, delta: 0,
          }));
          return;
        }
      }
    } catch {}
    entries = DEMO;
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

  function medal(r: number): string {
    if (r === 1) return '🥇';
    if (r === 2) return '🥈';
    if (r === 3) return '🥉';
    return '';
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
<div class="max-w-6xl mx-auto fade-in">
  <!-- Header -->
  <div class="flex items-end justify-between mb-8">
    <div>
      <h1 class="text-3xl font-bold display tracking-tight mb-1">Rankings</h1>
      <p class="text-sm text-[var(--text-secondary)]">{entries.length} teams competing · Updated {lastUpdate}</p>
    </div>
    <button class="btn btn-ghost text-xs gap-2" onclick={() => goto('/dashboard/submit')}>
      <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
        <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
      </svg>
      Submit code
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
        <div class="text-2xl mb-1">🥈</div>
        <div class="text-sm font-semibold mono truncate">{entries[1].team}</div>
        <div class="text-2xl font-bold mono mt-2 {scoreColor(entries[1].score)}">{entries[1].score.toFixed(3)}</div>
        <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">{fmt(entries[1].throughput)} ops · {latFmt(entries[1].p99)}</div>
      </div>

      <!-- 1st Place -->
      <div class="card p-8 text-center bg-gradient-to-b {podiumGradient(1)} order-2 relative card-glow">
        <!-- Crown glow -->
        <div class="absolute inset-0 pointer-events-none rounded-[var(--radius-lg)]"
             style="background: radial-gradient(circle at 50% 0%, rgba(251,191,36,0.06), transparent 60%);"></div>
        <div class="relative z-10">
          <div class="w-16 h-16 mx-auto rounded-2xl flex items-center justify-center text-lg font-bold mono mb-3"
               style="background: rgba(251,191,36,0.1); color: rgb(251,191,36);">
            {entries[0].team.slice(0, 2).toUpperCase()}
          </div>
          <div class="text-3xl mb-1">🥇</div>
          <div class="text-base font-bold mono truncate">{entries[0].team}</div>
          <div class="text-3xl font-black mono mt-2 {scoreColor(entries[0].score)}">{entries[0].score.toFixed(3)}</div>
          <div class="text-[10px] text-[var(--text-muted)] mt-1 mono">{fmt(entries[0].throughput)} ops · {latFmt(entries[0].p99)}</div>
          <div class="badge badge-emerald mt-3 mx-auto">{entries[0].runs} runs</div>
        </div>
      </div>

      <!-- 3rd Place -->
      <div class="card p-6 text-center bg-gradient-to-b {podiumGradient(3)} order-3 self-end">
        <div class="w-12 h-12 mx-auto rounded-2xl flex items-center justify-center text-sm font-bold mono mb-3"
             style="background: rgba(180,83,9,0.1); color: rgb(180,83,9);">
          {entries[2].team.slice(0, 2).toUpperCase()}
        </div>
        <div class="text-2xl mb-1">🥉</div>
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
        type="text"
        bind:value={search}
        class="input pl-10 !bg-[var(--bg-card)] text-sm"
        placeholder="Search teams..."
      />
    </div>
    <div class="flex items-center gap-1.5">
      <div class="status-dot status-dot-online" style="width: 6px; height: 6px;"></div>
      <span class="text-[10px] text-[var(--text-muted)]">Live</span>
    </div>
  </div>

  <!-- Full Table -->
  <div class="card overflow-hidden mb-8">
    <table class="data-table">
      <thead>
        <tr>
          <th class="w-16">#</th>
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
        {#each filteredEntries() as entry (entry.team)}
          <tr
            class="{entry.team === team ? 'highlight' : ''} cursor-pointer"
            onclick={() => expandedTeam = expandedTeam === entry.team ? null : entry.team}
          >
            <td>
              <span class="{entry.rank <= 3 ? 'text-lg' : 'mono text-[var(--text-muted)] text-sm'}">{entry.rank <= 3 ? medal(entry.rank) : entry.rank}</span>
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
                <span class="text-base font-bold mono {scoreColor(entry.score)}">{entry.score.toFixed(3)}</span>
              </div>
            </td>
            <td class="text-right mono text-sm text-[var(--text-secondary)]">
              {fmt(entry.throughput)}<span class="text-[var(--text-ghost)] text-xs ml-1">ops</span>
            </td>
            <td class="text-right mono text-sm {scoreColor(entry.correctness)}">
              {(entry.correctness * 100).toFixed(1)}%
            </td>
            <td class="text-right mono text-sm text-[var(--text-secondary)]">
              {latFmt(entry.p99)}
            </td>
            <td class="text-right mono text-sm text-[var(--text-muted)]">
              {entry.runs}
            </td>
            <td class="text-right">
              <span class="text-xs mono {deltaColor(entry.delta)}">{deltaIcon(entry.delta)}</span>
            </td>
          </tr>

          <!-- Expanded Detail -->
          {#if expandedTeam === entry.team}
            <tr>
              <td colspan="9" class="!p-0">
                <div class="px-6 py-5 bg-[var(--bg-surface)] border-y border-[var(--border)] fade-in">
                  <div class="grid grid-cols-4 gap-6">
                    <div>
                      <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Throughput</div>
                      <div class="text-lg font-bold mono">{fmt(entry.throughput)}</div>
                      <div class="text-[10px] text-[var(--text-ghost)] mono">orders/sec</div>
                    </div>
                    <div>
                      <div class="text-[10px] text-[var(--text-muted)] uppercase tracking-wider mb-1">Correctness</div>
                      <div class="text-lg font-bold mono {scoreColor(entry.correctness)}">{(entry.correctness * 100).toFixed(2)}%</div>
                      <div class="text-[10px] text-[var(--text-ghost)] mono">FIFO validated</div>
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

                  <!-- Score breakdown bar -->
                  <div class="mt-5">
                    <div class="flex gap-1 h-2 rounded-full overflow-hidden">
                      <div class="rounded-l-full" style="width: 40%; background: var(--accent); opacity: {entry.correctness}"></div>
                      <div style="width: 30%; background: var(--cyan); opacity: {entry.throughput / 1000000}"></div>
                      <div class="rounded-r-full" style="width: 30%; background: var(--amber); opacity: {1 - entry.p99 / 20000}"></div>
                    </div>
                    <div class="flex justify-between mt-2 text-[9px] text-[var(--text-ghost)] uppercase tracking-wider">
                      <span>Correctness 40%</span>
                      <span>Throughput 30%</span>
                      <span>Latency 30%</span>
                    </div>
                  </div>
                </div>
              </td>
            </tr>
          {/if}
        {/each}
      </tbody>
    </table>

    {#if filteredEntries().length === 0}
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

  <!-- Scoring Legend -->
  <div class="grid md:grid-cols-3 gap-4 stagger">
    <div class="card p-6 group">
      <div class="flex items-center gap-3 mb-3">
        <div class="w-8 h-8 rounded-lg flex items-center justify-center" style="background: var(--accent-glow);">
          <svg class="w-4 h-4 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M9 12.75L11.25 15 15 9.75M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/>
          </svg>
        </div>
        <div class="text-2xl font-black display" style="color: var(--accent);">40%</div>
      </div>
      <h3 class="text-xs font-bold uppercase tracking-wider text-[var(--text-secondary)] mb-1.5">Correctness</h3>
      <p class="text-xs text-[var(--text-muted)] leading-relaxed">
        Every fill validated against a deterministic reference model for FIFO price-time priority.
      </p>
    </div>
    <div class="card p-6 group">
      <div class="flex items-center gap-3 mb-3">
        <div class="w-8 h-8 rounded-lg flex items-center justify-center" style="background: var(--cyan-dim);">
          <svg class="w-4 h-4 text-[var(--cyan)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 13.5l10.5-11.25L12 10.5h8.25L9.75 21.75 12 13.5H3.75z"/>
          </svg>
        </div>
        <div class="text-2xl font-black display text-[var(--cyan)]">30%</div>
      </div>
      <h3 class="text-xs font-bold uppercase tracking-wider text-[var(--text-secondary)] mb-1.5">Throughput</h3>
      <p class="text-xs text-[var(--text-muted)] leading-relaxed">
        Orders processed per second under sustained 30-second load. Zero drops required.
      </p>
    </div>
    <div class="card p-6 group">
      <div class="flex items-center gap-3 mb-3">
        <div class="w-8 h-8 rounded-lg flex items-center justify-center" style="background: var(--amber-dim);">
          <svg class="w-4 h-4 text-[var(--amber)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M12 6v6h4.5m4.5 0a9 9 0 11-18 0 9 9 0 0118 0z"/>
          </svg>
        </div>
        <div class="text-2xl font-black display text-[var(--amber)]">30%</div>
      </div>
      <h3 class="text-xs font-bold uppercase tracking-wider text-[var(--text-secondary)] mb-1.5">Latency</h3>
      <p class="text-xs text-[var(--text-muted)] leading-relaxed">
        99th percentile round-trip response time. Measured at wire boundary. Lower is better.
      </p>
    </div>
  </div>
</div>
{/if}
