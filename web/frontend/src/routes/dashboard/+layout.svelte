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

    // Simulate WS connection
    setTimeout(() => wsConnected = true, 500);

    return () => { unsub(); pathUnsub(); clearInterval(clock); };
  });

  const navItems = [
    { path: '/dashboard', label: 'Overview', icon: 'grid' },
    { path: '/dashboard/submit', label: 'Submit', icon: 'upload' },
    { path: '/dashboard/leaderboard', label: 'Rankings', icon: 'trophy' },
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
        <div class="w-9 h-9 rounded-xl flex items-center justify-center"
             style="background: linear-gradient(135deg, var(--accent-glow), rgba(139,92,246,0.1));">
          <span class="text-sm">⚡</span>
        </div>
        <div>
          <span class="text-base font-bold tracking-tight display group-hover:text-[var(--accent)] transition-colors">IICPC</span>
          <span class="block text-[9px] text-[var(--text-ghost)] uppercase tracking-[0.15em] font-medium">Arena</span>
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
      {@render children()}
    </div>
  </main>
</div>
