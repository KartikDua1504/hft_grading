<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { login } from '$lib/auth';

  let mode = $state<'login' | 'register'>('login');
  let teamName = $state('');
  let password = $state('');
  let confirmPassword = $state('');
  let loading = $state(false);
  let error = $state('');
  let mounted = $state(false);
  let passwordStrength = $state(0);
  let mouseX = $state(0);
  let mouseY = $state(0);

  onMount(() => {
    mounted = true;
    const params = new URLSearchParams(window.location.search);
    if (params.get('tab') === 'register') mode = 'register';

    const handleMouse = (e: MouseEvent) => {
      mouseX = (e.clientX / window.innerWidth - 0.5) * 20;
      mouseY = (e.clientY / window.innerHeight - 0.5) * 20;
    };
    window.addEventListener('mousemove', handleMouse);
    return () => window.removeEventListener('mousemove', handleMouse);
  });

  function checkPasswordStrength(pw: string): number {
    let score = 0;
    if (pw.length >= 6) score++;
    if (pw.length >= 10) score++;
    if (/[A-Z]/.test(pw)) score++;
    if (/[0-9]/.test(pw)) score++;
    if (/[^A-Za-z0-9]/.test(pw)) score++;
    return Math.min(score, 4);
  }

  $effect(() => {
    passwordStrength = checkPasswordStrength(password);
  });

  async function handleSubmit() {
    error = '';
    if (!teamName.trim()) { error = 'Team name is required.'; return; }
    if (!password) { error = 'Password is required.'; return; }
    if (mode === 'register' && password !== confirmPassword) {
      error = 'Passwords do not match.';
      return;
    }
    if (mode === 'register' && password.length < 6) {
      error = 'Password must be at least 6 characters.';
      return;
    }

    loading = true;
    const endpoint = mode === 'login' ? '/api/auth/login' : '/api/auth/register';

    try {
      const res = await fetch(endpoint, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ team_name: teamName, password }),
      });

      if (res.ok) {
        const data = await res.json();
        login(data.token, data.team_name);
        goto('/dashboard');
        return;
      }

      const data = await res.json().catch(() => ({}));
      error = data.detail || (mode === 'login' ? 'Invalid credentials.' : 'Registration failed.');
    } catch {
      error = 'Backend unreachable — is the server running? Use start.sh to launch everything.';
    } finally {
      loading = false;
    }
  }

  const strengthLabels = ['', 'Weak', 'Fair', 'Good', 'Strong'];
  const strengthColors = ['', 'bg-rose-500', 'bg-amber-500', 'bg-cyan-500', 'bg-emerald-500'];
</script>

<svelte:head>
  <title>{mode === 'login' ? 'Sign In' : 'Register'} — IICPC Arena</title>
</svelte:head>

<div class="min-h-screen flex items-center justify-center px-6 relative overflow-hidden">
  <!-- Animated cosmic background -->
  <div class="absolute inset-0 bg-cosmic"></div>

  <!-- Background Matrix/Orderbook Stream -->
  <div class="absolute inset-0 pointer-events-none overflow-hidden flex justify-center opacity-20" style="perspective: 800px;">
    <div class="flex gap-4 transform rotate-x-12 translate-y-[-10%] scale-150 blur-[2px]">
      {#each Array(40) as _, i}
        <div class="flex flex-col gap-2 {i % 2 === 0 ? 'text-[var(--emerald)]' : 'text-[var(--rose)]'} mono text-[8px] opacity-70"
             style="animation: slideDown {4 + Math.random() * 4}s linear infinite {Math.random()}s;">
          {#each Array(30) as _, j}
            <div>{(10000 + Math.random() * 500).toFixed(2)}</div>
          {/each}
        </div>
      {/each}
    </div>
  </div>

  <!-- Grid overlay -->
  <div class="absolute inset-0 bg-grid opacity-40 pointer-events-none"></div>

  <!-- Scanline effect -->
  <div
    class="absolute inset-x-0 h-px pointer-events-none"
    style="
      background: linear-gradient(90deg, transparent, rgba(99,91,255,0.15), transparent);
      animation: scanline 8s linear infinite;
    "
  ></div>

  {#if mounted}
    <div class="w-full max-w-[420px] relative z-10 fade-in-scale">
      <!-- Back button -->
      <button class="btn btn-ghost text-xs mb-8 px-3 py-2 gap-2" onclick={() => goto('/')}>
        <svg width="14" height="14" fill="none" stroke="currentColor" stroke-width="2"><path d="M10 2L4 7l6 5"/></svg>
        Back to home
      </button>

      <!-- Glass card -->
      <div class="card-glass p-10">
        <!-- Logo mark -->
        <div class="flex justify-center mb-8">
          <div class="w-16 h-16 rounded-2xl flex items-center justify-center relative"
               style="background: linear-gradient(135deg, var(--accent-glow), rgba(139,92,246,0.1));">
            <span class="text-2xl font-black tracking-tighter display text-gradient-accent">⚡</span>
            <div class="absolute inset-0 rounded-2xl" style="animation: borderGlow 3s ease-in-out infinite; border: 1px solid transparent;"></div>
          </div>
        </div>

        <!-- Header -->
        <div class="text-center mb-8">
          <h1 class="text-2xl font-bold display tracking-tight mb-2 glitch-hover">
            {mode === 'login' ? 'System Initialization' : 'Establish Connection'}
          </h1>
          <p class="text-sm text-[var(--text-secondary)] mono">
            {mode === 'login'
              ? 'Authenticate to deploy engine.'
              : 'Register your algorithmic entity.'}
          </p>
        </div>

        <!-- Tabs -->
        <div class="flex mb-7 p-1 rounded-xl bg-[var(--bg-surface)] border border-[var(--border)]">
          <button
            class="flex-1 py-2.5 text-sm font-medium rounded-lg transition-all duration-300
              {mode === 'login'
                ? 'bg-[var(--bg-elevated)] text-[var(--text-primary)] shadow-sm shadow-black/20'
                : 'text-[var(--text-muted)] hover:text-[var(--text-secondary)]'}"
            onclick={() => { mode = 'login'; error = ''; }}
          >Sign In</button>
          <button
            class="flex-1 py-2.5 text-sm font-medium rounded-lg transition-all duration-300
              {mode === 'register'
                ? 'bg-[var(--bg-elevated)] text-[var(--text-primary)] shadow-sm shadow-black/20'
                : 'text-[var(--text-muted)] hover:text-[var(--text-secondary)]'}"
            onclick={() => { mode = 'register'; error = ''; }}
          >Register</button>
        </div>

        <!-- Form -->
        <form onsubmit={(e) => { e.preventDefault(); handleSubmit(); }} class="space-y-5">
          <div>
            <label for="team-name" class="block text-xs font-semibold text-[var(--text-secondary)] mb-2 uppercase tracking-[0.1em]">
              Team Name
            </label>
            <input
              id="team-name"
              bind:value={teamName}
              type="text"
              class="input mono"
              placeholder="e.g. cache_lords"
              autocomplete="username"
            />
          </div>

          <div>
            <label for="password" class="block text-xs font-semibold text-[var(--text-secondary)] mb-2 uppercase tracking-[0.1em]">
              Password
            </label>
            <input
              id="password"
              bind:value={password}
              type="password"
              class="input"
              placeholder="••••••••"
              autocomplete={mode === 'login' ? 'current-password' : 'new-password'}
            />

            <!-- Password strength -->
            {#if mode === 'register' && password.length > 0}
              <div class="mt-3 fade-in">
                <div class="flex gap-1 mb-1.5">
                  {#each [1, 2, 3, 4] as level}
                    <div
                      class="h-1 flex-1 rounded-full transition-all duration-300
                        {passwordStrength >= level ? strengthColors[passwordStrength] : 'bg-[var(--bg-elevated)]'}"
                    ></div>
                  {/each}
                </div>
                <span class="text-[10px] font-medium uppercase tracking-wider
                  {passwordStrength <= 1 ? 'text-rose-400' :
                   passwordStrength === 2 ? 'text-amber-400' :
                   passwordStrength === 3 ? 'text-cyan-400' : 'text-emerald-400'}">
                  {strengthLabels[passwordStrength]}
                </span>
              </div>
            {/if}
          </div>

          {#if mode === 'register'}
            <div class="fade-up">
              <label for="confirm-password" class="block text-xs font-semibold text-[var(--text-secondary)] mb-2 uppercase tracking-[0.1em]">
                Confirm Password
              </label>
              <input
                id="confirm-password"
                bind:value={confirmPassword}
                type="password"
                class="input"
                placeholder="••••••••"
                autocomplete="new-password"
              />
            </div>
          {/if}

          {#if error}
            <div class="flex items-center gap-2.5 text-xs text-rose-400 bg-rose-400/[0.06] border border-rose-400/15 rounded-xl px-4 py-3 fade-in">
              <svg class="w-4 h-4 flex-shrink-0" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M12 9v3.75m9-.75a9 9 0 11-18 0 9 9 0 0118 0zm-9 3.75h.008v.008H12v-.008z"/>
              </svg>
              {error}
            </div>
          {/if}

          <button
            type="submit"
            class="btn btn-primary btn-pill w-full py-4 text-sm mt-2 group"
            disabled={loading}
          >
            {#if loading}
              <span class="flex items-center gap-2">
                <svg class="w-4 h-4" style="animation: spin-slow 1s linear infinite;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5">
                  <path d="M12 2v4m0 12v4m-7.07-3.93l2.83-2.83m8.48-8.48l2.83-2.83M2 12h4m12 0h4M4.93 4.93l2.83 2.83m8.48 8.48l2.83 2.83" stroke-linecap="round"/>
                </svg>
                Processing...
              </span>
            {:else}
              <span class="relative z-10">{mode === 'login' ? 'Sign In' : 'Create Team'}</span>
              <svg class="w-4 h-4 transition-transform group-hover:translate-x-0.5" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M13.5 4.5L21 12m0 0l-7.5 7.5M21 12H3"/>
              </svg>
            {/if}
          </button>
        </form>

        <div class="mt-6 pt-5 border-t border-[var(--border)]">
          <p class="text-xs text-[var(--text-muted)] text-center">
            {mode === 'login' ? "Don't have a team?" : 'Already registered?'}
            <button
              class="text-[var(--accent)] hover:text-[var(--accent-hover)] font-medium ml-1 transition-colors"
              onclick={() => { mode = mode === 'login' ? 'register' : 'login'; error = ''; }}
            >
              {mode === 'login' ? 'Register now' : 'Sign in'}
            </button>
          </p>
        </div>
      </div>

      <!-- Bottom hint -->
      <div class="mt-8 text-center text-[10px] text-[var(--text-ghost)] uppercase tracking-widest mono flex flex-col items-center gap-1">
        <div>[ SECURE SESSION ESTABLISHED ]</div>
        <div class="flex gap-2">
          <span class="text-[var(--emerald)] flicker">●</span> <span class="opacity-50">ISOLATED ENCLAVE</span>
          <span class="text-[var(--emerald)] flicker" style="animation-delay: 0.5s;">●</span> <span class="opacity-50">DETERMINISTIC CLOCK</span>
        </div>
      </div>
    </div>
  {/if}
</div>
