<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { auth } from '$lib/auth';

  let token = $state('');
  let team = $state('');
  let file = $state<File | null>(null);
  let fileContent = $state('');
  let dragOver = $state(false);
  let submitting = $state(false);
  let result = $state<{ success?: boolean; jobId?: string; error?: string } | null>(null);
  let mounted = $state(false);
  let uploadProgress = $state(0);

  // Job tracking
  let jobPhase = $state<'idle' | 'uploading' | 'queued' | 'compiling' | 'running' | 'scored' | 'failed'>('idle');
  let terminalLines = $state<Array<{ text: string; type: 'prompt' | 'output' | 'success' | 'error' }>>([]);
  let jobResult = $state<any>(null);
  let pollTimer = $state<ReturnType<typeof setInterval> | null>(null);

  // Cooldown timer (30s between submissions)
  let cooldownRemaining = $state(0);
  let cooldownTimer = $state<ReturnType<typeof setInterval> | null>(null);

  // Real submissions from /api/submissions
  interface Submission {
    job_id: string;
    filename: string;
    submitted_at: string;
    status: string;
    score?: number | null;
    error?: string | null;
  }
  let submissions = $state<Submission[]>([]);

  onMount(() => {
    mounted = true;
    const unsub = auth.subscribe(s => {
      if (!s.authenticated) goto('/auth');
      token = s.token || '';
      team = s.team || '';
    });
    loadSubmissions();
    return () => {
      unsub();
      if (pollTimer) clearInterval(pollTimer);
      if (cooldownTimer) clearInterval(cooldownTimer);
    };
  });

  async function loadSubmissions() {
    try {
      const res = await fetch('/api/submissions', {
        headers: { 'Authorization': `Bearer ${token}` },
      });
      if (res.ok) {
        submissions = await res.json();
      }
    } catch {}
  }

  function handleDrop(e: DragEvent) {
    e.preventDefault();
    dragOver = false;
    const f = e.dataTransfer?.files[0];
    if (f) selectFile(f);
  }

  function handleSelect(e: Event) {
    const input = e.target as HTMLInputElement;
    if (input.files?.[0]) selectFile(input.files[0]);
  }

  async function selectFile(f: File) {
    file = f;
    result = null;
    jobPhase = 'idle';
    terminalLines = [];
    jobResult = null;

    try {
      const text = await f.text();
      fileContent = text;
    } catch {
      fileContent = '';
    }
  }

  function getPreviewLines(): string[] {
    if (!fileContent) return [];
    return fileContent.split('\n').slice(0, 25);
  }

  function startCooldown() {
    cooldownRemaining = 180;
    if (cooldownTimer) clearInterval(cooldownTimer);
    cooldownTimer = setInterval(() => {
      cooldownRemaining--;
      if (cooldownRemaining <= 0) {
        cooldownRemaining = 0;
        if (cooldownTimer) clearInterval(cooldownTimer);
      }
    }, 1000);
  }

  async function submit() {
    if (!file || cooldownRemaining > 0) return;

    // Client-side extension check
    const ext = file.name.split('.').pop()?.toLowerCase() || '';
    if (!['cpp', 'cc', 'cxx', 'c', 'h', 'hpp'].includes(ext)) {
      terminalLines = [
        { text: `$ iicpc submit ${file.name}`, type: 'prompt' },
        { text: `Error: Invalid file type '.${ext}'. Only C/C++ source files accepted.`, type: 'error' },
      ];
      result = { success: false, error: `Invalid file type '.${ext}'` };
      return;
    }

    submitting = true;
    result = null;
    jobPhase = 'uploading';
    uploadProgress = 0;
    terminalLines = [
      { text: `$ iicpc submit ${file.name}`, type: 'prompt' },
      { text: `Uploading ${(file.size / 1024).toFixed(1)} KB...`, type: 'output' },
    ];

    try {
      const fd = new FormData();
      fd.append('file', file);
      const res = await fetch('/api/submit', {
        method: 'POST',
        headers: { 'Authorization': `Bearer ${token}` },
        body: fd,
      });

      uploadProgress = 100;

      if (res.ok) {
        const data = await res.json();
        result = { success: true, jobId: data.job_id };
        terminalLines = [...terminalLines,
          { text: 'Upload complete ✓', type: 'success' },
          { text: `Job ${data.job_id.slice(0, 8)}... queued (position: ${data.position})`, type: 'output' },
          { text: '', type: 'output' },
        ];
        jobPhase = 'queued';
        startCooldown();
        startJobPolling(data.job_id);
      } else {
        const data = await res.json().catch(() => ({}));
        const errorMsg = data.detail || `Submission rejected (HTTP ${res.status})`;
        result = { success: false, error: errorMsg };
        jobPhase = 'failed';
        terminalLines = [...terminalLines,
          { text: `Error: ${errorMsg}`, type: 'error' },
        ];
      }
    } catch {
      result = { success: false, error: 'Backend unreachable — is the server running?' };
      jobPhase = 'failed';
      terminalLines = [...terminalLines,
        { text: 'Error: Backend unreachable. Run start.sh to launch the server.', type: 'error' },
      ];
    } finally {
      submitting = false;
    }
  }

  function startJobPolling(jobId: string) {
    // Poll /api/job/{id} every 2 seconds until terminal state
    pollTimer = setInterval(async () => {
      try {
        const res = await fetch(`/api/job/${jobId}`, {
          headers: { 'Authorization': `Bearer ${token}` },
        });
        if (!res.ok) return;
        const data = await res.json();

        if (data.status !== jobPhase) {
          // Status changed
          if (data.status === 'compiling') {
            jobPhase = 'compiling';
            terminalLines = [...terminalLines,
              { text: '$ g++ -O3 -std=c++23 -march=native contestant.cpp', type: 'prompt' },
              { text: 'Compiling...', type: 'output' },
            ];
          } else if (data.status === 'running') {
            jobPhase = 'running';
            terminalLines = [...terminalLines,
              { text: 'Compilation successful ✓', type: 'success' },
              { text: '', type: 'output' },
              { text: `$ ./run_contest --contestant ${jobId.slice(0, 8)}`, type: 'prompt' },
              { text: 'Running benchmark...', type: 'output' },
            ];
          } else if (data.status === 'scored') {
            jobPhase = 'scored';
            jobResult = data;
            terminalLines = [...terminalLines,
              { text: '', type: 'output' },
              { text: '═══ RESULTS ═══', type: 'success' },
              { text: `  Score:        ${data.score?.toFixed(4) || 'N/A'}`, type: 'success' },
              { text: `  Throughput:   ${data.throughput?.toLocaleString() || 'N/A'} ops/sec`, type: 'output' },
              { text: `  Correctness:  ${((data.correctness || 0) * 100).toFixed(2)}%`, type: 'output' },
              { text: `  p99 Latency:  ${data.p99_latency_ns || 'N/A'} ns`, type: 'output' },
              { text: '', type: 'output' },
              { text: 'Leaderboard updated ✓', type: 'success' },
            ];
            if (pollTimer) clearInterval(pollTimer);
            loadSubmissions(); // Refresh history
          } else if (data.status === 'failed') {
            jobPhase = 'failed';
            terminalLines = [...terminalLines,
              { text: `Error: ${data.error || 'Job failed'}`, type: 'error' },
            ];
            result = { success: false, error: data.error };
            if (pollTimer) clearInterval(pollTimer);
            loadSubmissions();
          }
        }
      } catch {}
    }, 2000);
  }

  function statusLabel(status: string): string {
    if (status === 'scored') return 'badge-emerald';
    if (status === 'failed') return 'badge-rose';
    return 'badge-amber';
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
</script>

<svelte:head>
  <title>Submit — IICPC Arena</title>
</svelte:head>

{#if mounted}
<div class="w-full xl:max-w-[1400px] mx-auto px-6 fade-in">
  <!-- Header -->
  <div class="mb-8 flex justify-between items-end border-b border-[var(--border)] pb-4">
    <div>
      <div class="flex items-center gap-2 mb-2">
        <div class="w-2 h-2 rounded-full bg-[var(--accent)] flicker"></div>
        <span class="text-[10px] mono uppercase tracking-widest text-[var(--accent)]">Secure Enclave Ready</span>
      </div>
      <h1 class="text-3xl font-bold display tracking-tight mb-1 glitch-hover">Deployment Matrix</h1>
      <p class="text-sm text-[var(--text-secondary)] mono">Upload, compile, benchmark, and score your orderbook engine.</p>
    </div>
  </div>

  <div class="grid lg:grid-cols-5 gap-6">
    <!-- Left: Upload + Terminal -->
    <div class="lg:col-span-3 space-y-5">
      <!-- Drop zone -->
      <button
        class="w-full card p-0 overflow-hidden transition-all duration-300 text-left relative group
          {dragOver ? 'border-[var(--accent)] shadow-[0_0_32px_var(--accent-glow)]' : ''}"
        ondragover={(e) => { e.preventDefault(); dragOver = true; }}
        ondragleave={() => dragOver = false}
        ondrop={handleDrop}
        onclick={() => document.getElementById('file-input')?.click()}
      >
        <input id="file-input" type="file" accept=".cpp,.cc,.cxx,.c,.hpp,.h" class="hidden" onchange={handleSelect} />
        <div class="absolute inset-0 bg-gradient-to-br from-[var(--accent)]/5 to-transparent opacity-0 group-hover:opacity-100 transition-opacity"></div>

        <div class="p-10 text-center relative z-10">
          {#if file}
            <div class="flex items-center justify-center gap-4">
              <div class="w-12 h-12 rounded-2xl flex items-center justify-center" style="background: var(--emerald-dim);">
                <svg class="w-5 h-5 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                  <path stroke-linecap="round" stroke-linejoin="round" d="M9 12.75L11.25 15 15 9.75M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/>
                </svg>
              </div>
              <div class="text-left">
                <div class="text-base font-semibold mono">{file.name}</div>
                <div class="text-xs text-[var(--text-muted)]">{(file.size / 1024).toFixed(1)} KB · click to change</div>
              </div>
            </div>
          {:else}
            <div class="w-14 h-14 mx-auto rounded-2xl flex items-center justify-center mb-4 group-hover:scale-110 transition-transform" style="background: var(--accent-glow);">
              <svg class="w-6 h-6 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
              </svg>
            </div>
            <div class="text-base font-semibold mb-2">Drop your source file here</div>
            <div class="text-xs text-[var(--text-muted)]">Accepts .cpp .cc .cxx .c .hpp .h · Max 50MB</div>
          {/if}
        </div>

        <!-- Upload progress bar -->
        {#if uploadProgress > 0 && uploadProgress < 100}
          <div class="h-1 bg-[var(--bg-elevated)]">
            <div class="h-full bg-[var(--accent)] transition-all duration-300" style="width: {uploadProgress}%"></div>
          </div>
        {/if}
      </button>

      <!-- Code preview -->
      {#if file && fileContent}
        <div class="card p-0 overflow-hidden">
          <div class="px-5 py-3 border-b border-[var(--border)] flex items-center justify-between">
            <span class="text-xs mono text-[var(--text-secondary)]">{file.name}</span>
            <span class="text-[10px] text-[var(--text-ghost)] mono">{fileContent.split('\n').length} lines</span>
          </div>
          <div class="p-4 overflow-x-auto max-h-[300px] overflow-y-auto bg-[#0a0a0a]">
            <pre class="text-xs mono text-[var(--text-secondary)] leading-relaxed"><code>{#each getPreviewLines() as line, i}<span class="text-[var(--text-ghost)] select-none mr-4">{String(i + 1).padStart(3)}</span>{line}
{/each}</code></pre>
            {#if fileContent.split('\n').length > 25}
              <div class="text-[var(--text-ghost)] mt-2 text-xs">... {fileContent.split('\n').length - 25} more lines</div>
            {/if}
          </div>
        </div>
      {/if}

      <!-- Optimization Hints -->
      {#if !file}
        <div class="card p-5 border-[var(--border-subtle)] fade-in">
          <div class="text-[10px] font-bold uppercase tracking-widest text-[var(--accent)] mb-3 flex items-center gap-2">
            <svg class="w-3.5 h-3.5" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24"><path stroke-linecap="round" stroke-linejoin="round" d="M12 18v-5.25m0 0a6.01 6.01 0 001.5-.189m-1.5.189a6.01 6.01 0 01-1.5-.189m3.75 7.478a12.06 12.06 0 01-4.5 0m3.75 2.383a14.406 14.406 0 01-3 0M14.25 18v-.192c0-.983.658-1.829 1.58-1.936a4.61 4.61 0 00-1.58-1.936M9.75 18v-.192c0-.983-.658-1.829-1.58-1.936a4.61 4.61 0 011.58-1.936"/></svg>
            System Hints
          </div>
          <ul class="space-y-2 text-xs text-[var(--text-muted)] mono">
            <li>> Align your order structs to 64-byte cache lines.</li>
            <li>> Avoid dynamic allocation (`new`/`malloc`) on the hot path.</li>
            <li>> Branch prediction failures cost ~15 cycles. Avoid conditionals in loops.</li>
          </ul>
        </div>
      {/if}

      <!-- Submit Button -->
      <button
        class="btn btn-primary btn-pill w-full py-4 text-sm group"
        disabled={!file || submitting || cooldownRemaining > 0 || jobPhase === 'compiling' || jobPhase === 'running' || jobPhase === 'queued'}
        onclick={submit}
      >
        {#if submitting}
          <svg class="w-4 h-4" style="animation: spin-slow 1s linear infinite;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5">
            <path d="M12 2v4m0 12v4m-7.07-3.93l2.83-2.83m8.48-8.48l2.83-2.83M2 12h4m12 0h4M4.93 4.93l2.83 2.83m8.48 8.48l2.83 2.83" stroke-linecap="round"/>
          </svg>
          <span class="mono">Uploading...</span>
        {:else if jobPhase === 'queued'}
          <span class="mono">Queued — waiting...</span>
        {:else if jobPhase === 'compiling'}
          <span class="mono">Compiling...</span>
        {:else if jobPhase === 'running'}
          <span class="mono">Running Benchmark...</span>
        {:else if cooldownRemaining > 0}
          <span class="mono uppercase tracking-wider font-bold">Cooldown {Math.floor(cooldownRemaining / 60)}:{String(cooldownRemaining % 60).padStart(2, '0')}</span>
        {:else}
          <span class="relative z-10 mono uppercase tracking-wider font-bold">Deploy Engine</span>
          <svg class="w-4 h-4 transition-transform group-hover:translate-x-0.5" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M13.5 4.5L21 12m0 0l-7.5 7.5M21 12H3"/>
          </svg>
        {/if}
      </button>

      <!-- Terminal Output -->
      {#if terminalLines.length > 0}
        <div class="terminal fade-up">
          <div class="terminal-header flex justify-between items-center px-4 py-2 bg-[#0a0a0a] border-b border-[var(--border-subtle)]">
            <div class="flex items-center gap-2">
              <div class="terminal-dot" style="background: #ff5f56;"></div>
              <div class="terminal-dot" style="background: #ffbd2e;"></div>
              <div class="terminal-dot" style="background: #27c93f;"></div>
              <span class="text-[10px] text-[var(--text-ghost)] ml-2 mono uppercase tracking-wider">iicpc-runner</span>
            </div>
            <span class="text-[9px] mono text-[var(--text-ghost)]">{jobPhase.toUpperCase()}</span>
          </div>
          <div class="terminal-body">
            {#each terminalLines as line}
              {#if line.text === ''}
                <br/>
              {:else}
                <div class="terminal-{line.type}">{line.text}</div>
              {/if}
            {/each}
            {#if jobPhase === 'compiling' || jobPhase === 'running' || jobPhase === 'uploading' || jobPhase === 'queued'}
              <span class="terminal-prompt" style="animation: pulse-dot 1s ease infinite;">▊</span>
            {/if}
          </div>
        </div>
      {/if}

      <!-- Score Result -->
      {#if jobPhase === 'scored' && jobResult}
        <div class="card p-6 border-emerald-500/20 fade-up">
          <div class="flex items-center gap-3 mb-4">
            <div class="w-10 h-10 rounded-xl flex items-center justify-center" style="background: var(--emerald-dim);">
              <svg class="w-5 h-5 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M4.5 12.75l6 6 9-13.5"/>
              </svg>
            </div>
            <div>
              <div class="font-semibold display">Benchmark Complete</div>
              <div class="text-xs text-[var(--text-muted)] mono mt-0.5">Score: {jobResult.score?.toFixed(4)}</div>
            </div>
          </div>
          <button class="btn btn-ghost text-xs w-full" onclick={() => goto('/dashboard/leaderboard')}>
            View Rankings →
          </button>
        </div>
      {/if}

      {#if result && !result.success}
        <div class="card p-5 border-rose-500/20 fade-up">
          <div class="flex items-start gap-3">
            <svg class="w-5 h-5 text-rose-400 flex-shrink-0 mt-0.5" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M6 18L18 6M6 6l12 12"/>
            </svg>
            <div>
              <div class="font-semibold text-sm text-rose-400">Submission Failed</div>
              <div class="text-xs text-[var(--text-secondary)] mt-1">{result.error}</div>
            </div>
          </div>
        </div>
      {/if}
    </div>

    <!-- Right: Submission History -->
    <div class="lg:col-span-2">
      <div class="card p-0 overflow-hidden sticky top-8">
        <div class="px-5 py-4 border-b border-[var(--border)]">
          <span class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)]">Your Submissions</span>
        </div>
        {#if submissions.length > 0}
          <div class="divide-y divide-[var(--border-subtle)]">
            {#each submissions as sub}
              <div class="px-5 py-4 hover:bg-white/[0.01] transition-colors">
                <div class="flex items-center justify-between mb-1.5">
                  <span class="text-sm font-medium mono">{sub.filename}</span>
                  <span class="badge {statusLabel(sub.status)}">{sub.status}</span>
                </div>
                <div class="flex items-center justify-between text-[10px] text-[var(--text-muted)]">
                  <span class="mono">{sub.job_id.slice(0, 8)}</span>
                  <span>{timeAgo(sub.submitted_at)}</span>
                </div>
                {#if sub.score != null}
                  <div class="mt-2">
                    <div class="text-xs text-[var(--text-muted)] mb-1">Score</div>
                    <div class="progress-bar">
                      <div class="progress-bar-fill" style="width: {sub.score * 100}%"></div>
                    </div>
                    <div class="text-right text-xs mono text-[var(--text-secondary)] mt-1">{sub.score.toFixed(3)}</div>
                  </div>
                {/if}
                {#if sub.error}
                  <div class="mt-2 text-xs text-rose-400 mono">{sub.error.slice(0, 100)}</div>
                {/if}
              </div>
            {/each}
          </div>
        {:else}
          <div class="px-5 py-12 text-center">
            <div class="text-[var(--text-ghost)] text-sm">No submissions yet</div>
            <div class="text-[10px] text-[var(--text-ghost)] mt-1">Upload your first engine above</div>
          </div>
        {/if}

        <!-- Spec card -->
        <div class="px-5 py-4 border-t border-[var(--border)] bg-[rgba(255,255,255,0.01)]">
          <span class="text-[10px] font-bold uppercase tracking-[0.1em] text-[var(--text-ghost)] block mb-3">Benchmark Spec</span>
          <div class="space-y-2 text-[11px]">
            <div class="flex justify-between">
              <span class="text-[var(--text-muted)]">Duration</span>
              <span class="mono text-[var(--text-secondary)]">30s</span>
            </div>
            <div class="flex justify-between">
              <span class="text-[var(--text-muted)]">Bot workers</span>
              <span class="mono text-[var(--text-secondary)]">8 threads</span>
            </div>
            <div class="flex justify-between">
              <span class="text-[var(--text-muted)]">Isolation</span>
              <span class="mono text-[var(--text-secondary)]">cgroups v2</span>
            </div>
            <div class="flex justify-between">
              <span class="text-[var(--text-muted)]">CPU</span>
              <span class="mono text-[var(--text-secondary)]">Pinned cores</span>
            </div>
            <div class="flex justify-between">
              <span class="text-[var(--text-muted)]">Compiler</span>
              <span class="mono text-[var(--text-secondary)]">g++ -O3 -march=native</span>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</div>
{/if}
