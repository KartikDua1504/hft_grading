<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { auth, logout } from '$lib/auth';
  import { page } from '$app/stores';

  let { children } = $props();

  let team = $state('');
  let currentTime = $state('');
  let wsConnected = $state(false);
  let currentPath = $state('');
  let mobileMenuOpen = $state(false);

  // Competition timer
  let compState = $state('stopped');
  let compRemaining = $state(0);
  let compTimer: ReturnType<typeof setInterval> | null = null;

  // System test state
  let systestState = $state('idle');
  let systestProgress = $state('');

  onMount(() => {
    const unsub = auth.subscribe(s => {
      if (!s.authenticated) goto('/auth');
      team = s.team || '';
    });

    const pathUnsub = page.subscribe(p => {
      currentPath = p.url.pathname;
    });

    const clock = setInterval(() => {
      currentTime = new Date().toLocaleTimeString('en-US', { hour12: false });
    }, 1000);
    currentTime = new Date().toLocaleTimeString('en-US', { hour12: false });

    // Real WebSocket connection with auto-reconnect
    let ws: WebSocket | null = null;
    let wsRetryDelay = 1000;
    let wsRetryTimer: ReturnType<typeof setTimeout> | null = null;
    let destroyed = false;

    function connectWs() {
      if (destroyed) return;
      const proto = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
      const url = `${proto}//${window.location.host}/ws/live`;
      try {
        ws = new WebSocket(url);
        ws.onopen = () => {
          wsConnected = true;
          wsRetryDelay = 1000; // Reset backoff on success
        };
        ws.onclose = () => {
          wsConnected = false;
          if (!destroyed) {
            wsRetryTimer = setTimeout(connectWs, wsRetryDelay);
            wsRetryDelay = Math.min(wsRetryDelay * 2, 15000);
          }
        };
        ws.onerror = () => { wsConnected = false; };
        ws.onmessage = (ev) => {
          try {
            const msg = JSON.parse(ev.data);
            if (msg.type === 'ping') {
              ws?.send('pong');
            }
            if (msg.type === 'system_test' && msg.data) {
              systestState = msg.data.state || systestState;
              systestProgress = msg.data.progress || systestProgress;
            }
          } catch {}
        };
      } catch {
        wsConnected = false;
        if (!destroyed) {
          wsRetryTimer = setTimeout(connectWs, wsRetryDelay);
        }
      }
    }
    connectWs();

    // Competition timer poll
    async function pollComp() {
      try {
        const res = await fetch('/api/competition/status');
        if (res.ok) {
          const d = await res.json();
          compState = d.state;
          compRemaining = d.remaining_secs;
        }
      } catch {}
    }
    pollComp();
    compTimer = setInterval(pollComp, 5000);
    // Local countdown between polls
    const localTick = setInterval(() => {
      if (compState === 'running' && compRemaining > 0) compRemaining--;
    }, 1000);

    // System test polling
    async function pollSystest() {
      try {
        const res = await fetch('/api/system-test/status');
        if (res.ok) {
          const d = await res.json();
          systestState = d.state;
          systestProgress = d.progress || '';
        }
      } catch {}
    }
    pollSystest();
    const systestTimer = setInterval(pollSystest, 5000);

    return () => {
      destroyed = true;
      unsub(); pathUnsub(); clearInterval(clock);
      if (compTimer) clearInterval(compTimer);
      clearInterval(localTick);
      clearInterval(systestTimer);
      if (wsRetryTimer) clearTimeout(wsRetryTimer);
      if (ws) { ws.onclose = null; ws.close(); }
    };
  });

  const navItems = [
    { path: '/dashboard', label: 'Overview', icon: 'grid' },
    { path: '/dashboard/submit', label: 'Submit', icon: 'upload' },
    { path: '/dashboard/leaderboard', label: 'Rankings', icon: 'trophy' },
    { path: '/dashboard/guidelines', label: 'Guidelines', icon: 'book' },
  ];

  function isActive(path: string): boolean {
    if (path === '/dashboard') return currentPath === '/dashboard';
    return currentPath.startsWith(path);
  }
</script>

<div class="dashboard-layout bg-[var(--bg-void)]">
  <!-- Sidebar -->
  <aside class="dashboard-sidebar bg-[var(--bg-deep)] relative">
    <!-- Subtle gradient overlay on sidebar -->
    <div class="absolute inset-0 pointer-events-none"
         style="background: linear-gradient(180deg, rgba(99,91,255,0.02) 0%, transparent 30%);"></div>

    <div class="relative z-10 flex flex-col h-full">
      <!-- Logo -->
      <button
        class="flex items-center gap-3 px-2 mb-10 group"
        onclick={() => goto('/')}
      >
        <div class="w-10 h-10 rounded-xl flex items-center justify-center"
             style="background: linear-gradient(135deg, var(--accent-glow), rgba(139,92,246,0.1));">
          <span class="text-lg">⚡</span>
        </div>
        <div>
          <span class="text-xl font-bold tracking-tight display group-hover:text-[var(--accent)] transition-colors">IICPC</span>
          <span class="block text-[10px] text-[var(--text-ghost)] uppercase tracking-[0.15em] font-bold">Arena</span>
        </div>
      </button>

      <!-- Nav -->
      <nav class="space-y-1 flex-1">
        <span class="block text-[9px] text-[var(--text-ghost)] uppercase tracking-[0.15em] font-bold px-3 mb-3">Platform</span>
        {#each navItems as item}
          <button
            class="sidebar-link relative {isActive(item.path) ? 'sidebar-link-active' : ''}"
            onclick={() => goto(item.path)}
          >
            {#if item.icon === 'grid'}
              <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 6A2.25 2.25 0 016 3.75h2.25A2.25 2.25 0 0110.5 6v2.25a2.25 2.25 0 01-2.25 2.25H6a2.25 2.25 0 01-2.25-2.25V6zM3.75 15.75A2.25 2.25 0 016 13.5h2.25a2.25 2.25 0 012.25 2.25V18a2.25 2.25 0 01-2.25 2.25H6A2.25 2.25 0 013.75 18v-2.25zM13.5 6a2.25 2.25 0 012.25-2.25H18A2.25 2.25 0 0120.25 6v2.25A2.25 2.25 0 0118 10.5h-2.25a2.25 2.25 0 01-2.25-2.25V6zM13.5 15.75a2.25 2.25 0 012.25-2.25H18a2.25 2.25 0 012.25 2.25V18A2.25 2.25 0 0118 20.25h-2.25A2.25 2.25 0 0113.5 18v-2.25z"/>
              </svg>
            {:else if item.icon === 'upload'}
              <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
              </svg>
            {:else if item.icon === 'trophy'}
              <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M16.5 18.75h-9m9 0a3 3 0 013 3h-15a3 3 0 013-3m9 0v-3.375c0-.621-.503-1.125-1.125-1.125h-.871M7.5 18.75v-3.375c0-.621.504-1.125 1.125-1.125h.872m5.007 0H9.497m5.007 0a7.454 7.454 0 01-.982-3.172M9.497 14.25a7.454 7.454 0 00.981-3.172M5.25 4.236c-.982.143-1.954.317-2.916.52A6.003 6.003 0 007.73 9.728M5.25 4.236V4.5c0 2.108.966 3.99 2.48 5.228M5.25 4.236V2.721C7.456 2.41 9.71 2.25 12 2.25c2.291 0 4.545.16 6.75.47v1.516M18.75 4.236c.982.143 1.954.317 2.916.52A6.003 6.003 0 0016.27 9.728M18.75 4.236V4.5c0 2.108-.966 3.99-2.48 5.228m0 0a6.003 6.003 0 01-5.54 0"/>
              </svg>
            {:else if item.icon === 'book'}
              <svg class="w-[18px] h-[18px]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M12 6.042A8.967 8.967 0 006 3.75c-1.052 0-2.062.18-3 .512v14.25A8.987 8.987 0 016 18c2.305 0 4.408.867 6 2.292m0-14.25a8.966 8.966 0 016-2.292c1.052 0 2.062.18 3 .512v14.25A8.987 8.987 0 0018 18a8.967 8.967 0 00-6 2.292m0-14.25v14.25"/>
              </svg>
            {/if}
            <span>{item.label}</span>
          </button>
        {/each}
      </nav>

      <!-- Bottom section -->
      <div class="mt-auto space-y-4">
        <!-- Connection status -->
        <div class="flex items-center gap-2 px-3 py-2">
          <div class="status-dot {wsConnected ? 'status-dot-online' : 'status-dot-offline'}"></div>
          <span class="text-[11px] text-[var(--text-muted)]">{wsConnected ? 'Connected' : 'Disconnected'}</span>
        </div>

        <!-- Divider -->
        <div class="border-t border-[var(--border)]"></div>

        <!-- User -->
        <div class="flex items-center justify-between px-2">
          <div class="flex items-center gap-2.5">
            <div class="w-8 h-8 rounded-lg flex items-center justify-center text-xs font-bold mono"
                 style="background: var(--accent-glow); color: var(--accent);">
              {team.slice(0, 2).toUpperCase()}
            </div>
            <div>
              <div class="text-sm font-medium mono truncate max-w-[120px]">{team}</div>
              <div class="text-[10px] text-[var(--text-muted)]">{currentTime}</div>
            </div>
          </div>
          <button
            class="btn-icon w-8 h-8 border-0"
            onclick={() => { logout(); goto('/'); }}
            title="Sign out"
          >
            <svg class="w-4 h-4" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M15.75 9V5.25A2.25 2.25 0 0013.5 3h-6a2.25 2.25 0 00-2.25 2.25v13.5A2.25 2.25 0 007.5 21h6a2.25 2.25 0 002.25-2.25V15m3 0l3-3m0 0l-3-3m3 3H9"/>
            </svg>
          </button>
        </div>
      </div>
    </div>
  </aside>

  <!-- Main content -->
  <main class="dashboard-content relative">
    <!-- Subtle top gradient -->
    <div class="absolute inset-x-0 top-0 h-32 pointer-events-none"
         style="background: linear-gradient(180deg, rgba(99,91,255,0.015), transparent);"></div>

    <div class="relative z-10">
      {#if compState === 'running' && compRemaining > 0}
        <div class="mx-6 mt-2 mb-0 px-5 py-2.5 rounded-xl flex items-center justify-between"
             style="background: linear-gradient(135deg, rgba(16,185,129,0.08), rgba(99,91,255,0.08)); border: 1px solid rgba(16,185,129,0.2);">
          <div class="flex items-center gap-2">
            <span class="text-[10px] uppercase tracking-widest font-bold text-[var(--emerald)]">⚡ Competition Active</span>
          </div>
          <div class="mono font-black text-sm text-[var(--accent)]">
            {Math.floor(compRemaining / 3600)}:{String(Math.floor((compRemaining % 3600) / 60)).padStart(2, '0')}:{String(compRemaining % 60).padStart(2, '0')}
          </div>
        </div>
      {:else if compState === 'finished'}
        <div class="mx-6 mt-2 mb-0 px-5 py-2.5 rounded-xl flex items-center justify-center gap-2"
             style="background: rgba(245,158,11,0.06); border: 1px solid rgba(245,158,11,0.2);">
          <span class="text-xs font-bold mono uppercase tracking-wider text-[var(--amber)]">🏁 Competition Ended — Submissions Closed</span>
        </div>
      {/if}

      {#if systestState === 'running'}
        <div class="mx-6 mt-2 mb-0 px-5 py-3 rounded-xl flex items-center justify-between"
             style="background: linear-gradient(135deg, rgba(139,92,246,0.08), rgba(99,91,255,0.08)); border: 1px solid rgba(139,92,246,0.3);">
          <div class="flex items-center gap-3">
            <div class="w-2 h-2 rounded-full bg-violet-400 animate-pulse"></div>
            <span class="text-xs font-bold mono uppercase tracking-wider text-violet-300">🔬 System Tests Running</span>
          </div>
          <div class="flex items-center gap-3">
            <span class="text-[11px] text-violet-400/70 mono">Rejudging all submissions against stress scenarios</span>
            <span class="mono font-black text-sm text-violet-300">{systestProgress}</span>
          </div>
        </div>
      {:else if systestState === 'completed'}
        <div class="mx-6 mt-2 mb-0 px-5 py-2.5 rounded-xl flex items-center justify-center gap-2"
             style="background: rgba(139,92,246,0.06); border: 1px solid rgba(139,92,246,0.2);">
          <span class="text-xs font-bold mono uppercase tracking-wider text-violet-300">✅ System Tests Complete — Final Standings</span>
        </div>
      {/if}

      {@render children()}
    </div>
  </main>
</div>
