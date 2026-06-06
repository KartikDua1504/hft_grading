<script lang="ts">
  import { onMount } from 'svelte';
  import { goto } from '$app/navigation';
  import { auth } from '$lib/auth';

  let mounted = $state(false);

  onMount(() => {
    mounted = true;
    const unsub = auth.subscribe(s => {
      if (!s.authenticated) goto('/auth');
    });
    return () => unsub();
  });
</script>

<svelte:head>
  <title>Submission Guidelines — IICPC Arena</title>
</svelte:head>

{#if mounted}
<div class="w-full xl:max-w-[1400px] mx-auto px-6 fade-in">
  <!-- Header -->
  <div class="mb-10 border-b border-[var(--border)] pb-5">
    <div class="flex items-center gap-2 mb-3">
      <div class="w-2 h-2 rounded-full bg-[var(--accent)] flicker"></div>
      <span class="text-[10px] mono uppercase tracking-widest text-[var(--accent)]">Documentation</span>
    </div>
    <h1 class="text-4xl font-black display tracking-tight mb-2 glitch-hover">Submission Guidelines</h1>
    <p class="text-sm text-[var(--text-secondary)] max-w-2xl">Everything you need to know to write, submit, and benchmark your orderbook matching engine.</p>
  </div>

  <div class="grid lg:grid-cols-3 gap-8">
    <!-- Main Content -->
    <div class="lg:col-span-2 space-y-8">

      <!-- What You're Building -->
      <div class="card p-8">
        <div class="flex items-center gap-3 mb-5">
          <div class="w-10 h-10 rounded-xl flex items-center justify-center" style="background: var(--accent-glow);">
            <svg class="w-5 h-5 text-[var(--accent)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M17.25 6.75L22.5 12l-5.25 5.25m-10.5 0L1.5 12l5.25-5.25m7.5-3l-4.5 16.5"/>
            </svg>
          </div>
          <h2 class="text-xl font-bold display">What You're Building</h2>
        </div>
        <div class="space-y-4 text-sm text-[var(--text-secondary)] leading-relaxed">
          <p>You are implementing a <span class="text-[var(--text-primary)] font-semibold">limit order book matching engine</span> that receives orders from the platform and processes them. Your engine must implement the <code class="mono bg-[var(--bg-elevated)] px-1.5 py-0.5 rounded text-[var(--accent)]">IStrategy</code> interface from the SDK.</p>
          <p>The platform compiles your C++ source, then <span class="text-[var(--text-primary)] font-semibold">blasts realistic order flow</span> at your engine for 10 seconds. You must match crossing orders, maintain the book, and send responses. Your score is based on:</p>
          <div class="grid sm:grid-cols-3 gap-3 mt-4">
            <div class="bg-[var(--bg-surface)] rounded-xl p-4 border border-[var(--border-subtle)]">
              <div class="text-2xl font-black mono text-[var(--accent)] mb-1">40%</div>
              <div class="text-xs font-bold uppercase tracking-wider text-[var(--text-muted)] mb-1">Correctness</div>
              <div class="text-[11px] text-[var(--text-ghost)]">Fills validated against shadow orderbook</div>
            </div>
            <div class="bg-[var(--bg-surface)] rounded-xl p-4 border border-[var(--border-subtle)]">
              <div class="text-2xl font-black mono text-[var(--cyan)] mb-1">30%</div>
              <div class="text-xs font-bold uppercase tracking-wider text-[var(--text-muted)] mb-1">Throughput</div>
              <div class="text-[11px] text-[var(--text-ghost)]">Orders processed per second</div>
            </div>
            <div class="bg-[var(--bg-surface)] rounded-xl p-4 border border-[var(--border-subtle)]">
              <div class="text-2xl font-black mono text-[var(--amber)] mb-1">30%</div>
              <div class="text-xs font-bold uppercase tracking-wider text-[var(--text-muted)] mb-1">Latency</div>
              <div class="text-[11px] text-[var(--text-ghost)]">p99 round-trip response time</div>
            </div>
          </div>
        </div>
      </div>

      <!-- File Requirements -->
      <div class="card p-8">
        <div class="flex items-center gap-3 mb-5">
          <div class="w-10 h-10 rounded-xl flex items-center justify-center" style="background: var(--emerald-dim);">
            <svg class="w-5 h-5 text-[var(--emerald)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M9 12.75L11.25 15 15 9.75M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/>
            </svg>
          </div>
          <h2 class="text-xl font-bold display">File Requirements</h2>
        </div>
        <div class="space-y-4">
          <div class="flex items-start gap-3 text-sm">
            <span class="text-[var(--emerald)] mono font-bold mt-0.5">✓</span>
            <div>
              <span class="text-[var(--text-primary)] font-medium">Accepted extensions:</span>
              <span class="text-[var(--text-secondary)]"> <code class="mono text-[var(--accent)]">.cpp</code> <code class="mono text-[var(--accent)]">.cc</code> <code class="mono text-[var(--accent)]">.cxx</code> <code class="mono text-[var(--accent)]">.c</code> <code class="mono text-[var(--accent)]">.h</code> <code class="mono text-[var(--accent)]">.hpp</code></span>
            </div>
          </div>
          <div class="flex items-start gap-3 text-sm">
            <span class="text-[var(--emerald)] mono font-bold mt-0.5">✓</span>
            <div>
              <span class="text-[var(--text-primary)] font-medium">Max file size:</span>
              <span class="text-[var(--text-secondary)]"> 50 MB</span>
            </div>
          </div>
          <div class="flex items-start gap-3 text-sm">
            <span class="text-[var(--emerald)] mono font-bold mt-0.5">✓</span>
            <div>
              <span class="text-[var(--text-primary)] font-medium">Compiler:</span>
              <span class="text-[var(--text-secondary)]"> <code class="mono">g++ -std=c++23 -O2 -Wall -Wextra</code></span>
            </div>
          </div>
          <div class="flex items-start gap-3 text-sm">
            <span class="text-[var(--emerald)] mono font-bold mt-0.5">✓</span>
            <div>
              <span class="text-[var(--text-primary)] font-medium">SDK include path:</span>
              <span class="text-[var(--text-secondary)]"> <code class="mono">sdk/include/</code> is available via <code class="mono">#include "sdk/strategy_sdk.hpp"</code></span>
            </div>
          </div>
          <div class="flex items-start gap-3 text-sm">
            <span class="text-rose-400 mono font-bold mt-0.5">✗</span>
            <div>
              <span class="text-[var(--text-primary)] font-medium">No external dependencies.</span>
              <span class="text-[var(--text-secondary)]"> Only standard library + SDK headers.</span>
            </div>
          </div>
          <div class="flex items-start gap-3 text-sm">
            <span class="text-rose-400 mono font-bold mt-0.5">✗</span>
            <div>
              <span class="text-[var(--text-primary)] font-medium">No network access, no filesystem access, no fork/exec.</span>
            </div>
          </div>
        </div>
      </div>

      <!-- Strategy Interface -->
      <div class="card p-8">
        <div class="flex items-center gap-3 mb-5">
          <div class="w-10 h-10 rounded-xl flex items-center justify-center" style="background: var(--cyan-dim);">
            <svg class="w-5 h-5 text-[var(--cyan)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M14.25 9.75L16.5 12l-2.25 2.25m-4.5 0L7.5 12l2.25-2.25M6 20.25h12A2.25 2.25 0 0020.25 18V6A2.25 2.25 0 0018 3.75H6A2.25 2.25 0 003.75 6v12A2.25 2.25 0 006 20.25z"/>
            </svg>
          </div>
          <h2 class="text-xl font-bold display">Engine Interface</h2>
        </div>
        <p class="text-sm text-[var(--text-secondary)] mb-4">Your submission must implement these callbacks and export a <code class="mono text-[var(--accent)]">create_strategy()</code> factory function:</p>
        <div class="bg-[#0a0a0a] rounded-xl p-5 overflow-x-auto border border-[var(--border-subtle)]">
          <pre class="text-xs mono text-[var(--text-secondary)] leading-relaxed"><code><span class="text-[var(--text-ghost)]">// Required: #include "sdk/strategy_sdk.hpp"</span>

<span class="text-[var(--cyan)]">class</span> <span class="text-[var(--accent)]">MyEngine</span> : <span class="text-[var(--cyan)]">public</span> iicpc::IStrategy {'{'} 
  <span class="text-[var(--cyan)]">void</span> <span class="text-[var(--emerald)]">on_session_start</span>(const SessionStart&) <span class="text-[var(--cyan)]">override</span>;
  <span class="text-[var(--cyan)]">void</span> <span class="text-[var(--emerald)]">on_order</span>(const OrderEntry& order, IResponseSender& sender) <span class="text-[var(--cyan)]">override</span>;
  <span class="text-[var(--cyan)]">void</span> <span class="text-[var(--emerald)]">on_cancel</span>(const CancelRequest& cancel, IResponseSender& sender) <span class="text-[var(--cyan)]">override</span>;
  <span class="text-[var(--cyan)]">void</span> <span class="text-[var(--emerald)]">on_session_end</span>(const SessionEnd&) <span class="text-[var(--cyan)]">override</span>;
{'}'};

<span class="text-[var(--text-ghost)]">// Factory — required by the platform</span>
<span class="text-[var(--cyan)]">namespace</span> iicpc {'{'} 
  IStrategy* <span class="text-[var(--emerald)]">create_strategy</span>() {'{'} 
    <span class="text-[var(--cyan)]">static</span> MyEngine instance;
    <span class="text-[var(--cyan)]">return</span> &instance;
  {'}'}
{'}'}</code></pre>
        </div>
      </div>

      <!-- Common Errors -->
      <div class="card p-8">
        <div class="flex items-center gap-3 mb-5">
          <div class="w-10 h-10 rounded-xl flex items-center justify-center" style="background: rgba(244,63,94,0.1);">
            <svg class="w-5 h-5 text-rose-400" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M12 9v3.75m-9.303 3.376c-.866 1.5.217 3.374 1.948 3.374h14.71c1.73 0 2.813-1.874 1.948-3.374L13.949 3.378c-.866-1.5-3.032-1.5-3.898 0L2.697 16.126zM12 15.75h.007v.008H12v-.008z"/>
            </svg>
          </div>
          <h2 class="text-xl font-bold display">Common Errors</h2>
        </div>
        <div class="space-y-4">
          <div class="bg-[var(--bg-surface)] rounded-xl p-4 border border-[var(--border-subtle)]">
            <div class="text-sm font-semibold text-rose-400 mono mb-1">Compilation Failed</div>
            <div class="text-xs text-[var(--text-muted)]">Syntax errors, missing includes, or wrong C++ standard. The compiler error message will show exactly what's wrong.</div>
          </div>
          <div class="bg-[var(--bg-surface)] rounded-xl p-4 border border-[var(--border-subtle)]">
            <div class="text-sm font-semibold text-rose-400 mono mb-1">SIGSEGV (Segmentation Fault)</div>
            <div class="text-xs text-[var(--text-muted)]">Null pointer dereference, out-of-bounds access, or use-after-free. Check your pointer arithmetic and array bounds.</div>
          </div>
          <div class="bg-[var(--bg-surface)] rounded-xl p-4 border border-[var(--border-subtle)]">
            <div class="text-sm font-semibold text-rose-400 mono mb-1">Engine Failed to Connect</div>
            <div class="text-xs text-[var(--text-muted)]">Your code compiled but didn't connect to the gateway. Make sure you implement <code class="mono">iicpc::create_strategy()</code> and include <code class="mono">strategy_main.cpp</code> in your build.</div>
          </div>
          <div class="bg-[var(--bg-surface)] rounded-xl p-4 border border-[var(--border-subtle)]">
            <div class="text-sm font-semibold text-rose-400 mono mb-1">Benchmark Timed Out</div>
            <div class="text-xs text-[var(--text-muted)]">Infinite loop, deadlock, or blocking I/O. All callbacks must return quickly — avoid <code class="mono">sleep()</code>, mutexes, or heavy I/O.</div>
          </div>
        </div>
      </div>

      <!-- Performance Tips -->
      <div class="card p-8">
        <div class="flex items-center gap-3 mb-5">
          <div class="w-10 h-10 rounded-xl flex items-center justify-center" style="background: var(--amber-dim);">
            <svg class="w-5 h-5 text-[var(--amber)]" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
              <path stroke-linecap="round" stroke-linejoin="round" d="M3.75 13.5l10.5-11.25L12 10.5h8.25L9.75 21.75 12 13.5H3.75z"/>
            </svg>
          </div>
          <h2 class="text-xl font-bold display">Performance Tips</h2>
        </div>
        <div class="space-y-3 text-sm text-[var(--text-secondary)]">
          <div class="flex items-start gap-3">
            <span class="text-[var(--amber)] mono font-bold mt-0.5">></span>
            <span>Align structs to <strong class="text-[var(--text-primary)]">64-byte cache lines</strong> to avoid false sharing.</span>
          </div>
          <div class="flex items-start gap-3">
            <span class="text-[var(--amber)] mono font-bold mt-0.5">></span>
            <span>Avoid <code class="mono text-rose-400">new</code>/<code class="mono text-rose-400">malloc</code> on the hot path. Pre-allocate everything in <code class="mono">on_session_start()</code>.</span>
          </div>
          <div class="flex items-start gap-3">
            <span class="text-[var(--amber)] mono font-bold mt-0.5">></span>
            <span>Branch mispredictions cost ~15 cycles. Use <strong class="text-[var(--text-primary)]">branchless logic</strong> or sorted structures.</span>
          </div>
          <div class="flex items-start gap-3">
            <span class="text-[var(--amber)] mono font-bold mt-0.5">></span>
            <span>Use <code class="mono text-[var(--accent)]">__builtin_expect()</code> for unlikely error paths.</span>
          </div>
          <div class="flex items-start gap-3">
            <span class="text-[var(--amber)] mono font-bold mt-0.5">></span>
            <span>Keep your working set under <strong class="text-[var(--text-primary)]">L1 cache (32KB)</strong>. Profile with <code class="mono">perf stat</code>.</span>
          </div>
          <div class="flex items-start gap-3">
            <span class="text-[var(--amber)] mono font-bold mt-0.5">></span>
            <span>Your p99 latency is your <em>actual</em> latency. Optimize tail, not average.</span>
          </div>
        </div>
      </div>
    </div>

    <!-- Sidebar -->
    <div class="space-y-5">
      <!-- Quick Start -->
      <div class="card p-6 sticky top-8">
        <h3 class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)] mb-4">Quick Start</h3>
        <div class="space-y-3 text-sm">
          <div class="flex items-start gap-3">
            <div class="w-6 h-6 rounded-lg flex items-center justify-center bg-[var(--accent)]/[0.08] text-[var(--accent)] text-[10px] font-bold mono flex-shrink-0">1</div>
            <span class="text-[var(--text-secondary)]">Copy the example strategy from <code class="mono text-xs text-[var(--accent)]">sdk/examples/example_mm.cpp</code></span>
          </div>
          <div class="flex items-start gap-3">
            <div class="w-6 h-6 rounded-lg flex items-center justify-center bg-[var(--accent)]/[0.08] text-[var(--accent)] text-[10px] font-bold mono flex-shrink-0">2</div>
            <span class="text-[var(--text-secondary)]">Implement your orderbook matching logic in the callbacks</span>
          </div>
          <div class="flex items-start gap-3">
            <div class="w-6 h-6 rounded-lg flex items-center justify-center bg-[var(--accent)]/[0.08] text-[var(--accent)] text-[10px] font-bold mono flex-shrink-0">3</div>
            <span class="text-[var(--text-secondary)]">Upload your <code class="mono text-xs">.cpp</code> file via the Submit page</span>
          </div>
          <div class="flex items-start gap-3">
            <div class="w-6 h-6 rounded-lg flex items-center justify-center bg-[var(--accent)]/[0.08] text-[var(--accent)] text-[10px] font-bold mono flex-shrink-0">4</div>
            <span class="text-[var(--text-secondary)]">Watch compilation + benchmarking in the terminal</span>
          </div>
          <div class="flex items-start gap-3">
            <div class="w-6 h-6 rounded-lg flex items-center justify-center bg-[var(--accent)]/[0.08] text-[var(--accent)] text-[10px] font-bold mono flex-shrink-0">5</div>
            <span class="text-[var(--text-secondary)]">Check the leaderboard for your ranking</span>
          </div>
        </div>
        <button class="btn btn-primary btn-pill w-full py-3 text-sm mt-5 group" onclick={() => goto('/dashboard/submit')}>
          <span class="mono uppercase tracking-wider font-bold">Submit Now</span>
          <svg class="w-4 h-4 transition-transform group-hover:translate-x-0.5" fill="none" stroke="currentColor" stroke-width="2" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M13.5 4.5L21 12m0 0l-7.5 7.5M21 12H3"/>
          </svg>
        </button>
        <a href="/api/sdk/download" download class="btn w-full py-3 text-sm mt-2 group flex items-center justify-center gap-2"
           style="background: var(--bg-elevated); border: 1px solid var(--border); color: var(--text-secondary); border-radius: 999px;">
          <svg class="w-4 h-4" fill="none" stroke="currentColor" stroke-width="1.5" viewBox="0 0 24 24">
            <path stroke-linecap="round" stroke-linejoin="round" d="M3 16.5v2.25A2.25 2.25 0 005.25 21h13.5A2.25 2.25 0 0021 18.75V16.5M16.5 12L12 16.5m0 0L7.5 12m4.5 4.5V3"/>
          </svg>
          <span class="mono uppercase tracking-wider font-bold text-xs">Download SDK</span>
        </a>
      </div>

      <!-- Benchmark Spec -->
      <div class="card p-6">
        <h3 class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)] mb-4">Benchmark Environment</h3>
        <div class="space-y-2.5 text-[12px]">
          <div class="flex justify-between">
            <span class="text-[var(--text-muted)]">Duration</span>
            <span class="mono text-[var(--text-secondary)]">10s (dev) / 30s (prod)</span>
          </div>
          <div class="flex justify-between">
            <span class="text-[var(--text-muted)]">Order rate</span>
            <span class="mono text-[var(--text-secondary)]">100K ops/sec</span>
          </div>
          <div class="flex justify-between">
            <span class="text-[var(--text-muted)]">Order types</span>
            <span class="mono text-[var(--text-secondary)]">60% limit / 20% market / 20% cancel</span>
          </div>
          <div class="flex justify-between">
            <span class="text-[var(--text-muted)]">Batch size</span>
            <span class="mono text-[var(--text-secondary)]">128 orders/batch</span>
          </div>
          <div class="flex justify-between">
            <span class="text-[var(--text-muted)]">Compile timeout</span>
            <span class="mono text-[var(--text-secondary)]">30s</span>
          </div>
          <div class="flex justify-between">
            <span class="text-[var(--text-muted)]">Run timeout</span>
            <span class="mono text-[var(--text-secondary)]">120s</span>
          </div>
          <div class="flex justify-between">
            <span class="text-[var(--text-muted)]">Cooldown</span>
            <span class="mono text-[var(--text-secondary)]">3 min between submissions</span>
          </div>
        </div>
      </div>

      <!-- Rate Limits -->
      <div class="card p-6">
        <h3 class="text-xs font-bold uppercase tracking-[0.1em] text-[var(--text-secondary)] mb-4">Rate Limits</h3>
        <div class="space-y-2 text-xs text-[var(--text-muted)]">
          <div>• 3 minute cooldown between submissions</div>
          <div>• Max 50 jobs in queue</div>
          <div>• Max 50MB per file upload</div>
          <div>• Best score is kept on leaderboard</div>
        </div>
      </div>
    </div>
  </div>
</div>
{/if}
