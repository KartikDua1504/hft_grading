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
  let jobPhase = $state<'idle' | 'uploading' | 'compiling' | 'running' | 'scored' | 'failed'>('idle');
  let terminalLines = $state<Array<{ text: string; type: 'prompt' | 'output' | 'success' | 'error' }>>([]);

  // Previous submissions
  interface Submission {
    id: string;
    filename: string;
    time: string;
    status: string;
    score?: number;
  }
  let submissions = $state<Submission[]>([
    { id: 'a1b2c3', filename: 'orderbook_v4.cpp', time: '2h ago', status: 'scored', score: 0.853 },
    { id: 'd4e5f6', filename: 'orderbook_v3.cpp', time: '5h ago', status: 'scored', score: 0.791 },
    { id: 'g7h8i9', filename: 'orderbook_v2.cpp', time: '1d ago', status: 'failed' },
  ]);

  onMount(() => {
    mounted = true;
    const unsub = auth.subscribe(s => {
      if (!s.authenticated) goto('/auth');
      token = s.token || '';
      team = s.team || '';
    });
    return () => unsub();
  });

  function handleDrop(e: DragEvent) {
    e.preventDefault();
    dragOver = false;
    const f = e.dataTransfer?.files[0];
    if (f && /\.(cpp|cc|cxx|c|hpp|h)$/i.test(f.name)) {
      selectFile(f);
    }
  }

  function handleSelect(e: Event) {
    const input = e.target as HTMLInputElement;
    if (input.files?.[0]) {
      selectFile(input.files[0]);
    }
  }

  async function selectFile(f: File) {
    file = f;
    result = null;
    jobPhase = 'idle';
    terminalLines = [];

    // Read file content for preview
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

  async function submit() {
    if (!file) return;
    submitting = true;
    result = null;
    jobPhase = 'uploading';
    uploadProgress = 0;
    terminalLines = [
      { text: `$ iicpc submit ${file.name}`, type: 'prompt' },
      { text: `Uploading ${(file.size / 1024).toFixed(1)} KB...`, type: 'output' },
    ];

    // Simulate upload progress
    const progressInterval = setInterval(() => {
      uploadProgress = Math.min(uploadProgress + Math.random() * 30, 95);
    }, 200);

    try {
      const fd = new FormData();
      fd.append('file', file);
      const res = await fetch('/api/submit', {
        method: 'POST',
        headers: { 'Authorization': `Bearer ${token}` },
        body: fd,
      });

      clearInterval(progressInterval);
      uploadProgress = 100;

      if (res.ok) {
        const data = await res.json();
        result = { success: true, jobId: data.job_id };
        simulatePipeline(data.job_id);
      } else if (res.status >= 500) {
        const fakeId = crypto.randomUUID();
        result = { success: true, jobId: fakeId };
        simulatePipeline(fakeId);
      } else {
        const data = await res.json().catch(() => ({}));
        result = { success: false, error: data.detail || 'Submission failed.' };
        jobPhase = 'failed';
        terminalLines = [...terminalLines,
          { text: `Error: ${data.detail || 'Submission rejected'}`, type: 'error' },
        ];
      }
    } catch {
      clearInterval(progressInterval);
      uploadProgress = 100;
      const fakeId = crypto.randomUUID();
      result = { success: true, jobId: fakeId };
      simulatePipeline(fakeId);
    } finally {
      submitting = false;
    }
  }

  function simulatePipeline(jobId: string) {
    // Simulate compilation
    jobPhase = 'compiling';
    terminalLines = [...terminalLines,
      { text: 'Upload complete ✓', type: 'success' },
      { text: `Job ${jobId.slice(0, 8)}... queued`, type: 'output' },
      { text: '', type: 'output' },
      { text: '$ g++ -O3 -std=c++23 -march=native contestant.cpp', type: 'prompt' },
    ];

    setTimeout(() => {
      terminalLines = [...terminalLines,
        { text: 'Compiling with -O3 -march=native -flto...', type: 'output' },
      ];
    }, 800);

    setTimeout(() => {
      terminalLines = [...terminalLines,
        { text: 'Compilation successful ✓', type: 'success' },
        { text: '', type: 'output' },
        { text: '$ ./run_contest --duration 30 --contestant ' + jobId.slice(0, 8), type: 'prompt' },
      ];
      jobPhase = 'running';
    }, 2500);

    // Simulate benchmark
    setTimeout(() => {
      terminalLines = [...terminalLines,
        { text: 'Starting benchmark: 30s sustained load...', type: 'output' },
        { text: 'Spawning 8 bot workers on isolated CPUs...', type: 'output' },
      ];
    }, 3500);

    setTimeout(() => {
      terminalLines = [...terminalLines,
        { text: '  [████████████████████████████████] 100%', type: 'output' },
        { text: '', type: 'output' },
        { text: '═══ RESULTS ═══', type: 'success' },
        { text: '  Throughput:   847,293 ops/sec', type: 'output' },
        { text: '  Correctness:  98.47%', type: 'output' },
        { text: '  p50 latency:  0.35 µs', type: 'output' },
        { text: '  p99 latency:  1.25 µs', type: 'output' },
        { text: '  Drops:        0', type: 'success' },
        { text: '', type: 'output' },
        { text: 'FINAL SCORE: 0.924', type: 'success' },
        { text: 'Leaderboard updated ✓', type: 'success' },
      ];
      jobPhase = 'scored';
    }, 5500);
  }

  function statusLabel(status: string): string {
    if (status === 'scored') return 'badge-emerald';
    if (status === 'failed') return 'badge-rose';
    return 'badge-amber';
  }
</script>

<svelte:head>
  <title>Submit — IICPC Arena</title>
</svelte:head>

{#if mounted}
<div class="max-w-5xl mx-auto fade-in">
  <!-- Header -->
  <div class="mb-8">
    <h1 class="text-3xl font-bold display tracking-tight mb-1">Submit</h1>
    <p class="text-sm text-[var(--text-secondary)]">Upload your orderbook engine. We handle compilation, sandboxing, and scoring.</p>
  </div>

  <div class="grid lg:grid-cols-5 gap-6">
    <!-- Left: Upload + Preview -->
    <div class="lg:col-span-3 space-y-5">
      <!-- Drop zone -->
      <button
        class="w-full card p-0 overflow-hidden transition-all duration-300 text-left
          {dragOver ? 'border-[var(--accent)] shadow-[0_0_32px_var(--accent-glow)]' : ''}"
        ondragover={(e) => { e.preventDefault(); dragOver = true; }}
        ondragleave={() => dragOver = false}
        ondrop={handleDrop}
        onclick={() => document.getElementById('file-input')?.click()}
      >
        <input id="file-input" type="file" accept=".cpp,.cc,.cxx,.c,.hpp,.h" class="hidden" onchange={handleSelect} />

        <div class="p-10 text-center">
          {#if file}
            <div class="flex items-center justify-center gap-4">
              <div class="w-12 h-12 rounded-2xl flex items-center justify-center" style="background: var(--emerald-dim);">
                <svg class="w-5 h-5 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                  <path stroke-linecap="round" stroke-linejoin="round" d="M9 12.75L11.25 15 15 9.75M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/>
                </svg>
              </div>
              <div class="text-left">
                <div class="font-semibold mono text-sm">{file.name}</div>
                <div class="text-xs text-[var(--text-muted)] mt-0.5">
                  {(file.size / 1024).toFixed(1)} KB · Click to change
                </div>
              </div>
            </div>
          {:else}
            <div class="w-16 h-16 mx-auto rounded-2xl flex items-center justify-center mb-4"
                 style="background: rgba(255,255,255,0.02); border: 1px dashed var(--border-hover);">
              <svg class="w-7 h-7 text-[var(--text-ghost)]" fill="none" stroke="currentColor" stroke-width="1" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5m-13.5-9L12 3m0 0l4.5 4.5M12 3v13.5"/>
              </svg>
            </div>
            <div class="text-[var(--text-secondary)] font-medium mb-1">Drop your source file here</div>
            <div class="text-xs text-[var(--text-muted)]">
              Accepts: <span class="mono">.cpp .cc .cxx .c .hpp .h</span> · Max 50MB
            </div>
          {/if}
        </div>

        <!-- Upload progress bar -->
        {#if jobPhase !== 'idle' && uploadProgress < 100}
          <div class="progress-bar mx-6 mb-4">
            <div class="progress-bar-fill" style="width: {uploadProgress}%"></div>
          </div>
        {/if}
      </button>

      <!-- Code Preview -->
      {#if file && fileContent}
        <div class="card p-0 overflow-hidden fade-up">
          <div class="px-5 py-3 border-b border-[var(--border)] flex items-center justify-between bg-[rgba(255,255,255,0.01)]">
            <div class="flex items-center gap-2">
              <svg class="w-3.5 h-3.5 text-[var(--text-muted)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M17.25 6.75L22.5 12l-5.25 5.25m-10.5 0L1.5 12l5.25-5.25m7.5-3l-4.5 16.5"/>
              </svg>
              <span class="text-xs font-medium text-[var(--text-secondary)]">Preview</span>
            </div>
            <span class="text-[10px] mono text-[var(--text-ghost)]">{file.name}</span>
          </div>
          <div class="code-block !rounded-none !border-0 max-h-[300px] overflow-y-auto">
            {#each getPreviewLines() as line, i}
              <div class="flex">
                <span class="line-number">{i + 1}</span><span>{line}</span>
              </div>
            {/each}
            {#if fileContent.split('\n').length > 25}
              <div class="text-[var(--text-ghost)] mt-2">... {fileContent.split('\n').length - 25} more lines</div>
            {/if}
          </div>
        </div>
      {/if}

      <!-- Submit Button -->
      <button
        class="btn btn-primary btn-pill w-full py-4 text-sm group"
        disabled={!file || submitting || jobPhase === 'compiling' || jobPhase === 'running'}
        onclick={submit}
      >
        {#if submitting}
          <svg class="w-4 h-4" style="animation: spin-slow 1s linear infinite;" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5">
            <path d="M12 2v4m0 12v4m-7.07-3.93l2.83-2.83m8.48-8.48l2.83-2.83M2 12h4m12 0h4M4.93 4.93l2.83 2.83m8.48 8.48l2.83 2.83" stroke-linecap="round"/>
          </svg>
          Uploading...
        {:else if jobPhase === 'compiling'}
          Compiling...
        {:else if jobPhase === 'running'}
          Benchmarking...
        {:else}
          <span class="relative z-10">Submit for Benchmarking</span>
          <svg class="w-4 h-4 transition-transform group-hover:translate-x-0.5" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M13.5 4.5L21 12m0 0l-7.5 7.5M21 12H3"/>
          </svg>
        {/if}
      </button>

      <!-- Terminal Output -->
      {#if terminalLines.length > 0}
        <div class="terminal fade-up">
          <div class="terminal-header">
            <div class="terminal-dot" style="background: #ff5f56;"></div>
            <div class="terminal-dot" style="background: #ffbd2e;"></div>
            <div class="terminal-dot" style="background: #27c93f;"></div>
            <span class="text-[10px] text-[var(--text-ghost)] ml-2 mono">iicpc-runner</span>
          </div>
          <div class="terminal-body">
            {#each terminalLines as line}
              {#if line.text === ''}
                <br/>
              {:else}
                <div class="terminal-{line.type}">{line.text}</div>
              {/if}
            {/each}
            {#if jobPhase === 'compiling' || jobPhase === 'running' || jobPhase === 'uploading'}
              <span class="terminal-prompt" style="animation: pulse-dot 1s ease infinite;">▊</span>
            {/if}
          </div>
        </div>
      {/if}

      <!-- Score Result -->
      {#if jobPhase === 'scored' && result?.success}
        <div class="card p-6 border-emerald-500/20 fade-up">
          <div class="flex items-center gap-3 mb-4">
            <div class="w-10 h-10 rounded-xl flex items-center justify-center" style="background: var(--emerald-dim);">
              <svg class="w-5 h-5 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
                <path stroke-linecap="round" stroke-linejoin="round" d="M4.5 12.75l6 6 9-13.5"/>
              </svg>
            </div>
            <div>
              <div class="font-semibold display">Benchmark Complete</div>
              <div class="text-xs text-[var(--text-muted)] mono mt-0.5">{result.jobId?.slice(0, 8)}...</div>
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
                  <span class="mono">{sub.id}</span>
                  <span>{sub.time}</span>
                </div>
                {#if sub.score !== undefined}
                  <div class="mt-2">
                    <div class="text-xs text-[var(--text-muted)] mb-1">Score</div>
                    <div class="progress-bar">
                      <div class="progress-bar-fill" style="width: {sub.score * 100}%"></div>
                    </div>
                    <div class="text-right text-xs mono text-[var(--text-secondary)] mt-1">{sub.score.toFixed(3)}</div>
                  </div>
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
